/* outgoing mail: queue/<id>.eml + <id>.env, delivered straight to the
   recipient's MX over IPv4 (the SPF record covers the A record), STARTTLS when
   offered. Temporary failures retry for 2 days; permanent ones bounce to the
   sender's inbox. If a relay is configured it's tried when direct delivery
   gets refused. */
#include "queue.h"
#include "mime.h"
#include "store.h"
#include "util.h"

#include <arpa/nameser.h>
#include <curl/curl.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <pthread.h>
#include <resolv.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_RCPT 20
#define GIVE_UP (2 * 86400)

enum { OK, TEMP, PERM };

static pthread_mutex_t qmu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t qcv = PTHREAD_COND_INITIALIZER;
static SSL_CTX *ctx;

struct env {
    char from[320];
    time_t next, first;
    int tries, n;
    char rcpt[MAX_RCPT][320];
};

static void qdir(char *out, size_t n) { snprintf(out, n, "%s/queue", cfg.dir); }

static int env_write(const char *id, const struct env *e)
{
    char d[1100], p[1300], t[1300];
    qdir(d, sizeof d);
    snprintf(p, sizeof p, "%s/%s.env", d, id);
    snprintf(t, sizeof t, "%s/%s.env.tmp", d, id);
    FILE *f = fopen(t, "w");
    if (!f) return -1;
    fprintf(f, "from %s\nnext %ld\nfirst %ld\ntries %d\n", e->from, (long)e->next, (long)e->first, e->tries);
    for (int i = 0; i < e->n; i++) fprintf(f, "rcpt %s\n", e->rcpt[i]);
    int bad = fflush(f) || fsync(fileno(f));
    fclose(f);
    if (bad || rename(t, p)) return -1;
    return 0;
}

static int env_read(const char *path, struct env *e)
{
    memset(e, 0, sizeof *e);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[400];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = 0;
        if (!strncmp(line, "from ", 5)) snprintf(e->from, sizeof e->from, "%s", line + 5);
        else if (!strncmp(line, "next ", 5)) e->next = atol(line + 5);
        else if (!strncmp(line, "first ", 6)) e->first = atol(line + 6);
        else if (!strncmp(line, "tries ", 6)) e->tries = atoi(line + 6);
        else if (!strncmp(line, "rcpt ", 5) && e->n < MAX_RCPT) snprintf(e->rcpt[e->n++], 320, "%s", line + 5);
    }
    fclose(f);
    return 0;
}

