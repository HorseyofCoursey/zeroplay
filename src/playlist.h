#pragma once

#define PLAYLIST_MAX_ITEMS      1024
#define PLAYLIST_ITEM_PATH_SIZE 2048

typedef enum {
    ITEM_VIDEO,
    ITEM_IMAGE,
    ITEM_UNKNOWN
} PlaylistItemType;

typedef struct {
    char             path[PLAYLIST_ITEM_PATH_SIZE];
    char             path_audio[PLAYLIST_ITEM_PATH_SIZE];
    PlaylistItemType type;
} PlaylistItem;

typedef struct {
    PlaylistItem *items;
    int           count;
    int           current;
    int           loop;
    int           shuffle;
} Playlist;

/*
 * Open a playlist from:
 *   - a directory   (flat scan, alphabetical)
 *   - a .txt/.m3u   (one path per line, # = comment)
 *   - a single file (treated as a one-item playlist)
 *
 * Returns 0 on success, -1 on error.
 */
int           playlist_open(Playlist *pl, const char *path, const char *path_audio, int loop, int shuffle);
void          playlist_close(Playlist *pl);

/* Returns pointer to current item, or NULL if empty. */
PlaylistItem *playlist_current(Playlist *pl);

/*
 * Advance to next item.
 * Returns 0 on success, -1 if the playlist is exhausted and loop=false.
 * If loop=true, wraps around to the beginning and returns 0.
 */
int           playlist_advance(Playlist *pl);

/*
 * Go back to the previous item.
 * Returns 0 on success, -1 if already at the beginning (and loop=false).
 */
int           playlist_prev(Playlist *pl);
