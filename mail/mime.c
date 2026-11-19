#include "mime.h"
#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <iconv.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MAX_PARTS 200
#define MAX_DEPTH 8

const char *hdrs_parse(const char *p, const char *end, struct hdrs *out)
{
    out->h = NULL;
    out->n = 0;
    int cap = 0;
    while (p < end) {
        if (*p == '\n') return p + 1;
        if (*p == '\r' && p + 1 < end && p[1] == '\n') return p + 2;

        buf line = {0};
        for (;;) {
            const char *nl = memchr(p, '\n', end - p);
            const char *le = nl ? nl : end;
            const char *ce = le;
            if (ce > p && ce[-1] == '\r') ce--;
            bput(&line, p, ce - p);
            p = nl ? nl + 1 : end;
            if (p < end && (*p == ' ' || *p == '\t')) continue;   /* folded */
            break;
        }
        char *colon = line.p ? strchr(line.p, ':') : NULL;
        if (colon && colon != line.p) {
            *colon = 0;
            char *v = colon + 1;
            while (*v == ' ' || *v == '\t') v++;
            if (out->n == cap) {
                cap = cap ? cap * 2 : 16;
                out->h = realloc(out->h, cap * sizeof *out->h);
            }
            out->h[out->n].name = strdup(line.p);
            trim(out->h[out->n].name);
            out->h[out->n].val = strdup(v);
            out->n++;
        }
        bfree(&line);
        if (out->n > 1000) break;
    }
    return p;
}

const char *hget(const struct hdrs *h, const char *name)
{
    for (int i = 0; i < h->n; i++)
        if (!strcasecmp(h->h[i].name, name)) return h->h[i].val;
    return NULL;
}

void hfree(struct hdrs *h)
{
    for (int i = 0; i < h->n; i++) {
        free(h->h[i].name);
        free(h->h[i].val);
    }
    free(h->h);
    h->h = NULL;
    h->n = 0;
}

static int hexv(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c = tolower(c);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static void b64dec(buf *o, const char *s, size_t n)
{
    unsigned v = 0;
    int bits = 0;
    for (size_t i = 0; i < n; i++) {
        int c = (unsigned char)s[i], d;
        if (c >= 'A' && c <= 'Z') d = c - 'A';
        else if (c >= 'a' && c <= 'z') d = c - 'a' + 26;
        else if (c >= '0' && c <= '9') d = c - '0' + 52;
        else if (c == '+' || c == '-') d = 62;
        else if (c == '/' || c == '_') d = 63;
        else if (c == '=') break;
        else continue;
        v = (v << 6) | d;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            char ch = (v >> bits) & 0xFF;
            bput(o, &ch, 1);
        }
    }
}

static void qpdec(buf *o, const char *s, size_t n, int header)
{
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '=' && i + 1 < n) {
            if (s[i + 1] == '\n') { i++; continue; }
            if (s[i + 1] == '\r' && i + 2 < n && s[i + 2] == '\n') { i += 2; continue; }
            if (i + 2 < n && hexv(s[i + 1]) >= 0 && hexv(s[i + 2]) >= 0) {
                char ch = hexv(s[i + 1]) * 16 + hexv(s[i + 2]);
                bput(o, &ch, 1);
                i += 2;
                continue;
            }
        }
        if (header && c == '_') c = ' ';
        bput(o, &c, 1);
    }
}

char *to_utf8(const char *s, size_t n, const char *charset)
{
    char cs[64] = "";
    if (charset) snprintf(cs, sizeof cs, "%s", charset);
    char *star = strchr(cs, '*');   /* RFC 2231 language suffix */
    if (star) *star = 0;
    lower(cs);
    if (!*cs || !strcmp(cs, "utf-8") || !strcmp(cs, "utf8") || !strcmp(cs, "us-ascii") ||
        !strcmp(cs, "ascii")) {
        char *r = malloc(n + 1);
        memcpy(r, s, n);
        r[n] = 0;
        return r;
    }
    if (!strcmp(cs, "iso-8859-1") || !strcmp(cs, "latin1")) strcpy(cs, "windows-1252");
    iconv_t cd = iconv_open("UTF-8", cs);
    if (cd == (iconv_t)-1) {
        char *r = malloc(n + 1);
        memcpy(r, s, n);
        r[n] = 0;
        return r;
    }
    size_t cap = n * 4 + 16;
    char *out = malloc(cap), *op = out;
    char *ip = (char *)s;
    size_t il = n, ol = cap - 1;
    while (il) {
        if (iconv(cd, &ip, &il, &op, &ol) == (size_t)-1) {
            if (errno == E2BIG) break;
            ip++;          /* bad byte: skip it */
            il--;
        }
    }
    *op = 0;
    iconv_close(cd);
    return out;
}

