#include "buttons.h"

#include <glib.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>

#include "event.h"
#include "timer.h"

static struct {
    const timer_config_t* cfg;
    int ports[BTN_COUNT];
    guint subscription[BTN_COUNT];
    bool subscribed[BTN_COUNT];
    guint debug_subscription;
    bool debug_subscribed;
} g;

int buttons_config_slot(int idx) {
    switch (idx) {
    case BTN_1:
        return 0;
    case BTN_2:
        return 1;
    case BTN_3:
        return 2;
    case BTN_MUTE:
        return 3;
    default:
        return -1; // Volume up/down are fixed, not assignable
    }
}

void buttons_describe(int idx, char* out, size_t outlen) {
    if (!g.cfg) {
        g_strlcpy(out, "", outlen);
        return;
    }

    char step[16];
    if (idx == BTN_VOLUP) {
        timer_format_time(g.cfg->step_up_seconds, step, sizeof(step));
        snprintf(out, outlen, "Add %s", step);
        return;
    }
    if (idx == BTN_VOLDOWN) {
        timer_format_time(g.cfg->step_down_seconds, step, sizeof(step));
        snprintf(out, outlen, "Subtract %s", step);
        return;
    }

    int slot = buttons_config_slot(idx);
    if (slot < 0) {
        g_strlcpy(out, "", outlen);
        return;
    }

    const button_cfg_t* b = &g.cfg->buttons[slot];
    if (button_action_uses_value(b->action)) {
        char value[16];
        timer_format_time(b->value_seconds, value, sizeof(value));
        snprintf(out, outlen, "%s %s", button_action_label(b->action), value);
    } else {
        g_strlcpy(out, button_action_label(b->action), outlen);
    }
}

// Runs the timer side of a press. Kept separate from buttons_press() so
// the Rule engine condition fires on exactly the same path whether or
// not the button is assigned to do anything to the timer.
static void run_action(int idx) {
    if (!g.cfg)
        return;

    if (idx == BTN_VOLUP) {
        timer_add(g.cfg->step_up_seconds);
        return;
    }
    if (idx == BTN_VOLDOWN) {
        timer_add(-g.cfg->step_down_seconds);
        return;
    }

    int slot = buttons_config_slot(idx);
    if (slot < 0)
        return;

    const button_cfg_t* b = &g.cfg->buttons[slot];
    switch (b->action) {
    case BTN_ACTION_START_PAUSE:
        timer_toggle();
        break;
    case BTN_ACTION_START:
        timer_start();
        break;
    case BTN_ACTION_PAUSE:
        timer_pause();
        break;
    case BTN_ACTION_STOP_RESET:
        timer_stop_reset();
        break;
    case BTN_ACTION_QUICK:
        timer_quick(b->value_seconds);
        break;
    case BTN_ACTION_ADD:
        timer_add(b->value_seconds);
        break;
    case BTN_ACTION_FINISH:
        timer_finish();
        break;
    case BTN_ACTION_NONE:
    case BTN_ACTION_TRIGGER:
    default:
        // Nothing to do to the timer. TRIGGER still fires its Rule
        // engine condition in buttons_press() below - that's the whole
        // point of the mode.
        break;
    }
}

void buttons_press(int idx, bool from_web) {
    if (idx < 0 || idx >= BTN_COUNT)
        return;

    char what[64];
    buttons_describe(idx, what, sizeof(what));
    syslog(LOG_INFO, "%s pressed (%s): %s", BTN_DISPLAY_NAME[idx], from_web ? "web" : "physical", what);

    run_action(idx);

    if (from_web) {
        // A web click is one momentary HTTP request - there's no
        // separate release to wait for - so simulate a full
        // press-then-release in place.
        events_fire_button(idx, true);
        events_fire_button(idx, false);
    } else {
        events_fire_button(idx, true);
    }
}

typedef struct {
    int button_idx;
} button_ctx_t;

static void button_callback(guint subscription, AXEvent* event, gpointer user_data) {
    (void)subscription;
    button_ctx_t* ctx = user_data;

    const AXEventKeyValueSet* kvs = ax_event_get_key_value_set(event);
    gboolean active               = FALSE;
    ax_event_key_value_set_get_boolean(kvs, "state", NULL, &active, NULL);

    if (active) {
        buttons_press(ctx->button_idx, false);
    } else {
        // Only the release edge of the Rule engine condition - the timer
        // action already ran on the press, and running it again here
        // would make every button fire twice.
        events_fire_button(ctx->button_idx, false);
    }

    ax_event_free(event);
}

