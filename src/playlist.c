#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <sys/stat.h>
#include <dirent.h>
#include <limits.h>
#include <time.h>
#include "playlist.h"
#include "log.h"

#define YOUTUBE_DEFAULT_QUALITY 480

/* ------------------------------------------------------------------ */
/* File type detection                                                  */
/* ------------------------------------------------------------------ */

PlaylistItemType type_from_ext(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot) return ITEM_UNKNOWN;

    if (strcasecmp(dot, ".jpg")  == 0 ||
        strcasecmp(dot, ".jpeg") == 0 ||
        strcasecmp(dot, ".png")  == 0 ||
        strcasecmp(dot, ".bmp")  == 0)
        return ITEM_IMAGE;

    if (strcasecmp(dot, ".mp4")  == 0 ||
        strcasecmp(dot, ".mkv")  == 0 ||
        strcasecmp(dot, ".mov")  == 0 ||
        strcasecmp(dot, ".avi")  == 0 ||
        strcasecmp(dot, ".ts")   == 0 ||
        strcasecmp(dot, ".m4v")  == 0 ||
        strcasecmp(dot, ".h264") == 0)
        return ITEM_VIDEO;

    return ITEM_UNKNOWN;
}

static int is_playlist_file(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot) return 0;
    return strcasecmp(dot, ".txt") == 0 || strcasecmp(dot, ".m3u") == 0;
}

/* ------------------------------------------------------------------ */
/* YouTube URL detection and yt-dlp resolution                         */
/* ------------------------------------------------------------------ */

static int is_youtube_url(const char *path)
{
    return strstr(path, "youtube.com/watch")  != NULL ||
           strstr(path, "youtu.be/")          != NULL ||
           strstr(path, "youtube.com/shorts") != NULL;
}

/*
 * Use yt-dlp to resolve a YouTube URL into separate video and audio
 * stream URLs. yt_quality caps the video height (e.g. 480, 720, 1080).
 * Returns 0 on success, -1 on failure.
 */