char *decode_words(const char *s)
{
    buf o = {0};
    bput(&o, "", 0);
    if (!s) return o.p;
    const char *ws = NULL;
    int last_ew = 0;
    while (*s) {
        if (s[0] == '=' && s[1] == '?') {
            const char *cs = s + 2, *q1 = strchr(cs, '?');
            if (q1 && (q1[1] == 'B' || q1[1] == 'b' || q1[1] == 'Q' || q1[1] == 'q') && q1[2] == '?') {
                const char *txt = q1 + 3, *e = strstr(txt, "?=");
                if (e && q1 - cs < 40) {
                    if (ws && !last_ew) bput(&o, ws, s - ws);
                    ws = NULL;
                    char charset[48];
                    snprintf(charset, sizeof charset, "%.*s", (int)(q1 - cs), cs);
                    buf raw = {0};
                    if (q1[1] == 'B' || q1[1] == 'b') b64dec(&raw, txt, e - txt);
                    else qpdec(&raw, txt, e - txt, 1);
                    if (raw.n) {
                        char *u = to_utf8(raw.p, raw.n, charset);
                        bputs(&o, u);
                        free(u);
                    }
                    bfree(&raw);
                    s = e + 2;
                    last_ew = 1;
                    continue;
                }
            }
        }
        if (*s == ' ' || *s == '\t') {
            if (!ws) ws = s;
            s++;
            continue;
        }
        if (ws) bput(&o, ws, s - ws);
        ws = NULL;
        last_ew = 0;
        bput(&o, s, 1);
        s++;
    }
    /* control chars out (header injection into our own pages is escaped
       anyway, but keep lists tidy) */
    for (char *p = o.p; *p; p++)
        if ((unsigned char)*p < 32) *p = ' ';
    return o.p;
}

/* finds param `name` in `val` after the first ';'. raw value, quotes removed */
static char *param_raw(const char *val, const char *name)
{
    const char *p = strchr(val, ';');
    size_t nl = strlen(name);
    while (p) {
        p++;
        while (*p == ' ' || *p == '\t') p++;
        const char *eq = strchr(p, '=');
        const char *semi = strchr(p, ';');
        if (!eq || (semi && semi < eq)) { p = semi; continue; }
        const char *ke = eq;
        while (ke > p && (ke[-1] == ' ' || ke[-1] == '\t')) ke--;
        const char *v = eq + 1;
        while (*v == ' ' || *v == '\t') v++;
        buf o = {0};
        const char *after;
        if (*v == '"') {
            v++;
            while (*v && *v != '"') {
                if (*v == '\\' && v[1]) v++;
                bput(&o, v, 1);
                v++;
            }
            after = *v ? v + 1 : v;
        } else {
            const char *e = v;
            while (*e && *e != ';' && *e != ' ' && *e != '\t') e++;
            bput(&o, v, e - v);
            after = e;
        }
        if ((size_t)(ke - p) == nl && !strncasecmp(p, name, nl)) {
            if (!o.p) bput(&o, "", 0);
            return o.p;
        }
        bfree(&o);
        p = strchr(after, ';');
    }
    return NULL;
}

/* RFC 2231: charset'lang'%xx */
static char *ext_decode(const char *s)
{
    const char *q1 = strchr(s, '\''), *q2 = q1 ? strchr(q1 + 1, '\'') : NULL;
    char cs[48] = "";
    const char *data = s;
    if (q2) {
        snprintf(cs, sizeof cs, "%.*s", (int)(q1 - s), s);
        data = q2 + 1;
    }
    buf raw = {0};
    for (; *data; data++) {
        if (*data == '%' && hexv(data[1]) >= 0 && hexv(data[2]) >= 0) {
            char ch = hexv(data[1]) * 16 + hexv(data[2]);
            bput(&raw, &ch, 1);
            data += 2;
        } else bput(&raw, data, 1);
    }
    char *r = to_utf8(raw.p ? raw.p : "", raw.n, cs);
    bfree(&raw);
    return r;
}

char *hparam(const char *val, const char *name)
{
    if (!val) return NULL;
    char key[80];
    snprintf(key, sizeof key, "%s*", name);
    char *v = param_raw(val, key);
    if (v) {
        char *r = ext_decode(v);
        free(v);
        return r;
    }
    /* continuations: name*0*, name*1*, ... (or without the trailing *) */
    buf all = {0};
    int ext = 0;
    for (int i = 0; i < 30; i++) {
        snprintf(key, sizeof key, "%s*%d*", name, i);
        char *c = param_raw(val, key);
        if (c) ext = 1;
        else {
            snprintf(key, sizeof key, "%s*%d", name, i);
            c = param_raw(val, key);
        }
        if (!c) break;
        bputs(&all, c);
        free(c);
    }
    if (all.p) {
        char *r = ext ? ext_decode(all.p) : strdup(all.p);
        bfree(&all);
        return r;
    }
    return param_raw(val, name);
}

