/* webmail at /mail/, behind Caddy on 127.0.0.1 */
#include "mime.h"
#include "smtp.h"
#include "store.h"
#include "util.h"

#include <ctype.h>
#include <curl/curl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define MAX_BODY (2 << 20)

struct req {
    char method[8];
    char path[1024];
    char *query;
    char tok[65];
    char ip[64];
    char origin[256];
    char *body;
    size_t blen;
    char user[32];
    int fd;
};

/* ---------- sessions and limits (in memory, a restart logs people out) */

static pthread_mutex_t smu = PTHREAD_MUTEX_INITIALIZER;
static struct {
    char tok[65], user[32];
    time_t exp;
} sess[1024];

static struct counter {
    char key[64];
    int n;
    time_t since;
} counters[2048];

/* counts one event for key in a window; returns the count so far */
static int bump(const char *kind, const char *who, long window, int add)
{
    char key[64];
    snprintf(key, sizeof key, "%s:%s", kind, who);
    time_t now = time(NULL);
    pthread_mutex_lock(&smu);
    int slot = -1, oldest = 0;
    for (int i = 0; i < 2048; i++) {
        if (!strcmp(counters[i].key, key)) { slot = i; break; }
        if (counters[i].since < counters[oldest].since) oldest = i;
    }
    if (slot < 0 || now - counters[slot].since > window) {
        if (slot < 0) slot = oldest;
        snprintf(counters[slot].key, sizeof counters[slot].key, "%s", key);
        counters[slot].n = 0;
        counters[slot].since = now;
    }
    counters[slot].n += add;
    int n = counters[slot].n;
    pthread_mutex_unlock(&smu);
    return n;
}

static void sess_new(const char *user, char *tok)
{
    randhex(tok, 32);
    time_t now = time(NULL);
    pthread_mutex_lock(&smu);
    int slot = 0;
    for (int i = 0; i < 1024; i++) {
        if (sess[i].exp < now) { slot = i; break; }
        if (sess[i].exp < sess[slot].exp) slot = i;
    }
    strcpy(sess[slot].tok, tok);
    snprintf(sess[slot].user, sizeof sess[slot].user, "%s", user);
    sess[slot].exp = now + 30 * 86400;
    pthread_mutex_unlock(&smu);
}

static int sess_get(const char *tok, char *user)
{
    if (strlen(tok) != 64) return 0;
    time_t now = time(NULL);
    int ok = 0;
    pthread_mutex_lock(&smu);
    for (int i = 0; i < 1024; i++)
        if (sess[i].exp > now && !strcmp(sess[i].tok, tok)) {
            strcpy(user, sess[i].user);
            ok = 1;
            break;
        }
    pthread_mutex_unlock(&smu);
    return ok;
}

static void sess_drop(const char *tok)
{
    pthread_mutex_lock(&smu);
    for (int i = 0; i < 1024; i++)
        if (!strcmp(sess[i].tok, tok)) memset(&sess[i], 0, sizeof sess[i]);
    pthread_mutex_unlock(&smu);
}

/* ---------- responses */

static const char *status_text(int c)
{
    switch (c) {
    case 200: return "OK";
    case 303: return "See Other";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 413: return "Payload Too Large";
    case 429: return "Too Many Requests";
    default: return "Error";
    }
}

static void respond(int fd, int code, const char *ctype, const char *extra, const void *body, size_t n)
{
    buf h = {0};
    bprintf(&h,
            "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n"
            "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nReferrer-Policy: no-referrer\r\n%s\r\n",
            code, status_text(code), ctype, n, extra ? extra : "");
    write_all(fd, h.p, h.n);
    write_all(fd, body, n);
    bfree(&h);
}

static const char *page_csp =
    "Content-Security-Policy: default-src 'none'; style-src 'self'; img-src 'self'; form-action 'self'; "
    "frame-ancestors 'none'; base-uri 'none'\r\n";

static void send_page(struct req *r, int code, buf *b, const char *extra)
{
    buf h = {0};
    bputs(&h, page_csp);
    if (extra) bputs(&h, extra);
    bputs(b, "</main></body></html>\n");
    respond(r->fd, code, "text/html; charset=utf-8", h.p, b->p, b->n);
    bfree(&h);
}

static void redirect(struct req *r, const char *to, const char *extra)
{
    buf h = {0};
    bprintf(&h, "Location: %s\r\n%s", to, extra ? extra : "");
    respond(r->fd, 303, "text/plain", h.p, "", 0);
    bfree(&h);
}

static int unread(const char *user)
{
    struct msginfo *m;
    int n = list_folder(user, "inbox", &m, 0), u = 0;
    for (int i = 0; i < n; i++) u += !m[i].seen;
    free(m);
    return u;
}

static void page_head(buf *b, struct req *r, const char *title)
{
    int u = r->user[0] ? unread(r->user) : 0;
    bputs(b, "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
             "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>");
    if (u) bprintf(b, "(%d) ", u);
    besc(b, title);
    bputs(b, " - sekmail</title><link rel=\"stylesheet\" href=\"/mail/style.css\"></head><body><header>"
             "<a class=\"logo\" href=\"/mail/\">sekmail</a>");
    if (r->user[0]) {
        bprintf(b, "<a href=\"/mail/\">inbox%s", u ? " " : "");
        if (u) bprintf(b, "(%d)", u);
        bputs(b, "</a><a href=\"/mail/sent\">sent</a><a href=\"/mail/write\">write</a>"
                 "<a href=\"/mail/settings\">settings</a><span class=\"me\">");
        besc(b, r->user);
        bprintf(b, "@%s</span><form class=\"inline\" method=\"post\" action=\"/mail/logout\">"
                   "<button>log out</button></form>", cfg.domain);
    }
    bputs(b, "</header><main>");
}

