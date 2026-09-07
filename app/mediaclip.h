/**
 * mediaclip - lists and plays the audio clips already stored on this
 * device, so the timer can sound an alarm when it finishes.
 *
 * Listing uses the legacy parameter group the Media Clip API documents:
 *   GET /axis-cgi/param.cgi?action=list&group=MediaClip
 *   -> root.MediaClip.M0.Name=My new clip
 *      root.MediaClip.M0.Location=/etc/audioclips/MediaClip.M0.au
 *      root.MediaClip.M0.Type=audio
 *
 * Playing uses playclip.cgi, which takes the clip id plus the repeat
 * count and volume:
 *   GET /axis-cgi/playclip.cgi?clip=0&repeat=0&volume=100
 *
 * This app only ever reads and plays. Uploading, renaming and deleting
 * clips are deliberately left to the device's own Audio > Clips page -
 * there's no reason for a timer to be able to delete someone's audio.
 */
#ifndef MEDIACLIP_H
#define MEDIACLIP_H

#include <glib.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    int id;
    char name[96];
} media_clip_t;

/**
 * Fetches the device's clip list. Returns a GArray of media_clip_t that
 * the caller frees with g_array_unref(), or NULL on failure.
 *
 * Results are cached briefly (see mediaclip.c) because the settings page
 * asks for this on every render, and re-listing on each page load would
 * put a VAPIX round trip in the path of drawing a form.
 */
GArray* mediaclip_list(void);

/** Drops the cache, so the next mediaclip_list() re-reads from the device. */
void mediaclip_invalidate_cache(void);

/**
 * Plays clip `id`. `repeat` is 0 for once and -1 for forever; `volume`
 * is a percentage, 0-1000, per the API. Returns true if the device
 * accepted the request.
 */
bool mediaclip_play(int id, int repeat, int volume);

/** Stops whatever clip is currently playing. */
bool mediaclip_stop(void);

/**
 * Looks up a clip's name by id, writing "" if it isn't in the list (for
 * example because it was deleted from the device after being selected
 * here). Never fails; the caller decides what an empty name means.
 */
void mediaclip_name_for(int id, char* dest, size_t destlen);

#endif // MEDIACLIP_H
