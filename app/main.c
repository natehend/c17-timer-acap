/**
 * c17timer - a countdown timer for the AXIS C17 Series Network Display
 * Speaker, driven from an AXIS C8310 Volume Controller, from an HTTP
 * API, or from the device's own rule engine.
 *
 * Built on the same foundation as the C8310 Customizer: the C8310 wires
 * into the speaker's I/O connector and its six buttons appear as digital
 * input ports, which this app subscribes to. Volume up/down set the
 * duration; the other four buttons are assignable (Start/Pause, Stop &
 * reset, quick timers, and so on).
 *
 * The countdown itself goes on the C17's 1920x480 display through the
 * Speaker display notification API, changing color as configured
 * thresholds are crossed, and an audio clip plays when it finishes.
 *
 * This file is the wiring. Each piece lives in its own module:
 *
 *   timer.c        the countdown state machine - state and timing only,
 *                  no side effects
 *   display.c      the Speaker display notification client, plus the
 *                  shared "what should be on screen right now" calculation
 *   mediaclip.c    listing and playing the device's stored audio clips
 *   event.c        the custom rule engine conditions this app declares
 *   buttons.c      C8310 I/O subscriptions and what each button does
 *   webui.c        the whole user interface and the HTTP API
 *   local_config.c the settings file under localdata/
 *   local_vapix.c  shared plumbing for calling this device's own VAPIX
 *
 * Everything with a side effect hangs off on_timer_event() below, so the
 * rules about when the display repaints, when a clip plays and when a
 * condition fires are all readable in one place instead of being spread
 * across the modules that happen to perform them.
 */
#include <axsdk/axevent.h>
#include <axsdk/axparameter.h>
#include <glib-unix.h>
#include <glib.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>

#include "app_config.h"
#include "buttons.h"
#include "display.h"
#include "event.h"
#include "local_config.h"
#include "mediaclip.h"
#include "timer.h"
#include "webui.h"

// How long the display shows the newly-dialled duration after a Volume
// up/down press while the timer is idle. Without this the C8310's up and
// down buttons would have no visible effect at all until someone pressed
// start - you'd be setting a timer blind, in front of a screen that
// stays dark.
#define IDLE_NUDGE_MS 3500

static struct {
    AXParameter* axparam;
    AXEventHandler* event_handler;
    timer_config_t cfg;

    // Whether this app currently has something of its own on the screen.
    // Tracked so the display is released exactly once on the way back to
    // idle, rather than a stop request being fired every tick - and so a
    // freshly started app never clears a screen it hasn't written to,
    // which would stomp on whatever AAMP or a rule had put there.
    bool display_owned;

    // The last frame actually posted, kept so "keep the last frame on
    // screen" can re-post it with no expiry when the timer goes idle.
    display_frame_t last_frame;
    bool have_last_frame;

    guint idle_nudge_source;
} s;

static void cancel_idle_nudge(void) {
    if (s.idle_nudge_source) {
        g_source_remove(s.idle_nudge_source);
        s.idle_nudge_source = 0;
    }
}

// Hands the screen back, if this app is the one holding it. With
// "release the display" turned off, the last frame is re-posted with no
// expiry instead, so it stays up rather than quietly timing out a few
// seconds later - which is what someone choosing that option means.
static void release_display(void) {
    if (!s.display_owned)
        return;

    if (s.cfg.release_display_when_idle) {
        display_stop();
    } else if (s.have_last_frame) {
        display_show(s.last_frame.message,
                     s.last_frame.text_color,
                     s.last_frame.bg_color,
                     s.last_frame.text_size,
                     s.last_frame.scroll_direction,
                     s.last_frame.scroll_speed,
                     0); // 0 = indefinite
    }
    s.display_owned = false;
}

static void post_frame(const display_frame_t* frame) {
    display_show(frame->message,
                 frame->text_color,
                 frame->bg_color,
                 frame->text_size,
                 frame->scroll_direction,
                 frame->scroll_speed,
                 frame->duration_ms);
    s.last_frame      = *frame;
    s.have_last_frame = true;
    s.display_owned   = true;
}