static void subscribe_button(AXEventHandler* handler, int idx) {
    if (g.ports[idx] < 0)
        return; // unresolved, skip

    gint port_val = g.ports[idx];

    AXEventKeyValueSet* kvs = ax_event_key_value_set_new();
    ax_event_key_value_set_add_key_values(kvs,
                                          NULL,
                                          "topic0",
                                          "tns1",
                                          "Device",
                                          AX_VALUE_TYPE_STRING,
                                          "topic1",
                                          "tnsaxis",
                                          "IO",
                                          AX_VALUE_TYPE_STRING,
                                          "topic2",
                                          "tnsaxis",
                                          "Port",
                                          AX_VALUE_TYPE_STRING,
                                          "port",
                                          NULL,
                                          &port_val,
                                          AX_VALUE_TYPE_INT,
                                          "state",
                                          NULL,
                                          NULL,
                                          AX_VALUE_TYPE_BOOL,
                                          NULL);

    button_ctx_t* ctx = g_new0(button_ctx_t, 1);
    ctx->button_idx   = idx;

    guint subscription = 0;
    ax_event_handler_subscribe(
        handler, kvs, &subscription, (AXSubscriptionCallback)button_callback, ctx, NULL);
    ax_event_key_value_set_free(kvs);

    g.subscription[idx] = subscription;
    g.subscribed[idx]   = true;

    syslog(LOG_INFO,
          "Subscribed to I/O port %d for '%s' (subscription id %u)",
          g.ports[idx],
          BTN_DISPLAY_NAME[idx],
          subscription);
}

// Logs every digital input transition on the device, with no port filter
// at all. Purely diagnostic, and the fastest way to work out which port
// a given physical button really is: press each one and read the app log
// for the port number that lit up, then set the matching *Port override
// if it doesn't match this app's defaults. The C8310 Customizer needed
// exactly this on real hardware - the port numbering turned out not to
// be identical across units.
static void debug_callback(guint subscription, AXEvent* event, gpointer user_data) {
    (void)subscription;
    (void)user_data;
    const AXEventKeyValueSet* kvs = ax_event_get_key_value_set(event);
    gint port_val                 = -1;
    gboolean active               = FALSE;
    ax_event_key_value_set_get_integer(kvs, "port", NULL, &port_val, NULL);
    ax_event_key_value_set_get_boolean(kvs, "state", NULL, &active, NULL);
    syslog(LOG_INFO, "I/O port debug: port=%d state=%s", port_val, active ? "active" : "inactive");
    ax_event_free(event);
}

static void subscribe_debug(AXEventHandler* handler) {
    AXEventKeyValueSet* kvs = ax_event_key_value_set_new();
    ax_event_key_value_set_add_key_values(kvs,
                                          NULL,
                                          "topic0",
                                          "tns1",
                                          "Device",
                                          AX_VALUE_TYPE_STRING,
                                          "topic1",
                                          "tnsaxis",
                                          "IO",
                                          AX_VALUE_TYPE_STRING,
                                          "topic2",
                                          "tnsaxis",
                                          "Port",
                                          AX_VALUE_TYPE_STRING,
                                          "port",
                                          NULL,
                                          NULL, // no value = match any port number
                                          AX_VALUE_TYPE_INT,
                                          "state",
                                          NULL,
                                          NULL,
                                          AX_VALUE_TYPE_BOOL,
                                          NULL);

    guint subscription = 0;
    if (ax_event_handler_subscribe(
            handler, kvs, &subscription, (AXSubscriptionCallback)debug_callback, NULL, NULL)) {
        g.debug_subscription = subscription;
        g.debug_subscribed   = true;
        syslog(LOG_INFO,
              "Subscribed to all I/O ports for diagnostic logging (subscription id %u)",
              subscription);
    }
    ax_event_key_value_set_free(kvs);
}

void buttons_init(AXEventHandler* handler, const timer_config_t* cfg, const int ports[BTN_COUNT]) {
    memset(&g, 0, sizeof(g));
    g.cfg = cfg;
    for (int i = 0; i < BTN_COUNT; i++)
        g.ports[i] = ports[i];

    for (int i = 0; i < BTN_COUNT; i++)
        subscribe_button(handler, i);
    subscribe_debug(handler);
}

void buttons_shutdown(AXEventHandler* handler) {
    for (int i = 0; i < BTN_COUNT; i++)
        if (g.subscribed[i])
            ax_event_handler_unsubscribe(handler, g.subscription[i], NULL);
    if (g.debug_subscribed)
        ax_event_handler_unsubscribe(handler, g.debug_subscription, NULL);
}