static const char css[] =
    "body{background:#111;color:#ddd;font:15px/1.5 monospace;margin:0}\n"
    "header{border-bottom:1px solid #333;padding:.5rem 1rem;display:flex;gap:1.1rem;flex-wrap:wrap;align-items:center}\n"
    "header .me{margin-left:auto;color:#777}\n"
    "main{max-width:54rem;margin:0 auto;padding:1rem}\n"
    "a{color:#8ab4f8;text-decoration:none}a:hover{text-decoration:underline}\n"
    ".logo{color:#fff;font-weight:bold}\n"
    "h1{font-size:1.2rem;color:#fff;margin:.2rem 0 1rem;font-weight:normal}\n"
    "table{width:100%;border-collapse:collapse;table-layout:fixed}\n"
    "td{padding:.3rem .4rem;border-bottom:1px solid #222;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}\n"
    "td.who{width:30%}td.when{width:7.5rem;color:#777;text-align:right}\n"
    "tr.new td{color:#fff;font-weight:bold}tr:hover td{background:#181818}\n"
    "td a{color:inherit;display:block;overflow:hidden;text-overflow:ellipsis}td a:hover{text-decoration:none}\n"
    "input,textarea,button{font:inherit;background:#1a1a1a;color:#ddd;border:1px solid #444;padding:.35rem .5rem;border-radius:0}\n"
    "input[type=text],input[type=password],textarea{width:100%;box-sizing:border-box}\n"
    "textarea{min-height:20rem;resize:vertical}\n"
    "button{cursor:pointer}button:hover{border-color:#8ab4f8;color:#fff}\n"
    "label{display:block;margin:.7rem 0 .2rem;color:#999}\n"
    "pre{white-space:pre-wrap;overflow-wrap:anywhere;font:inherit;margin:0}\n"
    ".meta{color:#999;border-bottom:1px solid #333;padding-bottom:.7rem;margin-bottom:1rem}\n"
    ".meta span{color:#ddd}\n"
    ".bar{display:flex;gap:.8rem;align-items:center;flex-wrap:wrap;margin:1rem 0}\n"
    ".files{border-top:1px solid #333;margin-top:1rem;padding-top:.6rem}\n"
    ".err{color:#f88}.ok{color:#9e9}.muted{color:#777}\n"
    ".inline{display:inline;margin:0}\n"
    ".box{max-width:22rem;margin:2.5rem auto}.box button{margin-top:1rem}\n"
    ".hp{position:absolute;left:-9999px}\n"
    "@media(max-width:600px){td.who{width:38%}td.when{width:4.5rem}}\n";

static void fmt_when(char *out, size_t n, time_t t)
{
    struct tm tm, now;
    time_t nt = time(NULL);
    localtime_r(&t, &tm);
    localtime_r(&nt, &now);
    if (tm.tm_year == now.tm_year && tm.tm_yday == now.tm_yday) strftime(out, n, "%H:%M", &tm);
    else if (tm.tm_year == now.tm_year) strftime(out, n, "%d %b", &tm);
    else strftime(out, n, "%Y-%m-%d", &tm);
}

/* plain text to html, links clickable */
static void text_html(buf *b, const char *s)
{
    while (*s) {
        const char *h = strstr(s, "http");
        while (h && strncmp(h, "http://", 7) && strncmp(h, "https://", 8)) h = strstr(h + 4, "http");
        if (!h) { besc(b, s); return; }
        bescn(b, s, h - s);
        size_t k = 0;
        while (h[k] && !isspace((unsigned char)h[k]) && !strchr("<>\"'", h[k])) k++;
        while (k > 8 && strchr(".,;:!?)]", h[k - 1])) k--;
        bputs(b, "<a href=\"");
        bescn(b, h, k);
        bputs(b, "\" rel=\"noopener noreferrer\" target=\"_blank\">");
        bescn(b, h, k);
        bputs(b, "</a>");
        s = h + k;
    }
}

static int same_origin(struct req *r)
{
    if (!r->origin[0]) return 1;
    char want[300];
    snprintf(want, sizeof want, "https://%s", cfg.domain);
    if (!strcmp(r->origin, want)) return 1;
    return !strncmp(r->origin, "http://127.0.0.1:", 17) || !strncmp(r->origin, "http://localhost:", 17);
}

static char *field(struct req *r, const char *k)
{
    char *v = form_get(r->body, k);
    return v ? v : strdup("");
}

static char *qfield(struct req *r, const char *k)
{
    char *v = form_get(r->query, k);
    return v ? v : strdup("");
}

static void login_cookie(char *out, size_t n, const char *tok)
{
    snprintf(out, n, "Set-Cookie: sm=%s; Path=/mail; HttpOnly; Secure; SameSite=Lax; Max-Age=2592000\r\n", tok);
}

/* ---------- login / signup */

