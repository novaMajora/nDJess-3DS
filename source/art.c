#include "art.h"

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include "vendor/stb_image.h"

#define SLOTS 48
#define MAX_SOURCE_PIXELS (2500 * 2500) // refuse absurd images (memory)

typedef enum { SLOT_EMPTY, SLOT_QUEUED, SLOT_DECODING, SLOT_READY, SLOT_LOADED, SLOT_NONE } SlotState;

typedef struct {
    int album;
    SlotState state;
    u32 last_used;
    u8 *pixels;          // ART_SIZE^2 RGBA, filled by the thread
    bool tex_ready;
    C3D_Tex tex;
    Tex3DS_SubTexture sub;
    C2D_Image img;
} Slot;

struct ArtCache {
    const Library *lib;
    Slot slots[SLOTS];
    u32 frame;
    LightLock lock;
    LightEvent wake;
    Thread thread;
    volatile bool quit;
};

// ---------------------------------------------------------------------------------------------
// Decoding (background thread)

static unsigned char *read_file(const char *path, int *len) {
    *len = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > 16 * 1024 * 1024) { fclose(f); return NULL; }
    unsigned char *buf = malloc(n);
    if (buf && fread(buf, 1, n, f) != (size_t)n) { free(buf); buf = NULL; }
    fclose(f);
    if (buf) *len = (int)n;
    return buf;
}

// Center-crops to a square and box-filters down to ART_SIZE.
static void downscale(const u8 *src, int w, int h, int comp, u8 *dst) {
    int side = w < h ? w : h, ox = (w - side) / 2, oy = (h - side) / 2;
    for (int y = 0; y < ART_SIZE; y++) {
        int y0 = oy + y * side / ART_SIZE, y1 = oy + (y + 1) * side / ART_SIZE;
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < ART_SIZE; x++) {
            int x0 = ox + x * side / ART_SIZE, x1 = ox + (x + 1) * side / ART_SIZE;
            if (x1 <= x0) x1 = x0 + 1;
            // Sample at most 4x4 points per output pixel: fast and smooth enough.
            unsigned r = 0, g = 0, b = 0, n = 0;
            int sy = (y1 - y0 + 3) / 4, sx = (x1 - x0 + 3) / 4;
            if (sy < 1) sy = 1;
            if (sx < 1) sx = 1;
            for (int yy = y0; yy < y1; yy += sy)
                for (int xx = x0; xx < x1; xx += sx) {
                    const u8 *p = src + ((size_t)yy * w + xx) * comp;
                    r += p[0]; g += p[1]; b += p[2]; n++;
                }
            u8 *o = dst + (y * ART_SIZE + x) * 4;
            o[0] = r / n; o[1] = g / n; o[2] = b / n; o[3] = 255;
        }
    }
}

static bool decode(const unsigned char *data, int len, u8 *out) {
    int w, h, comp;
    if (!stbi_info_from_memory(data, len, &w, &h, &comp) || (long long)w * h > MAX_SOURCE_PIXELS) return false;
    u8 *px = stbi_load_from_memory(data, len, &w, &h, &comp, 3);
    if (!px) return false;
    downscale(px, w, h, 3, out);
    stbi_image_free(px);
    return true;
}

static bool load_album(const Library *lib, int album, u8 *out) {
    if (album < 0 || album >= lib->album_count) return false;
    const Group *g = &lib->albums[album];
    // 1. A picture embedded in one of the album's files.
    for (int i = 0; i < g->count && i < 4; i++) {
        const Track *t = &lib->tracks[g->tracks[i]];
        if (!t->has_art) continue;
        int len;
        unsigned char *pic = id3_picture(t->path, &len);
        bool ok = pic && decode(pic, len, out);
        free(pic);
        if (ok) return true;
    }
    // 2. cover/folder/front image next to the files.
    if (g->count) {
        const char *dir = lib->folders[lib->tracks[g->tracks[0]].folder].path;
        static const char *names[] = {"cover.jpg", "folder.jpg", "front.jpg", "cover.png", "folder.png", "album.jpg"};
        char path[1100];
        for (unsigned i = 0; i < sizeof names / sizeof *names; i++) {
            snprintf(path, sizeof path, "%s/%s", dir, names[i]);
            int len;
            unsigned char *data = read_file(path, &len);
            bool ok = data && decode(data, len, out);
            free(data);
            if (ok) return true;
        }
    }
    return false;
}

static void thread_main(void *arg) {
    ArtCache *c = arg;
    u8 *buf = malloc(ART_SIZE * ART_SIZE * 4);
    while (!c->quit) {
        LightEvent_Wait(&c->wake);
        for (;;) {
            if (c->quit) break;
            // Most recently requested first: what's on screen now.
            LightLock_Lock(&c->lock);
            int pick = -1;
            for (int i = 0; i < SLOTS; i++)
                if (c->slots[i].state == SLOT_QUEUED && (pick < 0 || c->slots[i].last_used > c->slots[pick].last_used)) pick = i;
            int album = pick >= 0 ? c->slots[pick].album : -1;
            if (pick >= 0) c->slots[pick].state = SLOT_DECODING;
            LightLock_Unlock(&c->lock);
            if (pick < 0) break;

            bool ok = buf && load_album(c->lib, album, buf);
            LightLock_Lock(&c->lock);
            Slot *s = &c->slots[pick];
            if (s->album == album && s->state == SLOT_DECODING) {
                if (ok && s->pixels) {
                    memcpy(s->pixels, buf, ART_SIZE * ART_SIZE * 4);
                    s->state = SLOT_READY;
                } else {
                    s->state = SLOT_NONE;
                }
            }
            LightLock_Unlock(&c->lock);
        }
    }
    free(buf);
}

