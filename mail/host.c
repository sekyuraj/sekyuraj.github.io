/* /host/: people with a sekmail account ask the admin for a website; once
   approved they manage files here and Caddy serves them at /~name/ (with a
   CSP sandbox, so their pages can't read anyone's mail). */
#define _GNU_SOURCE
#include "mime.h"
#include "store.h"
#include "util.h"
#include "web.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_FILES 1000

static pthread_mutex_t hmu = PTHREAD_MUTEX_INITIALIZER;

enum { NONE, PENDING, APPROVED, DENIED };
static const char *st_name[] = {"none", "pending", "approved", "denied"};

struct site {
    char user[32];
    int st;
    time_t since;
};

/* ---------- the sites list: <dir>/host/sites, "user status since" */

static int load_sites(struct site **out)
{
    char p[1100];
    snprintf(p, sizeof p, "%s/host/sites", cfg.dir);
    *out = NULL;
    FILE *f = fopen(p, "r");
    if (!f) return 0;
    int n = 0, cap = 0;
    char line[200];
    while (fgets(line, sizeof line, f)) {
        char u[64], st[16];
        long t;
        if (sscanf(line, "%63s %15s %ld", u, st, &t) != 3 || strlen(u) >= 32) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 32;
            *out = realloc(*out, cap * sizeof **out);
        }
        struct site *s = &(*out)[n];
        strcpy(s->user, u);
        s->st = NONE;
        for (int i = 1; i < 4; i++)
            if (!strcmp(st, st_name[i])) s->st = i;
        s->since = t;
        if (s->st != NONE) n++;
    }
    fclose(f);
    return n;
}

static void save_sites(struct site *s, int n)
{
    char d[1100], p[1200], t[1200];
    snprintf(d, sizeof d, "%s/host", cfg.dir);
    mkdir(d, 0700);
    snprintf(p, sizeof p, "%s/sites", d);
    snprintf(t, sizeof t, "%s/sites.tmp", d);
    FILE *f = fopen(t, "w");
    if (!f) return;
    for (int i = 0; i < n; i++)
        if (s[i].st != NONE) fprintf(f, "%s %s %ld\n", s[i].user, st_name[s[i].st], (long)s[i].since);
    if (!fflush(f) && !fsync(fileno(f))) rename(t, p);
    fclose(f);
}

static int status_of(const char *user, time_t *since)
{
    pthread_mutex_lock(&hmu);
    struct site *s;
    int n = load_sites(&s), st = NONE;
    for (int i = 0; i < n; i++)
        if (!strcmp(s[i].user, user)) {
            st = s[i].st;
            if (since) *since = s[i].since;
        }
    pthread_mutex_unlock(&hmu);
    free(s);
    return st;
}

static void set_status(const char *user, int st)
{
    pthread_mutex_lock(&hmu);
    struct site *s;
    int n = load_sites(&s), found = 0;
    for (int i = 0; i < n; i++)
        if (!strcmp(s[i].user, user)) {
            s[i].st = st;
            s[i].since = time(NULL);
            found = 1;
        }
    if (!found && st != NONE) {
        s = realloc(s, (n + 1) * sizeof *s);
        snprintf(s[n].user, sizeof s[n].user, "%s", user);
        s[n].st = st;
        s[n].since = time(NULL);
        n++;
    }
    save_sites(s, n);
    pthread_mutex_unlock(&hmu);
    free(s);
}

void host_request(const char *user)
{
    int st = status_of(user, NULL);
    if (st == NONE || st == DENIED) {
        set_status(user, PENDING);
        logmsg("host: %s asked for a site", user);
    }
}

int host_pending(void)
{
    pthread_mutex_lock(&hmu);
    struct site *s;
    int n = load_sites(&s), k = 0;
    pthread_mutex_unlock(&hmu);
    for (int i = 0; i < n; i++) k += s[i].st == PENDING;
    free(s);
    return k;
}

/* ---------- files */

static const char *text_ext[] = {"html", "htm", "css", "js", "mjs", "txt", "json", "svg", "md", "xml", "csv", NULL};
static const char *ok_ext[] = {"html", "htm", "css", "js", "mjs", "txt", "json", "svg", "md", "xml", "csv",
                               "png", "jpg", "jpeg", "gif", "webp", "avif", "ico", "bmp", "woff", "woff2",
                               "ttf", "otf", "mp3", "ogg", "wav", "flac", "mp4", "webm", "pdf", "zip", "wasm", NULL};

