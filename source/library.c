#include "library.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

// ---------------------------------------------------------------------------------------------
// Small string helpers

static char *dupn(const char *s, size_t n) {
    char *d = malloc(n + 1);
    if (!d) return NULL;
    memcpy(d, s, n);
    d[n] = 0;
    return d;
}

static char *dup(const char *s) { return dupn(s, strlen(s)); }

static void trim(char *s) {
    if (!s) return;
    size_t n = strlen(s);
    while (n && (unsigned char)s[n - 1] <= ' ') s[--n] = 0;
    size_t i = 0;
    while (s[i] && (unsigned char)s[i] <= ' ') i++;
    if (i) memmove(s, s + i, n - i + 1);
}

static bool has_ext(const char *name, const char *ext) {
    size_t n = strlen(name), e = strlen(ext);
    return n > e && strcasecmp(name + n - e, ext) == 0;
}

// Appends one Unicode code point as UTF-8.
static size_t put_utf8(char *out, size_t cap, size_t at, unsigned cp) {
    char b[4];
    size_t n;
    if (cp < 0x80) { b[0] = cp; n = 1; }
    else if (cp < 0x800) { b[0] = 0xC0 | (cp >> 6); b[1] = 0x80 | (cp & 0x3F); n = 2; }
    else if (cp < 0x10000) { b[0] = 0xE0 | (cp >> 12); b[1] = 0x80 | ((cp >> 6) & 0x3F); b[2] = 0x80 | (cp & 0x3F); n = 3; }
    else { b[0] = 0xF0 | (cp >> 18); b[1] = 0x80 | ((cp >> 12) & 0x3F); b[2] = 0x80 | ((cp >> 6) & 0x3F); b[3] = 0x80 | (cp & 0x3F); n = 4; }
    if (at + n >= cap) return at;
    memcpy(out + at, b, n);
    return at + n;
}

// Decodes an ID3v2 text frame body (encoding byte + text) to a UTF-8 string.
static char *id3_text(const unsigned char *p, size_t len) {
    if (len < 1) return NULL;
    int enc = p[0];
    p++; len--;
    size_t cap = len * 3 + 4;
    char *out = malloc(cap);
    if (!out) return NULL;
    size_t o = 0;
    if (enc == 0) { // ISO-8859-1
        for (size_t i = 0; i < len && p[i]; i++) o = put_utf8(out, cap, o, p[i]);
    } else if (enc == 3) { // UTF-8
        for (size_t i = 0; i < len && p[i]; i++) out[o++] = p[i];
    } else { // UTF-16 with BOM (1) or big-endian (2)
        bool be = enc == 2;
        size_t i = 0;
        if (enc == 1 && len >= 2) {
            if (p[0] == 0xFE && p[1] == 0xFF) { be = true; i = 2; }
            else if (p[0] == 0xFF && p[1] == 0xFE) { be = false; i = 2; }
        }
        for (; i + 1 < len; i += 2) {
            unsigned u = be ? (p[i] << 8 | p[i + 1]) : (p[i] | p[i + 1] << 8);
            if (!u) break;
            if (u >= 0xD800 && u < 0xDC00 && i + 3 < len) {
                unsigned lo = be ? (p[i + 2] << 8 | p[i + 3]) : (p[i + 2] | p[i + 3] << 8);
                if (lo >= 0xDC00 && lo < 0xE000) {
                    u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                    i += 2;
                }
            }
            o = put_utf8(out, cap, o, u);
        }
    }
    out[o] = 0;
    trim(out);
    if (!out[0]) { free(out); return NULL; }
    return out;
}

static unsigned syncsafe(const unsigned char *b) { return (b[0] & 0x7F) << 21 | (b[1] & 0x7F) << 14 | (b[2] & 0x7F) << 7 | (b[3] & 0x7F); }