static void page_login(struct req *r, const char *err, const char *name)
{
    buf b = {0};
    page_head(&b, r, "log in");
    bputs(&b, "<div class=\"box\"><h1>sekmail</h1><p class=\"muted\">Email at ");
    besc(&b, cfg.domain);
    bputs(&b, ".</p>");
    if (err) {
        bputs(&b, "<p class=\"err\">");
        besc(&b, err);
        bputs(&b, "</p>");
    }
    bputs(&b, "<form method=\"post\" action=\"/mail/login\"><label for=\"u\">name</label>"
              "<input type=\"text\" id=\"u\" name=\"u\" autocomplete=\"username\" autocapitalize=\"none\" value=\"");
    besc(&b, name);
    bputs(&b, "\" required autofocus><label for=\"p\">password</label>"
              "<input type=\"password\" id=\"p\" name=\"p\" autocomplete=\"current-password\" required>"
              "<button>log in</button></form>");
    if (cfg.signups) bputs(&b, "<p>No address yet? <a href=\"/mail/signup\">Get one</a>.</p>");
    bputs(&b, "</div>");
    send_page(r, err ? 403 : 200, &b, NULL);
    bfree(&b);
}

static void do_login(struct req *r)
{
    char *u = field(r, "u"), *p = field(r, "p");
    lower(u);
    trim(u);
    u[strcspn(u, "@")] = 0;
    if (bump("fail", r->ip, 900, 0) >= 10) page_login(r, "Too many tries. Wait 15 minutes.", u);
    else if (!user_check(u, p)) {
        bump("fail", r->ip, 900, 1);
        page_login(r, "Wrong name or password.", u);
    } else {
        char tok[65], ck[200];
        sess_new(u, tok);
        login_cookie(ck, sizeof ck, tok);
        redirect(r, "/mail/", ck);
    }
    free(u);
    free(p);
}

static void page_signup(struct req *r, const char *err, const char *name)
{
    buf b = {0};
    page_head(&b, r, "sign up");
    bputs(&b, "<div class=\"box\"><h1>get an address</h1>");
    if (!cfg.signups) {
        bputs(&b, "<p>Sign-ups are closed right now.</p></div>");
        send_page(r, 200, &b, NULL);
        bfree(&b);
        return;
    }
    if (err) {
        bputs(&b, "<p class=\"err\">");
        besc(&b, err);
        bputs(&b, "</p>");
    }
    bprintf(&b, "<form method=\"post\" action=\"/mail/signup\"><label for=\"u\">name (you get name@%s)</label>"
                "<input type=\"text\" id=\"u\" name=\"u\" autocomplete=\"username\" autocapitalize=\"none\" "
                "maxlength=\"24\" value=\"",
            cfg.domain);
    besc(&b, name);
    bputs(&b, "\" required autofocus><label for=\"p\">password (8 or more characters)</label>"
              "<input type=\"password\" id=\"p\" name=\"p\" autocomplete=\"new-password\" minlength=\"8\" required>"
              "<label for=\"p2\">same password again</label>"
              "<input type=\"password\" id=\"p2\" name=\"p2\" autocomplete=\"new-password\" required>"
              "<input class=\"hp\" type=\"text\" name=\"website\" tabindex=\"-1\" autocomplete=\"off\">"
              "<button>create</button></form><p class=\"muted\">Lowercase letters, digits, dot, dash and "
              "underscore. There is no password reset, so keep it somewhere.</p></div>");
    send_page(r, err ? 400 : 200, &b, NULL);
    bfree(&b);
}

static void do_signup(struct req *r)
{
    char *u = field(r, "u"), *p = field(r, "p"), *p2 = field(r, "p2"), *hp = field(r, "website");
    lower(u);
    trim(u);
    const char *err = NULL;
    if (!cfg.signups) err = "Sign-ups are closed right now.";
    else if (*hp) err = "Something went wrong.";
    else if (bump("signup", r->ip, 86400, 0) >= cfg.signups_per_ip) err = "Too many new accounts from your address today.";
    else if (!valid_name(u, 1)) err = "That name has characters that aren't allowed, or is too short or long.";
    else if (!valid_name(u, 0)) err = "That name is reserved.";
    else if (strlen(p) < 8 || strlen(p) > 200) err = "The password needs 8 or more characters.";
    else if (strcmp(p, p2)) err = "The passwords don't match.";
    else {
        int k = user_add(u, p, 0);
        if (k == -1) err = "That name is taken.";
        else if (k) err = "Couldn't save the account. Try again later.";
    }
    if (err) page_signup(r, err, u);
    else {
        bump("signup", r->ip, 86400, 1);
        logmsg("signup %s from %s", u, r->ip);
        char tok[65], ck[200];
        sess_new(u, tok);
        login_cookie(ck, sizeof ck, tok);
        redirect(r, "/mail/", ck);
    }
    free(u);
    free(p);
    free(p2);
    free(hp);
}

/* ---------- folders */

