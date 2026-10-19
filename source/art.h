// Album art: decoded on a background thread, uploaded as GPU textures, cached LRU.
#pragma once

#include <citro2d.h>
#include <stdbool.h>

#include "library.h"

#define ART_SIZE 128 // textures are ART_SIZE x ART_SIZE

typedef struct ArtCache ArtCache;

ArtCache *art_create(const Library *lib);
void art_destroy(ArtCache *c);
// The image for an album, or NULL while it loads (it's queued) or when the album has no art.
const C2D_Image *art_get(ArtCache *c, int album);
// Main thread, once per frame: uploads finished decodes.
void art_update(ArtCache *c);

// Loads a PNG/JPEG from romfs (or anywhere) into a texture-backed image (power-of-two padded).
// `w`/`h` receive the picture size. Returns false on failure.
bool image_load(const char *path, C2D_Image *out, C3D_Tex *tex, Tex3DS_SubTexture *sub, int *w, int *h);