// The 80 standard ID3v1 genres plus the most common Winamp extensions.
static const char *GENRES[] = {
    "Blues", "Classic Rock", "Country", "Dance", "Disco", "Funk", "Grunge", "Hip-Hop", "Jazz", "Metal",
    "New Age", "Oldies", "Other", "Pop", "R&B", "Rap", "Reggae", "Rock", "Techno", "Industrial",
    "Alternative", "Ska", "Death Metal", "Pranks", "Soundtrack", "Euro-Techno", "Ambient", "Trip-Hop", "Vocal", "Jazz+Funk",
    "Fusion", "Trance", "Classical", "Instrumental", "Acid", "House", "Game", "Sound Clip", "Gospel", "Noise",
    "Alternative Rock", "Bass", "Soul", "Punk", "Space", "Meditative", "Instrumental Pop", "Instrumental Rock", "Ethnic", "Gothic",
    "Darkwave", "Techno-Industrial", "Electronic", "Pop-Folk", "Eurodance", "Dream", "Southern Rock", "Comedy", "Cult", "Gangsta",
    "Top 40", "Christian Rap", "Pop/Funk", "Jungle", "Native American", "Cabaret", "New Wave", "Psychedelic", "Rave", "Showtunes",
    "Trailer", "Lo-Fi", "Tribal", "Acid Punk", "Acid Jazz", "Polka", "Retro", "Musical", "Rock & Roll", "Hard Rock",
    "Folk", "Folk-Rock", "National Folk", "Swing", "Fast Fusion", "Bebop", "Latin", "Revival", "Celtic", "Bluegrass",
    "Avantgarde", "Gothic Rock", "Progressive Rock", "Psychedelic Rock", "Symphonic Rock", "Slow Rock", "Big Band", "Chorus", "Easy Listening", "Acoustic",
    "Humour", "Speech", "Chanson", "Opera", "Chamber Music", "Sonata", "Symphony", "Booty Bass", "Primus", "Porn Groove",
    "Satire", "Slow Jam", "Club", "Tango", "Samba", "Folklore", "Ballad", "Power Ballad", "Rhythmic Soul", "Freestyle",
    "Duet", "Punk Rock", "Drum Solo", "A Cappella", "Euro-House", "Dance Hall",
};
#define GENRE_COUNT ((int)(sizeof GENRES / sizeof *GENRES))

// "(17)", "17", "(17)Rock" or "Rock" → a genre name (malloc'd).
static char *genre_name(char *raw) {
    if (!raw) return NULL;
    const char *p = raw;
    if (*p == '(' && isdigit((unsigned char)p[1])) {
        int n = atoi(p + 1);
        const char *close = strchr(p, ')');
        if (close && close[1]) { char *d = dup(close + 1); free(raw); return d; }
        if (n >= 0 && n < GENRE_COUNT) { free(raw); return dup(GENRES[n]); }
    } else if (isdigit((unsigned char)*p)) {
        char *end;
        long n = strtol(p, &end, 10);
        if (!*end && n >= 0 && n < GENRE_COUNT) { free(raw); return dup(GENRES[n]); }
    }
    return raw;
}

// Walks the frames of an ID3v2 tag. `visit` sees every frame and returns true to stop; `body` is
// read only for frames `want` accepts (NULL otherwise, so large pictures are skipped cheaply).
typedef bool (*FrameVisitor)(const char *id, const unsigned char *body, unsigned len, void *user);

static void id3_walk(FILE *f, bool (*want)(const char *id), FrameVisitor visit, void *user) {
    unsigned char h[10];
    if (fseek(f, 0, SEEK_SET) || fread(h, 1, 10, f) != 10 || memcmp(h, "ID3", 3) || h[3] < 2 || h[3] > 4) return;
    int ver = h[3];
    long end = 10 + (long)syncsafe(h + 6);
    if (h[5] & 0x40 && ver >= 3) { // extended header
        unsigned char e[4];
        if (fread(e, 1, 4, f) != 4) return;
        long ext = ver == 4 ? (long)syncsafe(e) : (long)(e[0] << 24 | e[1] << 16 | e[2] << 8 | e[3]) + 4;
        fseek(f, 10 + ext, SEEK_SET);
    }
    int hdr = ver == 2 ? 6 : 10;
    for (int frames = 0; frames < 512; frames++) {
        long at = ftell(f);
        unsigned char fr[10];
        if (at + hdr > end || fread(fr, 1, hdr, f) != (size_t)hdr || !fr[0]) return;
        char id[5] = {0};
        unsigned len;
        if (ver == 2) {
            memcpy(id, fr, 3);
            len = fr[3] << 16 | fr[4] << 8 | fr[5];
        } else {
            memcpy(id, fr, 4);
            len = ver == 4 ? syncsafe(fr + 4) : (unsigned)(fr[4] << 24 | fr[5] << 16 | fr[6] << 8 | fr[7]);
        }
        if (len == 0 || at + hdr + (long)len > end) return;
        if (want(id) && len <= 16 * 1024 * 1024) {
            unsigned char *body = malloc(len);
            if (!body) return;
            if (fread(body, 1, len, f) != len) { free(body); return; }
            bool stop = visit(id, body, len, user);
            free(body);
            if (stop) return;
        } else {
            if (visit(id, NULL, len, user)) return;
            if (fseek(f, at + hdr + len, SEEK_SET)) return;
        }
    }
}