// Repaints the display to match whatever the timer is doing now, or
// releases it when there's nothing to show.
static void paint_display(void) {
    display_frame_t frame;
    if (display_resolve(&s.cfg, &frame)) {
        cancel_idle_nudge();
        post_frame(&frame);
    } else {
        // Nothing to show. Don't touch the screen while an idle nudge is
        // still up - that nudge IS the current frame, and clearing it
        // here would make it flash for a single tick.
        if (s.idle_nudge_source == 0)
            release_display();
    }
}

static gboolean idle_nudge_expired(gpointer user_data) {
    (void)user_data;
    s.idle_nudge_source = 0;
    release_display();
    return G_SOURCE_REMOVE;
}

// Briefly shows the duration that Volume up/down just dialled in, using
// the normal colors, then hands the screen back.
static void show_idle_nudge(void) {
    display_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    const display_state_t* st = &s.cfg.states[0];

    timer_format_template(st->text, timer_remaining_seconds(), frame.message, sizeof(frame.message));
    g_strlcpy(frame.text_color, st->text_color, sizeof(frame.text_color));
    g_strlcpy(frame.bg_color, st->bg_color, sizeof(frame.bg_color));
    g_strlcpy(frame.text_size, "large", sizeof(frame.text_size));
    g_strlcpy(frame.scroll_direction, "fromRightToLeft", sizeof(frame.scroll_direction));
    frame.scroll_speed = 0;
    frame.duration_ms  = IDLE_NUDGE_MS;

    post_frame(&frame);

    cancel_idle_nudge();
    // Slightly shorter than the frame's own expiry, so this app is what
    // takes the frame down rather than the notification lapsing first.
    s.idle_nudge_source = g_timeout_add(IDLE_NUDGE_MS - 300, idle_nudge_expired, NULL);
}

// Everything the timer changing state causes to happen, in one place.
static void on_timer_event(timer_event_t ev, void* user_data) {
    (void)user_data;

    switch (ev) {
    case TIMER_EV_STARTED:
        events_fire_timer(RULE_EVT_STARTED);
        // A fresh start only. Resuming has a clip of its own, so the two
        // can differ - or resume can stay silent, which is its default.
        mediaclip_play(s.cfg.start_clip.id, s.cfg.start_clip.repeat, s.cfg.start_clip.volume);
        break;
    case TIMER_EV_PAUSED:
        events_fire_timer(RULE_EVT_PAUSED);
        mediaclip_play(s.cfg.paused_clip.id, s.cfg.paused_clip.repeat, s.cfg.paused_clip.volume);
        break;
    case TIMER_EV_RESUMED:
        events_fire_timer(RULE_EVT_RESUMED);
        mediaclip_play(s.cfg.resume_clip.id, s.cfg.resume_clip.repeat, s.cfg.resume_clip.volume);
        break;
    case TIMER_EV_STOPPED:
        events_fire_timer(RULE_EVT_STOPPED);
        // A stop is an explicit "I am done with this" - any lingering
        // alarm clip should stop with it rather than playing on over a
        // screen that has already gone back to idle.
        mediaclip_stop();
        break;
    case TIMER_EV_FINISHED:
        events_fire_timer(RULE_EVT_FINISHED);
        mediaclip_play(s.cfg.finish_clip.id, s.cfg.finish_clip.repeat, s.cfg.finish_clip.volume);
        break;
    case TIMER_EV_THRESHOLD: {
        // The threshold's number is its index in states[], which is
        // exactly the numbering the settings page and the rule engine's
        // Threshold dropdown both use.
        int idx = timer_display_state_index();
        events_fire_threshold(idx);
        // Each threshold can sound its own clip as the countdown crosses
        // into it - a warning chime at 30 seconds, a sharper one at 10.
        // mediaclip_play() ignores an id of -1, so a threshold with no
        // clip set is silent without needing a check here.
        if (idx >= 1 && idx < s.cfg.state_count)
            mediaclip_play(s.cfg.states[idx].clip.id,
                           s.cfg.states[idx].clip.repeat,
                           s.cfg.states[idx].clip.volume);
        break;
    }
    case TIMER_EV_ADJUSTED:
        // Volume up/down (or an API set/add) while nothing is running:
        // flash the new duration so the buttons visibly do something.
        if (timer_get_state() == TIMER_IDLE) {
            show_idle_nudge();
            return; // show_idle_nudge() has already painted
        }
        break;
    case TIMER_EV_CLEARED:
    case TIMER_EV_TICK:
    default:
        break;
    }

    events_set_running(timer_get_state() == TIMER_RUNNING);
    paint_display();
}