static int resolve_youtube(const char *youtube_url, int yt_quality,
                            char *url_video, size_t video_size,
                            char *url_audio, size_t audio_size)
{
    /* Check yt-dlp is available */
    if (system("which yt-dlp > /dev/null 2>&1") != 0) {
        fprintf(stderr,
            "zeroplay: YouTube URL detected but yt-dlp not found.\n"
            "  Install with:\n"
            "  sudo curl -L https://github.com/yt-dlp/yt-dlp/releases/latest"
            "/download/yt-dlp -o /usr/local/bin/yt-dlp"
            " && sudo chmod +x /usr/local/bin/yt-dlp\n");
        return -1;
    }

    if (yt_quality <= 0) yt_quality = YOUTUBE_DEFAULT_QUALITY;

    fprintf(stderr, "zeroplay: resolving YouTube URL via yt-dlp (%dp)...\n",
            yt_quality);

    char cmd[PLAYLIST_ITEM_PATH_SIZE * 2];
    snprintf(cmd, sizeof(cmd),
             "yt-dlp -f \"bv[vcodec^=avc1][height<=%d]+ba\" --get-url \"%s\" 2>/dev/null",
             yt_quality, youtube_url);

    FILE *pipe = popen(cmd, "r");
    if (!pipe) {
        fprintf(stderr, "zeroplay: failed to run yt-dlp\n");
        return -1;
    }

    url_video[0] = '\0';
    url_audio[0] = '\0';

    if (!fgets(url_video, (int)video_size, pipe)) {
        pclose(pipe);
        fprintf(stderr, "zeroplay: yt-dlp returned no URLs — try updating yt-dlp\n");
        return -1;
    }
    fgets(url_audio, (int)audio_size, pipe);
    pclose(pipe);

    /* Strip trailing newlines */
    size_t vlen = strlen(url_video);
    while (vlen > 0 && (url_video[vlen-1] == '\n' || url_video[vlen-1] == '\r'))
        url_video[--vlen] = '\0';

    size_t alen = strlen(url_audio);
    while (alen > 0 && (url_audio[alen-1] == '\n' || url_audio[alen-1] == '\r'))
        url_audio[--alen] = '\0';

    if (!url_video[0]) {
        fprintf(stderr, "zeroplay: yt-dlp returned empty video URL\n");
        return -1;
    }

    fprintf(stderr, "zeroplay: YouTube resolved — playing at %dp\n", yt_quality);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Internal helpers                                                     */
/* ------------------------------------------------------------------ */

static int add_item(Playlist *pl, const char *path, int64_t duration_us)
{
    PlaylistItemType type = type_from_ext(path);
    if (type == ITEM_UNKNOWN) return 0;

    if (pl->count >= PLAYLIST_MAX_ITEMS) {
        fprintf(stderr, "playlist: max %d items reached, truncating\n",
                PLAYLIST_MAX_ITEMS);
        return -1;
    }

    PlaylistItem *item = &pl->items[pl->count++];
    strncpy(item->path, path, sizeof(item->path) - 1);
    item->type        = type;
    item->duration_us = duration_us;
    return 0;
}

/*
 * Parses a trailing duration token: digits optionally followed by "ms".
 * Returns the duration in microseconds, or -1 if tok isn't a plain
 * "<digits>" (seconds) or "<digits>ms" (milliseconds) duration.
 */
static int64_t parse_duration_token(const char *tok)
{
    if (!*tok) return -1;

    const char *p = tok;
    while (*p && isdigit((unsigned char)*p)) p++;
    if (p == tok) return -1;

    long value = strtol(tok, NULL, 10);

    if (*p == '\0')                 return (int64_t)value * 1000000;
    if (strcmp(p, "ms") == 0)       return (int64_t)value * 1000;
    return -1;
}

static int load_from_txt(Playlist *pl, const char *filepath)
{
    FILE *f = fopen(filepath, "r");
    if (!f) { perror("playlist: fopen"); return -1; }

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';

        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0' || *p == '#') continue;

        int64_t duration_us = -1;

        if (*p == '"') {
            p++;
            char *close = strchr(p, '"');
            if (!close) {
                fprintf(stderr,
                        "playlist: unterminated quote in '%s', treating "
                        "whole line as the path\n", filepath);
                add_item(pl, line, -1);
                continue;
            }
            *close = '\0';

            char *rest = close + 1;
            while (*rest == ' ' || *rest == '\t') rest++;
            if (*rest) {
                int64_t d = parse_duration_token(rest);
                if (d >= 0)
                    duration_us = d;
                else
                    fprintf(stderr,
                            "playlist: ignoring unrecognised duration '%s' "
                            "for '%s'\n", rest, p);
            }
            add_item(pl, p, duration_us);
        } else {
            /* Unquoted: split off a trailing duration token if the last
             * whitespace-separated word parses as one. Otherwise the
             * whole (trimmed) line is the path, so filenames with spaces
             * still work as long as no duration follows — quote the
             * path if you need both. */
            char *last_space = strrchr(p, ' ');
            if (last_space) {
                int64_t d = parse_duration_token(last_space + 1);
                if (d >= 0) {
                    *last_space = '\0';
                    size_t plen = strlen(p);
                    while (plen > 0 && p[plen - 1] == ' ') p[--plen] = '\0';
                    duration_us = d;
                }
            }
            add_item(pl, p, duration_us);
        }
    }
    fclose(f);
    return 0;
}

static int cmp_dirent(const struct dirent **a, const struct dirent **b)
{
    return strcasecmp((*a)->d_name, (*b)->d_name);
}

static int load_from_dir(Playlist *pl, const char *dirpath, int recurse)
{
    struct dirent **entries = NULL;
    int n = scandir(dirpath, &entries, NULL, cmp_dirent);
    if (n < 0) { perror("playlist: scandir"); return -1; }

    for (int i = 0; i < n; i++) {
        if (entries[i]->d_name[0] != '.') {
            char full[PATH_MAX];
            snprintf(full, sizeof(full), "%s/%s", dirpath, entries[i]->d_name);
            if (recurse && entries[i]->d_type == DT_DIR)
                load_from_dir(pl, full, recurse);
            else
                add_item(pl, full, -1);
        }
        free(entries[i]);
    }
    free(entries);
    return 0;
}

static void shuffle_items(Playlist *pl)
{
    srand((unsigned int)time(NULL));
    for (int i = pl->count - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        PlaylistItem tmp = pl->items[i];
        pl->items[i]     = pl->items[j];
        pl->items[j]     = tmp;
    }
}

/*
 * Re-reads the playlist's source .txt/.m3u file if its mtime changed on
 * disk since it was last loaded. No-op for playlists not opened from a
 * .txt/.m3u file (source_path is empty), or if the file vanished (keeps
 * the cached list rather than emptying the playlist).
 */
