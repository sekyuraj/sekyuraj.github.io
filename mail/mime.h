#pragma once
#include <stddef.h>

struct hdr {
    char *name, *val;
};
struct hdrs {
    struct hdr *h;
    int n;
};

/* parses headers in [p, end), returns where the body starts */
const char *hdrs_parse(const char *p, const char *end, struct hdrs *out);
const char *hget(const struct hdrs *h, const char *name);
void hfree(struct hdrs *h);

char *hparam(const char *val, const char *name);       /* malloc'd or NULL */
char *decode_words(const char *s);                     /* RFC 2047 -> UTF-8 */
char *to_utf8(const char *s, size_t n, const char *charset);
char *html_to_text(const char *s, size_t n);
void addr_spec(const char *s, char *out, size_t n);   /* "Name <a@b>" -> a@b */

struct part {
    char type[96];
    char charset[48];
    char enc[32];
    char *filename;
    char name[64];   /* form-data field name */
    const char *body;
    size_t len;
    int attach;
};

int mime_parts(const char *msg, size_t n, struct part **out);
void mime_free(struct part *p, int n);
char *part_decode(const struct part *p, size_t *outlen);
