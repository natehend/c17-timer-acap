#include "event.h"

#include <glib.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>

#include "app_config.h"
#include "local_config.h"
#include "timer.h"

// Shared by every declaration so the Rule engine groups them under one
// heading. topic0 is the fixed value the platform uses for all ACAP
// events; "Application" (the heading actually shown in the Condition
// picker) is the Rule engine's own label for third-party apps and is not
// something a manifest or nice name can override.
#define TOPIC0 "CameraApplicationPlatform"
#define TOPIC1 "C17Timer"
#define TOPIC1_NICE "C17 Timer"

// Internal ids for the momentary timer events - the "event" source key's
// values. Not shown to anyone; they just have to be stable and unique.
static const char* const EVT_ID[RULE_EVT_COUNT] = {
    "started", "paused", "resumed", "stopped", "finished",
};

// What the Rule engine's "Event" dropdown shows for each.
static const char* const EVT_LABEL[RULE_EVT_COUNT] = {
    "Started", "Paused", "Resumed", "Stopped", "Finished",
};

static struct {
    AXEventHandler* handler;

    guint evt_decl[RULE_EVT_COUNT];
    bool evt_ready[RULE_EVT_COUNT];

    guint running_decl;
    bool running_ready;
    bool running_last; // so a repeated set is a no-op

    guint threshold_decl[MAX_THRESHOLDS];
    bool threshold_ready[MAX_THRESHOLDS];
    // The name currently declared for each threshold, so a refresh can
    // skip the work when nothing has actually changed.
    char threshold_label[MAX_THRESHOLDS][48];

    guint button_decl[BTN_COUNT];
    bool button_ready[BTN_COUNT];
} g;

typedef struct {
    bool* ready_flag;
    char label[64];
} decl_ctx_t;

// Called once the declaration has round-tripped through the event system
// and its id is genuinely usable for sending. Sending against an
// incomplete declaration is asking for trouble, so every fire path
// checks its ready flag first. In practice these all complete at startup
// long before a button can be pressed, but the guard costs nothing.
static void declaration_complete(guint declaration, gpointer user_data) {
    decl_ctx_t* ctx = user_data;
    *ctx->ready_flag = true;
    syslog(LOG_INFO, "Declared '%s' condition (declaration id %u)", ctx->label, declaration);
    g_free(ctx);
}

// Declares one condition.
//
// `source_key`/`source_value`/`source_nice`/`source_label` are optional
// (pass NULL for source_key to declare a condition with no dropdown at
// all, which is what "Timer running" wants). When present, marking the
// key as a *source* is exactly what makes the Rule engine offer its
// values as a dropdown, grouping every declaration that shares the key
// into one condition entry - the same way the built-in "Digital input is
// active" condition groups all the physical ports into one Port list.
static void declare_one(const char* topic2,
                        const char* topic2_nice,
                        const char* source_key,
                        const char* source_value,
                        const char* source_label, // dropdown heading, e.g. "Event"
                        const char* source_nice,  // this value's entry, e.g. "Started"
                        guint* out_declaration,
                        bool* out_ready) {
    AXEventKeyValueSet* kvs = ax_event_key_value_set_new();

    // Declared TRUE rather than FALSE as a best-effort attempt to have
    // the Rule engine's unavoidable "state" checkbox default to checked
    // when the condition is first added - carried over from the C8310
    // Customizer, where it's noted as plausible but unconfirmed.
    gboolean initial_state = TRUE;

    ax_event_key_value_set_add_key_values(kvs,
                                          NULL,
                                          "topic0",
                                          "tnsaxis",
                                          TOPIC0,
                                          AX_VALUE_TYPE_STRING,
                                          "topic1",
                                          "tnsaxis",
                                          TOPIC1,
                                          AX_VALUE_TYPE_STRING,
                                          "topic2",
                                          "tnsaxis",
                                          topic2,
                                          AX_VALUE_TYPE_STRING,
                                          "state",
                                          NULL,
                                          &initial_state,
                                          AX_VALUE_TYPE_BOOL,
                                          NULL);

    if (source_key) {
        ax_event_key_value_set_add_key_value(
            kvs, source_key, NULL, source_value, AX_VALUE_TYPE_STRING, NULL);
        ax_event_key_value_set_mark_as_source(kvs, source_key, NULL, NULL);
        ax_event_key_value_set_mark_as_user_defined(kvs, source_key, NULL, source_key, NULL);
    }
    ax_event_key_value_set_mark_as_data(kvs, "state", NULL, NULL);

    ax_event_key_value_set_add_nice_names(kvs, "topic1", "tnsaxis", NULL, TOPIC1_NICE, NULL);
    ax_event_key_value_set_add_nice_names(kvs, "topic2", "tnsaxis", NULL, topic2_nice, NULL);
    if (source_key)
        ax_event_key_value_set_add_nice_names(kvs, source_key, NULL, source_label, source_nice, NULL);

    decl_ctx_t* ctx = g_new0(decl_ctx_t, 1);
    ctx->ready_flag = out_ready;
    snprintf(ctx->label, sizeof(ctx->label), "%s%s%s", topic2_nice, source_nice ? " / " : "",
             source_nice ? source_nice : "");

    guint declaration = 0;
    GError* error     = NULL;
    if (!ax_event_handler_declare(g.handler,
                                  kvs,
                                  FALSE, // stateful, matching a real digital input
                                  &declaration,
                                  (AXDeclarationCompleteCallback)declaration_complete,
                                  ctx,
                                  &error)) {
        syslog(LOG_ERR,
              "Failed to declare condition '%s': %s",
              ctx->label,
              error ? error->message : "?");
        if (error)
            g_error_free(error);
        g_free(ctx);
    } else {
        *out_declaration = declaration;
    }

    ax_event_key_value_set_free(kvs);
}

