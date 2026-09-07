/**
 * buttons - the AXIS C8310 Volume Controller's six physical buttons, and
 * what each one does to the timer.
 *
 * The C8310 is not a network device: it has one 4-pin cable into the
 * host speaker's I/O connector (the C8310 is a listed accessory for the
 * AXIS C1710), and the host's firmware exposes each button as its own
 * numbered digital input port - the same ports the device's own Events
 * page lists under "Digital input is active". This module subscribes to
 * those ports and turns a press into a timer action.
 *
 * Volume up and Volume down are fixed: they add and subtract one
 * configured step. Buttons 1, 2, 3 and Mute are freely assignable (see
 * button_action_t in local_config.h), defaulting to two quick timers,
 * Start/Pause and Stop & reset respectively.
 *
 * Every button also fires its own Rule engine condition on every press,
 * whatever it's assigned to - so a button can drive an unrelated
 * automation at the same time as running the timer, and a button set to
 * "Rule engine trigger only" does nothing but that.
 *
 * buttons_press() is the single entry point for both a real press and a
 * click on the web UI's virtual controller, so the two can never drift
 * apart in what they do.
 */
#ifndef BUTTONS_H
#define BUTTONS_H

#include <axsdk/axevent.h>
#include <stdbool.h>
#include <stddef.h>

#include "app_config.h"
#include "local_config.h"

/**
 * Subscribes to each resolved button port, plus one wildcard
 * subscription that logs every digital input transition on the device
 * regardless of port number. That last one is purely diagnostic and is
 * the fastest way to find out which port a given physical button
 * actually is on a particular unit: press each button once and read the
 * app log.
 *
 * `cfg` is borrowed - the caller owns the live config and this module
 * reads it on every press, so a settings save takes effect immediately
 * with nothing to re-register.
 */
void buttons_init(AXEventHandler* handler, const timer_config_t* cfg, const int ports[BTN_COUNT]);
void buttons_shutdown(AXEventHandler* handler);

/**
 * Runs button `idx`'s configured action and fires its Rule engine
 * condition.
 *
 * `from_web` distinguishes a virtual click - a single momentary HTTP
 * request with no separate release - from a physical press, which has
 * real press and release edges. A web click therefore sends the
 * condition as a TRUE-then-FALSE pulse, while a physical press passes
 * both real edges through.
 */
void buttons_press(int idx, bool from_web);

/**
 * One-line description of what this button currently does, for the web
 * UI's tooltips and settings summaries (e.g. "Quick timer 5:00",
 * "Start / Pause", "-1:00").
 */
void buttons_describe(int idx, char* out, size_t outlen);

/** Which cfg->buttons[] slot a button index maps to, or -1 if it isn't assignable. */
int buttons_config_slot(int idx);

#endif // BUTTONS_H
