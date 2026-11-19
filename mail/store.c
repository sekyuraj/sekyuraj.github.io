/* users file and Maildirs: <dir>/mail/<user>/<inbox|sent>/{tmp,new,cur} */
#include "store.h"
#include "mime.h"
#include "util.h"

#include <crypt.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static pthread_mutex_t umu = PTHREAD_MUTEX_INITIALIZER;

static const char *reserved[] = {
    "postmaster", "abuse", "root", "admin", "administrator", "hostmaster", "webmaster", "www",
    "mail", "mailer-daemon", "noreply", "no-reply", "support", "security", "info", "sekraj",
    "sekmail", "help", "contact", "owner", "staff", "mod", "nobody", NULL,
};

int valid_name(const char *s, int allow_reserved)
{
    size_t n = strlen(s);
    if (n < 2 || n > 24) return 0;
    if (!((s[0] >= 'a' && s[0] <= 'z') || (s[0] >= '0' && s[0] <= '9'))) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) return 0;
        if (c == '.' && (i + 1 == n || s[i + 1] == '.')) return 0;
    }
    if (!allow_reserved)
        for (int i = 0; reserved[i]; i++)
            if (!strcmp(s, reserved[i])) return 0;
    return 1;
}

int valid_id(const char *id)
{
    size_t n = strlen(id);
    if (!n || n > 90 || id[0] == '.') return 0;
    for (size_t i = 0; i < n; i++) {
        char c = id[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
              c == '_' || c == '-'))
            return 0;
    }
    return 1;
}

/* copies the stored hash for name into out; caller holds umu */
static int find_hash(const char *name, char *out, size_t n)
{
    char path[1100];
    snprintf(path, sizeof path, "%s/users", cfg.dir);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[512];
    int found = 0;
    size_t nl = strlen(name);
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, name, nl) && line[nl] == ':') {
            line[strcspn(line, "\n")] = 0;
            snprintf(out, n, "%s", line + nl + 1);
            found = 1;
            break;
        }
    }
    fclose(f);
    return found;
}

int user_exists(const char *name)
{
    char h[256];
    if (!valid_name(name, 1)) return 0;
    pthread_mutex_lock(&umu);
    int r = find_hash(name, h, sizeof h);
    pthread_mutex_unlock(&umu);
    return r;
}

static char *make_hash(const char *pass)
{
    char salt[CRYPT_GENSALT_OUTPUT_SIZE];
    if (!crypt_gensalt_rn("$y$", 0, NULL, 0, salt, sizeof salt)) return NULL;
    struct crypt_data *cd = calloc(1, sizeof *cd);
    char *h = crypt_r(pass, salt, cd);
    char *r = h && h[0] == '$' ? strdup(h) : NULL;
    free(cd);
    return r;
}

static pthread_once_t dummy_once = PTHREAD_ONCE_INIT;
static char dummy_hash[256] = "*";
static void make_dummy(void)
{
    char *h = make_hash("not a password");
    if (h) snprintf(dummy_hash, sizeof dummy_hash, "%s", h);
    free(h);
}

int user_check(const char *name, const char *pass)
{
    char h[256];
    int have = 0;
    if (valid_name(name, 1)) {
        pthread_mutex_lock(&umu);
        have = find_hash(name, h, sizeof h);
        pthread_mutex_unlock(&umu);
    }
    /* hash anyway so a missing user takes as long as a wrong password */
    if (!have) {
        pthread_once(&dummy_once, make_dummy);
        snprintf(h, sizeof h, "%s", dummy_hash);
    }
    struct crypt_data *cd = calloc(1, sizeof *cd);
    char *r = crypt_r(pass, h, cd);
    int ok = have && r && !strcmp(r, h);
    free(cd);
    return ok;
}

static int mkdirs(const char *user, const char *folder)
{
    char p[1300];
    snprintf(p, sizeof p, "%s/mail", cfg.dir);
    mkdir(p, 0700);
    snprintf(p, sizeof p, "%s/mail/%s", cfg.dir, user);
    mkdir(p, 0700);
    snprintf(p, sizeof p, "%s/mail/%s/%s", cfg.dir, user, folder);
    mkdir(p, 0700);
    const char *sub[] = {"tmp", "new", "cur"};
    for (int i = 0; i < 3; i++) {
        snprintf(p, sizeof p, "%s/mail/%s/%s/%s", cfg.dir, user, folder, sub[i]);
        if (mkdir(p, 0700) < 0 && errno != EEXIST) return -1;
    }
    return 0;
}

