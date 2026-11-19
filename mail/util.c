#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <unistd.h>

struct config cfg;

static void bgrow(buf *b, size_t add)
{
    if (b->n + add + 1 <= b->cap) return;
    size_t c = b->cap ? b->cap : 256;
    while (c < b->n + add + 1) c *= 2;
    b->p = realloc(b->p, c);
    if (!b->p) abort();
    b->cap = c;
}

void bput(buf *b, const void *s, size_t n)
{
    bgrow(b, n);
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}

void bputs(buf *b, const char *s) { bput(b, s, strlen(s)); }

void bprintf(buf *b, const char *fmt, ...)
{
    va_list a, a2;
    va_start(a, fmt);
    va_copy(a2, a);
    int n = vsnprintf(NULL, 0, fmt, a);
    va_end(a);
    if (n > 0) {
        bgrow(b, n);
        vsnprintf(b->p + b->n, n + 1, fmt, a2);
        b->n += n;
    }
    va_end(a2);
}

void bescn(buf *b, const char *s, size_t n)
{
    size_t run = 0;
    for (size_t i = 0; i < n; i++) {
        const char *rep = NULL;
        switch (s[i]) {
        case '<': rep = "&lt;"; break;
        case '>': rep = "&gt;"; break;
        case '&': rep = "&amp;"; break;
        case '"': rep = "&quot;"; break;
        case '\'': rep = "&#39;"; break;
        case 0: rep = ""; break;
        }
        if (rep) {
            bput(b, s + run, i - run);
            bputs(b, rep);
            run = i + 1;
        }
    }
    bput(b, s + run, n - run);
}

void besc(buf *b, const char *s) { if (s) bescn(b, s, strlen(s)); }

void bfree(buf *b)
{
    free(b->p);
    b->p = NULL;
    b->n = b->cap = 0;
}

void randhex(char *out, int bytes)
{
    unsigned char r[64];
    if (bytes > 64) bytes = 64;
    if (getrandom(r, bytes, 0) != bytes) abort();
    for (int i = 0; i < bytes; i++) sprintf(out + 2 * i, "%02x", r[i]);
    out[2 * bytes] = 0;
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c = tolower(c);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

void url_decode(char *s)
{
    char *o = s;
    for (; *s; s++) {
        if (*s == '+') *o++ = ' ';
        else if (*s == '%' && hexval(s[1]) >= 0 && hexval(s[2]) >= 0) {
            *o++ = hexval(s[1]) * 16 + hexval(s[2]);
            s += 2;
        } else *o++ = *s;
    }
    *o = 0;
}

char *form_get(const char *body, const char *key)
{
    if (!body) return NULL;
    size_t kl = strlen(key);
    const char *p = body;
    while (*p) {
        const char *amp = strchr(p, '&');
        size_t len = amp ? (size_t)(amp - p) : strlen(p);
        if (len > kl && !strncmp(p, key, kl) && p[kl] == '=') {
            char *v = strndup(p + kl + 1, len - kl - 1);
            url_decode(v);
            return v;
        }
        if (!amp) break;
        p = amp + 1;
    }
    return NULL;
}

int read_file(const char *path, buf *out, size_t max)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    char tmp[65536];
    ssize_t k;
    while ((k = read(fd, tmp, sizeof tmp)) > 0) {
        if (out->n + k > max) { close(fd); return -2; }
        bput(out, tmp, k);
    }
    close(fd);
    if (!out->p) bput(out, "", 0);
    return k < 0 ? -1 : 0;
}