int queue_add(const char *from, char rcpts[][320], int n, const char *msg, size_t len)
{
    char d[1100], p[1300], id[64], r[9];
    qdir(d, sizeof d);
    mkdir(d, 0700);
    randhex(r, 4);
    snprintf(id, sizeof id, "%ld.%s", (long)time(NULL), r);
    snprintf(p, sizeof p, "%s/%s.eml", d, id);
    int fd = open(p, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    if (write_all(fd, msg, len) < 0 || fsync(fd) < 0) {
        close(fd);
        unlink(p);
        return -1;
    }
    close(fd);
    struct env e = {.next = time(NULL), .first = time(NULL)};
    snprintf(e.from, sizeof e.from, "%s", from);
    for (int i = 0; i < n && i < MAX_RCPT; i++) snprintf(e.rcpt[e.n++], 320, "%s", rcpts[i]);
    if (env_write(id, &e) < 0) {
        unlink(p);
        return -1;
    }
    pthread_mutex_lock(&qmu);
    pthread_cond_signal(&qcv);
    pthread_mutex_unlock(&qmu);
    return 0;
}

/* ---------- SMTP client */

struct conn {
    int fd;
    SSL *ssl;
    char b[4096];
    size_t s, e;
};

static int c_write(struct conn *c, const char *p, size_t n)
{
    if (c->ssl) {
        while (n) {
            int k = SSL_write(c->ssl, p, n > 16384 ? 16384 : (int)n);
            if (k <= 0) return -1;
            p += k;
            n -= k;
        }
        return 0;
    }
    return write_all(c->fd, p, n);
}

static int c_getc(struct conn *c)
{
    if (c->s == c->e) {
        int k = c->ssl ? SSL_read(c->ssl, c->b, sizeof c->b) : (int)read(c->fd, c->b, sizeof c->b);
        if (k <= 0) return -1;
        c->s = 0;
        c->e = k;
    }
    return (unsigned char)c->b[c->s++];
}

/* reads a (multi-line) reply; returns the code, text gets the lines */
static int reply(struct conn *c, char *text, size_t tn, int *starttls, int *eightbit)
{
    text[0] = 0;
    for (int lines = 0; lines < 100; lines++) {
        char l[1024];
        size_t n = 0;
        int ch;
        while ((ch = c_getc(c)) >= 0 && ch != '\n')
            if (n < sizeof l - 1) l[n++] = ch;
        if (ch < 0) return -1;
        if (n && l[n - 1] == '\r') n--;
        l[n] = 0;
        if (n < 3) return -1;
        if (starttls && n >= 12 && !strncasecmp(l + 4, "STARTTLS", 8)) *starttls = 1;
        if (eightbit && n >= 12 && !strncasecmp(l + 4, "8BITMIME", 8)) *eightbit = 1;
        size_t tl = strlen(text);
        /* continuation lines without their "550-5.7.1" prefix */
        const char *part = l;
        if (tl) {
            part = l + 4;
            const char *sp = strchr(part, ' ');
            if (sp && sp - part < 10 && part[0] >= '2' && part[0] <= '5' && part[1] == '.') part = sp + 1;
        }
        snprintf(text + tl, tn - tl, "%s%s", tl ? " " : "", part);
        if (n == 3 || l[3] == ' ') return atoi(l);
    }
    return -1;
}

static int cmd(struct conn *c, char *text, size_t tn, const char *fmt, const char *arg)
{
    char line[700];
    int n = snprintf(line, sizeof line, fmt, arg);
    if (c_write(c, line, n) < 0) return -1;
    return reply(c, text, tn, NULL, NULL);
}

static int kind(int code) { return code >= 200 && code < 400 ? OK : code >= 500 ? PERM : TEMP; }

/* one SMTP session to one address. res[i] per recipient, why[i] the reason */
static int session(int fd, const char *host, const struct env *e, int *idx, int n, const buf *msg, int *res,
                   char why[][300])
{
    struct conn c = {.fd = fd};
    char t[2048];
    int tls = 0, eight = 0, code;
    for (int i = 0; i < n; i++) res[idx[i]] = TEMP;
#define FAIL_ALL(k, s)                                                                                    \
    do {                                                                                                  \
        for (int i = 0; i < n; i++) {                                                                     \
            res[idx[i]] = (k);                                                                            \
            snprintf(why[idx[i]], 300, "%s: %.250s", host, (s));                                          \
        }                                                                                                 \
        goto out;                                                                                         \
    } while (0)

    code = reply(&c, t, sizeof t, NULL, NULL);
    if (code < 0) FAIL_ALL(TEMP, "connection dropped");
    if (code != 220) FAIL_ALL(kind(code) == PERM ? PERM : TEMP, t);
    char ehlo[300];
    snprintf(ehlo, sizeof ehlo, "EHLO %s\r\n", cfg.domain);
    if (c_write(&c, ehlo, strlen(ehlo)) < 0 || (code = reply(&c, t, sizeof t, &tls, &eight)) < 0)
        FAIL_ALL(TEMP, "connection dropped");
    if (code != 250) FAIL_ALL(kind(code), t);
    if (tls) {
        code = cmd(&c, t, sizeof t, "STARTTLS\r\n%s", "");
        if (code == 220) {
            c.ssl = SSL_new(ctx);
            SSL_set_fd(c.ssl, fd);
            SSL_set_tlsext_host_name(c.ssl, host);
            if (SSL_connect(c.ssl) != 1) {
                SSL_free(c.ssl);
                c.ssl = NULL;
                FAIL_ALL(TEMP, "TLS handshake failed");
            }
            c.s = c.e = 0;
            eight = 0;
            if (c_write(&c, ehlo, strlen(ehlo)) < 0 || (code = reply(&c, t, sizeof t, NULL, &eight)) != 250)
                FAIL_ALL(TEMP, "EHLO after STARTTLS failed");
        }
    }
    char mf[400];
    snprintf(mf, sizeof mf, "MAIL FROM:<%s>%s\r\n", e->from, eight ? " BODY=8BITMIME" : "");
    if ((code = cmd(&c, t, sizeof t, "%s", mf)) < 0) FAIL_ALL(TEMP, "connection dropped");
    if (kind(code) != OK) FAIL_ALL(kind(code), t);
    int accepted = 0;
    for (int i = 0; i < n; i++) {
        if ((code = cmd(&c, t, sizeof t, "RCPT TO:<%s>\r\n", e->rcpt[idx[i]])) < 0) FAIL_ALL(TEMP, "connection dropped");
        res[idx[i]] = kind(code);
        if (res[idx[i]] == OK) {
            res[idx[i]] = -1;   /* accepted, waiting for DATA */
            accepted++;
        } else snprintf(why[idx[i]], 300, "%s: %.250s", host, t);
    }
    if (accepted) {
        if ((code = cmd(&c, t, sizeof t, "DATA\r\n%s", "")) != 354) {
            for (int i = 0; i < n; i++)
                if (res[idx[i]] == -1) {
                    res[idx[i]] = code < 0 ? TEMP : kind(code);
                    snprintf(why[idx[i]], 300, "%s: %.250s", host, code < 0 ? "connection dropped" : t);
                }
            goto out;
        }
        /* dot-stuffing; the message already has CRLF line ends */
        buf d = {0};
        const char *p = msg->p, *end = msg->p + msg->n;
        while (p < end) {
            const char *nl = memchr(p, '\n', end - p);
            size_t len = nl ? (size_t)(nl - p + 1) : (size_t)(end - p);
            if (*p == '.') bputs(&d, ".");
            bput(&d, p, len);
            p += len;
        }
        if (d.n && d.p[d.n - 1] != '\n') bputs(&d, "\r\n");
        bputs(&d, ".\r\n");
        int w = c_write(&c, d.p, d.n);
        bfree(&d);
        code = w < 0 ? -1 : reply(&c, t, sizeof t, NULL, NULL);
        for (int i = 0; i < n; i++)
            if (res[idx[i]] == -1) {
                res[idx[i]] = code < 0 ? TEMP : kind(code);
                snprintf(why[idx[i]], 300, "%s: %.250s", host, code < 0 ? "connection dropped after the message" : t);
            }
    }
    cmd(&c, t, sizeof t, "QUIT\r\n%s", "");
out:
    if (c.ssl) {
        SSL_shutdown(c.ssl);
        SSL_free(c.ssl);
    }
    return 0;
#undef FAIL_ALL
}

struct mx {
    int pref;
    char host[256];
};

static int cmp_mx(const void *a, const void *b) { return ((const struct mx *)a)->pref - ((const struct mx *)b)->pref; }

/* -1 = domain doesn't exist (permanent), 0 = DNS trouble, else count */
static int mx_lookup(const char *domain, struct mx *out, int max)
{
    unsigned char ans[4096];
    struct __res_state rs;
    memset(&rs, 0, sizeof rs);
    if (res_ninit(&rs)) return 0;
    int len = res_nquery(&rs, domain, ns_c_in, ns_t_mx, ans, sizeof ans);
    int herr = rs.res_h_errno;
    res_nclose(&rs);
    if (len < 0) {
        if (herr == HOST_NOT_FOUND) return -1;
        if (herr == NO_DATA) {   /* no MX: the domain itself (RFC 5321) */
            snprintf(out[0].host, sizeof out[0].host, "%s", domain);
            out[0].pref = 0;
            return 1;
        }
        return 0;
    }
    ns_msg h;
    if (ns_initparse(ans, len, &h)) return 0;
    int n = 0;
    for (int i = 0; i < ns_msg_count(h, ns_s_an) && n < max; i++) {
        ns_rr rr;
        if (ns_parserr(&h, ns_s_an, i, &rr) || ns_rr_type(rr) != ns_t_mx || ns_rr_rdlen(rr) < 3) continue;
        const unsigned char *rd = ns_rr_rdata(rr);
        out[n].pref = (rd[0] << 8) | rd[1];
        if (dn_expand(ns_msg_base(h), ns_msg_end(h), rd + 2, out[n].host, sizeof out[n].host) < 0) continue;
        n++;
    }
    if (n == 1 && !out[0].host[0]) return -2;   /* null MX: domain takes no mail */
    qsort(out, n, sizeof *out, cmp_mx);
    return n;
}

static void deliver_domain(const char *domain, const struct env *e, int *idx, int n, const buf *msg, int *res,
                           char why[][300])
{
    struct mx mx[10];
    int nmx = mx_lookup(domain, mx, 10);
    for (int i = 0; i < n; i++) res[idx[i]] = TEMP;
    if (nmx <= 0) {
        for (int i = 0; i < n; i++) {
            res[idx[i]] = nmx < 0 ? PERM : TEMP;
            snprintf(why[idx[i]], 300, "%s: %s", domain,
                     nmx == -1 ? "that domain doesn't exist" : nmx == -2 ? "that domain takes no mail" : "DNS lookup failed");
        }
        return;
    }
    for (int m = 0; m < nmx && m < 5; m++) {
        struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM}, *ai;
        if (getaddrinfo(mx[m].host, "25", &hints, &ai)) {
            for (int i = 0; i < n; i++) snprintf(why[idx[i]], 300, "%s: no IPv4 address", mx[m].host);
            continue;
        }
        int tried = 0;
        for (struct addrinfo *a = ai; a && tried < 2; a = a->ai_next, tried++) {
            int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
            struct timeval tv = {60, 0};
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
            tv.tv_sec = 30;
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
            if (connect(fd, a->ai_addr, a->ai_addrlen) < 0) {
                for (int i = 0; i < n; i++) snprintf(why[idx[i]], 300, "%s: can't connect (%s)", mx[m].host, strerror(errno));
                close(fd);
                continue;
            }
            session(fd, mx[m].host, e, idx, n, msg, res, why);
            close(fd);
            /* done unless everything is still temporary */
            int temp = 0;
            for (int i = 0; i < n; i++) temp += res[idx[i]] == TEMP;
            if (temp < n) {
                freeaddrinfo(ai);
                return;
            }
        }
        freeaddrinfo(ai);
    }
}

