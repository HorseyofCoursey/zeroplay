#pragma once

#include <limits.h>
#include <time.h>

#define PLAYLIST_MAX_ITEMS      1024
#define PLAYLIST_ITEM_PATH_SIZE PATH_MAX

typedef enum {
    ITEM_VIDEO,
    ITEM_IMAGE,
    ITEM_UNKNOWN
} PlaylistItemType;

typedef struct {
    char             path[PLAYLIST_ITEM_PATH_SIZE];
    char             path_audio[PLAYLIST_ITEM_PATH_SIZE];
    PlaylistItemType type;

    /* Per-item image display override, in microseconds. -1 means "not
     * set — use the global --image-duration default". Ignored for video
     * items. Set from the optional second field of a playlist line. */
    int64_t          duration_us;
} PlaylistItem;

typedef struct {
    PlaylistItem *items;
    int           count;
    int           current;
    int           loop;
    int           shuffle;

    /* Non-empty only when opened from a .txt/.m3u file; lets playlist_advance()
     * and playlist_prev() re-read the file on loop wrap if it changed on disk. */
    char          source_path[PLAYLIST_ITEM_PATH_SIZE];
    time_t        source_mtime;
} Playlist;

/*
 * Open a playlist from:
 *   - a directory   (flat scan, alphabetical)
 *   - a .txt/.m3u   (one entry per line, # = comment; see line format below)
 *   - a single file (treated as a one-item playlist)
 *   - a URL         (http/https/rtsp/rtmp/udp/rtp passed directly)
 *   - a YouTube URL (resolved via yt-dlp automatically)
 *
 * .txt/.m3u line format: "<path> [duration]"
 *   - duration is optional and only meaningful for images: a plain
 *     integer is seconds, or "<n>ms" for milliseconds (e.g. "4" or
 *     "2500ms"). Omit it to use the global --image-duration default.
 *   - if the path contains spaces, quote it: "my photo.png" 5
 *     (without quotes, a path with spaces is only safe when no
 *     duration follows it — the whole line is then taken as the path).
 *
 * A .txt/.m3u source is also re-read automatically whenever playback
 * loops back to the start, if the file's mtime changed on disk — so
 * edits made while zeroplay is running take effect on the next loop.
 * Directories and single files are not re-scanned.
 *
 * yt_quality caps YouTube stream height (e.g. 480, 720, 1080).
 * Pass 0 to use the default (480p).
 *
 * Returns 0 on success, -1 on error.
 */
int           playlist_open(Playlist *pl, const char *path, const char *path_audio,
                             int loop, int shuffle, int recurse, int yt_quality);
void          playlist_close(Playlist *pl);

PlaylistItemType type_from_ext(const char *path);

/* Returns pointer to current item, or NULL if empty. */
PlaylistItem *playlist_current(Playlist *pl);

/*
 * Advance to next item.
 * Returns 0 on success, -1 if the playlist is exhausted and loop=false.
 */
int           playlist_advance(Playlist *pl);

/*
 * Go back to the previous item.
 * Returns 0 on success, -1 if already at the beginning (and loop=false).
 */
int           playlist_prev(Playlist *pl);
