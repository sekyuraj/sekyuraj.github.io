/* inbound SMTP: takes mail for <user>@domain and drops it in their inbox.
   No relaying, no auth, no TLS. */
#include "smtp.h"
#include "store.h"
#include "util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define MAX_RCPT 50

struct rd {
    int fd;
    char b[8192];
    size_t s, e;
};

static int rgetc(struct rd *r)
{
    if (r->s == r->e) {
        ssize_t k = read(r->fd, r->b, sizeof r->b);
        if (k <= 0) return -1;
        r->s = 0;
        r->e = k;
    }
    return (unsigned char)r->b[r->s++];
}

/* one line without CR/LF. -1 = connection gone, -2 = longer than max */
static long readline(struct rd *r, buf *l, size_t max)
{
    l->n = 0;
    bput(l, "", 0);
    int over = 0, c, any = 0;
    while ((c = rgetc(r)) >= 0) {
        any = 1;
        if (c == '\n') break;
        if (l->n < max) {
            char ch = c;
            bput(l, &ch, 1);
        } else over = 1;
    }
    if (!any) return -1;
    if (l->n && l->p[l->n - 1] == '\r') l->p[--l->n] = 0;
    return over ? -2 : (long)l->n;
}

static void say(int fd, const char *s) { write_all(fd, s, strlen(s)); }

static void clean(char *s)
{
    for (; *s; s++)
        if ((unsigned char)*s < 32 || *s == 127) *s = '?';
}

/* "FROM:<a@b> SIZE=1" -> a@b (out may be empty for <>). 0 = syntax error */
static int path_arg(const char *arg, char *out, size_t n, const char **rest)
{
    while (*arg == ' ') arg++;
    const char *lt = strchr(arg, '<');
    const char *gt = lt ? strchr(lt, '>') : NULL;
    if (!lt || !gt) {
        /* some senders skip the brackets */
        size_t k = strcspn(arg, " ");
        if (!k) return 0;
        snprintf(out, n, "%.*s", (int)k, arg);
        if (rest) *rest = arg + k;
        return 1;
    }
    snprintf(out, n, "%.*s", (int)(gt - lt - 1), lt + 1);
    /* drop source routes: <@a,@b:user@c> */
    char *colon = out[0] == '@' ? strchr(out, ':') : NULL;
    if (colon) memmove(out, colon + 1, strlen(colon));
    if (rest) *rest = gt + 1;
    return 1;
}