int user_add(const char *name, const char *pass, int allow_reserved)
{
    if (!valid_name(name, allow_reserved)) return -2;
    char *h = make_hash(pass);
    if (!h) return -3;
    char path[1100], tmp[256];
    snprintf(path, sizeof path, "%s/users", cfg.dir);
    pthread_mutex_lock(&umu);
    int r = 0;
    if (find_hash(name, tmp, sizeof tmp)) r = -1;
    else {
        int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
        char line[512];
        int len = snprintf(line, sizeof line, "%s:%s\n", name, h);
        if (fd < 0 || write_all(fd, line, len) < 0 || fsync(fd) < 0) r = -3;
        if (fd >= 0) close(fd);
    }
    pthread_mutex_unlock(&umu);
    free(h);
    if (!r && (mkdirs(name, "inbox") < 0 || mkdirs(name, "sent") < 0)) r = -3;
    return r;
}

int user_passwd(const char *name, const char *pass)
{
    char *h = make_hash(pass);
    if (!h) return -1;
    char path[1100], tmpp[1100];
    snprintf(path, sizeof path, "%s/users", cfg.dir);
    snprintf(tmpp, sizeof tmpp, "%s/users.tmp", cfg.dir);
    pthread_mutex_lock(&umu);
    int r = -1;
    FILE *in = fopen(path, "r"), *out = fopen(tmpp, "w");
    if (in && out) {
        char line[512];
        size_t nl = strlen(name);
        int found = 0;
        while (fgets(line, sizeof line, in)) {
            if (!strncmp(line, name, nl) && line[nl] == ':') {
                fprintf(out, "%s:%s\n", name, h);
                found = 1;
            } else fputs(line, out);
        }
        if (found && fflush(out) == 0 && fsync(fileno(out)) == 0) r = 0;
    }
    if (in) fclose(in);
    if (out) fclose(out);
    if (!r && rename(tmpp, path) < 0) r = -1;
    if (r) unlink(tmpp);
    pthread_mutex_unlock(&umu);
    free(h);
    return r;
}

int resolve_local(const char *local, char *out, size_t n)
{
    char l[128];
    snprintf(l, sizeof l, "%s", local);
    lower(l);
    l[strcspn(l, "+")] = 0;
    const char *alias[] = {"postmaster", "abuse", "hostmaster", "webmaster", "mailer-daemon", "admin", NULL};
    for (int i = 0; alias[i]; i++)
        if (!strcmp(l, alias[i]) && *cfg.admin && !user_exists(l)) {
            snprintf(l, sizeof l, "%s", cfg.admin);
            break;
        }
    if (!user_exists(l)) return 0;
    snprintf(out, n, "%s", l);
    return 1;
}

static long dir_size(const char *p)
{
    long total = 0;
    DIR *d = opendir(p);
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        struct stat st;
        if (fstatat(dirfd(d), e->d_name, &st, 0) == 0 && S_ISREG(st.st_mode)) total += st.st_size;
    }
    closedir(d);
    return total;
}

long user_usage(const char *name)
{
    long t = 0;
    const char *f[] = {"inbox/new", "inbox/cur", "sent/new", "sent/cur"};
    for (int i = 0; i < 4; i++) {
        char p[1300];
        snprintf(p, sizeof p, "%s/mail/%s/%s", cfg.dir, name, f[i]);
        t += dir_size(p);
    }
    return t;
}

int deliver(const char *user, const char *folder, const char *data, size_t n, int seen, char *id_out)
{
    static atomic_ulong ctr;
    if (mkdirs(user, folder) < 0) return -1;
    char id[96], r[9], tmp[1400], dst[1400];
    randhex(r, 4);
    snprintf(id, sizeof id, "%ld.%lu_%s.sekmail", (long)time(NULL), atomic_fetch_add(&ctr, 1), r);
    snprintf(tmp, sizeof tmp, "%s/mail/%s/%s/tmp/%s", cfg.dir, user, folder, id);
    if (seen) snprintf(dst, sizeof dst, "%s/mail/%s/%s/cur/%s:2,S", cfg.dir, user, folder, id);
    else snprintf(dst, sizeof dst, "%s/mail/%s/%s/new/%s", cfg.dir, user, folder, id);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    if (write_all(fd, data, n) < 0 || fsync(fd) < 0) {
        close(fd);
        unlink(tmp);
        return -1;
    }
    close(fd);
    if (rename(tmp, dst) < 0) {
        unlink(tmp);
        return -1;
    }
    if (id_out) strcpy(id_out, id);
    return 0;
}

static int cmp_msg(const void *a, const void *b)
{
    const struct msginfo *x = a, *y = b;
    if (x->t != y->t) return x->t < y->t ? 1 : -1;
    return strcmp(y->id, x->id);
}