// "Threshold 1 - 0:30 left", or "Threshold 4 - not set" for an unused
// slot. The number stays in the name on purpose: it is the part that
// never goes stale, and it matches the numbering on the Timer -
// Thresholds page and in this app's own logs.
static void threshold_label_for(const timer_config_t* cfg, int idx, char* out, size_t outlen) {
    int state_idx = idx + 1; // states[0] is the normal state
    if (cfg && state_idx < cfg->state_count) {
        char when[16];
        timer_format_time(cfg->states[state_idx].at_seconds, when, sizeof(when));
        snprintf(out, outlen, "Threshold %d - %s left", idx + 1, when);
    } else {
        snprintf(out, outlen, "Threshold %d - not set", idx + 1);
    }
}

void events_init(AXEventHandler* handler, const timer_config_t* cfg) {
    memset(&g, 0, sizeof(g));
    g.handler = handler;

    for (int i = 0; i < RULE_EVT_COUNT; i++)
        declare_one("TimerEvent",
                    "C17 Timer: Timer event",
                    "event",
                    EVT_ID[i],
                    "Event",
                    EVT_LABEL[i],
                    &g.evt_decl[i],
                    &g.evt_ready[i]);

    // No source key: there's only one thing this can mean, so a dropdown
    // with a single entry would be noise. Stateful and long-lived, which
    // is what makes the Rule engine's "...while the rule is active"
    // action variants work with it - record audio while a timer runs,
    // hold an output on for the duration, and so on.
    declare_one("TimerRunning",
                "C17 Timer: Timer running",
                NULL,
                NULL,
                NULL,
                NULL,
                &g.running_decl,
                &g.running_ready);

    for (int i = 0; i < MAX_THRESHOLDS; i++) {
        char value[8];
        snprintf(value, sizeof(value), "%d", i + 1);
        threshold_label_for(cfg, i, g.threshold_label[i], sizeof(g.threshold_label[i]));
        declare_one("ThresholdReached",
                    "C17 Timer: Threshold reached",
                    "threshold",
                    value,
                    "Threshold",
                    g.threshold_label[i],
                    &g.threshold_decl[i],
                    &g.threshold_ready[i]);
    }

    for (int i = 0; i < BTN_COUNT; i++)
        declare_one("ButtonPress",
                    "C17 Timer: Button pressed",
                    "button",
                    BTN_EVENT_ID[i],
                    "Button",
                    BTN_DISPLAY_NAME[i],
                    &g.button_decl[i],
                    &g.button_ready[i]);
}

