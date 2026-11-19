/* sekmaild: mail for sekraj.duckdns.org.
     sekmaild               run (SMTP in on :25, webmail on 127.0.0.1:3003)
     sekmaild adduser NAME  make an account (password on stdin), may use reserved names
     sekmaild passwd NAME   set a password (stdin) */
#include "queue.h"
#include "smtp.h"
#include "store.h"
#include "util.h"

#include <arpa/inet.h>
#include <curl/curl.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

static atomic_int nconn[2];
static const int maxconn[2] = {100, 200};

struct job {
    int fd, kind;
    char ip[64];
};

static void *conn_thread(void *arg)
{
    struct job *j = arg;
    struct timeval tv = {j->kind ? 60 : 300, 0};
    setsockopt(j->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(j->fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    if (j->kind == 0) handle_smtp(j->fd, j->ip);
    else handle_http(j->fd);
    close(j->fd);
    atomic_fetch_sub(&nconn[j->kind], 1);
    free(j);
    return NULL;
}

static int listen_tcp(const char *addr, int port)
{
    int v6 = strchr(addr, ':') != NULL;
    int fd = socket(v6 ? AF_INET6 : AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    int one = 1, zero = 0;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    int r;
    if (v6) {
        setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof zero);
        struct sockaddr_in6 sa = {.sin6_family = AF_INET6, .sin6_port = htons(port)};
        inet_pton(AF_INET6, addr, &sa.sin6_addr);
        r = bind(fd, (struct sockaddr *)&sa, sizeof sa);
    } else {
        struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons(port)};
        inet_pton(AF_INET, addr, &sa.sin_addr);
        r = bind(fd, (struct sockaddr *)&sa, sizeof sa);
    }
    if (r < 0 || listen(fd, 64) < 0) {
        logmsg("can't listen on [%s]:%d: %s", addr, port, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static int read_pass(char *out, size_t n)
{
    if (!fgets(out, n, stdin)) return -1;
    out[strcspn(out, "\r\n")] = 0;
    return 0;
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);
    umask(077);
    config_load();
    mkdir(cfg.dir, 0700);

    if (argc == 3 && (!strcmp(argv[1], "adduser") || !strcmp(argv[1], "passwd"))) {
        char p[256];
        if (read_pass(p, sizeof p) < 0 || strlen(p) < 8) {
            fprintf(stderr, "password on stdin, 8 or more characters\n");
            return 1;
        }
        int r = !strcmp(argv[1], "adduser") ? user_add(argv[2], p, 1) : user_passwd(argv[2], p);
        if (r == -1 && !strcmp(argv[1], "adduser")) fprintf(stderr, "%s already exists\n", argv[2]);
        else if (r == -2) fprintf(stderr, "bad name (a-z 0-9 . _ -, 2-24 chars)\n");
        else if (r) fprintf(stderr, "failed\n");
        return r ? 1 : 0;
    }
    if (argc > 1) {
        fprintf(stderr, "usage: sekmaild [adduser NAME | passwd NAME]\n");
        return 2;
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);
    int ls[2];
    ls[0] = listen_tcp(cfg.smtp_bind, cfg.smtp_port);
    if (ls[0] < 0 && strchr(cfg.smtp_bind, ':')) ls[0] = listen_tcp("0.0.0.0", cfg.smtp_port);
    ls[1] = listen_tcp("127.0.0.1", cfg.http_port);
    if (ls[0] < 0 || ls[1] < 0) return 1;
    queue_start();
    logmsg("sekmail for %s: smtp :%d, web 127.0.0.1:%d", cfg.domain, cfg.smtp_port, cfg.http_port);

    struct pollfd pf[2] = {{ls[0], POLLIN, 0}, {ls[1], POLLIN, 0}};
    for (;;) {
        if (poll(pf, 2, -1) < 0) {
            if (errno == EINTR) continue;
            return 1;
        }
        for (int k = 0; k < 2; k++) {
            if (!(pf[k].revents & POLLIN)) continue;
            struct sockaddr_storage ss;
            socklen_t sl = sizeof ss;
            int c = accept4(ls[k], (struct sockaddr *)&ss, &sl, SOCK_CLOEXEC);
            if (c < 0) continue;
            if (atomic_load(&nconn[k]) >= maxconn[k]) {
                if (k == 0) write_all(c, "421 busy, try later\r\n", 21);
                close(c);
                continue;
            }
            struct job *j = calloc(1, sizeof *j);
            j->fd = c;
            j->kind = k;
            if (ss.ss_family == AF_INET6) {
                inet_ntop(AF_INET6, &((struct sockaddr_in6 *)&ss)->sin6_addr, j->ip, sizeof j->ip);
                if (!strncmp(j->ip, "::ffff:", 7)) memmove(j->ip, j->ip + 7, strlen(j->ip + 7) + 1);
            } else inet_ntop(AF_INET, &((struct sockaddr_in *)&ss)->sin_addr, j->ip, sizeof j->ip);
            atomic_fetch_add(&nconn[k], 1);
            pthread_t t;
            pthread_attr_t a;
            pthread_attr_init(&a);
            pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
            pthread_attr_setstacksize(&a, 512 << 10);
            if (pthread_create(&t, &a, conn_thread, j)) {
                close(c);
                free(j);
                atomic_fetch_sub(&nconn[k], 1);
            }
            pthread_attr_destroy(&a);
        }
    }
}
