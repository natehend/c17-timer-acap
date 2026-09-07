#include "timer.h"

#include <glib.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>

// How often the state machine wakes up. Fast enough that a threshold
// crossing and the moment of finishing land within a tenth of a second
// of where they should, without being anywhere near often enough to
// matter for CPU. Note this is NOT the display refresh rate: a display
// post only happens when the displayed second actually changes (see
// recompute() emitting TIMER_EV_TICK), so a running timer posts once a
// second regardless of how often this fires.
#define TICK_INTERVAL_MS 100

static struct {
    const timer_config_t* cfg;
    timer_observer_fn observer;
    void* user_data;

    timer_state_t state;
    int set_seconds; // the duration; see timer.h

    // While RUNNING, the countdown is defined entirely by this deadline
    // rather than by a decrementing counter - so a late or coalesced
    // timer callback can never make the timer drift long. While
    // PAUSED/IDLE, remaining_us holds the frozen value instead and
    // end_us is meaningless.
    gint64 end_us;
    gint64 remaining_us;

    // When the finished message should come off screen. Only meaningful
    // in TIMER_FINISHED.
    gint64 finished_until_us;

    // Last values actually reported, so the tick only emits when
    // something a viewer would notice has changed.
    int last_displayed_seconds;
    int last_state_index;

    // The default duration as of the last config read, so a save can
    // tell an actual change from an unrelated one - see
    // timer_config_changed().
    int last_known_default;

    guint tick_source;
    bool initialized;

    // Local clock time the current pause began. Wall clock rather than
    // monotonic on purpose - it is shown to a person, not used for
    // timing - and captured once, at the moment of pausing, so it can't
    // drift or re-read differently on each display refresh.
    //
    // Kept as components rather than a formatted string so the 12/24
    // hour setting is applied when the placeholder is expanded: changing
    // that setting then updates a screen that is already paused, instead
    // of only taking effect on the next pause.
    int paused_hour;
    int paused_minute;
    bool have_paused_time;
} g;

static void emit(timer_event_t ev) {
    if (g.observer)
        g.observer(ev, g.user_data);
}

timer_state_t timer_get_state(void) {
    return g.state;
}

const char* timer_state_name(timer_state_t s) {
    switch (s) {
    case TIMER_IDLE:
        return "idle";
    case TIMER_RUNNING:
        return "running";
    case TIMER_PAUSED:
        return "paused";
    case TIMER_FINISHED:
        return "finished";
    default:
        return "unknown";
    }
}

// Microseconds still to run, in whichever way the current state stores
// it. Never negative.
static gint64 remaining_us_now(void) {
    if (g.state == TIMER_RUNNING) {
        gint64 left = g.end_us - g_get_monotonic_time();
        return left > 0 ? left : 0;
    }
    return g.remaining_us > 0 ? g.remaining_us : 0;
}

int timer_remaining_seconds(void) {
    if (g.state == TIMER_FINISHED)
        return 0;
    gint64 us = remaining_us_now();
    // Round up: a timer with 0.4s left is still showing "1", and only
    // reads 0 once it has genuinely expired. Without this the display
    // would sit on 0 for a whole second before the alarm fired.
    return (int)((us + 999999) / 1000000);
}

int timer_set_seconds(void) {
    return g.set_seconds;
}

int timer_finished_seconds_left(void) {
    if (g.state != TIMER_FINISHED)
        return 0;
    gint64 left = g.finished_until_us - g_get_monotonic_time();
    if (left <= 0)
        return 0;
    return (int)((left + 999999) / 1000000);
}

// Which color state applies at `seconds` remaining. states[0] is the
// normal state and always matches; later entries win as their
// at_seconds thresholds are crossed. Walked forwards rather than
// picking the smallest match, so a config listing thresholds out of
// order (30, then 60) still behaves the way the settings page shows
// them - the last matching row wins.
static int state_index_for(int seconds) {
    if (!g.cfg)
        return 0;
    int idx = 0;
    for (int i = 1; i < g.cfg->state_count && i < MAX_DISPLAY_STATES; i++) {
        if (seconds <= g.cfg->states[i].at_seconds)
            idx = i;
    }
    return idx;
}

int timer_display_state_index(void) {
    if (g.state == TIMER_IDLE)
        return 0;
    return state_index_for(timer_remaining_seconds());
}