typedef struct { char **title, **artist, **album, **genre; int *track_no; bool *has_art; } TextTags;

static bool is_pic(const char *id) { return !strcmp(id, "APIC") || !strcmp(id, "PIC"); }

static bool text_visitor(const char *id, const unsigned char *body, unsigned len, void *user) {
    TextTags *t = user;
    char **dst = NULL;
    if (is_pic(id)) { *t->has_art = true; return false; }
    if (!body) return false;
    if (!strcmp(id, "TIT2") || !strcmp(id, "TT2")) dst = t->title;
    else if (!strcmp(id, "TPE1") || !strcmp(id, "TP1")) dst = t->artist;
    else if (!strcmp(id, "TALB") || !strcmp(id, "TAL")) dst = t->album;
    else if (!strcmp(id, "TCON") || !strcmp(id, "TCO")) dst = t->genre;
    if (dst && !*dst) {
        *dst = id3_text(body, len);
        if (dst == t->genre) *dst = genre_name(*dst);
    } else if ((!strcmp(id, "TRCK") || !strcmp(id, "TRK")) && !*t->track_no) {
        char *s = id3_text(body, len);
        if (s) { *t->track_no = atoi(s); free(s); }
    }
    return false;
}

// Pictures can be megabytes; only text frame bodies are read.
static bool want_text_only(const char *id) { return id[0] == 'T'; }

void id3_read(const char *path, char **title, char **artist, char **album, char **genre, int *track_no, bool *has_art) {
    *title = *artist = *album = *genre = NULL;
    *track_no = 0;
    *has_art = false;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    TextTags t = {title, artist, album, genre, track_no, has_art};
    id3_walk(f, want_text_only, text_visitor, &t);
    // ID3v1 fallback for whatever is still missing.
    if (!*title || !*artist || !*album || !*genre) {
        unsigned char v1[128];
        if (fseek(f, -128, SEEK_END) == 0 && fread(v1, 1, 128, f) == 128 && memcmp(v1, "TAG", 3) == 0) {
            char **dsts[3] = {title, artist, album};
            for (int k = 0; k < 3; k++) {
                if (*dsts[k]) continue;
                char tmp[31];
                memcpy(tmp, v1 + 3 + k * 30, 30);
                tmp[30] = 0;
                char *u = malloc(91);
                if (!u) continue;
                size_t o = 0;
                for (int i = 0; tmp[i]; i++) o = put_utf8(u, 91, o, (unsigned char)tmp[i]);
                u[o] = 0;
                trim(u);
                if (u[0]) *dsts[k] = u; else free(u);
            }
            if (!*track_no && v1[125] == 0 && v1[126]) *track_no = v1[126];
            if (!*genre && v1[127] < GENRE_COUNT) *genre = dup(GENRES[v1[127]]);
        }
    }
    fclose(f);
}

typedef struct { unsigned char *data; int len; } Picture;

static bool pic_visitor(const char *id, const unsigned char *body, unsigned len, void *user) {
    Picture *p = user;
    if (!body) return false;
    unsigned i;
    if (!strcmp(id, "PIC")) {
        i = 1 + 3 + 1; // encoding, 3-char format, picture type
    } else {
        i = 1;
        while (i < len && body[i]) i++; // MIME type
        i += 1 + 1;                      // NUL, picture type
    }
    int enc = body[0];
    // Description (encoding-dependent terminator)
    if (enc == 1 || enc == 2) {
        while (i + 1 < len && (body[i] || body[i + 1])) i += 2;
        i += 2;
    } else {
        while (i < len && body[i]) i++;
        i += 1;
    }
    if (i >= len) return false;
    p->len = (int)(len - i);
    p->data = malloc(p->len);
    if (p->data) memcpy(p->data, body + i, p->len);
    return true;
}