static void head_fields(const char *path, struct msginfo *m)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    char b[16384];
    ssize_t k = read(fd, b, sizeof b);
    close(fd);
    if (k <= 0) return;
    struct hdrs h;
    hdrs_parse(b, b + k, &h);
    const char *fields[] = {"from", "to", "subject"};
    char *dst[] = {m->from, m->to, m->subject};
    for (int i = 0; i < 3; i++) {
        char *v = decode_words(hget(&h, fields[i]));
        snprintf(dst[i], sizeof m->from, "%s", v);
        utf8_cut(dst[i], sizeof m->from);
        free(v);
    }
    hfree(&h);
}

int list_folder(const char *user, const char *folder, struct msginfo **out, int headers)
{
    char p[1300], q[1400];
    *out = NULL;
    /* new/ -> cur/ the Maildir way */
    snprintf(p, sizeof p, "%s/mail/%s/%s/new", cfg.dir, user, folder);
    DIR *d = opendir(p);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.') continue;
            snprintf(q, sizeof q, "%s/%s", p, e->d_name);
            char dst[1400];
            snprintf(dst, sizeof dst, "%s/mail/%s/%s/cur/%s:2,", cfg.dir, user, folder, e->d_name);
            rename(q, dst);
        }
        closedir(d);
    }
    snprintf(p, sizeof p, "%s/mail/%s/%s/cur", cfg.dir, user, folder);
    d = opendir(p);
    if (!d) return 0;
    int n = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 64;
            *out = realloc(*out, cap * sizeof **out);
        }
        struct msginfo *m = &(*out)[n];
        memset(m, 0, sizeof *m);
        snprintf(m->id, sizeof m->id, "%.*s", (int)strcspn(e->d_name, ":"), e->d_name);
        if (!valid_id(m->id)) continue;
        const char *fl = strstr(e->d_name, ":2,");
        m->seen = fl && strchr(fl + 3, 'S');
        m->t = strtol(e->d_name, NULL, 10);
        snprintf(q, sizeof q, "%s/%s", p, e->d_name);
        struct stat st;
        if (stat(q, &st) == 0) m->size = st.st_size;
        if (headers) head_fields(q, m);
        n++;
    }
    closedir(d);
    qsort(*out, n, sizeof **out, cmp_msg);
    return n;
}

/* finds the file for id in cur/ (or new/) */
static int find_file(const char *user, const char *folder, const char *id, char *path, size_t n, char *name,
                     size_t nn)
{
    if (!valid_id(id) || !valid_name(user, 1)) return 0;
    if (strcmp(folder, "inbox") && strcmp(folder, "sent")) return 0;
    char p[1300];
    snprintf(path, n, "%s/mail/%s/%s/new/%s", cfg.dir, user, folder, id);
    if (access(path, F_OK) == 0) {
        if (name) snprintf(name, nn, "%s", id);
        return 1;
    }
    snprintf(p, sizeof p, "%s/mail/%s/%s/cur", cfg.dir, user, folder);
    DIR *d = opendir(p);
    if (!d) return 0;
    struct dirent *e;
    size_t il = strlen(id);
    int found = 0;
    while ((e = readdir(d))) {
        if (!strncmp(e->d_name, id, il) && (e->d_name[il] == ':' || !e->d_name[il])) {
            snprintf(path, n, "%s/%s", p, e->d_name);
            if (name) snprintf(name, nn, "%s", e->d_name);
            found = 1;
            break;
        }
    }
    closedir(d);
    return found;
}

int msg_path(const char *user, const char *folder, const char *id, char *path, size_t n)
{
    return find_file(user, folder, id, path, n, NULL, 0);
}

void msg_mark_seen(const char *user, const char *folder, const char *id)
{
    char path[1400], name[256];
    if (!find_file(user, folder, id, path, sizeof path, name, sizeof name)) return;
    const char *fl = strstr(name, ":2,");
    if (fl && strchr(fl + 3, 'S')) return;
    char flags[32] = "";
    if (fl) snprintf(flags, sizeof flags, "%s", fl + 3);
    /* Maildir wants flags in ASCII order */
    char nf[40];
    size_t k = 0;
    int put = 0;
    for (size_t i = 0; flags[i] && k < sizeof nf - 2; i++) {
        if (!put && flags[i] > 'S') { nf[k++] = 'S'; put = 1; }
        nf[k++] = flags[i];
    }
    if (!put) nf[k++] = 'S';
    nf[k] = 0;
    char dst[1400];
    snprintf(dst, sizeof dst, "%s/mail/%s/%s/cur/%s:2,%s", cfg.dir, user, folder, id, nf);
    rename(path, dst);
}

int msg_delete(const char *user, const char *folder, const char *id)
{
    char path[1400];
    if (!find_file(user, folder, id, path, sizeof path, NULL, 0)) return -1;
    return unlink(path);
}