static void clamp_remaining(void) {
    gint64 max_us = (gint64)(g.cfg ? g.cfg->max_seconds : 35999) * 1000000;
    if (g.state == TIMER_RUNNING) {
        gint64 now = g_get_monotonic_time();
        if (g.end_us < now)
            g.end_us = now;
        if (g.end_us - now > max_us)
            g.end_us = now + max_us;
    } else {
        if (g.remaining_us < 0)
            g.remaining_us = 0;
        if (g.remaining_us > max_us)
            g.remaining_us = max_us;
    }
}

static void finish(void);

// Returns the timer to its resting state: the configured default
// duration, both as the duration it will next count down from and as the
// value now showing.
//
// Both the duration and the remaining time move together on purpose. If
// only the remaining time reset, the status line would read "set to
// 7:00" above a clock showing 5:00, which is worse than either choice.
static void park_at_default(void) {
    g.have_paused_time = false;
    int def            = g.cfg ? g.cfg->default_seconds : 300;
    if (def < 0)
        def = 0;
    g.set_seconds            = def;
    g.remaining_us           = (gint64)def * 1000000;
    g.last_displayed_seconds = -1;
    g.last_state_index       = 0;
}

// The one place that decides whether anything visible has changed.
// Called from the tick and from every transition, so a display update
// never depends on which path got here.
static void recompute(void) {
    if (g.state != TIMER_RUNNING)
        return;

    if (remaining_us_now() <= 0) {
        finish();
        return;
    }

    int secs = timer_remaining_seconds();
    int idx  = state_index_for(secs);

    if (idx != g.last_state_index) {
        g.last_state_index = idx;
        // The threshold event fires before the tick, so a rule reacting
        // to "30 seconds left" and the display's own color change land
        // in the same pass rather than a second apart.
        if (idx >= 1)
            emit(TIMER_EV_THRESHOLD);
    }

    if (secs != g.last_displayed_seconds) {
        g.last_displayed_seconds = secs;
        emit(TIMER_EV_TICK);
    }
}

static gboolean tick_cb(gpointer user_data) {
    (void)user_data;

    if (g.state == TIMER_RUNNING) {
        recompute();
    } else if (g.state == TIMER_FINISHED) {
        if (g_get_monotonic_time() >= g.finished_until_us) {
            // The finished message's time is up. Back to the configured
            // default duration - not to whatever this run happened to be
            // - and let main.c release the display. See timer.h.
            g.state = TIMER_IDLE;
            park_at_default();
            emit(TIMER_EV_CLEARED);
        }
    }

    return G_SOURCE_CONTINUE;
}

static void finish(void) {
    g.state        = TIMER_FINISHED;
    g.remaining_us = 0;
    int show_secs  = g.cfg ? g.cfg->finished_display_seconds : 10;
    if (show_secs < 0)
        show_secs = 0;
    g.finished_until_us      = g_get_monotonic_time() + (gint64)show_secs * 1000000;
    g.last_displayed_seconds = 0;
    g.last_state_index       = 0;
    syslog(LOG_INFO, "Timer finished");
    emit(TIMER_EV_FINISHED);
}

bool timer_finish(void) {
    // Already showing the finished message - restarting it would just
    // replay the alarm on top of itself.
    if (g.state == TIMER_FINISHED)
        return false;

    // Deliberately allowed from IDLE as well as from a run in progress:
    // as a button this is "end it now", and sounding the alarm without a
    // countdown first is a reasonable thing to ask of it.
    finish();
    return true;
}

void timer_init(const timer_config_t* cfg, timer_observer_fn observer, void* user_data) {
    memset(&g, 0, sizeof(g));
    g.cfg       = cfg;
    g.observer  = observer;
    g.user_data = user_data;
    g.state     = TIMER_IDLE;
    // Each process life starts at the configured default duration - see
    // the header's note on why this deliberately isn't persisted.
    g.set_seconds            = cfg ? cfg->default_seconds : 300;
    g.remaining_us           = (gint64)g.set_seconds * 1000000;
    g.last_displayed_seconds = -1;
    g.last_state_index       = 0;
    g.last_known_default     = g.set_seconds;
    g.tick_source            = g_timeout_add(TICK_INTERVAL_MS, tick_cb, NULL);
    g.initialized            = true;
    syslog(LOG_INFO, "Timer ready, default duration %d s", g.set_seconds);
}

void timer_shutdown(void) {
    if (g.tick_source) {
        g_source_remove(g.tick_source);
        g.tick_source = 0;
    }
    g.initialized = false;
}