unsigned char *id3_picture(const char *path, int *len) {
    *len = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    Picture p = {0};
    id3_walk(f, is_pic, pic_visitor, &p);
    fclose(f);
    *len = p.len;
    return p.data;
}

// ---------------------------------------------------------------------------------------------
// Scanning

typedef struct {
    Library *lib;
    int cap_tracks, cap_folders;
    ScanProgress progress;
    void *user;
    char **m3u;   // playlist files found during the scan
    int m3u_count, m3u_cap;
} Scan;

static int add_folder(Scan *s, const char *path, int parent) {
    Library *lib = s->lib;
    if (lib->folder_count == s->cap_folders) {
        int cap = s->cap_folders ? s->cap_folders * 2 : 64;
        Folder *n = realloc(lib->folders, cap * sizeof *n);
        if (!n) return -1;
        lib->folders = n;
        s->cap_folders = cap;
    }
    Folder *f = &lib->folders[lib->folder_count];
    f->path = dup(path);
    const char *slash = strrchr(path, '/');
    f->name = dup(slash ? slash + 1 : path);
    f->parent = parent;
    return lib->folder_count++;
}

static void add_track(Scan *s, const char *path, const char *name, int folder) {
    Library *lib = s->lib;
    if (lib->count >= LIB_MAX_TRACKS) { lib->truncated = true; return; }
    if (lib->count == s->cap_tracks) {
        int cap = s->cap_tracks ? s->cap_tracks * 2 : 256;
        Track *n = realloc(lib->tracks, cap * sizeof *n);
        if (!n) return;
        lib->tracks = n;
        s->cap_tracks = cap;
    }
    Track *t = &lib->tracks[lib->count];
    memset(t, 0, sizeof *t);
    t->path = dup(path);
    t->folder = folder;
    id3_read(path, &t->title, &t->artist, &t->album, &t->genre, &t->track_no, &t->has_art);
    if (!t->title) t->title = dupn(name, strlen(name) - 4); // file name without ".mp3"
    if (!t->artist) t->artist = dup("Unknown Artist");
    if (!t->album) t->album = dup("Unknown Album");
    if (!t->genre) t->genre = dup("Unknown Genre");
    struct stat st;
    if (stat(path, &st) == 0) t->mtime = st.st_mtime;
    lib->count++;
    if (s->progress) s->progress(lib->count, t->title, s->user);
}

static void scan_dir(Scan *s, const char *path, int folder, int depth) {
    if (depth > LIB_MAX_DEPTH) return;
    DIR *d = opendir(path);
    if (!d) return;
    struct dirent *e;
    char full[1024];
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        int n = snprintf(full, sizeof full, "%s/%s", path, e->d_name);
        if (n <= 0 || n >= (int)sizeof full) continue;
        bool is_dir = e->d_type == DT_DIR;
        if (e->d_type == DT_UNKNOWN) {
            struct stat st;
            is_dir = stat(full, &st) == 0 && S_ISDIR(st.st_mode);
        }
        if (is_dir) {
            int sub = add_folder(s, full, folder);
            if (sub >= 0) scan_dir(s, full, sub, depth + 1);
        } else if (has_ext(e->d_name, ".mp3")) {
            add_track(s, full, e->d_name, folder);
        } else if (has_ext(e->d_name, ".m3u") || has_ext(e->d_name, ".m3u8")) {
            if (s->m3u_count == s->m3u_cap) {
                s->m3u_cap = s->m3u_cap ? s->m3u_cap * 2 : 16;
                char **nm = realloc(s->m3u, s->m3u_cap * sizeof *nm);
                if (!nm) continue;
                s->m3u = nm;
            }
            s->m3u[s->m3u_count++] = dup(full);
        }
    }
    closedir(d);
}

// ---------------------------------------------------------------------------------------------
// Views

static const Library *sort_lib;

static int cmp_title(const void *a, const void *b) {
    const Track *x = &sort_lib->tracks[*(const int *)a], *y = &sort_lib->tracks[*(const int *)b];
    return strcasecmp(x->title, y->title);
}

