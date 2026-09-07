/**
 * timer - the countdown state machine, and the only place that knows
 * what time it is.
 *
 * Deliberately has no dependency on the display, the event system, the
 * media clip player or the web UI: it owns state and timing, and reports
 * what happened through a single observer callback that main.c
 * registers. Everything with a side effect - posting to the display,
 * firing a Rule engine event, playing a clip - hangs off that callback
 * in main.c instead, so the rules about *when* things happen all live in
 * one readable place rather than being scattered through this file.
 *
 * Timing is monotonic (g_get_monotonic_time), never wall clock, so an
 * NTP step or a manual clock change mid-countdown can't make a 5 minute
 * timer suddenly finish or run for an hour.
 *
 * ONE timer, not several. Quick-timer buttons set and start this same
 * timer rather than creating additional ones - which is the only model
 * the hardware can actually represent, since there's a single display.
 *
 * Duration vs. remaining, since these are easy to conflate:
 *   - `set_seconds` is the timer's *duration* - what it counts down
 *     from. Changed by Volume up/down while the timer isn't running, by
 *     the API's set action, and by a quick-timer button.
 *   - `remaining` is where the current run is up to.
 *   - Volume up/down *while running* extends or shortens the current
 *     run only, leaving the duration alone.
 *
 * The timer always comes to rest at the **configured default duration**.
 * Whenever it stops running - by finishing, or by Stop & reset - both the
 * duration and the remaining time go back to that default, not to
 * whatever the last run happened to be. So a quick-timer button, a dialled
 * -in adjustment or an API `set` all last exactly as long as the run they
 * belong to, and the timer is found in the same known state next time
 * somebody walks up to it.
 *
 * The one deliberate exception: pressing Start *while the finished
 * message is still on screen* re-runs the duration that just elapsed,
 * because "run that again" is plainly what it means there. Once the
 * message clears, the default applies as usual.
 *
 * set_seconds also starts each process life at the default. It is
 * deliberately not persisted across restarts: writing localdata on every
 * button press is exactly the continuous flash write Axis's own docs
 * warn against.
 */
#ifndef TIMER_H
#define TIMER_H

#include <stdbool.h>
#include <stddef.h>

#include "local_config.h"

typedef enum {
    TIMER_IDLE = 0, // not counting; remaining is parked, display released
    TIMER_RUNNING,
    TIMER_PAUSED,
    TIMER_FINISHED, // hit zero; the finished message is on screen
} timer_state_t;

typedef enum {
    TIMER_EV_STARTED = 0,
    TIMER_EV_PAUSED,
    TIMER_EV_RESUMED,
    TIMER_EV_STOPPED,   // stop & reset, whether by button, API or rule
    TIMER_EV_FINISHED,  // reached zero
    TIMER_EV_CLEARED,   // the finished message's time is up; nothing to show
    TIMER_EV_TICK,      // the displayed second changed
    TIMER_EV_THRESHOLD, // crossed into a new color state (index >= 1)
    TIMER_EV_ADJUSTED,  // duration or remaining changed without a state change
} timer_event_t;

typedef void (*timer_observer_fn)(timer_event_t ev, void* user_data);

// `cfg` is borrowed, not copied - main.c owns the single live
// timer_config_t and calls timer_config_changed() after re-reading it,
// so this module always sees current settings without a second copy to
// keep in sync.
void timer_init(const timer_config_t* cfg, timer_observer_fn observer, void* user_data);
void timer_shutdown(void);

// Call after the settings page rewrites the shared config, so a changed
// max/threshold list is re-applied to the run in progress.
void timer_config_changed(void);

timer_state_t timer_get_state(void);
const char* timer_state_name(timer_state_t s);

// Seconds still to run, rounded up - the number actually shown. A
// running timer displays "1" for the whole of its final second and only
// reads 0 once it has genuinely finished.
int timer_remaining_seconds(void);
// The duration this timer counts down from. See the header note above.
int timer_set_seconds(void);
// Index into cfg->states[] of the color state in force right now.
int timer_display_state_index(void);
// Seconds left of the finished message, or 0 when not in TIMER_FINISHED.
int timer_finished_seconds_left(void);

// Transitions. Each returns true if it actually changed anything, so
// callers (the API in particular) can report "started" vs "already
// running" honestly instead of always claiming success.
bool timer_start(void);      // IDLE/PAUSED/FINISHED -> RUNNING
bool timer_pause(void);      // RUNNING -> PAUSED
bool timer_toggle(void);     // whichever of the two applies
bool timer_stop_reset(void); // -> IDLE, remaining = set_seconds
bool timer_set(int seconds); // sets the duration (and remaining, unless running)
bool timer_add(int seconds); // signed; adjusts remaining, and the duration when not running
bool timer_quick(int seconds); // set duration to `seconds` and start immediately
// Jumps straight to the end: remaining goes to 0, the finished message
// and clip fire, and the Rule engine's Finished event goes out - exactly
// as if the countdown had run out on its own. Allowed from any state
// except FINISHED, which is already there.
bool timer_finish(void);

// Renders `seconds` as M:SS / MM:SS below an hour, H:MM:SS above.
void timer_format_time(int seconds, char* out, size_t outlen);

// Substitutes {time}, {hh}, {mm}, {ss} and {total} in a display text
// template. Unknown {placeholders} are left exactly as typed rather than
// silently deleted, so a typo is visible on screen instead of vanishing.
void timer_format_template(const char* template_str, int seconds, char* out, size_t outlen);

#endif // TIMER_H
