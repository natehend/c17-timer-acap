/**
 * app_config - the settings that are still AXParameter-backed (and
 * therefore visible behind "Show hidden parameters" in the app's native
 * three-dot Settings dialog): the six C8310 button I/O port numbers, and
 * nothing else.
 *
 * Everything a user actually configures day to day - timer steps, button
 * assignments, display colors and thresholds, the finished message, the
 * audio clips - lives in localdata/config.json instead, edited from the
 * web UI. See local_config.h for why.
 */
#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include <axsdk/axparameter.h>

#define APP_NAME "c17timer"

// TCP port the embedded web UI listens on, bound to loopback only.
//
// Not configurable, on purpose. The server is reachable solely through
// Apache's reverse proxy now, so this port is private plumbing between
// the two rather than an address anybody types - and manifest.json's
// reverseProxy target hardcodes the same number, so a runtime change
// could only ever break the one route in. 8082 rather than the C8310
// Customizer's 8081, so both apps can run on the same speaker.
#define WEB_PORT 8082

// The six C8310 buttons, in the order used by every per-button array in
// this app. Buttons 1-3 and Mute are freely assignable to timer actions;
// Volume up/down are fixed to "add/subtract one step" (see buttons.c).
enum {
    BTN_1 = 0,
    BTN_2,
    BTN_3,
    BTN_VOLUP,
    BTN_VOLDOWN,
    BTN_MUTE,
    BTN_COUNT
};

// AXParameter names for each button's manual I/O port override.
extern const char* const BTN_PARAM_NAME[BTN_COUNT];
// Substrings matched case-insensitively against each I/O port's nice
// name when auto-discovering which physical button is which.
extern const char* const BTN_NAME_HINT[BTN_COUNT];
// Stable internal id used as the "button" source key's value in the
// Rule engine button event - not shown to the user.
extern const char* const BTN_EVENT_ID[BTN_COUNT];
// Human-readable label shown in the Rule engine's Button dropdown and
// reused as the heading for each button's settings accordion.
extern const char* const BTN_DISPLAY_NAME[BTN_COUNT];

// Resolves each button's I/O port number: a manual override from
// AXParameter if one is set (>= 0), otherwise auto-discovery by nice
// name (io_discovery.c), otherwise -1 meaning "unresolved, skip".
void app_config_resolve_ports(AXParameter* axparam, int out_ports[BTN_COUNT]);

/**
 * Deletes the WebUiPort and WebUiPassword parameters left behind by
 * versions that declared them.
 *
 * Upgrading an ACAP in place does not remove parameters a new manifest
 * stops declaring - it just stops the app reading them - so without this
 * they would linger in "Show hidden parameters" forever, looking like
 * settings that still do something. Safe to call on every startup:
 * removing an already-gone parameter is a no-op.
 */
void app_config_purge_legacy_params(AXParameter* axparam);

#endif // APP_CONFIG_H
