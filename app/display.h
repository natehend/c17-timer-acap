/**
 * display - drives the C17's screen through the Speaker display
 * notification REST API.
 *
 *   POST /config/rest/speaker-display-notification/v1/simple
 *   POST /config/rest/speaker-display-notification/v1/stop
 *
 * called on the local device (127.0.0.12) with the app's own VAPIX
 * service account, which Axis documents as having admin access - the
 * level this API requires. See local_vapix.h.
 *
 * Request shape, from the API reference:
 *
 *   { "data": {
 *       "message": "<= 1000 chars",
 *       "textColor": "#RRGGBB",
 *       "backgroundColor": "#RRGGBB",
 *       "textSize": "small" | "medium" | "large",
 *       "scrollDirection": "fromRightToLeft" | "fromLeftToRight" | "fromBottomToTop",
 *       "scrollSpeed": 0-10,
 *       "duration": { "type": "repetitions"|"time"|"timeCompleteMessage", "value": 1.. }
 *   } }
 *
 * TWO THINGS THIS API DOES THAT SHAPE THE WHOLE DESIGN:
 *
 * 1. It explicitly does not support coexistence - whoever posted last
 *    owns the screen. That's why this app releases the display
 *    (display_stop) the moment it has nothing to show, rather than
 *    parking an idle screen there forever: while no timer is running,
 *    AAMP, the Rule engine and anything else are free to use it.
 *
 * 2. Every post replaces the current notification, restarting whatever
 *    animation it had. A running countdown re-posts once a second, so
 *    scrollSpeed must be 0 (static) for the countdown or the scroll
 *    animation restarts every second and never gets anywhere. The
 *    finished message is posted once and can scroll freely - which is
 *    why it carries its own separate scroll settings in the config.
 *
 * Each countdown post asks for a duration a few seconds longer than the
 * gap until the next one (DISPLAY_HOLD_MS). That overlap is deliberate:
 * the screen never blanks between posts, and if this app dies mid-
 * countdown the display clears itself a few seconds later instead of
 * leaving a frozen number up forever.
 */
#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdbool.h>

#include "local_config.h"

// How long each countdown post asks to stay up. Comfortably longer than
// the one-second repost interval - see the note above.
#define DISPLAY_HOLD_MS 5000

/**
 * Post a notification to the display.
 *
 * `duration_ms` uses the API's "time" duration type. Pass 0 for an
 * indefinite notification (the API's own documented meaning for a zero
 * duration in its response).
 *
 * Returns true on success. On failure a message is logged, rate-limited
 * so a display that's refusing every post once a second can't flood the
 * app log - see display.c.
 */
bool display_show(const char* message,
                  const char* text_color,
                  const char* bg_color,
                  const char* text_size,
                  const char* scroll_direction,
                  int scroll_speed,
                  int duration_ms);

/** Clear the display and hand it back to whatever else wants it. */
bool display_stop(void);

/**
 * Whether the last post attempt succeeded, and the message from the
 * most recent failure. Surfaced on the web UI's control page so a
 * misconfigured or unsupported device says so plainly instead of just
 * silently not lighting up - this is the single most likely thing to go
 * wrong on a device that isn't a C17.
 */
bool display_last_ok(void);
const char* display_last_error(void);

/**
 * How many rows the display will actually show, given the text size and
 * scroll direction, clamping a configured line count down to it.
 *
 *   large -> 1 row      medium -> 2 rows      small -> 3 rows
 *
 * A vertical scroll (fromBottomToTop) is always 1: it takes a single
 * line and adds its own breaks between words to fit the width.
 *
 * Exported because the web UI renders the same rule - the line dropdown
 * offers only the counts the chosen size can show, and the preview uses
 * this count rather than the raw setting.
 */
int display_effective_lines(const char* text_size, const char* scroll_direction, int configured);

/**
 * Everything needed to paint one frame of the display.
 */
typedef struct {
    bool active; // false means "nothing to show - release the screen"
    char message[512];
    char text_color[8];
    char bg_color[8];
    char text_size[8];
    char scroll_direction[24];
    int scroll_speed;
    int duration_ms;
} display_frame_t;

/**
 * Works out what belongs on the screen right now, from the timer's
 * current state and the config.
 *
 * Deliberately shared: main.c calls this to decide what to actually post
 * to the device, and the web UI calls the same function to render its
 * live preview. That's what makes the preview trustworthy - it isn't a
 * separate guess at what the display is doing, it is literally the same
 * calculation.
 *
 * Returns out->active, so callers can branch on the return value
 * directly.
 */
bool display_resolve(const timer_config_t* cfg, display_frame_t* out);

#endif // DISPLAY_H
