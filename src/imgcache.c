#include "imgcache.h"
#include "applog.h"
#include "twitch.h"
#include "player/curlio.h"
#include <stdio.h>                      /* (before jpeglib.h, which uses FILE) */
#include <jpeglib.h>
#include <png.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define IMG_SLOTS     72                /* pictures known at once */
#define IMG_TEX_BYTES (10u * 1024 * 1024)   /* texture memory kept (a 320x180 preview is 225 KB) */
#define IMG_MAX_FILE  (512 * 1024)
#define MAKE_PER_FRAME 3
#define WORKERS 3                       /* downloads at once (each HTTPS handshake takes a moment on the Vita) */
#define FREE_SLOTS_KEPT 12              /* img_frame frees old pictures so img_get always finds a slot */
#define MAX_TEX_SIDE 1024

enum { S_FREE, S_WANTED, S_LOADING, S_LOADED, S_READY, S_FAILED };

typedef struct {
    char url[220];
    int state;
    uint8_t *data;                      /* the downloaded file (S_LOADED) */
    size_t len;
    vita2d_texture *tex;
    size_t tex_bytes;
    unsigned used;                      /* frame it was last asked for */
    unsigned asked;                     /* frame it was first asked for (newest first) */
} Slot;

static Slot g_slot[IMG_SLOTS];
static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static unsigned g_frame = 1;
static size_t g_tex_total;

typedef struct { uint8_t *buf; size_t len, cap; } Mem;

static int mem_data(void *ctx, const uint8_t *d, size_t n)
{
    Mem *m = ctx;
    if (m->len + n > IMG_MAX_FILE) return 1;
    if (m->len + n > m->cap) {
        size_t nc = m->cap ? m->cap * 2 : 32768;
        while (nc < m->len + n) nc *= 2;
        uint8_t *nb = realloc(m->buf, nc);
        if (!nb) return 1;
        m->buf = nb;
        m->cap = nc;
    }
    memcpy(m->buf + m->len, d, n);
    m->len += n;
    return 0;
}

/* ---- JPEG: decoded here instead of with vita2d_load_JPEG_buffer, which refuses every JPEG that does not start
 * with a JFIF / Exif header (FF D8 FF E0 / E1). Twitch's previews and box art often start with FF D8 FF DB, so
 * none of them showed. Errors jump back here instead of ending the app (libjpeg's default is exit()). */
typedef struct { struct jpeg_error_mgr pub; jmp_buf jb; } JpegErr;

static void jpeg_fail(j_common_ptr ci)
{
    JpegErr *e = (JpegErr *)ci->err;
    longjmp(e->jb, 1);
}

static void jpeg_quiet(j_common_ptr ci, int level) { (void)ci; (void)level; }

static vita2d_texture *load_jpeg(const uint8_t *buf, size_t len)
{
    struct jpeg_decompress_struct ji;
    JpegErr je;
    vita2d_texture *volatile tex = NULL;
    memset(&ji, 0, sizeof ji);
    ji.err = jpeg_std_error(&je.pub);
    je.pub.error_exit = jpeg_fail;
    je.pub.emit_message = jpeg_quiet;
    if (setjmp(je.jb)) {                                     /* a broken picture */
        jpeg_destroy_decompress(&ji);
        if (tex) vita2d_free_texture(tex);
        return NULL;
    }
    jpeg_create_decompress(&ji);
    jpeg_mem_src(&ji, (unsigned char *)buf, (unsigned long)len);
    jpeg_read_header(&ji, TRUE);
    unsigned longer = ji.image_width > ji.image_height ? ji.image_width : ji.image_height;
    ji.scale_num = 1;
    ji.scale_denom = 1;
    while (longer / ji.scale_denom > MAX_TEX_SIDE && ji.scale_denom < 8) ji.scale_denom *= 2;
    if (ji.jpeg_color_space != JCS_GRAYSCALE) ji.out_color_space = JCS_RGB;
    jpeg_start_decompress(&ji);
    int gray = ji.out_color_space == JCS_GRAYSCALE || ji.output_components == 1;
    tex = vita2d_create_empty_texture_format(ji.output_width, ji.output_height,
                                             gray ? SCE_GXM_TEXTURE_FORMAT_U8_R111 : SCE_GXM_TEXTURE_FORMAT_U8U8U8_BGR);
    if (!tex) { jpeg_destroy_decompress(&ji); return NULL; }
    unsigned char *row = vita2d_texture_get_datap(tex);
    unsigned stride = vita2d_texture_get_stride(tex);
    while (ji.output_scanline < ji.output_height) {
        JSAMPROW r = row;
        jpeg_read_scanlines(&ji, &r, 1);
        row += stride;
    }
    jpeg_finish_decompress(&ji);
    jpeg_destroy_decompress(&ji);
    return tex;
}

