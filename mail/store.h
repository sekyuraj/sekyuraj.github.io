#pragma once
#include <stddef.h>
#include <time.h>

int valid_name(const char *s, int allow_reserved);
int user_exists(const char *name);
int user_check(const char *name, const char *pass);
int user_add(const char *name, const char *pass, int allow_reserved);  /* 0 ok, -1 taken, -2 bad, -3 io */
int user_passwd(const char *name, const char *pass);
/* local part of an address -> user name (handles +tags, postmaster...) */
int resolve_local(const char *local, char *out, size_t n);
long user_usage(const char *name);   /* bytes */

int deliver(const char *user, const char *folder, const char *data, size_t n, int seen, char *id_out);

struct msginfo {
    char id[96];
    time_t t;
    int seen;
    long size;
    char from[200], to[200], subject[200];
};
int list_folder(const char *user, const char *folder, struct msginfo **out, int headers);
int msg_path(const char *user, const char *folder, const char *id, char *path, size_t n);
void msg_mark_seen(const char *user, const char *folder, const char *id);
int msg_delete(const char *user, const char *folder, const char *id);
int valid_id(const char *id);