// Called by the web UI once a settings save has been written to disk.
static void on_config_saved(void) {
    syslog(LOG_INFO, "Settings saved, applying");
    timer_config_changed();
    // The color-change times are part of the threshold conditions' names
    // in the rule engine, so a change to them has to be reflected there
    // too - see events_refresh_thresholds().
    events_refresh_thresholds(&s.cfg);
    // A clip may well have been uploaded on the device's own Audio page
    // in the same sitting - don't make someone wait out the cache to see
    // it in the dropdown.
    mediaclip_invalidate_cache();
    paint_display();
}

static gboolean signal_handler(gpointer loop) {
    g_main_loop_quit((GMainLoop*)loop);
    return G_SOURCE_REMOVE;
}

int main(void) {
    openlog(APP_NAME, LOG_PID | LOG_CONS, LOG_USER);
    syslog(LOG_INFO, "Starting " APP_NAME);

    memset(&s, 0, sizeof(s));

    GError* error = NULL;
    s.axparam     = ax_parameter_new(APP_NAME, &error);
    if (!s.axparam) {
        syslog(LOG_ERR, "ax_parameter_new failed: %s", error ? error->message : "?");
        if (error)
            g_error_free(error);
        return 1;
    }

    local_config_load(&s.cfg);
    syslog(LOG_INFO,
          "Config: step +%ds/-%ds, default %ds, max %ds, %d display state(s), "
          "start clip %d, finish clip %d, finished message %ds, "
          "display %s when idle and %s when paused",
          s.cfg.step_up_seconds,
          s.cfg.step_down_seconds,
          s.cfg.default_seconds,
          s.cfg.max_seconds,
          s.cfg.state_count,
          s.cfg.start_clip.id,
          s.cfg.finish_clip.id,
          s.cfg.finished_display_seconds,
          s.cfg.release_display_when_idle ? "released" : "held",
          s.cfg.release_display_when_paused ? "released" : "held");

    int ports[BTN_COUNT];
    app_config_resolve_ports(s.axparam, ports);
    // Clears WebUiPort/WebUiPassword out of the native Settings dialog on
    // an upgrade from a version that still declared them.
    app_config_purge_legacy_params(s.axparam);

    timer_init(&s.cfg, on_timer_event, NULL);

    s.event_handler = ax_event_handler_new();
    events_init(s.event_handler, &s.cfg);
    buttons_init(s.event_handler, &s.cfg, ports);

    webui_start(s.axparam, &s.cfg, on_config_saved);

    // Nothing is posted to the display at startup on purpose: the timer
    // begins idle, and an app that has just been installed or restarted
    // has no business clearing or overwriting a screen it never wrote
    // to. See the coexistence note in display.h.

    GMainLoop* loop = g_main_loop_new(NULL, FALSE);
    g_unix_signal_add(SIGTERM, signal_handler, loop);
    g_unix_signal_add(SIGINT, signal_handler, loop);

    syslog(LOG_INFO, APP_NAME " running");
    g_main_loop_run(loop);

    syslog(LOG_INFO, "Shutting down");
    cancel_idle_nudge();
    // Leaving a frozen countdown on a wall-mounted screen after the app
    // stops would be worse than a blank one, so release it on the way
    // out - but only if this app is what put something there.
    if (s.display_owned)
        display_stop();
    timer_shutdown();
    buttons_shutdown(s.event_handler);
    events_shutdown(s.event_handler);
    ax_event_handler_free(s.event_handler);
    ax_parameter_free(s.axparam);
    g_main_loop_unref(loop);
    closelog();
    return 0;
}