void timer_config_changed(void) {
    if (!g.initialized)
        return;

    // A new default duration takes effect immediately on an idle timer,
    // so the clock on the Timer page shows what was just saved instead
    // of the previous duration. Only when it actually changed, so an
    // unrelated save does not discard a duration dialled in by the
    // buttons or the API - and only when idle, because re-parking a
    // countdown that is running or paused would silently destroy it.
    int def = g.cfg ? g.cfg->default_seconds : 0;
    if (def != g.last_known_default) {
        g.last_known_default = def;
        if (g.state == TIMER_IDLE)
            park_at_default();
    }

    // A shortened max, or an edited threshold list, has to apply to the
    // run already in progress - otherwise saving settings appears to do
    // nothing until the next run.
    clamp_remaining();
    g.last_state_index       = state_index_for(timer_remaining_seconds());
    g.last_displayed_seconds = -1; // force the next tick to repost
    recompute();
}

bool timer_start(void) {
    if (g.state == TIMER_RUNNING)
        return false;

    // Starting a zero-length timer would fire the alarm instantly, which
    // is never what anyone meant by pressing start - refuse instead, so
    // the button visibly does nothing rather than surprising a room.
    if (g.state != TIMER_FINISHED && remaining_us_now() <= 0)
        return false;

    bool resuming = (g.state == TIMER_PAUSED);

    if (g.state == TIMER_FINISHED) {
        // Start while the finished message is still on screen means "run
        // that again", so this deliberately re-runs the duration that
        // just elapsed rather than the default. Once the message clears,
        // the timer parks at the default and a later start runs that
        // instead - see the tick handler and timer.h.
        g.remaining_us = (gint64)g.set_seconds * 1000000;
        if (g.remaining_us <= 0)
            return false;
        resuming = false;
    }

    if (!resuming)
        g.have_paused_time = false;

    g.end_us                 = g_get_monotonic_time() + remaining_us_now();
    g.state                  = TIMER_RUNNING;
    g.last_displayed_seconds = -1;
    g.last_state_index       = state_index_for(timer_remaining_seconds());
    clamp_remaining();

    syslog(LOG_INFO, "Timer %s at %d s", resuming ? "resumed" : "started", timer_remaining_seconds());
    emit(resuming ? TIMER_EV_RESUMED : TIMER_EV_STARTED);
    recompute();
    return true;
}

bool timer_pause(void) {
    if (g.state != TIMER_RUNNING)
        return false;
    g.remaining_us = remaining_us_now();
    g.state        = TIMER_PAUSED;

    GDateTime* now = g_date_time_new_now_local();
    if (now) {
        g.paused_hour      = g_date_time_get_hour(now);
        g.paused_minute    = g_date_time_get_minute(now);
        g.have_paused_time = true;
        g_date_time_unref(now);
    }

    syslog(LOG_INFO, "Timer paused at %d s", timer_remaining_seconds());
    emit(TIMER_EV_PAUSED);
    return true;
}

bool timer_toggle(void) {
    return (g.state == TIMER_RUNNING) ? timer_pause() : timer_start();
}

bool timer_stop_reset(void) {
    int def = g.cfg ? g.cfg->default_seconds : 300;

    // Already idle at the default, with nothing dialled in on top: a
    // genuine no-op. Report that honestly rather than firing a stopped
    // event nothing asked for.
    bool already_parked = (g.state == TIMER_IDLE) && (g.set_seconds == def) &&
                          (remaining_us_now() == (gint64)def * 1000000);
    if (already_parked)
        return false;

    g.state = TIMER_IDLE;
    park_at_default();
    syslog(LOG_INFO, "Timer stopped and reset to the default, %d s", g.set_seconds);
    emit(TIMER_EV_STOPPED);
    return true;
}

bool timer_set(int seconds) {
    if (seconds < 0)
        seconds = 0;
    int max = g.cfg ? g.cfg->max_seconds : 35999;
    if (seconds > max)
        seconds = max;

    g.set_seconds = seconds;
    if (g.state == TIMER_RUNNING) {
        // Setting an absolute duration mid-run retimes the current run
        // to that duration from now, which is what "set it to 2 minutes"
        // means while something is counting.
        g.end_us = g_get_monotonic_time() + (gint64)seconds * 1000000;
    } else {
        g.remaining_us = (gint64)seconds * 1000000;
        if (g.state == TIMER_FINISHED)
            g.state = TIMER_IDLE;
    }
    g.last_displayed_seconds = -1;
    g.last_state_index       = state_index_for(timer_remaining_seconds());
    emit(TIMER_EV_ADJUSTED);
    recompute();
    return true;
}