void addr_spec(const char *s, char *out, size_t n)
{
    out[0] = 0;
    if (!s) return;
    const char *lt = strrchr(s, '<'), *gt = lt ? strchr(lt, '>') : NULL;
    if (lt && gt) snprintf(out, n, "%.*s", (int)(gt - lt - 1), lt + 1);
    else snprintf(out, n, "%s", s);
    trim(out);
}

static void walk(const char *p, const char *end, int depth, const char *deftype,
                 struct part **arr, int *n, int *cap)
{
    if (*n >= MAX_PARTS) return;
    struct hdrs h;
    const char *body = hdrs_parse(p, end, &h);
    const char *ct = hget(&h, "content-type");
    char type[96];
    if (ct) {
        snprintf(type, sizeof type, "%.*s", (int)strcspn(ct, ";"), ct);
        trim(type);
        lower(type);
    } else snprintf(type, sizeof type, "%s", deftype);

    if (!strncmp(type, "multipart/", 10) && depth < MAX_DEPTH) {
        char *bd = hparam(ct, "boundary");
        if (bd && *bd) {
            const char *child = !strcmp(type, "multipart/digest") ? "message/rfc822" : "text/plain";
            size_t bl = strlen(bd);
            const char *q = body, *start = NULL;
            int closed = 0;
            while (q < end) {
                if ((size_t)(end - q) >= 2 + bl && q[0] == '-' && q[1] == '-' && !memcmp(q + 2, bd, bl)) {
                    const char *after = q + 2 + bl;
                    int last = end - after >= 2 && after[0] == '-' && after[1] == '-';
                    if (start) {
                        const char *pe = q;
                        if (pe > start && pe[-1] == '\n') pe--;
                        if (pe > start && pe[-1] == '\r') pe--;
                        walk(start, pe, depth + 1, child, arr, n, cap);
                    }
                    if (last) { closed = 1; break; }
                    const char *nl = memchr(after, '\n', end - after);
                    if (!nl) { start = NULL; break; }
                    start = q = nl + 1;
                    continue;
                }
                const char *nl = memchr(q, '\n', end - q);
                if (!nl) break;
                q = nl + 1;
            }
            if (start && !closed) walk(start, end, depth + 1, child, arr, n, cap);
            free(bd);
            hfree(&h);
            return;
        }
        free(bd);
    }

    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 8;
        *arr = realloc(*arr, *cap * sizeof **arr);
    }
    struct part *pt = &(*arr)[(*n)++];
    memset(pt, 0, sizeof *pt);
    snprintf(pt->type, sizeof pt->type, "%s", type);
    char *cs = hparam(ct ? ct : "", "charset");
    if (cs) snprintf(pt->charset, sizeof pt->charset, "%s", cs);
    free(cs);
    const char *te = hget(&h, "content-transfer-encoding");
    if (te) {
        snprintf(pt->enc, sizeof pt->enc, "%s", te);
        trim(pt->enc);
        lower(pt->enc);
    }
    const char *cd = hget(&h, "content-disposition");
    char *fn = cd ? hparam(cd, "filename") : NULL;
    if (!fn && ct) fn = hparam(ct, "name");
    if (fn) {
        pt->filename = decode_words(fn);
        free(fn);
        /* no paths, no control chars */
        for (char *c = pt->filename; *c; c++)
            if (*c == '/' || *c == '\\') *c = '_';
    }
    if (cd) {
        char *fname = hparam(cd, "name");
        if (fname) snprintf(pt->name, sizeof pt->name, "%s", fname);
        free(fname);
    }
    pt->attach = (cd && !strncasecmp(cd, "attachment", 10)) || pt->filename || strncmp(type, "text/", 5);
    pt->body = body;
    pt->len = end - body;
    hfree(&h);
}

int mime_parts(const char *msg, size_t n, struct part **out)
{
    int cnt = 0, cap = 0;
    *out = NULL;
    walk(msg, msg + n, 0, "text/plain", out, &cnt, &cap);
    return cnt;
}

void mime_free(struct part *p, int n)
{
    for (int i = 0; i < n; i++) free(p[i].filename);
    free(p);
}

char *part_decode(const struct part *p, size_t *outlen)
{
    buf o = {0};
    if (!strcmp(p->enc, "base64")) b64dec(&o, p->body, p->len);
    else if (!strcmp(p->enc, "quoted-printable")) qpdec(&o, p->body, p->len, 0);
    else bput(&o, p->body, p->len);
    if (!o.p) bput(&o, "", 0);
    *outlen = o.n;
    return o.p;
}