static void page_list(struct req *r, const char *folder)
{
    struct msginfo *m;
    int n = list_folder(r->user, folder, &m, 1);
    int inbox = !strcmp(folder, "inbox");
    buf b = {0};
    page_head(&b, r, folder);
    char *note = qfield(r, "n");
    if (!strcmp(note, "sent")) bputs(&b, "<p class=\"ok\">Sent.</p>");
    if (!strcmp(note, "deleted")) bputs(&b, "<p class=\"ok\">Deleted.</p>");
    free(note);
    if (!n) {
        if (inbox) bprintf(&b, "<p class=\"muted\">Nothing here yet. Your address is %s@%s.</p>", r->user, cfg.domain);
        else bputs(&b, "<p class=\"muted\">You haven't sent anything.</p>");
    } else bputs(&b, "<table>");
    for (int i = 0; i < n && i < 500; i++) {
        char when[32];
        fmt_when(when, sizeof when, m[i].t);
        const char *who = inbox ? m[i].from : m[i].to;
        bprintf(&b, "<tr%s><td class=\"who\"><a href=\"/mail/%s/%s\">", m[i].seen || !inbox ? "" : " class=\"new\"",
                folder, m[i].id);
        if (inbox) {
            /* "Name <a@b>" -> Name */
            char nm[200];
            snprintf(nm, sizeof nm, "%s", who);
            char *lt = strchr(nm, '<');
            if (lt && lt != nm) {
                *lt = 0;
                trim(nm);
                size_t l = strlen(nm);
                if (l > 1 && nm[0] == '"' && nm[l - 1] == '"') {
                    nm[l - 1] = 0;
                    memmove(nm, nm + 1, l - 1);
                }
            }
            besc(&b, *nm ? nm : "(nobody)");
        } else besc(&b, *who ? who : "(nobody)");
        bprintf(&b, "</a></td><td><a href=\"/mail/%s/%s\">", folder, m[i].id);
        besc(&b, *m[i].subject ? m[i].subject : "(no subject)");
        bprintf(&b, "</a></td><td class=\"when\">%s</td></tr>", when);
    }
    if (n) bputs(&b, "</table>");
    if (n > 500) bprintf(&b, "<p class=\"muted\">Showing the newest 500 of %d.</p>", n);
    send_page(r, 200, &b, NULL);
    bfree(&b);
    free(m);
}

struct loaded {
    buf raw;
    struct hdrs h;
    struct part *parts;
    int nparts;
};

static int load_msg(struct req *r, const char *folder, const char *id, struct loaded *L)
{
    char path[1400];
    memset(L, 0, sizeof *L);
    if (!msg_path(r->user, folder, id, path, sizeof path)) return 0;
    if (read_file(path, &L->raw, cfg.max_size + (1 << 20)) < 0) return 0;
    hdrs_parse(L->raw.p, L->raw.p + L->raw.n, &L->h);
    L->nparts = mime_parts(L->raw.p, L->raw.n, &L->parts);
    return 1;
}

static void free_msg(struct loaded *L)
{
    bfree(&L->raw);
    hfree(&L->h);
    mime_free(L->parts, L->nparts);
}

static void not_found(struct req *r)
{
    buf b = {0};
    page_head(&b, r, "not found");
    bputs(&b, "<p>That message isn't here (maybe it was deleted). <a href=\"/mail/\">Back to the inbox</a>.</p>");
    send_page(r, 404, &b, NULL);
    bfree(&b);
}

/* first readable text; *html gets the index of an html part, if any */
static char *body_text(struct loaded *L, int *html)
{
    *html = -1;
    int plain = -1;
    for (int i = 0; i < L->nparts; i++) {
        if (L->parts[i].attach) continue;
        if (plain < 0 && !strcmp(L->parts[i].type, "text/plain")) plain = i;
        if (*html < 0 && !strcmp(L->parts[i].type, "text/html")) *html = i;
    }
    int pick = plain >= 0 ? plain : *html;
    if (pick < 0) return strdup("");
    size_t n;
    char *d = part_decode(&L->parts[pick], &n);
    char *u = to_utf8(d, n, L->parts[pick].charset);
    free(d);
    if (pick == plain) {
        /* CRLF -> LF */
        char *o = u;
        for (char *s = u; *s; s++)
            if (*s != '\r') *o++ = *s;
        *o = 0;
        return u;
    }
    char *t = html_to_text(u, strlen(u));
    free(u);
    return t;
}

static void meta_line(buf *b, const char *label, const char *raw)
{
    if (!raw) return;
    char *v = decode_words(raw);
    bprintf(b, "%s: <span>", label);
    besc(b, v);
    bputs(b, "</span><br>");
    free(v);
}

static void page_read(struct req *r, const char *folder, const char *id)
{
    struct loaded L;
    if (!load_msg(r, folder, id, &L)) { not_found(r); return; }
    if (!strcmp(folder, "inbox")) msg_mark_seen(r->user, folder, id);
    char *subj = decode_words(hget(&L.h, "subject"));
    buf b = {0};
    page_head(&b, r, *subj ? subj : "(no subject)");
    bputs(&b, "<h1>");
    besc(&b, *subj ? subj : "(no subject)");
    bputs(&b, "</h1><div class=\"meta\">");
    meta_line(&b, "from", hget(&L.h, "from"));
    meta_line(&b, "to", hget(&L.h, "to"));
    meta_line(&b, "cc", hget(&L.h, "cc"));
    meta_line(&b, "date", hget(&L.h, "date"));
    bputs(&b, "</div>");
    int html;
    char *text = body_text(&L, &html);
    bputs(&b, "<pre>");
    text_html(&b, text);
    bputs(&b, "</pre>");
    int files = 0;
    for (int i = 0; i < L.nparts; i++) {
        struct part *p = &L.parts[i];
        if (!p->attach) continue;
        if (!files++) bputs(&b, "<div class=\"files\">attachments:<br>");
        long sz = !strcmp(p->enc, "base64") ? (long)(p->len * 3 / 4) : (long)p->len;
        bprintf(&b, "<a href=\"/mail/%s/%s/part/%d\">", folder, id, i);
        besc(&b, p->filename ? p->filename : p->type);
        bprintf(&b, "</a> <span class=\"muted\">%ld KB</span><br>", (sz + 1023) / 1024);
    }
    if (files) bputs(&b, "</div>");
    bprintf(&b, "<div class=\"bar\"><a href=\"/mail/write?re=%s/%s\">reply</a>", folder, id);
    if (html >= 0) bprintf(&b, "<a href=\"/mail/%s/%s/html\" target=\"_blank\">view as HTML</a>", folder, id);
    bprintf(&b, "<a href=\"/mail/%s/%s/raw\">source</a><form class=\"inline\" method=\"post\" "
                "action=\"/mail/%s/%s/delete\"><button>delete</button></form></div>",
            folder, id, folder, id);
    send_page(r, 200, &b, NULL);
    bfree(&b);
    free(text);
    free(subj);
    free_msg(&L);
}