bool timer_add(int seconds) {
    if (seconds == 0)
        return false;

    gint64 delta_us = (gint64)seconds * 1000000;

    if (g.state == TIMER_RUNNING) {
        // Extends or shortens this run only. set_seconds is left alone
        // on purpose - see the duration-vs-remaining note in timer.h.
        g.end_us += delta_us;
        clamp_remaining();
    } else {
        // Not running, so this is dialling in the duration itself: both
        // the duration and the parked remaining move together, which is
        // what makes Volume up/down feel like setting a timer.
        if (g.state == TIMER_FINISHED)
            g.state = TIMER_IDLE;
        g.remaining_us += delta_us;
        clamp_remaining();
        g.set_seconds = (int)((remaining_us_now() + 999999) / 1000000);
    }

    g.last_displayed_seconds = -1;
    g.last_state_index       = state_index_for(timer_remaining_seconds());
    emit(TIMER_EV_ADJUSTED);
    recompute();
    return true;
}

bool timer_quick(int seconds) {
    if (seconds <= 0)
        return false;
    timer_set(seconds);
    // Force a clean start rather than going through timer_start()'s
    // resume path, so a quick-timer press always means the same thing
    // no matter what the timer was doing beforehand.
    g.state                  = TIMER_IDLE;
    g.remaining_us           = (gint64)g.set_seconds * 1000000;
    return timer_start();
}

void timer_format_time(int seconds, char* out, size_t outlen) {
    if (seconds < 0)
        seconds = 0;
    int h = seconds / 3600;
    int m = (seconds % 3600) / 60;
    int s = seconds % 60;
    if (h > 0)
        snprintf(out, outlen, "%d:%02d:%02d", h, m, s);
    else
        snprintf(out, outlen, "%02d:%02d", m, s);
}

void timer_format_template(const char* template_str, int seconds, char* out, size_t outlen) {
    if (!template_str || !out || outlen == 0)
        return;
    if (seconds < 0)
        seconds = 0;

    char time_buf[16];
    timer_format_time(seconds, time_buf, sizeof(time_buf));
    // Both clock formats are built here and offered as separate
    // placeholders, so the template itself says which one it wants -
    // no setting to find, and a display can show either (or both) at
    // once. All three are empty when no pause has been recorded, so a
    // template using one outside that state renders nothing rather than
    // a stale or invented time.
    char paused_24[16] = {0};
    char paused_12[16] = {0};
    if (g.have_paused_time) {
        snprintf(paused_24, sizeof(paused_24), "%02d:%02d", g.paused_hour, g.paused_minute);

        int h12 = g.paused_hour % 12;
        if (h12 == 0)
            h12 = 12; // midnight and noon are 12, not 0
        snprintf(paused_12,
                 sizeof(paused_12),
                 "%d:%02d %s",
                 h12,
                 g.paused_minute,
                 g.paused_hour < 12 ? "AM" : "PM");
    }

    char hh[8], mm[8], ss[8], total[16];
    snprintf(hh, sizeof(hh), "%d", seconds / 3600);
    snprintf(mm, sizeof(mm), "%02d", (seconds % 3600) / 60);
    snprintf(ss, sizeof(ss), "%02d", seconds % 60);
    snprintf(total, sizeof(total), "%d", seconds);

    struct {
        const char* token;
        const char* value;
    } subs[] = {
        {"{time}", time_buf},
        {"{hh}", hh},
        {"{mm}", mm},
        {"{ss}", ss},
        {"{total}", total},
        // Only the two explicit forms: a bare {paused_at} left the format
        // implicit, which is exactly the thing a template should state.
        {"{paused_at_24}", paused_24},
        {"{paused_at_12}", paused_12},
    };

    size_t o = 0;
    for (size_t i = 0; template_str[i] != '\0' && o + 1 < outlen;) {
        bool matched = false;
        if (template_str[i] == '{') {
            for (size_t k = 0; k < G_N_ELEMENTS(subs); k++) {
                size_t tlen = strlen(subs[k].token);
                if (strncmp(template_str + i, subs[k].token, tlen) == 0) {
                    size_t vlen = strlen(subs[k].value);
                    if (o + vlen + 1 >= outlen)
                        vlen = outlen - o - 1;
                    memcpy(out + o, subs[k].value, vlen);
                    o += vlen;
                    i += tlen;
                    matched = true;
                    break;
                }
            }
        }
        if (!matched)
            out[o++] = template_str[i++];
    }
    out[o] = '\0';
}