/* ---------- relay (optional) */

struct up {
    const char *p;
    size_t left;
};

static size_t up_read(char *dst, size_t size, size_t nmemb, void *ud)
{
    struct up *u = ud;
    size_t n = size * nmemb < u->left ? size * nmemb : u->left;
    memcpy(dst, u->p, n);
    u->p += n;
    u->left -= n;
    return n;
}

static int relay(const struct env *e, int *idx, int n, const buf *msg, char *err, size_t errn)
{
    CURL *c = curl_easy_init();
    if (!c) return TEMP;
    struct curl_slist *rc = NULL;
    for (int i = 0; i < n; i++) {
        char a[330];
        snprintf(a, sizeof a, "<%s>", e->rcpt[idx[i]]);
        rc = curl_slist_append(rc, a);
    }
    char mf[330];
    snprintf(mf, sizeof mf, "<%s>", *cfg.relay_from ? cfg.relay_from : e->from);
    struct up u = {msg->p, msg->n};
    char eb[CURL_ERROR_SIZE] = "";
    curl_easy_setopt(c, CURLOPT_URL, cfg.relay_url);
    curl_easy_setopt(c, CURLOPT_USE_SSL, (long)CURLUSESSL_ALL);
    if (*cfg.relay_user) {
        curl_easy_setopt(c, CURLOPT_USERNAME, cfg.relay_user);
        curl_easy_setopt(c, CURLOPT_PASSWORD, cfg.relay_pass);
    }
    curl_easy_setopt(c, CURLOPT_MAIL_FROM, mf);
    curl_easy_setopt(c, CURLOPT_MAIL_RCPT, rc);
    curl_easy_setopt(c, CURLOPT_READFUNCTION, up_read);
    curl_easy_setopt(c, CURLOPT_READDATA, &u);
    curl_easy_setopt(c, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 120L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_ERRORBUFFER, eb);
    CURLcode r = curl_easy_perform(c);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(rc);
    curl_easy_cleanup(c);
    if (r == CURLE_OK) return OK;
    snprintf(err, errn, "relay: %s", *eb ? eb : curl_easy_strerror(r));
    return code >= 500 ? PERM : TEMP;
}

