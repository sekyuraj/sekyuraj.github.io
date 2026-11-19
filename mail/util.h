#pragma once
#include <stddef.h>
#include <time.h>

typedef struct {
    char *p;
    size_t n, cap;
} buf;

void bput(buf *b, const void *s, size_t n);
void bputs(buf *b, const char *s);
void bprintf(buf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void besc(buf *b, const char *s);
void bescn(buf *b, const char *s, size_t n);
void bfree(buf *b);

void randhex(char *out, int bytes);   /* out needs 2*bytes+1 */
char *form_get(const char *body, const char *key);   /* malloc'd, or NULL */
void url_decode(char *s);
int read_file(const char *path, buf *out, size_t max);
int write_all(int fd, const void *p, size_t n);
void lower(char *s);
void trim(char *s);
void utf8_cut(char *s, size_t max);   /* cut to < max bytes on a char boundary */
void logmsg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void b64enc(buf *b, const unsigned char *s, size_t n);

struct config {
    char dir[1024];
    char domain[256];
    char smtp_bind[64];
    int smtp_port, http_port;
    long max_size;          /* biggest message we take */
    long quota_mb;          /* per user, inbox + sent */
    int signups;            /* 0 = closed */
    int signups_per_ip;     /* per day */
    int sends_per_day;
    char admin[32];         /* gets postmaster@ and abuse@ */
    char relay_url[512], relay_user[256], relay_pass[256], relay_from[256];
};
extern struct config cfg;
void config_load(void);