/* ---- PNG: decoded here instead of with vita2d_load_PNG_buffer. That loader runs both
 * png_set_tRNS_to_alpha and png_set_filler on a palette-or-grayscale PNG that carries tRNS
 * transparency, so the 0xFF filler overwrites the real alpha and the picture comes out
 * opaque -- which is exactly what emotes (often palette PNGs with tRNS) are. This path
 * normalises every colour type to RGBA8 and adds a filler only when there is no alpha. */
typedef struct { const uint8_t *data; size_t len, off; } PngSrc;

static void png_read_mem(png_structp png, png_bytep out, png_size_t n)
{
    PngSrc *s = (PngSrc *)png_get_io_ptr(png);
    if (!s || s->off + n > s->len) { png_error(png, "short PNG"); return; }
    memcpy(out, s->data + s->off, n);
    s->off += n;
}

static vita2d_texture *load_png(const uint8_t *buf, size_t len)
{
    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png) return NULL;
    png_infop info = png_create_info_struct(png);
    if (!info) { png_destroy_read_struct(&png, NULL, NULL); return NULL; }
    vita2d_texture *volatile tex = NULL;
    png_bytep *volatile rows = NULL;
    if (setjmp(png_jmpbuf(png))) {                              /* a broken / unsupported PNG */
        if (tex) vita2d_free_texture(tex);
        free(rows);
        png_destroy_read_struct(&png, &info, NULL);
        return NULL;
    }
    PngSrc src = { buf, len, 0 };
    png_set_read_fn(png, &src, png_read_mem);
    png_read_info(png, info);
    png_uint_32 w = 0, h = 0;
    int depth = 0, color = 0;
    png_get_IHDR(png, info, &w, &h, &depth, &color, NULL, NULL, NULL);
    if (!w || !h || w > (png_uint_32)MAX_TEX_SIDE || h > (png_uint_32)MAX_TEX_SIDE) png_error(png, "bad PNG size");
    int has_alpha = (color & PNG_COLOR_MASK_ALPHA) || png_get_valid(png, info, PNG_INFO_tRNS);
    if (color == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
    if (color == PNG_COLOR_TYPE_GRAY && depth < 8) png_set_expand_gray_1_2_4_to_8(png);
    if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);   /* real alpha */
    if (depth == 16) png_set_strip_16(png);
    if (color == PNG_COLOR_TYPE_GRAY || color == PNG_COLOR_TYPE_GRAY_ALPHA) png_set_gray_to_rgb(png);
    if (!has_alpha) png_set_filler(png, 0xFF, PNG_FILLER_AFTER);   /* RGB -> opaque RGBA (only when no alpha) */
    png_read_update_info(png, info);
    tex = vita2d_create_empty_texture_format(w, h, SCE_GXM_TEXTURE_FORMAT_A8B8G8R8);
    if (!tex) png_error(png, "no texture");
    uint8_t *dst = vita2d_texture_get_datap(tex);
    unsigned stride = vita2d_texture_get_stride(tex);
    rows = (png_bytep *)malloc(sizeof(png_bytep) * h);
    if (!rows) png_error(png, "no rows");
    for (png_uint_32 y = 0; y < h; y++) rows[y] = dst + (size_t)y * stride;
    png_read_image(png, rows);
    free(rows);
    rows = NULL;
    png_destroy_read_struct(&png, &info, NULL);
    return tex;
}