/* ---------- bounces */

static void bounce(const struct env *e, char failed[][320], char why[][300], int nf, const buf *msg, int gave_up)
{
    char user[64];
    snprintf(user, sizeof user, "%s", e->from);
    char *at = strchr(user, '@');
    if (!at || strcmp(at + 1, cfg.domain)) return;
    *at = 0;
    if (!user_exists(user)) return;
    struct hdrs h;
    const char *body = hdrs_parse(msg->p, msg->p + msg->n, &h);
    char *subj = decode_words(hget(&h, "subject"));
    char date[64];
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(date, sizeof date, "%a, %d %b %Y %H:%M:%S %z", &tm);
    buf b = {0};
    bprintf(&b, "From: Mail Delivery <mailer-daemon@%s>\r\nTo: %s\r\nDate: %s\r\nSubject: ", cfg.domain, e->from, date);
    /* subject may be non-ASCII; keep it simple and encode the whole line */
    buf s = {0};
    bprintf(&s, "Not delivered: %s", subj);
    int ascii = 1;
    for (char *c = s.p; *c; c++) if ((unsigned char)*c > 126) ascii = 0;
    if (ascii) bputs(&b, s.p);
    else {
        bputs(&b, "=?UTF-8?B?");
        b64enc(&b, (unsigned char *)s.p, s.n > 150 ? 150 : s.n);
        bputs(&b, "?=");
    }
    bputs(&b, "\r\nMIME-Version: 1.0\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Transfer-Encoding: 8bit\r\n\r\n");
    bprintf(&b, "Your message couldn't be delivered to:\r\n\r\n");
    for (int i = 0; i < nf; i++) bprintf(&b, "  %s\r\n    %s\r\n\r\n", failed[i], why[i]);
    if (gave_up) bputs(&b, "Gave up after trying for 2 days.\r\n\r\n");
    bputs(&b, "----- what you sent -----\r\n");
    bput(&b, msg->p, (size_t)(body - msg->p) + (msg->n - (body - msg->p) > 4000 ? 4000 : msg->n - (body - msg->p)));
    deliver(user, "inbox", b.p, b.n, 0, NULL);
    bfree(&b);
    bfree(&s);
    free(subj);
    hfree(&h);
}