static void send_part(struct req *r, const char *folder, const char *id, int idx)
{
    struct loaded L;
    if (!load_msg(r, folder, id, &L)) { not_found(r); return; }
    if (idx < 0 || idx >= L.nparts) {
        free_msg(&L);
        not_found(r);
        return;
    }
    struct part *p = &L.parts[idx];
    size_t n;
    char *d = part_decode(p, &n);
    char name[200];
    snprintf(name, sizeof name, "%s", p->filename ? p->filename : "attachment");
    buf h = {0}, enc = {0};
    for (unsigned char *c = (unsigned char *)name; *c; c++)
        if (isalnum(*c) || strchr(".-_", *c)) bput(&enc, c, 1);
        else bprintf(&enc, "%%%02X", *c);
    for (char *c = name; *c; c++)
        if (*c == '"' || *c == '\\' || (unsigned char)*c < 32 || (unsigned char)*c > 126) *c = '_';
    bprintf(&h, "Content-Disposition: attachment; filename=\"%s\"; filename*=UTF-8''%s\r\n"
                "Content-Security-Policy: sandbox; default-src 'none'\r\n",
            name, enc.p ? enc.p : "attachment");
    respond(r->fd, 200, "application/octet-stream", h.p, d, n);
    bfree(&h);
    bfree(&enc);
    free(d);
    free_msg(&L);
}

static void send_html(struct req *r, const char *folder, const char *id)
{
    struct loaded L;
    if (!load_msg(r, folder, id, &L)) { not_found(r); return; }
    int html;
    free(body_text(&L, &html));
    if (html < 0) {
        free_msg(&L);
        not_found(r);
        return;
    }
    size_t n;
    char *d = part_decode(&L.parts[html], &n);
    char *u = to_utf8(d, n, L.parts[html].charset);
    /* no scripts, no remote images (those are mostly trackers) */
    respond(r->fd, 200, "text/html; charset=utf-8",
            "Content-Security-Policy: sandbox; default-src 'none'; style-src 'unsafe-inline'; img-src data:\r\n", u,
            strlen(u));
    free(u);
    free(d);
    free_msg(&L);
}

static void send_raw(struct req *r, const char *folder, const char *id)
{
    struct loaded L;
    if (!load_msg(r, folder, id, &L)) { not_found(r); return; }
    respond(r->fd, 200, "text/plain; charset=utf-8", "Content-Security-Policy: sandbox; default-src 'none'\r\n",
            L.raw.p, L.raw.n);
    free_msg(&L);
}

/* ---------- writing */

static void page_write(struct req *r, const char *err, const char *to, const char *subj, const char *body,
                       const char *irt)
{
    buf b = {0};
    page_head(&b, r, "write");
    if (err) {
        bputs(&b, "<p class=\"err\">");
        besc(&b, err);
        bputs(&b, "</p>");
    }
    bputs(&b, "<form method=\"post\" action=\"/mail/write\"><label for=\"to\">to</label>"
              "<input type=\"text\" id=\"to\" name=\"to\" autocapitalize=\"none\" value=\"");
    besc(&b, to);
    bprintf(&b, "\" placeholder=\"name@%s\" required%s><label for=\"s\">subject</label>"
                "<input type=\"text\" id=\"s\" name=\"s\" value=\"",
            cfg.domain, *to ? "" : " autofocus");
    besc(&b, subj);
    bputs(&b, "\"><label for=\"b\">message</label><textarea id=\"b\" name=\"b\"");
    if (*to) bputs(&b, " autofocus");
    bputs(&b, ">");
    besc(&b, body);
    bputs(&b, "</textarea><input type=\"hidden\" name=\"irt\" value=\"");
    besc(&b, irt);
    bputs(&b, "\"><div class=\"bar\"><button>send</button>");
    if (!*cfg.relay_url)
        bprintf(&b, "<span class=\"muted\">Only addresses @%s work for now.</span>", cfg.domain);
    bputs(&b, "</div></form>");
    send_page(r, err ? 400 : 200, &b, NULL);
    bfree(&b);
}