/* crude but safe: tags out, block tags become line breaks */
char *html_to_text(const char *s, size_t n)
{
    buf o = {0};
    bput(&o, "", 0);
    size_t i = 0;
    while (i < n) {
        if (s[i] == '<') {
            size_t j = i + 1;
            int close = j < n && s[j] == '/';
            if (close) j++;
            char tag[16];
            int k = 0;
            while (j < n && k < 15 && isalnum((unsigned char)s[j])) tag[k++] = tolower((unsigned char)s[j++]);
            tag[k] = 0;
            if (!strncmp(s + i, "<!--", 4)) {
                const char *e = memmem(s + i, n - i, "-->", 3);
                i = e ? (size_t)(e - s) + 3 : n;
                continue;
            }
            const char *gt = memchr(s + j, '>', n - j);
            size_t after = gt ? (size_t)(gt - s) + 1 : n;
            if (!close && (!strcmp(tag, "script") || !strcmp(tag, "style") || !strcmp(tag, "head") ||
                           !strcmp(tag, "title"))) {
                char endtag[24];
                int el = snprintf(endtag, sizeof endtag, "</%s", tag);
                size_t m = after;
                while (m + el <= n && strncasecmp(s + m, endtag, el)) m++;
                const char *gt2 = m < n ? memchr(s + m, '>', n - m) : NULL;
                i = gt2 ? (size_t)(gt2 - s) + 1 : n;
                continue;
            }
            if (!strcmp(tag, "br") || !strcmp(tag, "p") || !strcmp(tag, "div") || !strcmp(tag, "tr") ||
                !strcmp(tag, "li") || !strcmp(tag, "h1") || !strcmp(tag, "h2") || !strcmp(tag, "h3") ||
                !strcmp(tag, "table") || !strcmp(tag, "blockquote"))
                bputs(&o, "\n");
            if (!close && !strcmp(tag, "li")) bputs(&o, "- ");
            if (!strcmp(tag, "td")) bputs(&o, " ");
            i = after;
            continue;
        }
        if (s[i] == '&') {
            static const struct { const char *e, *r; } ents[] = {
                {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&#39;", "'"},
                {"&apos;", "'"}, {"&nbsp;", " "}, {"&copy;", "\xc2\xa9"}, {"&zwnj;", ""}, {"&shy;", ""},
            };
            int done = 0;
            for (size_t e = 0; e < sizeof ents / sizeof *ents; e++) {
                size_t el = strlen(ents[e].e);
                if (i + el <= n && !strncasecmp(s + i, ents[e].e, el)) {
                    bputs(&o, ents[e].r);
                    i += el;
                    done = 1;
                    break;
                }
            }
            if (done) continue;
            if (i + 2 < n && s[i + 1] == '#') {
                char *e;
                unsigned long cp = s[i + 2] == 'x' || s[i + 2] == 'X' ? strtoul(s + i + 3, &e, 16)
                                                                     : strtoul(s + i + 2, &e, 10);
                if (*e == ';' && cp && cp < 0x110000) {
                    char u[4];
                    int l;
                    if (cp < 0x80) { u[0] = cp; l = 1; }
                    else if (cp < 0x800) { u[0] = 0xC0 | (cp >> 6); u[1] = 0x80 | (cp & 63); l = 2; }
                    else if (cp < 0x10000) {
                        u[0] = 0xE0 | (cp >> 12); u[1] = 0x80 | ((cp >> 6) & 63); u[2] = 0x80 | (cp & 63); l = 3;
                    } else {
                        u[0] = 0xF0 | (cp >> 18); u[1] = 0x80 | ((cp >> 12) & 63);
                        u[2] = 0x80 | ((cp >> 6) & 63); u[3] = 0x80 | (cp & 63); l = 4;
                    }
                    if (cp != 0x200c && cp != 0x34f && cp != 0xad) bput(&o, u, l);
                    i = e - s + 1;
                    continue;
                }
            }
        }
        if (s[i] == '\r') { i++; continue; }
        if (s[i] == '\n' || s[i] == '\t') {
            if (o.n && o.p[o.n - 1] != ' ' && o.p[o.n - 1] != '\n') bputs(&o, " ");
            i++;
            continue;
        }
        bput(&o, s + i, 1);
        i++;
    }
    /* tidy: strip spaces at line starts/ends, at most one empty line */
    buf t = {0};
    bput(&t, "", 0);
    int nl = 2;
    char *line = o.p;
    while (line && *line) {
        char *e = strchr(line, '\n');
        size_t len = e ? (size_t)(e - line) : strlen(line);
        while (len && line[0] == ' ') { line++; len--; }
        while (len && line[len - 1] == ' ') len--;
        if (len) {
            bput(&t, line, len);
            bputs(&t, "\n");
            nl = 0;
        } else if (nl < 1) {
            bputs(&t, "\n");
            nl++;
        }
        line = e ? e + 1 : NULL;
    }
    bfree(&o);
    return t.p;
}