static int cmp_album_order(const void *a, const void *b) {
    const Track *x = &sort_lib->tracks[*(const int *)a], *y = &sort_lib->tracks[*(const int *)b];
    int c = strcasecmp(x->album, y->album);
    if (c) return c;
    if (x->track_no != y->track_no) return x->track_no - y->track_no;
    return strcasecmp(x->path, y->path);
}

static int cmp_path(const void *a, const void *b) {
    return strcasecmp(sort_lib->tracks[*(const int *)a].path, sort_lib->tracks[*(const int *)b].path);
}

static int cmp_recent(const void *a, const void *b) {
    time_t x = sort_lib->tracks[*(const int *)a].mtime, y = sort_lib->tracks[*(const int *)b].mtime;
    return x < y ? 1 : x > y ? -1 : 0;
}

static int cmp_group(const void *a, const void *b) { return strcasecmp(((const Group *)a)->name, ((const Group *)b)->name); }

// Groups tracks by a string key (artist, album or genre).
static int build_groups(const Library *lib, const int *order, char *(*key)(const Track *), Group **out) {
    Group *g = NULL;
    int count = 0, cap = 0;
    for (int i = 0; i < lib->count; i++) {
        int t = order[i];
        const char *k = key(&lib->tracks[t]);
        int found = -1;
        for (int j = count - 1; j >= 0 && j >= count - 64; j--) // recent groups first (tracks are clustered)
            if (!strcasecmp(g[j].name, k)) { found = j; break; }
        if (found < 0)
            for (int j = 0; j < count; j++)
                if (!strcasecmp(g[j].name, k)) { found = j; break; }
        if (found < 0) {
            if (count == cap) {
                cap = cap ? cap * 2 : 64;
                Group *n = realloc(g, cap * sizeof *n);
                if (!n) break;
                g = n;
            }
            g[count] = (Group){.name = (char *)k, .tracks = NULL, .count = 0};
            found = count++;
        }
        Group *gr = &g[found];
        int *n = realloc(gr->tracks, (gr->count + 1) * sizeof(int));
        if (!n) continue;
        gr->tracks = n;
        gr->tracks[gr->count++] = t;
    }
    qsort(g, count, sizeof *g, cmp_group);
    *out = g;
    return count;
}

static char *key_artist(const Track *t) { return t->artist; }
static char *key_album(const Track *t) { return t->album; }
static char *key_genre(const Track *t) { return t->genre; }

int library_find_path(const Library *lib, const char *path) {
    if (!lib->by_path) return -1;
    int lo = 0, hi = lib->count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int c = strcasecmp(lib->tracks[lib->by_path[mid]].path, path);
        if (!c) return lib->by_path[mid];
        if (c < 0) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}

// Resolves an .m3u entry (absolute "sdmc:/..", "/..", or relative to the playlist) to a path.
static void resolve(const char *m3u, const char *entry, char *out, size_t cap) {
    char tmp[2304];
    if (!strncasecmp(entry, "sdmc:/", 6)) snprintf(tmp, sizeof tmp, "%s", entry);
    else if (entry[0] == '/' || entry[0] == '\\') snprintf(tmp, sizeof tmp, "sdmc:%s", entry);
    else {
        const char *slash = strrchr(m3u, '/');
        int dir = slash ? (int)(slash - m3u) : 0;
        snprintf(tmp, sizeof tmp, "%.*s/%s", dir, m3u, entry);
    }
    for (char *p = tmp; *p; p++) if (*p == '\\') *p = '/';
    // Collapse "/./" and "/x/../"
    char *parts[64];
    int n = 0;
    char *save = NULL, *prefix = "";
    char *rest = tmp;
    if (!strncasecmp(tmp, "sdmc:", 5)) { prefix = "sdmc:"; rest = tmp + 5; }
    for (char *tok = strtok_r(rest, "/", &save); tok && n < 64; tok = strtok_r(NULL, "/", &save)) {
        if (!strcmp(tok, ".")) continue;
        if (!strcmp(tok, "..")) { if (n) n--; continue; }
        parts[n++] = tok;
    }
    size_t o = snprintf(out, cap, "%s", prefix);
    for (int i = 0; i < n && o < cap; i++) o += snprintf(out + o, cap - o, "/%s", parts[i]);
}