static void write_form(struct req *r)
{
    char *re = qfield(r, "re"), *to = qfield(r, "to");
    char folder[16] = "", id[100] = "";
    char *slash = strchr(re, '/');
    if (slash && slash - re < 16) {
        snprintf(folder, sizeof folder, "%.*s", (int)(slash - re), re);
        snprintf(id, sizeof id, "%s", slash + 1);
    }
    struct loaded L;
    if (*folder && load_msg(r, folder, id, &L)) {
        int sent = !strcmp(folder, "sent");
        char addr[320];
        const char *rt = hget(&L.h, "reply-to");
        addr_spec(sent ? hget(&L.h, "to") : (rt ? rt : hget(&L.h, "from")), addr, sizeof addr);
        char *subj = decode_words(hget(&L.h, "subject"));
        buf s = {0}, body = {0};
        if (strncasecmp(subj, "re:", 3)) bputs(&s, "Re: ");
        bputs(&s, subj);
        char *from = decode_words(hget(&L.h, "from"));
        int html;
        char *text = body_text(&L, &html);
        bprintf(&body, "\n\n%s wrote:\n", from);
        for (char *line = text; line && *line;) {
            char *e = strchr(line, '\n');
            bputs(&body, ">");
            if (*line != '>' && *line != '\n') bputs(&body, " ");
            bput(&body, line, e ? (size_t)(e - line) : strlen(line));
            bputs(&body, "\n");
            line = e ? e + 1 : NULL;
        }
        const char *mid = hget(&L.h, "message-id");
        page_write(r, NULL, addr, s.p, body.p, mid ? mid : "");
        bfree(&s);
        bfree(&body);
        free(subj);
        free(from);
        free(text);
        free_msg(&L);
    } else page_write(r, NULL, to, "", "", "");
    free(re);
    free(to);
}

static void subject_header(buf *b, const char *s)
{
    int ascii = 1;
    for (const char *c = s; *c; c++)
        if ((unsigned char)*c > 126) ascii = 0;
    bputs(b, "Subject: ");
    if (ascii) {
        bputs(b, s);
        bputs(b, "\r\n");
        return;
    }
    size_t n = strlen(s), i = 0;
    while (i < n) {
        size_t k = n - i > 45 ? 45 : n - i;
        while (i + k < n && ((unsigned char)s[i + k] & 0xC0) == 0x80) k--;
        if (i) bputs(b, "\r\n ");
        bputs(b, "=?UTF-8?B?");
        b64enc(b, (const unsigned char *)s + i, k);
        bputs(b, "?=");
        i += k;
    }
    bputs(b, "\r\n");
}

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

static int relay_send(const char *from, char ext[][320], int next, const buf *msg, char *err, size_t errn)
{
    CURL *c = curl_easy_init();
    if (!c) return -1;
    struct curl_slist *rc = NULL;
    for (int i = 0; i < next; i++) {
        char a[330];
        snprintf(a, sizeof a, "<%s>", ext[i]);
        rc = curl_slist_append(rc, a);
    }
    char mf[330];
    snprintf(mf, sizeof mf, "<%s>", *cfg.relay_from ? cfg.relay_from : from);
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
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_ERRORBUFFER, eb);
    CURLcode res = curl_easy_perform(c);
    curl_slist_free_all(rc);
    curl_easy_cleanup(c);
    if (res != CURLE_OK) {
        snprintf(err, errn, "%s", *eb ? eb : curl_easy_strerror(res));
        return -1;
    }
    return 0;
}