static const char *ext_of(const char *path)
{
    const char *slash = strrchr(path, '/'), *dot = strrchr(path, '.');
    return dot && (!slash || dot > slash) ? dot + 1 : "";
}

static int in_list(const char *e, const char **l)
{
    for (int i = 0; l[i]; i++)
        if (!strcasecmp(e, l[i])) return 1;
    return 0;
}

static int is_text(const char *path) { return in_list(ext_of(path), text_ext); }

/* cleans "a/b c/x.html" -> "a/b-c/x.html"; 0 if it can't be made safe */
static int clean_path(const char *in, char *out, size_t n)
{
    size_t o = 0;
    int depth = 0, seglen = 0;
    while (*in == '/' || *in == ' ') in++;
    for (; *in; in++) {
        char c = *in;
        if (c == '/' || c == '\\') {
            if (!seglen) continue;
            if (++depth > 4) return 0;
            c = '/';
            seglen = 0;
        } else if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
                     c == '_' || c == '-'))
            c = '-';
        if (c == '.' && seglen == 0) return 0;   /* no dotfiles, no .. */
        if (o + 2 >= n) return 0;
        out[o++] = c;
        if (c != '/') seglen++;
    }
    while (o && out[o - 1] == '/') o--;
    out[o] = 0;
    if (!o || strlen(out) > 150) return 0;
    const char *base = strrchr(out, '/');
    base = base ? base + 1 : out;
    return in_list(ext_of(base), ok_ext);
}

static void site_dir(const char *user, char *out, size_t n) { snprintf(out, n, "%s/%s", cfg.host_dir, user); }

static int mkparents(const char *root, const char *rel)
{
    char p[1400];
    snprintf(p, sizeof p, "%s/%s", root, rel);
    char *s = p + strlen(root) + 1;
    for (char *c = s; *c; c++)
        if (*c == '/') {
            *c = 0;
            if (mkdir(p, 0755) < 0 && errno != EEXIST) return -1;
            chmod(p, 0755);
            *c = '/';
        }
    return 0;
}

static int write_site_file(const char *user, const char *rel, const char *data, size_t n)
{
    char root[700], p[1400], t[1450];
    site_dir(user, root, sizeof root);
    if (mkparents(root, rel) < 0) return -1;
    snprintf(p, sizeof p, "%s/%s", root, rel);
    snprintf(t, sizeof t, "%s.uploading", p);
    int fd = open(t, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd < 0) return -1;
    fchmod(fd, 0644);   /* Caddy (another user) has to read it */
    int bad = write_all(fd, data, n) < 0 || fsync(fd) < 0;
    close(fd);
    if (bad || rename(t, p) < 0) {
        unlink(t);
        return -1;
    }
    return 0;
}

struct fent {
    char rel[200];
    long size;
};

static struct fent *walk_files;
static int walk_n, walk_cap;
static size_t walk_root;
static long walk_bytes;
static pthread_mutex_t walk_mu = PTHREAD_MUTEX_INITIALIZER;   /* nftw has no user pointer */

static int walk_cb(const char *path, const struct stat *st, int type, struct FTW *ftw)
{
    (void)ftw;
    if (type != FTW_F) return 0;
    walk_bytes += st->st_size;
    if (walk_n == walk_cap) {
        walk_cap = walk_cap ? walk_cap * 2 : 64;
        walk_files = realloc(walk_files, walk_cap * sizeof *walk_files);
    }
    snprintf(walk_files[walk_n].rel, sizeof walk_files[0].rel, "%s", path + walk_root);
    walk_files[walk_n].size = st->st_size;
    walk_n++;
    return walk_n > MAX_FILES * 2;
}

static int cmp_fent(const void *a, const void *b) { return strcmp(((struct fent *)a)->rel, ((struct fent *)b)->rel); }

static int list_files(const char *user, struct fent **out, long *bytes)
{
    char root[700];
    site_dir(user, root, sizeof root);
    pthread_mutex_lock(&walk_mu);
    walk_files = NULL;
    walk_n = walk_cap = 0;
    walk_bytes = 0;
    walk_root = strlen(root) + 1;
    nftw(root, walk_cb, 16, FTW_PHYS);
    *out = walk_files;
    int n = walk_n;
    *bytes = walk_bytes;
    pthread_mutex_unlock(&walk_mu);
    if (n) qsort(*out, n, sizeof **out, cmp_fent);
    return n;
}

static int rm_cb(const char *path, const struct stat *st, int type, struct FTW *ftw)
{
    (void)st;
    (void)ftw;
    if (type == FTW_DP) rmdir(path);
    else unlink(path);
    return 0;
}

