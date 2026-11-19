#pragma once
#include <stddef.h>
#include "util.h"

struct req {
    char method[8];
    char path[1024];
    char *query;
    char tok[65];
    char ip[64];
    char origin[256];
    char ctype[256];
    char *body;
    size_t blen;
    char user[32];
    int fd;
};

int bump(const char *kind, const char *who, long window, int add);
void respond(int fd, int code, const char *ctype, const char *extra, const void *body, size_t n);
void send_page(struct req *r, int code, buf *b, const char *extra);
void redirect(struct req *r, const char *to, const char *extra);
void page_head(buf *b, struct req *r, const char *title);
char *field(struct req *r, const char *k);
char *qfield(struct req *r, const char *k);
void urlenc(buf *b, const char *s);
void request_link(buf *b, const char *user);   /* /mail/write?... to ask the admin for a site */

/* host.c */
void host_route(struct req *r, const char *p, int post);
void host_request(const char *user);
int host_pending(void);
