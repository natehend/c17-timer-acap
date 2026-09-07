/**
 * event - the app's custom Rule engine conditions.
 *
 * WHAT THIS CAN AND CANNOT DO, because it's the one place the design had
 * to bend around a platform limit:
 *
 * An ACAP can declare **events**, which the device's Rule engine offers
 * as **conditions** under its fixed "Application" heading. There is no
 * ACAP API for registering a Rule engine **action** - the action list is
 * the platform's own (play audio clip, speaker display notification,
 * toggle I/O, send MQTT, HTTP notification, and so on) and third-party
 * apps cannot add to it.
 *
 * So the two directions work differently:
 *   - Device reacts to the timer  -> the conditions declared here.
 *   - Device controls the timer   -> a rule whose action is the built-in
 *     "Notification: HTTP" pointed at this app's own local endpoint,
 *     e.g. http://127.0.0.1:8082/?action=start. The web UI's API page
 *     lists every one of those URLs ready to copy, so setting a rule up
 *     is paste-one-URL rather than anything hand-written.
 *
 * Four condition families are declared, all sharing topic0/topic1 so the
 * Rule engine groups them together:
 *
 *   Application > C17 Timer: Timer event         Event: Started | Paused |
 *                                                Resumed | Stopped | Finished
 *   Application > C17 Timer: Timer running       (stateful - true for the whole
 *                                                time a timer is counting, for
 *                                                "...while the rule is active" actions)
 *   Application > C17 Timer: Threshold reached   Threshold: 1..5
 *   Application > C17 Timer: Button pressed      Button: Button 1..3 |
 *                                                Volume up | Volume down | Mute
 *
 * Every declaration is stateful and carries a boolean "state" data key,
 * which the Rule engine surfaces as a checkbox under the dropdown. That
 * checkbox is unavoidable - the C8310 Customizer confirmed on real
 * hardware that splitting press/release into separate stateless
 * declarations does not remove it - so this app uses it the same way
 * that one settled on: checked means the value is TRUE, i.e. the moment
 * the thing happened. Momentary events send TRUE immediately followed by
 * FALSE; "Timer running" holds TRUE for the duration of a run.
 */
#ifndef EVENT_H
#define EVENT_H

#include <axsdk/axevent.h>
#include <stdbool.h>

#include "local_config.h"

// The momentary timer events, in declaration order. Kept separate from
// timer.h's own timer_event_t, which also covers internal happenings
// (ticks, adjustments) that deliberately have no Rule engine condition -
// firing an event 60 times a minute would be worse than useless.
typedef enum {
    RULE_EVT_STARTED = 0,
    RULE_EVT_PAUSED,
    RULE_EVT_RESUMED,
    RULE_EVT_STOPPED,
    RULE_EVT_FINISHED,
    RULE_EVT_COUNT
} rule_evt_t;

/**
 * Declares every condition. Call once, after ax_event_handler_new().
 *
 * `cfg` is read for the color-change times, which go into the threshold
 * conditions' names so the rule engine's dropdown reads "Threshold 1 -
 * 0:30 left" rather than a bare number.
 */
void events_init(AXEventHandler* handler, const timer_config_t* cfg);

/**
 * Re-declares the threshold conditions so their names match the times
 * currently configured. Call after a settings save.
 *
 * Safe to do: a rule binds to the event's topic and to its source key's
 * *value* (`threshold=1`), neither of which changes here - only the
 * human-readable name attached to that value does. So an existing rule
 * keeps matching across a re-declaration; all that changes is the label
 * shown in the picker.
 *
 * Does nothing if none of the names actually changed, so an unrelated
 * save doesn't churn the event system for no reason.
 */
void events_refresh_thresholds(const timer_config_t* cfg);

/** Undeclares everything. Call before ax_event_handler_free(). */
void events_shutdown(AXEventHandler* handler);

/** Fires one momentary timer event as a TRUE-then-FALSE pulse. */
void events_fire_timer(rule_evt_t which);

/**
 * Sets the stateful "Timer running" condition. Repeated calls with the
 * same value are ignored, so this can be called freely from anywhere the
 * timer's state might have changed without spamming the event system.
 */
void events_set_running(bool running);

/** Fires "Threshold reached" for threshold 1..MAX_THRESHOLDS. */
void events_fire_threshold(int threshold_number);

/**
 * Fires a button's condition. Physical presses pass the real press and
 * release edges through; a web UI click, which has no separate release,
 * sends a TRUE-then-FALSE pulse instead.
 */
void events_fire_button(int button_index, bool active);

#endif // EVENT_H