static void rm_site(const char *user)
{
    char root[700];
    site_dir(user, root, sizeof root);
    if (valid_name(user, 1)) nftw(root, rm_cb, 16, FTW_DEPTH | FTW_PHYS);
}

/* ---------- mail to the user */

static void notify(const char *user, const char *subject, const char *body)
{
    char date[64];
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(date, sizeof date, "%a, %d %b %Y %H:%M:%S %z", &tm);
    buf m = {0};
    bprintf(&m, "From: %s@%s\r\nTo: %s@%s\r\nDate: %s\r\nSubject: %s\r\nMIME-Version: 1.0\r\n"
                "Content-Type: text/plain; charset=utf-8\r\nContent-Transfer-Encoding: 8bit\r\n\r\n%s",
            *cfg.admin ? cfg.admin : "postmaster", cfg.domain, user, cfg.domain, date, subject, body);
    deliver(user, "inbox", m.p, m.n, 0, NULL);
    bfree(&m);
}

/* ---------- pages */

static void msg(buf *b, const char *cls, const char *text)
{
    bprintf(b, "<p class=\"%s\">", cls);
    besc(b, text);
    bputs(b, "</p>");
}

static void page_login(struct req *r)
{
    buf b = {0};
    page_head(&b, r, "websites");
    bprintf(&b, "<div class=\"box\"><h1>websites</h1><p class=\"muted\">Free little HTML sites at "
                "%s/~yourname/. Log in with your mail account.</p>"
                "<form method=\"post\" action=\"/mail/login\"><input type=\"hidden\" name=\"next\" value=\"/host/\">"
                "<label for=\"u\">name</label><input type=\"text\" id=\"u\" name=\"u\" autocomplete=\"username\" "
                "autocapitalize=\"none\" required autofocus><label for=\"p\">password</label>"
                "<input type=\"password\" id=\"p\" name=\"p\" autocomplete=\"current-password\" required>"
                "<button>log in</button></form>",
            cfg.domain);
    if (cfg.signups)
        bprintf(&b, "<p style=\"margin-top:2rem\">No account? <a href=\"/mail/signup?next=host\">Get a mail "
                    "address and ask %s for a site</a>.</p>",
                *cfg.admin ? cfg.admin : "the admin");
    bputs(&b, "</div>");
    send_page(r, 200, &b, NULL);
    bfree(&b);
}

static void when(char *out, size_t n, time_t t)
{
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(out, n, "%d %b %Y", &tm);
}