static void do_write(struct req *r)
{
    char *to = field(r, "to"), *subj = field(r, "s"), *body = field(r, "b"), *irt = field(r, "irt");
    for (char *c = to; *c; c++) if (*c == '\r' || *c == '\n') *c = ' ';
    for (char *c = subj; *c; c++) if (*c == '\r' || *c == '\n' || *c == '\t') *c = ' ';
    for (char *c = irt; *c; c++) if ((unsigned char)*c < 33 || *c == 127) *c = 0;
    utf8_cut(subj, 300);
    trim(subj);

    char local[20][32], ext[20][320], err[400] = "";
    int nl = 0, ne = 0;
    char *save = NULL, *tmp = strdup(to);
    for (char *t = strtok_r(tmp, ",;", &save); t && !*err; t = strtok_r(NULL, ",;", &save)) {
        char a[320];
        addr_spec(t, a, sizeof a);
        if (!*a) continue;
        lower(a);
        if (nl + ne >= 20) { snprintf(err, sizeof err, "At most 20 people per message."); break; }
        int okchars = 1;
        for (char *c = a; *c; c++)
            if ((unsigned char)*c <= 32 || strchr("<>()[]\\\",:;", *c)) okchars = 0;
        char *at = strchr(a, '@');
        if (!okchars || (at && (strchr(at + 1, '@') || !strchr(at + 1, '.') || at == a))) {
            snprintf(err, sizeof err, "\"%.200s\" doesn't look like an email address.", a);
            break;
        }
        if (!at || !strcmp(at + 1, cfg.domain)) {
            if (at) *at = 0;
            if (!resolve_local(a, local[nl], sizeof local[nl])) {
                snprintf(err, sizeof err, "There's nobody called %.40s@%s.", a, cfg.domain);
                break;
            }
            int dup = 0;
            for (int i = 0; i < nl; i++) dup |= !strcmp(local[i], local[nl]);
            if (!dup) nl++;
        } else if (!*cfg.relay_url) {
            snprintf(err, sizeof err,
                     "Mail to other servers can't go out yet (this connection blocks outgoing port 25). "
                     "Addresses @%s work.", cfg.domain);
        } else snprintf(ext[ne++], sizeof ext[0], "%s", a);
    }
    free(tmp);
    if (!*err && !nl && !ne) snprintf(err, sizeof err, "Who is it for?");
    if (!*err && strlen(body) > 1000000) snprintf(err, sizeof err, "That message is too long.");
    if (!*err && bump("send", r->user, 86400, 0) >= cfg.sends_per_day)
        snprintf(err, sizeof err, "You've sent %d messages today, that's the limit.", cfg.sends_per_day);
    if (*err) {
        page_write(r, err, to, subj, body, irt);
        goto out;
    }

    char from[100], date[64], mid[128], rnd[17];
    snprintf(from, sizeof from, "%s@%s", r->user, cfg.domain);
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(date, sizeof date, "%a, %d %b %Y %H:%M:%S %z", &tm);
    randhex(rnd, 8);
    snprintf(mid, sizeof mid, "<%ld.%s@%s>", (long)t, rnd, cfg.domain);

    buf m = {0};
    bprintf(&m, "From: %s\r\nTo: ", from);
    for (int i = 0; i < nl; i++) bprintf(&m, "%s%s@%s", i ? ", " : "", local[i], cfg.domain);
    for (int i = 0; i < ne; i++) bprintf(&m, "%s%s", i || nl ? ", " : "", ext[i]);
    bprintf(&m, "\r\nDate: %s\r\nMessage-ID: %s\r\n", date, mid);
    subject_header(&m, *subj ? subj : "(no subject)");
    if (*irt && irt[0] == '<' && strlen(irt) < 300) bprintf(&m, "In-Reply-To: %s\r\nReferences: %s\r\n", irt, irt);
    bputs(&m, "MIME-Version: 1.0\r\nContent-Type: text/plain; charset=utf-8\r\n"
              "Content-Transfer-Encoding: 8bit\r\nUser-Agent: sekmail\r\n\r\n");
    /* LF/CRLF -> CRLF, lines kept under 998 bytes */
    size_t col = 0;
    for (char *c = body; *c; c++) {
        if (*c == '\r') continue;
        if (*c == '\n') { bputs(&m, "\r\n"); col = 0; continue; }
        if (col >= 990 && ((unsigned char)*c & 0xC0) != 0x80) { bputs(&m, "\r\n"); col = 0; }
        bput(&m, c, 1);
        col++;
    }
    if (col) bputs(&m, "\r\n");

    if (ne && relay_send(from, ext, ne, &m, err, sizeof err) < 0) {
        logmsg("relay failed for %s: %s", r->user, err);
        char e2[500];
        snprintf(e2, sizeof e2, "Sending to the outside failed: %.300s. Nothing was sent.", err);
        page_write(r, e2, to, subj, body, irt);
        bfree(&m);
        goto out;
    }
    /* the copy everyone at this domain gets has a Received line too */
    buf in = {0};
    bprintf(&in, "Return-Path: <%s>\r\nReceived: from webmail by %s (sekmail); %s\r\n", from, cfg.domain, date);
    bput(&in, m.p, m.n);
    for (int i = 0; i < nl; i++) deliver(local[i], "inbox", in.p, in.n, 0, NULL);
    deliver(r->user, "sent", m.p, m.n, 1, NULL);
    bump("send", r->user, 86400, 1);
    logmsg("web %s: sent to %d local, %d outside", r->user, nl, ne);
    bfree(&in);
    bfree(&m);
    redirect(r, "/mail/sent?n=sent", NULL);
out:
    free(to);
    free(subj);
    free(body);
    free(irt);
}

/* ---------- settings */

static void page_settings(struct req *r, const char *msg, int bad)
{
    buf b = {0};
    page_head(&b, r, "settings");
    long used = user_usage(r->user);
    bprintf(&b, "<h1>settings</h1><p>Your address: <b>%s@%s</b><br>Space used: %.1f of %ld MB</p>", r->user,
            cfg.domain, used / 1048576.0, cfg.quota_mb);
    if (msg) {
        bprintf(&b, "<p class=\"%s\">", bad ? "err" : "ok");
        besc(&b, msg);
        bputs(&b, "</p>");
    }
    bputs(&b, "<form method=\"post\" action=\"/mail/settings\" class=\"box\" style=\"margin:1rem 0\">"
              "<label for=\"o\">current password</label>"
              "<input type=\"password\" id=\"o\" name=\"o\" autocomplete=\"current-password\" required>"
              "<label for=\"p\">new password</label>"
              "<input type=\"password\" id=\"p\" name=\"p\" autocomplete=\"new-password\" minlength=\"8\" required>"
              "<label for=\"p2\">new password again</label>"
              "<input type=\"password\" id=\"p2\" name=\"p2\" autocomplete=\"new-password\" required>"
              "<button>change password</button></form>");
    send_page(r, 200, &b, NULL);
    bfree(&b);
}

static void do_settings(struct req *r)
{
    char *o = field(r, "o"), *p = field(r, "p"), *p2 = field(r, "p2");
    if (bump("fail", r->ip, 900, 0) >= 10) page_settings(r, "Too many tries. Wait 15 minutes.", 1);
    else if (!user_check(r->user, o)) {
        bump("fail", r->ip, 900, 1);
        page_settings(r, "The current password is wrong.", 1);
    } else if (strlen(p) < 8 || strlen(p) > 200) page_settings(r, "The new password needs 8 or more characters.", 1);
    else if (strcmp(p, p2)) page_settings(r, "The new passwords don't match.", 1);
    else if (user_passwd(r->user, p) < 0) page_settings(r, "Couldn't save it. Try again later.", 1);
    else page_settings(r, "Password changed.", 0);
    free(o);
    free(p);
    free(p2);
}

/* ---------- request parsing and routing */