static volatile int g_logged;                               /* (the first pictures are logged, to see what happens) */

static void *worker(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&g_mx);
        int k = -1;
        for (;;) {                                           /* the most recently asked picture first */
            for (int i = 0; i < IMG_SLOTS; i++)
                if (g_slot[i].state == S_WANTED && g_slot[i].used + 30 >= g_frame && (k < 0 || g_slot[i].asked > g_slot[k].asked)) k = i;
            if (k >= 0) break;
            pthread_cond_wait(&g_cv, &g_mx);
        }
        char url[220];
        snprintf(url, sizeof url, "%s", g_slot[k].url);
        g_slot[k].state = S_LOADING;
        pthread_mutex_unlock(&g_mx);

        Mem m = { NULL, 0, 0 };
        CioRequest rq;
        memset(&rq, 0, sizeof rq);
        rq.url = url;
        rq.connect_timeout_s = 8;
        rq.idle_timeout_s = 10;
        rq.data = mem_data;
        rq.ctx = &m;
        rq.ua = TW_USER_AGENT;
        int status = 0;
        char err[80];
        int r = cio_get(&rq, &status, err, sizeof err);
        int ok = r >= 0 && status == 200 && m.len > 8;
        if (g_logged < 12 || !ok) {
            if (g_logged < 40) plog("img: %s -> %d, HTTP %d, %u bytes%s%s", url, r, status, (unsigned)m.len, ok ? "" : ", ", ok ? "" : err);
            g_logged++;
        }

        pthread_mutex_lock(&g_mx);
        if (g_slot[k].state == S_LOADING && !strcmp(g_slot[k].url, url)) {   /* (not taken for another picture meanwhile) */
            if (ok) { g_slot[k].data = m.buf; g_slot[k].len = m.len; g_slot[k].state = S_LOADED; m.buf = NULL; }
            else g_slot[k].state = S_FAILED;
        }
        pthread_mutex_unlock(&g_mx);
        free(m.buf);
        if (!ok) usleep(50000);
    }
    return NULL;
}

void img_start(void)
{
    for (int i = 0; i < WORKERS; i++) {
        pthread_t th;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setstacksize(&at, 128 * 1024);
        pthread_create(&th, &at, worker, NULL);
    }
}

/* (locked, UI thread) the texture goes; the GPU must be done with it */
static void slot_drop(Slot *s)
{
    if (s->tex) { vita2d_free_texture(s->tex); g_tex_total -= s->tex_bytes; }
    free(s->data);
    memset(s, 0, sizeof *s);
}