void events_refresh_thresholds(const timer_config_t* cfg) {
    if (!g.handler)
        return;

    for (int i = 0; i < MAX_THRESHOLDS; i++) {
        char want[48];
        threshold_label_for(cfg, i, want, sizeof(want));
        if (strcmp(want, g.threshold_label[i]) == 0)
            continue;

        syslog(LOG_INFO,
              "Renaming threshold condition %d: '%s' -> '%s'",
              i + 1,
              g.threshold_label[i],
              want);

        if (g.threshold_ready[i])
            ax_event_handler_undeclare(g.handler, g.threshold_decl[i], NULL);
        g.threshold_ready[i] = false;
        g.threshold_decl[i]  = 0;

        char value[8];
        snprintf(value, sizeof(value), "%d", i + 1);
        g_strlcpy(g.threshold_label[i], want, sizeof(g.threshold_label[i]));
        declare_one("ThresholdReached",
                    "C17 Timer: Threshold reached",
                    "threshold",
                    value,
                    "Threshold",
                    g.threshold_label[i],
                    &g.threshold_decl[i],
                    &g.threshold_ready[i]);
    }
}

void events_shutdown(AXEventHandler* handler) {
    for (int i = 0; i < RULE_EVT_COUNT; i++)
        if (g.evt_ready[i])
            ax_event_handler_undeclare(handler, g.evt_decl[i], NULL);
    if (g.running_ready)
        ax_event_handler_undeclare(handler, g.running_decl, NULL);
    for (int i = 0; i < MAX_THRESHOLDS; i++)
        if (g.threshold_ready[i])
            ax_event_handler_undeclare(handler, g.threshold_decl[i], NULL);
    for (int i = 0; i < BTN_COUNT; i++)
        if (g.button_ready[i])
            ax_event_handler_undeclare(handler, g.button_decl[i], NULL);
}

// Only the "state" key needs to travel on a send, not the full key set
// from declaration time - the event is tied to its declaration purely by
// the declaration id passed here. Confirmed from Axis's own send_event.c
// example, and relied on by the C8310 Customizer.
static void send_state(guint declaration, bool ready, gboolean active) {
    if (!ready)
        return; // declaration hasn't completed yet

    AXEventKeyValueSet* kvs = ax_event_key_value_set_new();
    ax_event_key_value_set_add_key_value(kvs, "state", NULL, &active, AX_VALUE_TYPE_BOOL, NULL);
    AXEvent* event = ax_event_new2(kvs, NULL);
    ax_event_key_value_set_free(kvs);

    ax_event_handler_send_event(g.handler, declaration, event, NULL);
    ax_event_free(event);
}

// A momentary event has no natural "off" edge, so it sends TRUE
// immediately followed by FALSE. That pulse is what a rule with the
// state checkbox ticked reacts to, and leaving the condition sitting
// TRUE afterwards would make it look permanently active.
static void pulse(guint declaration, bool ready) {
    send_state(declaration, ready, TRUE);
    send_state(declaration, ready, FALSE);
}

void events_fire_timer(rule_evt_t which) {
    if (which < 0 || which >= RULE_EVT_COUNT)
        return;
    syslog(LOG_INFO, "Rule engine event: Timer %s", EVT_LABEL[which]);
    pulse(g.evt_decl[which], g.evt_ready[which]);
}

void events_set_running(bool running) {
    if (running == g.running_last)
        return;
    g.running_last = running;
    syslog(LOG_INFO, "Rule engine event: Timer running = %s", running ? "true" : "false");
    send_state(g.running_decl, g.running_ready, running ? TRUE : FALSE);
}

void events_fire_threshold(int threshold_number) {
    int idx = threshold_number - 1;
    if (idx < 0 || idx >= MAX_THRESHOLDS)
        return;
    syslog(LOG_INFO, "Rule engine event: Threshold %d reached", threshold_number);
    pulse(g.threshold_decl[idx], g.threshold_ready[idx]);
}

void events_fire_button(int button_index, bool active) {
    if (button_index < 0 || button_index >= BTN_COUNT)
        return;
    send_state(g.button_decl[button_index], g.button_ready[button_index], active ? TRUE : FALSE);
}