static void load_playlist(Library *lib, Playlist *pl) {
    pl->tracks = NULL;
    pl->count = 0;
    FILE *f = fopen(pl->path, "r");
    if (!f) return;
    char line[1024], path[1024];
    int cap = 0;
    while (fgets(line, sizeof line, f)) {
        trim(line);
        // Skip a UTF-8 BOM and comments
        char *l = line;
        if ((unsigned char)l[0] == 0xEF && (unsigned char)l[1] == 0xBB && (unsigned char)l[2] == 0xBF) l += 3;
        if (!*l || *l == '#') continue;
        resolve(pl->path, l, path, sizeof path);
        int t = library_find_path(lib, path);
        if (t < 0) continue;
        if (pl->count == cap) {
            cap = cap ? cap * 2 : 32;
            int *n = realloc(pl->tracks, cap * sizeof(int));
            if (!n) break;
            pl->tracks = n;
        }
        pl->tracks[pl->count++] = t;
    }
    fclose(f);
}

static void add_playlist(Library *lib, const char *name, const char *path, PlaylistKind kind) {
    Playlist *n = realloc(lib->playlists, (lib->playlist_count + 1) * sizeof *n);
    if (!n) return;
    lib->playlists = n;
    Playlist *pl = &lib->playlists[lib->playlist_count++];
    pl->name = dup(name);
    pl->path = path ? dup(path) : NULL;
    pl->kind = kind;
    pl->tracks = NULL;
    pl->count = 0;
    if (kind != PLAYLIST_RECENT) load_playlist(lib, pl);
}

static int cmp_str(const void *a, const void *b) {
    const char *x = strrchr(*(char *const *)a, '/'), *y = strrchr(*(char *const *)b, '/');
    return strcasecmp(x ? x : "", y ? y : "");
}

bool library_scan(Library *lib, const char *root, const char *favorites_path, ScanProgress progress, void *user) {
    memset(lib, 0, sizeof *lib);
    struct stat st;
    if (stat(root, &st) != 0 || !S_ISDIR(st.st_mode)) return false;
    Scan s = {.lib = lib, .progress = progress, .user = user};
    int r = add_folder(&s, root, -1);
    if (r < 0) return false;
    scan_dir(&s, root, r, 0);

    int n = lib->count ? lib->count : 1;
    sort_lib = lib;
    lib->by_title = malloc(n * sizeof(int));
    lib->by_path = malloc(n * sizeof(int));
    int *album_order = malloc(n * sizeof(int));
    int *recent = malloc(n * sizeof(int));
    if (lib->by_title && lib->by_path && album_order && recent) {
        for (int i = 0; i < lib->count; i++) lib->by_title[i] = lib->by_path[i] = album_order[i] = recent[i] = i;
        qsort(lib->by_title, lib->count, sizeof(int), cmp_title);
        qsort(lib->by_path, lib->count, sizeof(int), cmp_path);
        qsort(album_order, lib->count, sizeof(int), cmp_album_order);
        qsort(recent, lib->count, sizeof(int), cmp_recent);
        lib->artist_count = build_groups(lib, album_order, key_artist, &lib->artists);
        lib->album_count = build_groups(lib, album_order, key_album, &lib->albums);
        lib->genre_count = build_groups(lib, album_order, key_genre, &lib->genres);
        for (int a = 0; a < lib->album_count; a++)
            for (int i = 0; i < lib->albums[a].count; i++) lib->tracks[lib->albums[a].tracks[i]].album_id = a;
    }
    free(album_order);

    // Playlists: Favorites, Recently Added, then the .m3u files found in the music folder.
    add_playlist(lib, "Favorites", favorites_path, PLAYLIST_FAVORITES);
    add_playlist(lib, "Recently Added", NULL, PLAYLIST_RECENT);
    if (recent && lib->playlist_count >= 2) {
        Playlist *pl = &lib->playlists[1];
        pl->count = lib->count < RECENT_COUNT ? lib->count : RECENT_COUNT;
        pl->tracks = recent;
        recent = NULL;
    }
    free(recent);
    qsort(s.m3u, s.m3u_count, sizeof(char *), cmp_str);
    for (int i = 0; i < s.m3u_count; i++) {
        char name[256];
        const char *slash = strrchr(s.m3u[i], '/');
        snprintf(name, sizeof name, "%s", slash ? slash + 1 : s.m3u[i]);
        char *dot = strrchr(name, '.');
        if (dot) *dot = 0;
        add_playlist(lib, name, s.m3u[i], PLAYLIST_FILE);
        free(s.m3u[i]);
    }
    free(s.m3u);
    return true;
}