void img_frame(void)
{
    g_frame++;
    pthread_mutex_lock(&g_mx);
    /* finished downloads become textures */
    int made = 0;
    for (int i = 0; i < IMG_SLOTS && made < MAKE_PER_FRAME; i++) {
        Slot *s = &g_slot[i];
        if (s->state != S_LOADED) continue;
        vita2d_texture *t = NULL;
        if (s->len > 8 && !memcmp(s->data, "\x89PNG", 4)) t = load_png(s->data, s->len);
        else if (s->len > 3 && s->data[0] == 0xFF && s->data[1] == 0xD8) t = load_jpeg(s->data, s->len);
        if (!t) plog("img: could not decode %s (%u bytes, starts %02X %02X %02X %02X)", s->url, (unsigned)s->len,
                     s->data[0], s->data[1], s->data[2], s->data[3]);
        free(s->data);
        s->data = NULL;
        s->len = 0;
        if (t) {
            s->tex = t;
            s->tex_bytes = (size_t)vita2d_texture_get_width(t) * vita2d_texture_get_height(t) * 4;
            g_tex_total += s->tex_bytes;
            s->state = S_READY;
        } else s->state = S_FAILED;
        made++;
    }
    /* over the memory limit, or few free slots left: free the least recently used pictures not drawn in the last
     * frames (only here, before the frame is drawn: never while the GPU may still be using them mid-frame) */
    int must_wait = 1;
    int nfree = 0;
    for (int i = 0; i < IMG_SLOTS; i++) if (g_slot[i].state == S_FREE) nfree++;
    while (nfree < FREE_SLOTS_KEPT) {
        int k = -1;
        for (int i = 0; i < IMG_SLOTS; i++) {
            Slot *s = &g_slot[i];
            if (s->state == S_FREE || s->state == S_LOADING || s->used + 2 >= g_frame) continue;
            if (k < 0 || s->used < g_slot[k].used) k = i;
        }
        if (k < 0) break;
        if (g_slot[k].tex && must_wait) { vita2d_wait_rendering_done(); must_wait = 0; }
        slot_drop(&g_slot[k]);
        nfree++;
    }
    while (g_tex_total > IMG_TEX_BYTES) {
        int k = -1;
        for (int i = 0; i < IMG_SLOTS; i++)
            if (g_slot[i].tex && g_slot[i].used + 2 < g_frame && (k < 0 || g_slot[i].used < g_slot[k].used)) k = i;
        if (k < 0) break;
        if (must_wait) { vita2d_wait_rendering_done(); must_wait = 0; }
        slot_drop(&g_slot[k]);
    }
    pthread_mutex_unlock(&g_mx);
}

vita2d_texture *img_get(const char *url)
{
    if (!url || !url[0]) return NULL;
    pthread_mutex_lock(&g_mx);
    int k = -1, free_k = -1;
    for (int i = 0; i < IMG_SLOTS; i++) {
        if (g_slot[i].state != S_FREE && !strcmp(g_slot[i].url, url)) { k = i; break; }
        if (free_k < 0 && g_slot[i].state == S_FREE) free_k = i;
    }
    if (k < 0) {
        if (free_k < 0) {                                    /* all taken: one without a texture, not in sight */
            for (int i = 0; i < IMG_SLOTS; i++) {
                Slot *s = &g_slot[i];
                if (s->tex || s->state == S_LOADING || s->used + 2 >= g_frame) continue;
                if (free_k < 0 || s->used < g_slot[free_k].used) free_k = i;
            }
            if (free_k >= 0) slot_drop(&g_slot[free_k]);     /* (no texture: nothing the GPU uses) */
        }
        if (free_k < 0) { pthread_mutex_unlock(&g_mx); return NULL; }   /* (img_frame frees some for next time) */
        k = free_k;
        snprintf(g_slot[k].url, sizeof g_slot[k].url, "%s", url);
        g_slot[k].state = S_WANTED;
        g_slot[k].asked = g_frame;
        pthread_cond_broadcast(&g_cv);
    }
    Slot *s = &g_slot[k];
    s->used = g_frame;
    if (s->state == S_FAILED && s->asked + 600 < g_frame) {  /* try a failed one again after ~10 s */
        s->state = S_WANTED;
        s->asked = g_frame;
        pthread_cond_broadcast(&g_cv);
    }
    vita2d_texture *t = s->state == S_READY ? s->tex : NULL;
    pthread_mutex_unlock(&g_mx);
    return t;
}

void img_shutdown(void)
{
    pthread_mutex_lock(&g_mx);
    vita2d_wait_rendering_done();
    for (int i = 0; i < IMG_SLOTS; i++) if (g_slot[i].state != S_LOADING) slot_drop(&g_slot[i]);
    pthread_mutex_unlock(&g_mx);
}
