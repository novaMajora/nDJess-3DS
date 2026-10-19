// The music library: every .mp3 under MUSIC_DIR, with tags, and browse views over it.
#pragma once

#include <stdbool.h>
#include <time.h>

#define LIB_MAX_TRACKS 8000
#define LIB_MAX_DEPTH 8
#define RECENT_COUNT 50

typedef struct {
    char *path;   // full sdmc: path
    char *title;
    char *artist;
    char *album;
    char *genre;
    int track_no; // 0 when unknown
    int folder;   // index into Library.folders
    int album_id; // index into Library.albums
    time_t mtime;
    bool has_art; // an embedded picture was seen in the tag
} Track;

typedef struct {
    char *path;   // full path
    char *name;   // last component
    int parent;   // -1 for the root
} Folder;

typedef struct {
    char *name;
    int *tracks;  // library indices, in album/track order
    int count;
} Group;

typedef enum { PLAYLIST_FAVORITES, PLAYLIST_RECENT, PLAYLIST_FILE } PlaylistKind;

typedef struct {
    char *name;
    char *path;   // .m3u path (NULL for Recently Added)
    PlaylistKind kind;
    int *tracks;
    int count;
} Playlist;

typedef struct {
    Track *tracks;
    int count;
    Folder *folders;
    int folder_count;
    int *by_title;        // all tracks sorted by title
    Group *artists;       // sorted by name; tracks ordered by album, track number
    int artist_count;
    Group *albums;        // sorted by name; tracks by track number
    int album_count;
    Group *genres;        // sorted by name
    int genre_count;
    Playlist *playlists;  // Favorites, Recently Added, then .m3u files by name
    int playlist_count;
    bool truncated;       // more than LIB_MAX_TRACKS files were found
    int *by_path;         // track indices sorted by path (for playlist lookups)
} Library;

typedef void (*ScanProgress)(int found, const char *current, void *user);

// Scans `root` recursively. Returns false if the folder doesn't exist.
bool library_scan(Library *lib, const char *root, const char *favorites_path, ScanProgress progress, void *user);
void library_free(Library *lib);

// Albums of one artist (indices into lib->albums), returned in a malloc'd array.
int library_artist_albums(const Library *lib, int artist, int **out);
// Tracks directly inside a folder (library indices), malloc'd.
int library_folder_tracks(const Library *lib, int folder, int **out);
// Subfolders of a folder (folder indices), malloc'd.
int library_subfolders(const Library *lib, int folder, int **out);
// Library index of a path, or -1.
int library_find_path(const Library *lib, const char *path);

bool library_is_favorite(const Library *lib, int track);
// Adds or removes a track from Favorites and saves the favorites file. Returns the new state.
bool library_toggle_favorite(Library *lib, int track, const char *favorites_path);

// Tag parsing (exposed for tests and the art loader).
void id3_read(const char *path, char **title, char **artist, char **album, char **genre, int *track_no, bool *has_art);
// Reads the first embedded picture (APIC/PIC) of a file. Returns malloc'd bytes or NULL.
unsigned char *id3_picture(const char *path, int *len);
