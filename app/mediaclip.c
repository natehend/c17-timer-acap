#include "mediaclip.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#include <glib.h>

#include "local_vapix.h"

// The settings page renders the clip dropdown on every load, and the
// control page's status poll doesn't need it at all. Caching for a
// short while keeps a VAPIX round trip out of the path of drawing a
// form, while still picking up a clip uploaded on the device's own Audio
// page within a few seconds of someone reopening settings.
#define CLIP_CACHE_TTL_US (15 * 1000000LL)

static GArray* g_cache      = NULL;
static gint64 g_cache_at_us = 0;

void mediaclip_invalidate_cache(void) {
    if (g_cache) {
        g_array_unref(g_cache);
        g_cache = NULL;
    }
    g_cache_at_us = 0;
}

// Parses the flat "root.MediaClip.M<n>.<Field>=<value>" list into
// media_clip_t entries. Only Name is kept - Location and Type exist in
// the response but nothing here needs them, since playclip.cgi addresses
// a clip by its numeric id.
static GArray* parse_clip_list(char* body) {
    GArray* clips = g_array_new(FALSE, TRUE, sizeof(media_clip_t));

    char* saveptr = NULL;
    char* line    = strtok_r(body, "\n", &saveptr);
    while (line) {
        char* eq = strchr(line, '=');
        if (!eq) {
            line = strtok_r(NULL, "\n", &saveptr);
            continue;
        }
        *eq             = '\0';
        const char* key = line;
        char* value     = eq + 1;

        size_t vlen = strlen(value);
        while (vlen > 0 && (value[vlen - 1] == '\r' || value[vlen - 1] == '\n'))
            value[--vlen] = '\0';

        // Only the .Name lines carry anything worth keeping, and they're
        // also the ones that establish which ids exist at all.
        //
        // Match on the full "MediaClip.M" rather than just ".M". An
        // earlier version searched for ".M" and found the ".M" of
        // ".MediaClip" itself - four characters into "root.MediaClip.M0.Name" -
        // so every clip parsed as id 0. That made the settings page look
        // like it wasn't saving the selection (it re-selected the first
        // clip every time, because they all shared an id) and sent
        // playback at the wrong clip.
        static const char MARKER[] = "MediaClip.M";
        const char* m              = strstr(key, MARKER);
        if (m && strstr(key, ".Name")) {
            const char* digits = m + sizeof(MARKER) - 1;
            if (!g_ascii_isdigit((guchar)*digits)) {
                line = strtok_r(NULL, "\n", &saveptr);
                continue;
            }
            int id = atoi(digits); // NOLINT
            media_clip_t c = {0};
            c.id           = id;
            g_strlcpy(c.name, value, sizeof(c.name));
            // A clip with a blank name still needs something selectable
            // in the dropdown, or it would render as an empty row.
            if (!c.name[0])
                snprintf(c.name, sizeof(c.name), "Clip %d", id);
            g_array_append_val(clips, c);
        }

        line = strtok_r(NULL, "\n", &saveptr);
    }

    return clips;
}

GArray* mediaclip_list(void) {
    gint64 now = g_get_monotonic_time();
    if (g_cache && now - g_cache_at_us < CLIP_CACHE_TTL_US)
        return g_array_ref(g_cache);

    char err[256] = {0};
    char* body    = local_vapix_get("/axis-cgi/param.cgi?action=list&group=MediaClip", err, sizeof(err));
    if (!body) {
        syslog(LOG_WARNING, "mediaclip: could not list clips: %s", err);
        // Hand back the stale cache rather than nothing if there is one:
        // a transient failure shouldn't make the settings page's clip
        // dropdown suddenly appear empty and lose the user's selection.
        return g_cache ? g_array_ref(g_cache) : NULL;
    }

    GArray* clips = parse_clip_list(body);
    free(body);

    mediaclip_invalidate_cache();
    g_cache       = clips;
    g_cache_at_us = now;
    return g_array_ref(g_cache);
}

void mediaclip_name_for(int id, char* dest, size_t destlen) {
    dest[0] = '\0';
    if (id < 0)
        return;
    GArray* clips = mediaclip_list();
    if (!clips)
        return;
    for (guint i = 0; i < clips->len; i++) {
        media_clip_t* c = &g_array_index(clips, media_clip_t, i);
        if (c->id == id) {
            g_strlcpy(dest, c->name, destlen);
            break;
        }
    }
    g_array_unref(clips);
}

bool mediaclip_play(int id, int repeat, int volume) {
    if (id < 0)
        return false; // "no clip" is a valid configuration, not a failure

    if (volume < 0)
        volume = 0;
    if (volume > 1000)
        volume = 1000;
    if (repeat < -1)
        repeat = -1;

    // The device plays clips first-come-first-served: a clip already
    // sounding wins, and the new request is simply dropped. That is
    // wrong for a timer, where the later sound is always the more urgent
    // one - a threshold warning has to cut through the clip a previous
    // threshold started, and the finish alarm has to cut through both.
    // Stop whatever is playing first so the newest clip always wins.
    mediaclip_stop();

    char path[160];
    snprintf(path,
            sizeof(path),
            "/axis-cgi/playclip.cgi?clip=%d&repeat=%d&volume=%d",
            id,
            repeat,
            volume);

    char err[256] = {0};
    char* resp    = local_vapix_get(path, err, sizeof(err));
    if (!resp) {
        syslog(LOG_ERR, "mediaclip: could not play clip %d: %s", id, err);
        return false;
    }
    syslog(LOG_INFO, "mediaclip: played clip %d (repeat %d, volume %d%%)", id, repeat, volume);
    free(resp);
    return true;
}

bool mediaclip_stop(void) {
    char err[256] = {0};
    char* resp    = local_vapix_get("/axis-cgi/stopclip.cgi", err, sizeof(err));
    if (!resp) {
        syslog(LOG_WARNING, "mediaclip: could not stop playback: %s", err);
        return false;
    }
    free(resp);
    return true;
}