static void page_dash(struct req *r, const char *note, int bad)
{
    time_t since = 0;
    int st = status_of(r->user, &since);
    buf b = {0};
    page_head(&b, r, "website");
    int admin = *cfg.admin && !strcmp(r->user, cfg.admin);
    if (admin) {
        int p = host_pending();
        bprintf(&b, "<p><a href=\"/host/admin\">site requests%s", p ? " " : "");
        if (p) bprintf(&b, "(%d new)", p);
        bputs(&b, "</a></p>");
    }
    if (note) msg(&b, bad ? "err" : "ok", note);
    char d[32];
    when(d, sizeof d, since);
    if (st == NONE || st == DENIED) {
        bputs(&b, "<h1>your website</h1>");
        if (st == DENIED) bprintf(&b, "<p>Your last request (%s) wasn't approved. You can ask again.</p>", d);
        else bprintf(&b, "<p>You don't have a site yet. Sites are approved by hand, so write %s a few lines "
                         "about what you want to put up.</p>",
                     *cfg.admin ? cfg.admin : "the admin");
        if (*cfg.admin) {
            buf l = {0};
            request_link(&l, r->user);
            bputs(&b, "<p><a href=\"");
            besc(&b, l.p);
            bputs(&b, "\">Write the request</a></p>");
            bfree(&l);
        }
    } else if (st == PENDING) {
        bprintf(&b, "<h1>your website</h1><p>Your request from %s is waiting for %s. You'll get a mail in "
                    "<a href=\"/mail/\">your inbox</a> when it's decided.</p>",
                d, *cfg.admin ? cfg.admin : "the admin");
    } else {
        struct fent *f;
        long bytes;
        int n = list_files(r->user, &f, &bytes);
        bprintf(&b, "<h1>your website</h1><p><a href=\"/~%s/\" target=\"_blank\">https://%s/~%s/</a><br>"
                    "<span class=\"muted\">%d files, %.1f of %ld MB</span></p>",
                r->user, cfg.domain, r->user, n, bytes / 1048576.0, cfg.host_quota_mb);
        if (n) bputs(&b, "<table>");
        for (int i = 0; i < n; i++) {
            bputs(&b, "<tr><td>");
            if (is_text(f[i].rel)) {
                bputs(&b, "<a href=\"/host/edit?f=");
                urlenc(&b, f[i].rel);
                bputs(&b, "\">");
                besc(&b, f[i].rel);
                bputs(&b, "</a>");
            } else besc(&b, f[i].rel);
            bprintf(&b, "</td><td class=\"when\">%.1f KB</td><td class=\"when\"><a href=\"/~%s/", f[i].size / 1024.0,
                    r->user);
            besc(&b, f[i].rel);
            bputs(&b, "\" target=\"_blank\">open</a></td><td class=\"when\"><form class=\"inline\" method=\"post\" "
                      "action=\"/host/delete\"><input type=\"hidden\" name=\"f\" value=\"");
            besc(&b, f[i].rel);
            bputs(&b, "\"><button>delete</button></form></td></tr>");
        }
        if (n) bputs(&b, "</table>");
        free(f);
        bputs(&b, "<h1 style=\"margin-top:2rem\">upload</h1>"
                  "<form method=\"post\" action=\"/host/upload\" enctype=\"multipart/form-data\">"
                  "<input type=\"file\" name=\"files\" multiple required>"
                  "<label for=\"dir\">into folder (empty = top)</label>"
                  "<input type=\"text\" id=\"dir\" name=\"dir\" placeholder=\"e.g. img\">"
                  "<div class=\"bar\"><button>upload</button><span class=\"muted\">html, css, js, images, "
                  "fonts, audio, video, pdf. 30 MB per upload.</span></div></form>"
                  "<h1 style=\"margin-top:2rem\">new file</h1>"
                  "<form method=\"get\" action=\"/host/edit\"><input type=\"text\" name=\"f\" "
                  "placeholder=\"about.html\" required><div class=\"bar\"><button>create</button></div></form>"
                  "<p class=\"muted\">Your pages run sandboxed: scripts work, but no cookies or "
                  "localStorage. index.html is the front page.</p>");
    }
    send_page(r, 200, &b, NULL);
    bfree(&b);
}

static int approved(struct req *r) { return status_of(r->user, NULL) == APPROVED; }

static void page_edit(struct req *r, const char *f, const char *err, const char *ok)
{
    char rel[200];
    if (!clean_path(f, rel, sizeof rel) || !is_text(rel)) {
        page_dash(r, "That file name isn't allowed. Use letters, digits, - _ . and an ending like .html", 1);
        return;
    }
    char root[700], p[1000];
    site_dir(r->user, root, sizeof root);
    snprintf(p, sizeof p, "%s/%s", root, rel);
    buf c = {0};
    read_file(p, &c, 2 << 20);
    buf b = {0};
    page_head(&b, r, rel);
    bputs(&b, "<h1>");
    besc(&b, rel);
    bputs(&b, "</h1>");
    if (err) msg(&b, "err", err);
    if (ok) msg(&b, "ok", ok);
    bputs(&b, "<form method=\"post\" action=\"/host/save\"><input type=\"hidden\" name=\"f\" value=\"");
    besc(&b, rel);
    bputs(&b, "\"><textarea name=\"c\" spellcheck=\"false\" style=\"min-height:32rem\">");
    if (c.p) bescn(&b, c.p, c.n);
    bprintf(&b, "</textarea><div class=\"bar\"><button>save</button><a href=\"/~%s/", r->user);
    besc(&b, rel);
    bputs(&b, "\" target=\"_blank\">open</a><a href=\"/host/\">back</a></div></form>");
    send_page(r, 200, &b, NULL);
    bfree(&b);
    bfree(&c);
}

static void do_save(struct req *r)
{
    char *f = field(r, "f"), *c = field(r, "c");
    char rel[200];
    /* textareas send CRLF */
    char *o = c;
    for (char *s = c; *s; s++)
        if (*s != '\r') *o++ = *s;
    *o = 0;
    struct fent *fl;
    long bytes;
    int n = list_files(r->user, &fl, &bytes);
    free(fl);
    if (!clean_path(f, rel, sizeof rel) || !is_text(rel)) page_dash(r, "That file name isn't allowed.", 1);
    else if (bytes + (long)strlen(c) > cfg.host_quota_mb << 20) page_edit(r, rel, "Your site is full.", NULL);
    else if (n >= MAX_FILES) page_edit(r, rel, "Too many files.", NULL);
    else if (write_site_file(r->user, rel, c, strlen(c)) < 0) page_edit(r, rel, "Couldn't save. Try again.", NULL);
    else {
        buf to = {0};
        bputs(&to, "/host/edit?n=saved&f=");
        urlenc(&to, rel);
        redirect(r, to.p, NULL);
        bfree(&to);
    }
    free(f);
    free(c);
}