// ---------------------------------------------------------------------------------------------
// Textures

static inline u32 morton(u32 x, u32 y) {
    return (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
}

// RGBA pixels (row-major, top row first) → a tiled GPU_RGBA8 texture of size tw x th.
static void upload(C3D_Tex *tex, const u8 *rgba, int w, int h, int tw) {
    u32 *dst = tex->data;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const u8 *p = rgba + (y * w + x) * 4;
            u32 i = ((y >> 3) * (tw >> 3) + (x >> 3)) * 64 + morton(x & 7, y & 7);
            dst[i] = (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3];
        }
    GSPGPU_FlushDataCache(tex->data, tex->size);
}

ArtCache *art_create(const Library *lib) {
    ArtCache *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->lib = lib;
    LightLock_Init(&c->lock);
    LightEvent_Init(&c->wake, RESET_ONESHOT);
    for (int i = 0; i < SLOTS; i++) {
        c->slots[i].album = -1;
        c->slots[i].pixels = malloc(ART_SIZE * ART_SIZE * 4);
    }
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    // Lower priority than the UI: art never stalls drawing or audio.
    c->thread = threadCreate(thread_main, c, 64 * 1024, prio + 1, -2, false);
    return c;
}

void art_destroy(ArtCache *c) {
    if (!c) return;
    c->quit = true;
    LightEvent_Signal(&c->wake);
    if (c->thread) {
        threadJoin(c->thread, U64_MAX);
        threadFree(c->thread);
    }
    for (int i = 0; i < SLOTS; i++) {
        if (c->slots[i].tex_ready) C3D_TexDelete(&c->slots[i].tex);
        free(c->slots[i].pixels);
    }
    free(c);
}

const C2D_Image *art_get(ArtCache *c, int album) {
    if (!c || album < 0) return NULL;
    LightLock_Lock(&c->lock);
    int lru = -1;
    for (int i = 0; i < SLOTS; i++) {
        Slot *s = &c->slots[i];
        if (s->album == album) {
            s->last_used = c->frame;
            const C2D_Image *img = s->state == SLOT_LOADED ? &s->img : NULL;
            LightLock_Unlock(&c->lock);
            return img;
        }
        bool evictable = s->state == SLOT_EMPTY || s->state == SLOT_LOADED || s->state == SLOT_NONE || s->state == SLOT_QUEUED;
        if (evictable && s->last_used != c->frame && (lru < 0 || s->last_used < c->slots[lru].last_used)) lru = i;
    }
    if (lru >= 0) {
        Slot *s = &c->slots[lru];
        s->album = album;
        s->state = SLOT_QUEUED;
        s->last_used = c->frame;
        LightEvent_Signal(&c->wake);
    }
    LightLock_Unlock(&c->lock);
    return NULL;
}

void art_update(ArtCache *c) {
    if (!c) return;
    c->frame++;
    int uploads = 0;
    LightLock_Lock(&c->lock);
    for (int i = 0; i < SLOTS && uploads < 2; i++) {
        Slot *s = &c->slots[i];
        if (s->state != SLOT_READY) continue;
        if (!s->tex_ready) {
            if (!C3D_TexInit(&s->tex, ART_SIZE, ART_SIZE, GPU_RGBA8)) { s->state = SLOT_NONE; continue; }
            C3D_TexSetFilter(&s->tex, GPU_LINEAR, GPU_LINEAR);
            s->tex_ready = true;
        }
        upload(&s->tex, s->pixels, ART_SIZE, ART_SIZE, ART_SIZE);
        s->sub = (Tex3DS_SubTexture){ART_SIZE, ART_SIZE, 0.0f, 1.0f, 1.0f, 0.0f};
        s->img = (C2D_Image){&s->tex, &s->sub};
        s->state = SLOT_LOADED;
        uploads++;
    }
    LightLock_Unlock(&c->lock);
}

bool image_load(const char *path, C2D_Image *out, C3D_Tex *tex, Tex3DS_SubTexture *sub, int *w, int *h) {
    int len, comp;
    unsigned char *data = read_file(path, &len);
    if (!data) return false;
    u8 *px = stbi_load_from_memory(data, len, w, h, &comp, 4);
    free(data);
    if (!px) return false;
    int tw = 8, th = 8;
    while (tw < *w) tw <<= 1;
    while (th < *h) th <<= 1;
    if (tw > 1024 || th > 1024 || !C3D_TexInit(tex, tw, th, GPU_RGBA8)) { stbi_image_free(px); return false; }
    memset(tex->data, 0, tex->size);
    C3D_TexSetFilter(tex, GPU_LINEAR, GPU_LINEAR);
    upload(tex, px, *w, *h, tw);
    stbi_image_free(px);
    *sub = (Tex3DS_SubTexture){(u16)*w, (u16)*h, 0.0f, 1.0f, (float)*w / tw, 1.0f - (float)*h / th};
    *out = (C2D_Image){tex, sub};
    return true;
}