bool library_is_favorite(const Library *lib, int track) {
    if (!lib->playlist_count) return false;
    const Playlist *f = &lib->playlists[0];
    for (int i = 0; i < f->count; i++) if (f->tracks[i] == track) return true;
    return false;
}

bool library_toggle_favorite(Library *lib, int track, const char *favorites_path) {
    if (!lib->playlist_count || track < 0 || track >= lib->count) return false;
    Playlist *f = &lib->playlists[0];
    bool now;
    int at = -1;
    for (int i = 0; i < f->count; i++) if (f->tracks[i] == track) at = i;
    if (at >= 0) {
        memmove(f->tracks + at, f->tracks + at + 1, (f->count - at - 1) * sizeof(int));
        f->count--;
        now = false;
    } else {
        int *n = realloc(f->tracks, (f->count + 1) * sizeof(int));
        if (!n) return false;
        f->tracks = n;
        f->tracks[f->count++] = track;
        now = true;
    }
    FILE *out = fopen(favorites_path, "w");
    if (out) {
        fputs("#EXTM3U\n", out);
        for (int i = 0; i < f->count; i++) fprintf(out, "%s\n", lib->tracks[f->tracks[i]].path);
        fclose(out);
    }
    return now;
}

void library_free(Library *lib) {
    for (int i = 0; i < lib->count; i++) {
        free(lib->tracks[i].path);
        free(lib->tracks[i].title);
        free(lib->tracks[i].artist);
        free(lib->tracks[i].album);
        free(lib->tracks[i].genre);
    }
    for (int i = 0; i < lib->folder_count; i++) {
        free(lib->folders[i].path);
        free(lib->folders[i].name);
    }
    for (int i = 0; i < lib->artist_count; i++) free(lib->artists[i].tracks);
    for (int i = 0; i < lib->album_count; i++) free(lib->albums[i].tracks);
    for (int i = 0; i < lib->genre_count; i++) free(lib->genres[i].tracks);
    for (int i = 0; i < lib->playlist_count; i++) {
        free(lib->playlists[i].name);
        free(lib->playlists[i].path);
        free(lib->playlists[i].tracks);
    }
    free(lib->tracks);
    free(lib->folders);
    free(lib->by_title);
    free(lib->by_path);
    free(lib->artists);
    free(lib->albums);
    free(lib->genres);
    free(lib->playlists);
    memset(lib, 0, sizeof *lib);
}

int library_artist_albums(const Library *lib, int artist, int **out) {
    *out = NULL;
    if (artist < 0 || artist >= lib->artist_count) return 0;
    const Group *a = &lib->artists[artist];
    int *res = malloc((a->count ? a->count : 1) * sizeof(int));
    if (!res) return 0;
    int n = 0;
    for (int i = 0; i < a->count; i++) {
        int album = lib->tracks[a->tracks[i]].album_id;
        bool seen = false;
        for (int j = 0; j < n; j++) if (res[j] == album) { seen = true; break; }
        if (!seen) res[n++] = album;
    }
    *out = res;
    return n;
}

int library_folder_tracks(const Library *lib, int folder, int **out) {
    int *res = malloc((lib->count ? lib->count : 1) * sizeof(int));
    int n = 0;
    if (res)
        for (int i = 0; i < lib->count; i++)
            if (lib->tracks[i].folder == folder) res[n++] = i;
    *out = res;
    return n;
}

static int cmp_folder_name(const void *a, const void *b) {
    return strcasecmp(sort_lib->folders[*(const int *)a].name, sort_lib->folders[*(const int *)b].name);
}

int library_subfolders(const Library *lib, int folder, int **out) {
    int *res = malloc((lib->folder_count ? lib->folder_count : 1) * sizeof(int));
    int n = 0;
    if (res)
        for (int i = 0; i < lib->folder_count; i++)
            if (lib->folders[i].parent == folder) res[n++] = i;
    sort_lib = lib;
    if (res) qsort(res, n, sizeof(int), cmp_folder_name);
    *out = res;
    return n;
}