static int read_req(int fd, struct req *r, buf *raw)
{
    char tmp[8192];
    char *end = NULL;
    while (!end) {
        ssize_t k = read(fd, tmp, sizeof tmp);
        if (k <= 0) return -1;
        bput(raw, tmp, k);
        end = strstr(raw->p, "\r\n\r\n");
        if (!end && raw->n > 32768) return -1;
    }
    *end = 0;
    char *body = end + 4;
    char *line = raw->p, *save = NULL;
    char *rl = strtok_r(line, "\r\n", &save);
    if (!rl || sscanf(rl, "%7s %1023s", r->method, r->path) != 2) return -1;
    char *q = strchr(r->path, '?');
    if (q) {
        *q = 0;
        r->query = q + 1;
    }
    long clen = 0;
    for (char *h = strtok_r(NULL, "\r\n", &save); h; h = strtok_r(NULL, "\r\n", &save)) {
        char *colon = strchr(h, ':');
        if (!colon) continue;
        *colon = 0;
        char *v = colon + 1;
        while (*v == ' ') v++;
        if (!strcasecmp(h, "content-length")) clen = atol(v);
        else if (!strcasecmp(h, "x-forwarded-for")) {
            /* Caddy sets this; the first entry is the client */
            snprintf(r->ip, sizeof r->ip, "%.*s", (int)strcspn(v, ", "), v);
        } else if (!strcasecmp(h, "origin")) snprintf(r->origin, sizeof r->origin, "%s", v);
        else if (!strcasecmp(h, "cookie")) {
            char *c = strstr(v, "sm=");
            while (c && c != v && c[-1] != ' ' && c[-1] != ';') c = strstr(c + 1, "sm=");
            if (c) snprintf(r->tok, sizeof r->tok, "%.*s", (int)strcspn(c + 3, "; "), c + 3);
        }
    }
    if (clen < 0 || clen > MAX_BODY) return -2;
    size_t have = raw->n - (body - raw->p);
    size_t off = body - raw->p;
    while (have < (size_t)clen) {
        ssize_t k = read(fd, tmp, sizeof tmp);
        if (k <= 0) return -1;
        bput(raw, tmp, k);
        have += k;
    }
    r->body = raw->p + off;
    r->body[clen] = 0;
    r->blen = clen;
    return 0;
}

void handle_http(int fd)
{
    struct req r = {.fd = fd};
    buf raw = {0};
    int k = read_req(fd, &r, &raw);
    if (k == -2) respond(fd, 413, "text/plain", NULL, "too big\n", 8);
    if (k < 0) { bfree(&raw); return; }
    if (!r.ip[0]) strcpy(r.ip, "local");
    if (*r.tok && !sess_get(r.tok, r.user)) r.tok[0] = 0;
    int post = !strcmp(r.method, "POST");
    const char *p = r.path;

    if (strncmp(p, "/mail", 5)) {
        respond(fd, 404, "text/plain", NULL, "not found\n", 10);
        goto done;
    }
    p += 5;
    if (!*p) { redirect(&r, "/mail/", NULL); goto done; }
    if (!strcmp(p, "/style.css")) {
        respond(fd, 200, "text/css", "Cache-Control: max-age=3600\r\n", css, sizeof css - 1);
        goto done;
    }
    if (post && !same_origin(&r)) {
        respond(fd, 403, "text/plain", NULL, "bad origin\n", 11);
        goto done;
    }
    if (!strcmp(p, "/login")) {
        if (post) do_login(&r);
        else if (r.user[0]) redirect(&r, "/mail/", NULL);
        else page_login(&r, NULL, "");
        goto done;
    }
    if (!strcmp(p, "/signup")) {
        if (post) do_signup(&r);
        else page_signup(&r, NULL, "");
        goto done;
    }
    if (!r.user[0]) {
        if (!strcmp(p, "/")) page_login(&r, NULL, "");
        else redirect(&r, "/mail/login", NULL);
        goto done;
    }
    if (!strcmp(p, "/logout") && post) {
        sess_drop(r.tok);
        redirect(&r, "/mail/login", "Set-Cookie: sm=; Path=/mail; HttpOnly; Secure; SameSite=Lax; Max-Age=0\r\n");
        goto done;
    }
    if (!strcmp(p, "/")) { page_list(&r, "inbox"); goto done; }
    if (!strcmp(p, "/sent")) { page_list(&r, "sent"); goto done; }
    if (!strcmp(p, "/write")) {
        if (post) do_write(&r);
        else write_form(&r);
        goto done;
    }
    if (!strcmp(p, "/settings")) {
        if (post) do_settings(&r);
        else page_settings(&r, NULL, 0);
        goto done;
    }
    /* /inbox/<id>[/raw|/html|/part/N|/delete] */
    char folder[16], id[100], rest[64] = "";
    if (sscanf(p, "/%15[a-z]/%99[A-Za-z0-9._-]%63s", folder, id, rest) >= 2 &&
        (!strcmp(folder, "inbox") || !strcmp(folder, "sent"))) {
        int idx;
        if (!*rest && !post) page_read(&r, folder, id);
        else if (!strcmp(rest, "/raw")) send_raw(&r, folder, id);
        else if (!strcmp(rest, "/html")) send_html(&r, folder, id);
        else if (sscanf(rest, "/part/%d", &idx) == 1) send_part(&r, folder, id, idx);
        else if (!strcmp(rest, "/delete") && post) {
            msg_delete(r.user, folder, id);
            redirect(&r, !strcmp(folder, "sent") ? "/mail/sent?n=deleted" : "/mail/?n=deleted", NULL);
        } else not_found(&r);
        goto done;
    }
    not_found(&r);
done:
    bfree(&raw);
}