static void reload_if_changed(Playlist *pl)
{
    if (pl->source_path[0] == '\0') return;

    struct stat st;
    if (stat(pl->source_path, &st) < 0) return;
    if (st.st_mtime == pl->source_mtime) return;

    pl->count = 0;
    load_from_txt(pl, pl->source_path);
    pl->source_mtime = st.st_mtime;

    vlog("playlist: reloaded '%s' — %d item(s)\n", pl->source_path, pl->count);
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

int playlist_open(Playlist *pl, const char *path, const char *path_audio,
                  int loop, int shuffle, int recurse, int yt_quality)
{
    memset(pl, 0, sizeof(*pl));
    pl->loop    = loop;
    pl->shuffle = shuffle;

    pl->items = calloc(PLAYLIST_MAX_ITEMS, sizeof(PlaylistItem));
    if (!pl->items) { perror("playlist: calloc"); return -1; }

    /* YouTube URLs — resolve via yt-dlp into separate video/audio streams */
    if (is_youtube_url(path)) {
        if (pl->count >= PLAYLIST_MAX_ITEMS) {
            fprintf(stderr, "playlist: max items reached\n");
            free(pl->items);
            pl->items = NULL;
            return -1;
        }
        PlaylistItem *item = &pl->items[pl->count++];
        if (resolve_youtube(path, yt_quality,
                            item->path,       sizeof(item->path),
                            item->path_audio, sizeof(item->path_audio)) < 0) {
            free(pl->items);
            pl->items = NULL;
            return -1;
        }
        item->type        = ITEM_VIDEO;
        item->duration_us = -1;

    /* Other URLs — pass directly, skip stat() */
    } else if (strncmp(path, "http://",  7) == 0 ||
               strncmp(path, "https://", 8) == 0 ||
               strncmp(path, "rtsp://",  7) == 0 ||
               strncmp(path, "rtmp://",  7) == 0 ||
               strncmp(path, "udp://",   6) == 0 ||
               strncmp(path, "rtp://",   6) == 0) {
        if (pl->count >= PLAYLIST_MAX_ITEMS) {
            fprintf(stderr, "playlist: max items reached\n");
            free(pl->items);
            pl->items = NULL;
            return -1;
        }
        PlaylistItem *item = &pl->items[pl->count++];
        strncpy(item->path,       path,       sizeof(item->path) - 1);
        strncpy(item->path_audio, path_audio, sizeof(item->path_audio) - 1);
        item->type        = ITEM_VIDEO;
        item->duration_us = -1;

    /* Local files and directories */
    } else {
        struct stat st;
        if (stat(path, &st) < 0) {
            fprintf(stderr, "playlist: cannot stat '%s'\n", path);
            free(pl->items);
            pl->items = NULL;
            return -1;
        }

        if (S_ISDIR(st.st_mode)) {
            load_from_dir(pl, path, recurse);
        } else if (is_playlist_file(path)) {
            load_from_txt(pl, path);
            strncpy(pl->source_path, path, sizeof(pl->source_path) - 1);
            pl->source_mtime = st.st_mtime;
        } else {
            add_item(pl, path, -1);
        }
    }

    if (pl->count == 0) {
        fprintf(stderr, "playlist: no playable items in '%s'\n", path);
        free(pl->items);
        pl->items = NULL;
        return -1;
    }

    if (shuffle) shuffle_items(pl);

    vlog("playlist: %d item(s)%s\n",
            pl->count, shuffle ? " (shuffled)" : "");
    return 0;
}

void playlist_close(Playlist *pl)
{
    free(pl->items);
    pl->items   = NULL;
    pl->count   = 0;
    pl->current = 0;
}

PlaylistItem *playlist_current(Playlist *pl)
{
    if (!pl->items || pl->count == 0) return NULL;
    if (pl->current < 0 || pl->current >= pl->count) return NULL;
    return &pl->items[pl->current];
}

int playlist_advance(Playlist *pl)
{
    if (!pl->items || pl->count == 0) return -1;

    pl->current++;
    if (pl->current >= pl->count) {
        if (pl->loop) {
            reload_if_changed(pl);
            if (pl->count == 0) return -1;
            if (pl->shuffle) shuffle_items(pl);
            pl->current = 0;
            return 0;
        }
        return -1;
    }
    return 0;
}

int playlist_prev(Playlist *pl)
{
    if (!pl->items || pl->count == 0) return -1;

    pl->current--;
    if (pl->current < 0) {
        if (pl->loop) {
            reload_if_changed(pl);
            if (pl->count == 0) return -1;
            pl->current = pl->count - 1;
            return 0;
        }
        pl->current = 0;
        return -1;
    }
    return 0;
}
