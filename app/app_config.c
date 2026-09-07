#include "app_config.h"

#include <glib.h>
#include <stddef.h>
#include <syslog.h>

#include "axparam_util.h"
#include "io_discovery.h"

const char* const BTN_PARAM_NAME[BTN_COUNT] = {
    "Button1Port",
    "Button2Port",
    "Button3Port",
    "VolUpPort",
    "VolDownPort",
    "MutePort",
};

const char* const BTN_NAME_HINT[BTN_COUNT] = {
    "source 1",
    "source 2",
    "source 3",
    "volume up",
    "volume down",
    "mute",
};

const char* const BTN_EVENT_ID[BTN_COUNT] = {
    "button1",
    "button2",
    "button3",
    "volup",
    "voldown",
    "mute",
};

const char* const BTN_DISPLAY_NAME[BTN_COUNT] = {
    "Button 1",
    "Button 2",
    "Button 3",
    "Volume up",
    "Volume down",
    "Mute",
};

void app_config_resolve_ports(AXParameter* axparam, int out_ports[BTN_COUNT]) {
    for (int i = 0; i < BTN_COUNT; i++) {
        int manual = get_int_param(axparam, BTN_PARAM_NAME[i], -1);
        if (manual >= 0) {
            out_ports[i] = manual;
            syslog(LOG_INFO, "%s: using configured port %d", BTN_PARAM_NAME[i], manual);
            continue;
        }

        // A manual value of -1 is the explicit "try auto-detection
        // instead" opt-in documented in README.md, not a failure state -
        // the shipped defaults are real port numbers (1000-1005), so
        // this branch is only reached when someone deliberately blanked
        // one out.
        int discovered = -1;
        if (io_discover_port_by_name(BTN_NAME_HINT[i], &discovered)) {
            out_ports[i] = discovered;
        } else {
            out_ports[i] = -1;
            syslog(LOG_ERR,
                  "Could not resolve I/O port for '%s' automatically. Set %s in the app's "
                  "configuration page to the correct port number (look it up under System > "
                  "Events > Rules > add rule > I/O > Digital input is active > Port).",
                  BTN_NAME_HINT[i],
                  BTN_PARAM_NAME[i]);
        }
    }
}

void app_config_purge_legacy_params(AXParameter* axparam) {
    if (!axparam)
        return;

    // Declared by versions up to 0.1.5, before the web UI moved behind
    // the device's own login and its port became internal plumbing.
    static const char* const legacy[] = {"WebUiPort", "WebUiPassword"};

    for (size_t i = 0; i < G_N_ELEMENTS(legacy); i++) {
        GError* error = NULL;
        if (ax_parameter_remove(axparam, legacy[i], &error)) {
            syslog(LOG_INFO, "Removed leftover parameter %s", legacy[i]);
        } else if (error) {
            // Overwhelmingly this is "already removed on a previous
            // startup" - not worth a log line on every boot.
            g_error_free(error);
        }
    }
}