/* ---------- worker */

static const int backoff[] = {300, 900, 1800, 3600, 7200, 14400};

static void process(const char *id)
{
    char d[1100], ep[1300], mp[1300];
    qdir(d, sizeof d);
    snprintf(ep, sizeof ep, "%s/%s.env", d, id);
    snprintf(mp, sizeof mp, "%s/%s.eml", d, id);
    struct env e;
    if (env_read(ep, &e) < 0) return;
    if (e.next > time(NULL)) return;
    buf msg = {0};
    if (read_file(mp, &msg, cfg.max_size + (1 << 20)) < 0) {
        unlink(ep);
        return;
    }
    int res[MAX_RCPT];
    char why[MAX_RCPT][300];
    memset(why, 0, sizeof why);
    for (int i = 0; i < e.n; i++) res[i] = -2;   /* not tried */
    for (int i = 0; i < e.n; i++) {
        if (res[i] != -2) continue;
        const char *dom = strchr(e.rcpt[i], '@');
        if (!dom) { res[i] = PERM; snprintf(why[i], 300, "bad address"); continue; }
        int idx[MAX_RCPT], k = 0;
        for (int j = i; j < e.n; j++) {
            const char *dj = strchr(e.rcpt[j], '@');
            if (res[j] == -2 && dj && !strcasecmp(dj, dom)) idx[k++] = j;
        }
        deliver_domain(dom + 1, &e, idx, k, &msg, res, why);
        if (*cfg.relay_url) {
            int ridx[MAX_RCPT], rk = 0;
            for (int j = 0; j < k; j++)
                if (res[idx[j]] == PERM || (res[idx[j]] == TEMP && e.tries >= 2)) ridx[rk++] = idx[j];
            if (rk) {
                char err[300] = "";
                int r = relay(&e, ridx, rk, &msg, err, sizeof err);
                for (int j = 0; j < rk; j++) {
                    res[ridx[j]] = r;
                    if (r != OK) snprintf(why[ridx[j]], 300, "%.140s; %.140s", why[ridx[j]], err);
                }
            }
        }
    }
    char failed[MAX_RCPT][320], fwhy[MAX_RCPT][300];
    int nf = 0, nok = 0;
    struct env left = e;
    left.n = 0;
    int give_up = time(NULL) - e.first > GIVE_UP;
    for (int i = 0; i < e.n; i++) {
        if (res[i] == OK) { nok++; continue; }
        if (res[i] == PERM || give_up) {
            strcpy(failed[nf], e.rcpt[i]);
            strcpy(fwhy[nf++], why[i]);
        } else strcpy(left.rcpt[left.n++], e.rcpt[i]);
    }
    logmsg("out %s from %s: %d delivered, %d failed, %d later%s%s", id, e.from, nok, nf, left.n,
           nf || left.n ? " - " : "", nf ? fwhy[0] : left.n ? why[0] : "");
    if (nf) bounce(&e, failed, fwhy, nf, &msg, give_up);
    if (!left.n) {
        unlink(ep);
        unlink(mp);
    } else {
        left.tries++;
        int b = left.tries - 1 < 6 ? backoff[left.tries - 1] : backoff[5];
        left.next = time(NULL) + b;
        env_write(id, &left);
    }
    bfree(&msg);
}

static void *worker(void *arg)
{
    (void)arg;
    for (;;) {
        char d[1100];
        qdir(d, sizeof d);
        DIR *dir = opendir(d);
        if (dir) {
            struct dirent *de;
            char ids[256][64];
            int n = 0;
            while ((de = readdir(dir)) && n < 256) {
                size_t l = strlen(de->d_name);
                if (l > 4 && l < 60 && !strcmp(de->d_name + l - 4, ".env")) {
                    snprintf(ids[n], 64, "%.*s", (int)(l - 4), de->d_name);
                    n++;
                }
            }
            closedir(dir);
            for (int i = 0; i < n; i++) process(ids[i]);
        }
        pthread_mutex_lock(&qmu);
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 60;
        pthread_cond_timedwait(&qcv, &qmu, &ts);
        pthread_mutex_unlock(&qmu);
    }
    return NULL;
}

void queue_start(void)
{
    char d[1100];
    qdir(d, sizeof d);
    mkdir(d, 0700);
    ctx = SSL_CTX_new(TLS_client_method());
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    /* MTA-to-MTA TLS is opportunistic: encrypt, don't insist on a valid cert */
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
    pthread_t t;
    pthread_create(&t, NULL, worker, NULL);
    pthread_detach(t);
}
