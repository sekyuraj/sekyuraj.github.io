/* capedump: writes the outside face of every tux-mc cape as <outdir>/<id>.png,
 * painted by the mod's own capes.c, so the website shows what the game draws. */
#include <cairo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tuxmc.h"

static uint32_t *load_logo(const char *dir, const char *id, int *w, int *h)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s.png", dir, id);
    cairo_surface_t *s = cairo_image_surface_create_from_png(path);
    if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "capedump: no logo %s\n", path);
        cairo_surface_destroy(s);
        return NULL;
    }
    cairo_surface_flush(s);
    *w = cairo_image_surface_get_width(s);
    *h = cairo_image_surface_get_height(s);
    int stride = cairo_image_surface_get_stride(s);
    unsigned char *data = cairo_image_surface_get_data(s);
    uint32_t *px = malloc(sizeof *px * *w * *h);
    for (int y = 0; y < *h; y++)
        for (int x = 0; x < *w; x++) {
            uint32_t p = ((uint32_t *)(data + y * stride))[x];
            uint32_t a = p >> 24;
            if (a && a < 255) {              /* cairo is premultiplied, capes.c wants straight alpha */
                uint32_t r = ((p >> 16) & 255) * 255 / a, g = ((p >> 8) & 255) * 255 / a, b = (p & 255) * 255 / a;
                p = a << 24 | r << 16 | g << 8 | b;
            }
            px[y * *w + x] = p;
        }
    cairo_surface_destroy(s);
    return px;
}

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "usage: capedump LOGO_DIR OUT_DIR SCALE\n");
        return 2;
    }
    int s = atoi(argv[3]);
    tuxmc_pal_t pal = {0xFFFFFFFF, 0xFF444444, 0xFF101010, 0xFFFFFFFF, 0xFFFFAA00};
    uint32_t *tex = malloc(sizeof *tex * 64 * s * 32 * s);
    int fw = 10 * s, fh = 16 * s;

    for (int i = 0; tuxmc_cape_ids[i]; i++) {
        const char *id = tuxmc_cape_ids[i];
        if (!strcmp(id, "image"))
            continue;
        tuxmc_img_t img = {0};
        uint32_t *logo = NULL;
        if (tuxmc_cape_logo(id)) {
            logo = load_logo(argv[1], id, &img.w, &img.h);
            if (!logo)
                continue;
            img.px = logo;
        }
        tuxmc_cape(id, s, &pal, logo ? &img : NULL, tex);

        cairo_surface_t *out = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, fw, fh);
        unsigned char *d = cairo_image_surface_get_data(out);
        int stride = cairo_image_surface_get_stride(out);
        for (int y = 0; y < fh; y++)
            for (int x = 0; x < fw; x++)
                ((uint32_t *)(d + y * stride))[x] = tex[(s + y) * 64 * s + s + x] | 0xFF000000;
        cairo_surface_mark_dirty(out);
        char path[512];
        snprintf(path, sizeof path, "%s/%s.png", argv[2], id);
        cairo_surface_write_to_png(out, path);
        cairo_surface_destroy(out);
        /* the edge colour, for the cape's sides on the page */
        printf("%s #%06x\n", id, tex[s * 64 * s] & 0xFFFFFF);
        free(logo);
    }
    free(tex);
    return 0;
}