static void do_upload(struct req *r)
{
    if (strncasecmp(r->ctype, "multipart/form-data", 19)) {
        page_dash(r, "Upload didn't come through.", 1);
        return;
    }
    buf m = {0};
    bprintf(&m, "Content-Type: %s\r\n\r\n", r->ctype);
    bput(&m, r->body, r->blen);
    struct part *pt;
    int np = mime_parts(m.p, m.n, &pt);
    char dir[120] = "";
    for (int i = 0; i < np; i++)
        if (!strcmp(pt[i].name, "dir")) {
            snprintf(dir, sizeof dir, "%.*s", (int)(pt[i].len < 119 ? pt[i].len : 119), pt[i].body);
            trim(dir);
        }
    struct fent *fl;
    long bytes;
    int nfiles = list_files(r->user, &fl, &bytes);
    free(fl);
    int saved = 0, skipped = 0;
    for (int i = 0; i < np; i++) {
        if (strcmp(pt[i].name, "files") || !pt[i].filename || !*pt[i].filename) continue;
        char want[300], rel[200];
        snprintf(want, sizeof want, "%s%s%s", dir, *dir ? "/" : "", pt[i].filename);
        if (!clean_path(want, rel, sizeof rel) || bytes + (long)pt[i].len > cfg.host_quota_mb << 20 ||
            nfiles >= MAX_FILES || write_site_file(r->user, rel, pt[i].body, pt[i].len) < 0) {
            skipped++;
            continue;
        }
        bytes += pt[i].len;
        nfiles++;
        saved++;
    }
    mime_free(pt, np);
    bfree(&m);
    char note[200];
    if (skipped)
        snprintf(note, sizeof note, "Uploaded %d, skipped %d (file type not allowed, bad name, or site full).", saved,
                 skipped);
    else snprintf(note, sizeof note, "Uploaded %d file%s.", saved, saved == 1 ? "" : "s");
    logmsg("host: %s uploaded %d, skipped %d", r->user, saved, skipped);
    page_dash(r, note, skipped > 0);
}

static void do_delete(struct req *r)
{
    char *f = field(r, "f");
    char rel[200], root[700], p[1000];
    if (clean_path(f, rel, sizeof rel)) {
        site_dir(r->user, root, sizeof root);
        snprintf(p, sizeof p, "%s/%s", root, rel);
        unlink(p);
        /* drop folders that are now empty */
        for (char *s = strrchr(p, '/'); s && s > p + strlen(root); s = strrchr(p, '/')) {
            *s = 0;
            if (rmdir(p) < 0) break;
        }
    }
    free(f);
    redirect(r, "/host/", NULL);
}

/* ---------- admin */

static void page_admin(struct req *r)
{
    pthread_mutex_lock(&hmu);
    struct site *s;
    int n = load_sites(&s);
    pthread_mutex_unlock(&hmu);
    buf b = {0};
    page_head(&b, r, "site requests");
    char *note = qfield(r, "n");
    if (*note) msg(&b, "ok", note);
    free(note);
    const char *heads[] = {"", "waiting", "sites", "said no"};
    for (int st = PENDING; st <= DENIED; st++) {
        int any = 0;
        for (int i = 0; i < n; i++) {
            if (s[i].st != st) continue;
            if (!any++) bprintf(&b, "<h1 style=\"margin-top:1.5rem\">%s</h1><table>", heads[st]);
            char d[32];
            when(d, sizeof d, s[i].since);
            bputs(&b, "<tr><td>");
            if (st == APPROVED) bprintf(&b, "<a href=\"/~%s/\" target=\"_blank\">~%s</a>", s[i].user, s[i].user);
            else besc(&b, s[i].user);
            bprintf(&b, "</td><td class=\"when\">%s</td><td style=\"text-align:right\">", d);
            const char *acts[3][2] = {{"approve", "approve"}, {"deny", "say no"}, {"remove", "take down"}};
            for (int a = 0; a < 3; a++) {
                if ((a == 0 && st == APPROVED) || (a == 1 && st != PENDING) || (a == 2 && st != APPROVED)) continue;
                bprintf(&b, "<form class=\"inline\" method=\"post\" action=\"/host/admin/%s\">"
                            "<input type=\"hidden\" name=\"u\" value=\"%s\"><button>%s</button></form> ",
                        acts[a][0], s[i].user, acts[a][1]);
            }
            bputs(&b, "</td></tr>");
        }
        if (any) bputs(&b, "</table>");
    }
    if (!n) bputs(&b, "<p class=\"muted\">No requests yet.</p>");
    bputs(&b, "<p class=\"muted\" style=\"margin-top:2rem\">Requests also land in your inbox as mail with the "
              "subject \"Website request\".</p>");
    send_page(r, 200, &b, NULL);
    bfree(&b);
    free(s);
}