int write_all(int fd, const void *p, size_t n)
{
    const char *c = p;
    while (n) {
        ssize_t k = write(fd, c, n);
        if (k < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        c += k;
        n -= k;
    }
    return 0;
}

void lower(char *s) { for (; *s; s++) *s = tolower((unsigned char)*s); }

void trim(char *s)
{
    char *a = s;
    while (*a && isspace((unsigned char)*a)) a++;
    size_t n = strlen(a);
    while (n && isspace((unsigned char)a[n - 1])) n--;
    memmove(s, a, n);
    s[n] = 0;
}

void utf8_cut(char *s, size_t max)
{
    size_t n = strlen(s);
    if (n < max) return;
    n = max - 1;
    while (n && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
    s[n] = 0;
}

void logmsg(const char *fmt, ...)
{
    char ts[32];
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm);
    va_list a;
    va_start(a, fmt);
    fprintf(stderr, "%s ", ts);
    vfprintf(stderr, fmt, a);
    fputc('\n', stderr);
    va_end(a);
    fflush(stderr);
}

void b64enc(buf *b, const unsigned char *s, size_t n)
{
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = s[i] << 16;
        if (i + 1 < n) v |= s[i + 1] << 8;
        if (i + 2 < n) v |= s[i + 2];
        char o[4] = {t[v >> 18], t[(v >> 12) & 63], i + 1 < n ? t[(v >> 6) & 63] : '=',
                     i + 2 < n ? t[v & 63] : '='};
        bput(b, o, 4);
    }
}

static void setstr(char *dst, size_t n, const char *v) { snprintf(dst, n, "%s", v); }

void config_load(void)
{
    const char *d = getenv("SEKMAIL_DIR");
    if (d) setstr(cfg.dir, sizeof cfg.dir, d);
    else snprintf(cfg.dir, sizeof cfg.dir, "%s/.local/share/sekmail", getenv("HOME") ? getenv("HOME") : ".");
    setstr(cfg.domain, sizeof cfg.domain, "sekraj.duckdns.org");
    setstr(cfg.smtp_bind, sizeof cfg.smtp_bind, "::");
    cfg.smtp_port = 25;
    cfg.http_port = 3003;
    cfg.max_size = 20L << 20;
    cfg.quota_mb = 500;
    cfg.signups = 1;
    cfg.signups_per_ip = 3;
    cfg.sends_per_day = 100;
    setstr(cfg.host_dir, sizeof cfg.host_dir, "/srv/sekhost");
    cfg.host_quota_mb = 50;

    char path[1100];
    snprintf(path, sizeof path, "%s/config", cfg.dir);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (line[0] == '#' || !eq) continue;
        *eq = 0;
        char *k = line, *v = eq + 1;
        trim(k);
        trim(v);
        if (!strcmp(k, "domain")) setstr(cfg.domain, sizeof cfg.domain, v);
        else if (!strcmp(k, "smtp_bind")) setstr(cfg.smtp_bind, sizeof cfg.smtp_bind, v);
        else if (!strcmp(k, "smtp_port")) cfg.smtp_port = atoi(v);
        else if (!strcmp(k, "http_port")) cfg.http_port = atoi(v);
        else if (!strcmp(k, "max_size_mb")) cfg.max_size = atol(v) << 20;
        else if (!strcmp(k, "quota_mb")) cfg.quota_mb = atol(v);
        else if (!strcmp(k, "signups")) cfg.signups = !strcmp(v, "open") || !strcmp(v, "1");
        else if (!strcmp(k, "signups_per_ip")) cfg.signups_per_ip = atoi(v);
        else if (!strcmp(k, "sends_per_day")) cfg.sends_per_day = atoi(v);
        else if (!strcmp(k, "host_dir")) setstr(cfg.host_dir, sizeof cfg.host_dir, v);
        else if (!strcmp(k, "host_quota_mb")) cfg.host_quota_mb = atol(v);
        else if (!strcmp(k, "admin")) setstr(cfg.admin, sizeof cfg.admin, v);
        else if (!strcmp(k, "relay_url")) setstr(cfg.relay_url, sizeof cfg.relay_url, v);
        else if (!strcmp(k, "relay_user")) setstr(cfg.relay_user, sizeof cfg.relay_user, v);
        else if (!strcmp(k, "relay_pass")) setstr(cfg.relay_pass, sizeof cfg.relay_pass, v);
        else if (!strcmp(k, "relay_from")) setstr(cfg.relay_from, sizeof cfg.relay_from, v);
        else logmsg("config: unknown key %s", k);
    }
    fclose(f);
    lower(cfg.domain);
    lower(cfg.admin);
}