void handle_smtp(int fd, const char *ip)
{
    struct rd r = {.fd = fd};
    buf line = {0}, data = {0};
    char helo[256] = "unknown", from[320] = "", rcpt[MAX_RCPT][32];
    int nrcpt = 0, have_from = 0, errors = 0, msgs = 0;
    char out[512];

    snprintf(out, sizeof out, "220 %s ESMTP sekmail\r\n", cfg.domain);
    say(fd, out);
    for (;;) {
        long n = readline(&r, &line, 1000);
        if (n == -1) break;
        if (n == -2) {
            say(fd, "500 5.5.2 line too long\r\n");
            if (++errors > 10) break;
            continue;
        }
        char *cmd = line.p, *arg = line.p + (line.n >= 4 ? 4 : line.n);
        if (!strncasecmp(cmd, "EHLO", 4) || !strncasecmp(cmd, "HELO", 4)) {
            snprintf(helo, sizeof helo, "%s", arg[0] == ' ' ? arg + 1 : "unknown");
            clean(helo);
            have_from = nrcpt = 0;
            if (!strncasecmp(cmd, "EHLO", 4)) {
                snprintf(out, sizeof out, "250-%s\r\n250-SIZE %ld\r\n250-8BITMIME\r\n250 ENHANCEDSTATUSCODES\r\n",
                         cfg.domain, cfg.max_size);
                say(fd, out);
            } else {
                snprintf(out, sizeof out, "250 %s\r\n", cfg.domain);
                say(fd, out);
            }
        } else if (!strncasecmp(cmd, "MAIL FROM:", 10)) {
            const char *rest = NULL;
            if (!path_arg(cmd + 10, from, sizeof from, &rest)) {
                say(fd, "501 5.5.4 bad sender\r\n");
                errors++;
                continue;
            }
            clean(from);
            const char *sz = rest ? strcasestr(rest, "SIZE=") : NULL;
            if (sz && atol(sz + 5) > cfg.max_size) {
                say(fd, "552 5.3.4 message too big\r\n");
                continue;
            }
            have_from = 1;
            nrcpt = 0;
            say(fd, "250 2.1.0 ok\r\n");
        } else if (!strncasecmp(cmd, "RCPT TO:", 8)) {
            if (!have_from) {
                say(fd, "503 5.5.1 MAIL first\r\n");
                errors++;
                continue;
            }
            char to[320], user[32];
            if (!path_arg(cmd + 8, to, sizeof to, NULL)) {
                say(fd, "501 5.5.4 bad recipient\r\n");
                errors++;
                continue;
            }
            char *at = strrchr(to, '@');
            if (at) {
                *at = 0;
                lower(at + 1);
            }
            /* bare "postmaster" is allowed without a domain (RFC 5321) */
            if (at ? strcmp(at + 1, cfg.domain) : strcasecmp(to, "postmaster")) {
                say(fd, "550 5.7.1 relaying denied\r\n");
                errors++;
            } else if (!resolve_local(to, user, sizeof user)) {
                say(fd, "550 5.1.1 no such user here\r\n");
                errors++;
            } else if (user_usage(user) > cfg.quota_mb << 20) {
                say(fd, "452 4.2.2 mailbox full\r\n");
            } else if (nrcpt >= MAX_RCPT) {
                say(fd, "452 4.5.3 too many recipients\r\n");
            } else {
                int dup = 0;
                for (int i = 0; i < nrcpt; i++) dup |= !strcmp(rcpt[i], user);
                if (!dup) strcpy(rcpt[nrcpt++], user);
                say(fd, "250 2.1.5 ok\r\n");
            }
        } else if (!strncasecmp(cmd, "DATA", 4)) {
            if (!nrcpt) {
                say(fd, "503 5.5.1 no valid recipients\r\n");
                errors++;
                continue;
            }
            say(fd, "354 go ahead, end with <CRLF>.<CRLF>\r\n");
            data.n = 0;
            char date[64];
            time_t t = time(NULL);
            struct tm tm;
            localtime_r(&t, &tm);
            strftime(date, sizeof date, "%a, %d %b %Y %H:%M:%S %z", &tm);
            bprintf(&data, "Return-Path: <%s>\r\nReceived: from %s ([%s])\r\n\tby %s (sekmail); %s\r\n", from,
                    helo, ip, cfg.domain, date);
            int big = 0, gone = 0;
            for (;;) {
                long k = readline(&r, &line, 1 << 16);
                if (k == -1) { gone = 1; break; }
                if (!strcmp(line.p, ".")) break;
                const char *s = line.p[0] == '.' ? line.p + 1 : line.p;
                if (data.n + strlen(s) + 2 > (size_t)cfg.max_size) big = 1;
                if (!big) {
                    bputs(&data, s);
                    bputs(&data, "\r\n");
                }
            }
            if (gone) break;
            if (big) {
                say(fd, "552 5.3.4 message too big\r\n");
            } else {
                char id[96] = "";
                int fails = 0;
                for (int i = 0; i < nrcpt; i++)
                    if (deliver(rcpt[i], "inbox", data.p, data.n, 0, id) < 0) fails++;
                if (fails == nrcpt) {
                    say(fd, "451 4.3.0 could not store the message\r\n");
                    logmsg("smtp %s: store failed", ip);
                } else {
                    snprintf(out, sizeof out, "250 2.0.0 ok %s\r\n", id);
                    say(fd, out);
                    logmsg("smtp %s: <%s> -> %d local, %zu bytes", ip, from, nrcpt, data.n);
                }
            }
            have_from = nrcpt = 0;
            if (++msgs >= 100) break;
        } else if (!strncasecmp(cmd, "RSET", 4)) {
            have_from = nrcpt = 0;
            say(fd, "250 2.0.0 ok\r\n");
        } else if (!strncasecmp(cmd, "NOOP", 4)) {
            say(fd, "250 2.0.0 ok\r\n");
        } else if (!strncasecmp(cmd, "VRFY", 4)) {
            say(fd, "252 2.1.5 send some mail and see\r\n");
        } else if (!strncasecmp(cmd, "QUIT", 4)) {
            say(fd, "221 2.0.0 bye\r\n");
            break;
        } else {
            say(fd, "502 5.5.2 not implemented\r\n");
            errors++;
        }
        if (errors > 20) {
            say(fd, "421 4.7.0 too many errors\r\n");
            break;
        }
    }
    bfree(&line);
    bfree(&data);
}