static void do_admin(struct req *r, const char *what)
{
    char *u = field(r, "u");
    char note[200] = "";
    if (!valid_name(u, 1) || !user_exists(u)) snprintf(note, sizeof note, "No such user.");
    else if (!strcmp(what, "approve")) {
        set_status(u, APPROVED);
        char root[700], p[800];
        site_dir(u, root, sizeof root);
        mkdir(root, 0755);
        chmod(root, 0755);
        snprintf(p, sizeof p, "%s/index.html", root);
        if (access(p, F_OK)) {
            buf h = {0};
            bprintf(&h, "<!doctype html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n"
                        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n<title>%s</title>\n"
                        "</head>\n<body>\n<h1>%s's site</h1>\n<p>Nothing here yet.</p>\n</body>\n</html>\n",
                    u, u);
            write_site_file(u, "index.html", h.p, h.n);
            bfree(&h);
        }
        char body[700];
        snprintf(body, sizeof body,
                 "Your website is approved.\r\n\r\nIt's at https://%s/~%s/\r\n\r\nUpload and edit your files at "
                 "https://%s/host/ (same login as your mail).\r\n",
                 cfg.domain, u, cfg.domain);
        notify(u, "Your website is approved", body);
        snprintf(note, sizeof note, "~%s approved, they got a mail.", u);
    } else if (!strcmp(what, "deny")) {
        set_status(u, DENIED);
        notify(u, "About your website request",
               "Your website request wasn't approved this time. Reply to this mail if you want to talk about it.\r\n");
        snprintf(note, sizeof note, "Said no to %s, they got a mail.", u);
    } else if (!strcmp(what, "remove")) {
        rm_site(u);
        set_status(u, NONE);
        notify(u, "Your website was taken down",
               "Your website was taken down. Reply to this mail if you want to know why.\r\n");
        snprintf(note, sizeof note, "~%s taken down and deleted.", u);
    }
    logmsg("host admin: %s %s", what, u);
    buf to = {0};
    bputs(&to, "/host/admin?n=");
    urlenc(&to, note);
    redirect(r, to.p, NULL);
    bfree(&to);
    free(u);
}

void host_route(struct req *r, const char *p, int post)
{
    if (!*p) {
        redirect(r, "/host/", NULL);
        return;
    }
    if (!r->user[0]) {
        if (!strcmp(p, "/")) page_login(r);
        else redirect(r, "/host/", NULL);
        return;
    }
    int admin = *cfg.admin && !strcmp(r->user, cfg.admin);
    if (!strcmp(p, "/")) {
        char *n = qfield(r, "n");
        page_dash(r, !strcmp(n, "asked") ? "Request sent. You'll get a mail when it's decided." : NULL, 0);
        free(n);
    } else if (!strncmp(p, "/admin", 6) && admin) {
        if (!post) page_admin(r);
        else if (!strcmp(p, "/admin/approve")) do_admin(r, "approve");
        else if (!strcmp(p, "/admin/deny")) do_admin(r, "deny");
        else if (!strcmp(p, "/admin/remove")) do_admin(r, "remove");
        else redirect(r, "/host/admin", NULL);
    } else if (!approved(r)) {
        redirect(r, "/host/", NULL);
    } else if (!strcmp(p, "/edit") && !post) {
        char *f = qfield(r, "f"), *n = qfield(r, "n");
        page_edit(r, f, NULL, !strcmp(n, "saved") ? "Saved." : NULL);
        free(f);
        free(n);
    } else if (!strcmp(p, "/save") && post) do_save(r);
    else if (!strcmp(p, "/upload") && post) do_upload(r);
    else if (!strcmp(p, "/delete") && post) do_delete(r);
    else redirect(r, "/host/", NULL);
}
