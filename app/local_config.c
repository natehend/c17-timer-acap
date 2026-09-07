#include "local_config.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

// Kept as explicit id strings rather than relying on the enum's numeric
// value, so reordering the enum (or inserting a new action in the
// middle) can never silently reinterpret an already-saved config.
static const char* const ACTION_IDS[BTN_ACTION_COUNT] = {
    "none", "startpause", "start", "pause", "stopreset", "quick", "add", "trigger", "finish",
};

static const char* const ACTION_LABELS[BTN_ACTION_COUNT] = {
    "Nothing",
    "Start / Pause",
    "Start only",
    "Pause only",
    "Stop and reset",
    "Quick timer",
    "Add time",
    "Rule engine trigger only",
    "Finish now",
};

const char* button_action_id(button_action_t a) {
    if (a < 0 || a >= BTN_ACTION_COUNT)
        return ACTION_IDS[BTN_ACTION_NONE];
    return ACTION_IDS[a];
}

button_action_t button_action_from_id(const char* id) {
    if (!id)
        return BTN_ACTION_NONE;
    for (int i = 0; i < BTN_ACTION_COUNT; i++)
        if (strcmp(id, ACTION_IDS[i]) == 0)
            return (button_action_t)i;
    return BTN_ACTION_NONE;
}

const char* button_action_label(button_action_t a) {
    if (a < 0 || a >= BTN_ACTION_COUNT)
        return ACTION_LABELS[BTN_ACTION_NONE];
    return ACTION_LABELS[a];
}

bool button_action_uses_value(button_action_t a) {
    return a == BTN_ACTION_QUICK || a == BTN_ACTION_ADD;
}

void local_config_defaults(timer_config_t* cfg) {
    memset(cfg, 0, sizeof(*cfg));

    cfg->step_up_seconds   = 60; // one minute per Volume up press
    cfg->step_down_seconds = 60; // and per Volume down press
    cfg->default_seconds = 300;   // 5:00
    cfg->max_seconds     = 35999; // 9:59:59 - one digit short of needing a 4th hour column

    // Buttons 1 and 2 default to quick timers rather than to "Nothing",
    // so a fresh install is immediately usable without opening settings
    // at all: press Button 1 for a 5 minute timer, Button 2 for 10.
    // Button 3 and Mute default to Start/Pause and Stop & reset
    // respectively, which is the layout the buttons' physical positions
    // suggest (Mute sits alone under the pair, like a reset key).
    cfg->buttons[0].action        = BTN_ACTION_QUICK;
    cfg->buttons[0].value_seconds = 300;
    cfg->buttons[1].action        = BTN_ACTION_QUICK;
    cfg->buttons[1].value_seconds = 600;
    cfg->buttons[2].action        = BTN_ACTION_START_PAUSE;
    cfg->buttons[3].action        = BTN_ACTION_STOP_RESET;

    // Green until 30 seconds remain, amber until 10, then red - the
    // exact progression the feature was described with. Text color
    // flips to black on amber because white on amber is the one pairing
    // in this set that fails a contrast check at 12 m.
    cfg->state_count = 3;
    // Every state starts with no clip; the two timer-level sounds below
    // are the only ones configured out of the box.
    for (int i = 0; i < MAX_DISPLAY_STATES; i++) {
        cfg->states[i].clip.id     = -1;
        cfg->states[i].clip.repeat = 0;
        cfg->states[i].clip.volume = 100;
    }

    cfg->states[0]   = (display_state_t){.at_seconds = -1, .clip = {.id = -1, .volume = 100}};
    g_strlcpy(cfg->states[0].bg_color, "#107c10", sizeof(cfg->states[0].bg_color));
    g_strlcpy(cfg->states[0].text_color, "#ffffff", sizeof(cfg->states[0].text_color));
    g_strlcpy(cfg->states[0].text, "{time}", sizeof(cfg->states[0].text));

    cfg->states[1] = (display_state_t){.at_seconds = 30, .clip = {.id = -1, .volume = 100}};
    g_strlcpy(cfg->states[1].bg_color, "#e8a800", sizeof(cfg->states[1].bg_color));
    g_strlcpy(cfg->states[1].text_color, "#000000", sizeof(cfg->states[1].text_color));
    g_strlcpy(cfg->states[1].text, "{time}", sizeof(cfg->states[1].text));

    cfg->states[2] = (display_state_t){.at_seconds = 10, .clip = {.id = -1, .volume = 100}};
    g_strlcpy(cfg->states[2].bg_color, "#c00000", sizeof(cfg->states[2].bg_color));
    g_strlcpy(cfg->states[2].text_color, "#ffffff", sizeof(cfg->states[2].text_color));
    g_strlcpy(cfg->states[2].text, "{time}", sizeof(cfg->states[2].text));

    cfg->paused_lines = 2;
    g_strlcpy(cfg->paused_text_size, "medium", sizeof(cfg->paused_text_size));
    g_strlcpy(cfg->paused_line1, "PAUSED AT {paused_at_12}", sizeof(cfg->paused_line1));
    g_strlcpy(cfg->paused_line2, "{time} REMAINING", sizeof(cfg->paused_line2));
    cfg->paused_line3[0] = '\0';
    g_strlcpy(cfg->paused_bg, "#107c10", sizeof(cfg->paused_bg));
    g_strlcpy(cfg->paused_text_color, "#ffffff", sizeof(cfg->paused_text_color));
    // Horizontal and static. A vertical scroll would cap the layout at
    // one line (see display_effective_lines), so it cannot be the
    // default for a two-line paused screen.
    g_strlcpy(cfg->paused_scroll_direction, "fromRightToLeft", sizeof(cfg->paused_scroll_direction));
    cfg->paused_scroll_speed = 0;

    cfg->finished_lines = 1;
    g_strlcpy(cfg->finished_text_size, "large", sizeof(cfg->finished_text_size));
    g_strlcpy(cfg->finished_line1, "TIME'S UP", sizeof(cfg->finished_line1));
    cfg->finished_line2[0] = '\0';
    cfg->finished_line3[0] = '\0';
    g_strlcpy(cfg->finished_bg, "#c00000", sizeof(cfg->finished_bg));
    g_strlcpy(cfg->finished_text_color, "#ffffff", sizeof(cfg->finished_text_color));
    g_strlcpy(cfg->finished_scroll_direction, "fromRightToLeft", sizeof(cfg->finished_scroll_direction));
    cfg->finished_scroll_speed    = 5;
    cfg->finished_display_seconds = 10;

    // No clip until one is picked, for either event.
    cfg->paused_clip.id     = -1;
    cfg->paused_clip.repeat = 0;
    cfg->paused_clip.volume = 100;
    cfg->resume_clip.id     = -1;
    cfg->resume_clip.repeat = 0;
    cfg->resume_clip.volume = 100;
    cfg->start_clip.id      = -1;
    cfg->start_clip.repeat  = 0; // play once
    cfg->start_clip.volume  = 100;
    cfg->finish_clip.id     = -1;
    cfg->finish_clip.repeat = 0;
    cfg->finish_clip.volume = 100;

    cfg->release_display_when_idle   = true;
    cfg->release_display_when_paused = false;
}

// --- JSON helpers -----------------------------------------------------
//
// Each of these leaves *dest untouched when the key is absent or the
// wrong type, which is what makes a config file written by an older
// version load cleanly: anything it didn't know about keeps the default
// local_config_defaults() already put there.

static void get_int(json_t* obj, const char* key, int* dest) {
    json_t* v = json_object_get(obj, key);
    if (json_is_integer(v))
        *dest = (int)json_integer_value(v);
    else if (json_is_number(v)) {
        // Via a named double rather than casting the call's result
        // directly - -Wbad-function-cast (which this project builds
        // with) rejects the shorter form.
        double d = json_number_value(v);
        *dest    = (int)d;
    }
}

static void get_bool(json_t* obj, const char* key, bool* dest) {
    json_t* v = json_object_get(obj, key);
    if (json_is_boolean(v))
        *dest = json_is_true(v);
}

static void get_str(json_t* obj, const char* key, char* dest, size_t destlen) {
    json_t* v = json_object_get(obj, key);
    if (json_is_string(v))
        g_strlcpy(dest, json_string_value(v), destlen);
}

static json_t* clip_to_json(const clip_cfg_t* c) {
    json_t* o = json_object();
    json_object_set_new(o, "id", json_integer(c->id));
    json_object_set_new(o, "name", json_string(c->name));
    json_object_set_new(o, "repeat", json_integer(c->repeat));
    json_object_set_new(o, "volume", json_integer(c->volume));
    return o;
}

static void get_clip(json_t* root, const char* key, clip_cfg_t* dest) {
    json_t* obj = json_object_get(root, key);
    if (!json_is_object(obj))
        return;
    get_int(obj, "id", &dest->id);
    get_str(obj, "name", dest->name, sizeof(dest->name));
    get_int(obj, "repeat", &dest->repeat);
    get_int(obj, "volume", &dest->volume);
}

void local_config_load(timer_config_t* cfg) {
    local_config_defaults(cfg);

    json_error_t jerr;
    json_t* root = json_load_file(LOCAL_CONFIG_PATH, 0, &jerr);
    if (!root) {
        // Expected on a fresh install - not worth an error log line.
        syslog(LOG_INFO, "local_config: no %s yet, using defaults", LOCAL_CONFIG_PATH);
        return;
    }

    // A config written before the two steps were separate carries a
    // single "step_seconds" - read it into both, then let the newer keys
    // override, so an upgrade keeps behaving exactly as it did.
    int legacy_step = cfg->step_up_seconds;
    get_int(root, "step_seconds", &legacy_step);
    cfg->step_up_seconds   = legacy_step;
    cfg->step_down_seconds = legacy_step;
    get_int(root, "step_up_seconds", &cfg->step_up_seconds);
    get_int(root, "step_down_seconds", &cfg->step_down_seconds);
    get_int(root, "default_seconds", &cfg->default_seconds);
    get_int(root, "max_seconds", &cfg->max_seconds);

    json_t* buttons = json_object_get(root, "buttons");
    if (json_is_array(buttons)) {
        size_t n = json_array_size(buttons);
        for (size_t i = 0; i < ASSIGNABLE_BUTTONS && i < n; i++) {
            json_t* b = json_array_get(buttons, i);
            if (!json_is_object(b))
                continue;
            char action_id[32] = {0};
            get_str(b, "action", action_id, sizeof(action_id));
            if (action_id[0])
                cfg->buttons[i].action = button_action_from_id(action_id);
            get_int(b, "value_seconds", &cfg->buttons[i].value_seconds);
        }
    }

    json_t* states = json_object_get(root, "states");
    if (json_is_array(states)) {
        size_t n = json_array_size(states);
        if (n > MAX_DISPLAY_STATES)
            n = MAX_DISPLAY_STATES;
        if (n > 0) {
            // A saved states[] replaces the defaults outright rather
            // than merging into them - otherwise deleting a threshold in
            // the UI could never actually remove it, since the default
            // would keep reappearing underneath.
            memset(cfg->states, 0, sizeof(cfg->states));
            cfg->state_count = (int)n;
            for (size_t i = 0; i < n; i++) {
                json_t* s = json_array_get(states, i);
                if (!json_is_object(s))
                    continue;
                cfg->states[i].at_seconds = (i == 0) ? -1 : 0;
                get_int(s, "at_seconds", &cfg->states[i].at_seconds);
                get_str(s, "bg_color", cfg->states[i].bg_color, sizeof(cfg->states[i].bg_color));
                get_str(s, "text_color", cfg->states[i].text_color, sizeof(cfg->states[i].text_color));
                get_str(s, "text", cfg->states[i].text, sizeof(cfg->states[i].text));
                cfg->states[i].clip.id     = -1;
                cfg->states[i].clip.repeat = 0;
                cfg->states[i].clip.volume = 100;
                get_clip(s, "clip", &cfg->states[i].clip);
                // states[0] is the normal state by definition, whatever
                // the file happens to say.
                if (i == 0)
                    cfg->states[0].at_seconds = -1;
            }
        }
    }

    // A single "paused_text" is what 0.1.13 wrote - read it into line one
    // so an upgrade keeps whatever was configured.
    get_str(root, "paused_text", cfg->paused_line1, sizeof(cfg->paused_line1));
    get_int(root, "paused_lines", &cfg->paused_lines);
    get_str(root, "paused_text_size", cfg->paused_text_size, sizeof(cfg->paused_text_size));
    get_str(root, "paused_line1", cfg->paused_line1, sizeof(cfg->paused_line1));
    get_str(root, "paused_line2", cfg->paused_line2, sizeof(cfg->paused_line2));
    get_str(root, "paused_line3", cfg->paused_line3, sizeof(cfg->paused_line3));
    get_str(root, "paused_bg", cfg->paused_bg, sizeof(cfg->paused_bg));
    get_str(root, "paused_text_color", cfg->paused_text_color, sizeof(cfg->paused_text_color));
    get_str(root,
            "paused_scroll_direction",
            cfg->paused_scroll_direction,
            sizeof(cfg->paused_scroll_direction));
    get_int(root, "paused_scroll_speed", &cfg->paused_scroll_speed);

    // Versions before this one stored the finished message as one
    // "finished_text" - read it into line one for the same reason.
    get_str(root, "finished_text", cfg->finished_line1, sizeof(cfg->finished_line1));
    get_int(root, "finished_lines", &cfg->finished_lines);
    get_str(root, "finished_text_size", cfg->finished_text_size, sizeof(cfg->finished_text_size));
    get_str(root, "finished_line1", cfg->finished_line1, sizeof(cfg->finished_line1));
    get_str(root, "finished_line2", cfg->finished_line2, sizeof(cfg->finished_line2));
    get_str(root, "finished_line3", cfg->finished_line3, sizeof(cfg->finished_line3));
    get_str(root, "finished_bg", cfg->finished_bg, sizeof(cfg->finished_bg));
    get_str(root, "finished_text_color", cfg->finished_text_color, sizeof(cfg->finished_text_color));
    get_str(root,
            "finished_scroll_direction",
            cfg->finished_scroll_direction,
            sizeof(cfg->finished_scroll_direction));
    get_int(root, "finished_scroll_speed", &cfg->finished_scroll_speed);
    get_int(root, "finished_display_seconds", &cfg->finished_display_seconds);

    // Clamp the two line counts: they index arrays and pick the text
    // size, so a hand-edited file must not be able to put them out of
    // range.
    if (cfg->paused_lines < 1 || cfg->paused_lines > 3)
        cfg->paused_lines = 1;
    if (cfg->finished_lines < 1 || cfg->finished_lines > 3)
        cfg->finished_lines = 1;
    g_strlcpy(cfg->finished_text_size, "large", sizeof(cfg->finished_text_size));

    get_int(root, "clip_id", &cfg->finish_clip.id);
    get_str(root, "clip_name", cfg->finish_clip.name, sizeof(cfg->finish_clip.name));
    get_int(root, "clip_repeat", &cfg->finish_clip.repeat);
    get_int(root, "clip_volume", &cfg->finish_clip.volume);

    get_clip(root, "paused_clip", &cfg->paused_clip);
    get_clip(root, "resume_clip", &cfg->resume_clip);
    get_clip(root, "start_clip", &cfg->start_clip);
    get_clip(root, "finish_clip", &cfg->finish_clip);

    get_bool(root, "release_display_when_idle", &cfg->release_display_when_idle);
    get_bool(root, "release_display_when_paused", &cfg->release_display_when_paused);

    json_decref(root);
}

bool local_config_save(const timer_config_t* cfg) {
    if (g_mkdir_with_parents(LOCAL_CONFIG_DIR, 0700) != 0) {
        syslog(LOG_ERR, "local_config: could not create %s directory", LOCAL_CONFIG_DIR);
        return false;
    }

    json_t* root = json_object();
    json_object_set_new(root, "step_up_seconds", json_integer(cfg->step_up_seconds));
    json_object_set_new(root, "step_down_seconds", json_integer(cfg->step_down_seconds));
    json_object_set_new(root, "default_seconds", json_integer(cfg->default_seconds));
    json_object_set_new(root, "max_seconds", json_integer(cfg->max_seconds));

    json_t* buttons = json_array();
    for (int i = 0; i < ASSIGNABLE_BUTTONS; i++) {
        json_t* b = json_object();
        json_object_set_new(b, "action", json_string(button_action_id(cfg->buttons[i].action)));
        json_object_set_new(b, "value_seconds", json_integer(cfg->buttons[i].value_seconds));
        json_array_append_new(buttons, b);
    }
    json_object_set_new(root, "buttons", buttons);

    json_t* states = json_array();
    for (int i = 0; i < cfg->state_count && i < MAX_DISPLAY_STATES; i++) {
        json_t* s = json_object();
        json_object_set_new(s, "at_seconds", json_integer(cfg->states[i].at_seconds));
        json_object_set_new(s, "bg_color", json_string(cfg->states[i].bg_color));
        json_object_set_new(s, "text_color", json_string(cfg->states[i].text_color));
        json_object_set_new(s, "text", json_string(cfg->states[i].text));
        json_object_set_new(s, "clip", clip_to_json(&cfg->states[i].clip));
        json_array_append_new(states, s);
    }
    json_object_set_new(root, "states", states);

    json_object_set_new(root, "paused_lines", json_integer(cfg->paused_lines));
    json_object_set_new(root, "paused_text_size", json_string(cfg->paused_text_size));
    json_object_set_new(root, "paused_line1", json_string(cfg->paused_line1));
    json_object_set_new(root, "paused_line2", json_string(cfg->paused_line2));
    json_object_set_new(root, "paused_line3", json_string(cfg->paused_line3));
    json_object_set_new(root, "paused_bg", json_string(cfg->paused_bg));
    json_object_set_new(root, "paused_text_color", json_string(cfg->paused_text_color));
    json_object_set_new(
        root, "paused_scroll_direction", json_string(cfg->paused_scroll_direction));
    json_object_set_new(root, "paused_scroll_speed", json_integer(cfg->paused_scroll_speed));

    json_object_set_new(root, "finished_lines", json_integer(cfg->finished_lines));
    json_object_set_new(root, "finished_text_size", json_string(cfg->finished_text_size));
    json_object_set_new(root, "finished_line1", json_string(cfg->finished_line1));
    json_object_set_new(root, "finished_line2", json_string(cfg->finished_line2));
    json_object_set_new(root, "finished_line3", json_string(cfg->finished_line3));
    json_object_set_new(root, "finished_bg", json_string(cfg->finished_bg));
    json_object_set_new(root, "finished_text_color", json_string(cfg->finished_text_color));
    json_object_set_new(
        root, "finished_scroll_direction", json_string(cfg->finished_scroll_direction));
    json_object_set_new(root, "finished_scroll_speed", json_integer(cfg->finished_scroll_speed));
    json_object_set_new(root, "finished_display_seconds", json_integer(cfg->finished_display_seconds));

    json_object_set_new(root, "paused_clip", clip_to_json(&cfg->paused_clip));
    json_object_set_new(root, "resume_clip", clip_to_json(&cfg->resume_clip));
    json_object_set_new(root, "start_clip", clip_to_json(&cfg->start_clip));
    json_object_set_new(root, "finish_clip", clip_to_json(&cfg->finish_clip));

    json_object_set_new(root, "release_display_when_idle", json_boolean(cfg->release_display_when_idle));
    json_object_set_new(
        root, "release_display_when_paused", json_boolean(cfg->release_display_when_paused));

    char tmp_path[256];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", LOCAL_CONFIG_PATH);

    // json_dump_file writes and closes its own fd; the rename afterwards
    // is what makes this atomic - rename() replaces the destination in
    // one filesystem operation, so a crash before it leaves the old file
    // untouched and a crash after leaves the new one whole. There's no
    // window in which the real path points at a half-written file.
    int rc = json_dump_file(root, tmp_path, JSON_INDENT(2));
    json_decref(root);
    if (rc != 0) {
        syslog(LOG_ERR, "local_config: failed to write %s", tmp_path);
        return false;
    }

    if (g_rename(tmp_path, LOCAL_CONFIG_PATH) != 0) {
        syslog(LOG_ERR, "local_config: failed to move %s into place as %s", tmp_path, LOCAL_CONFIG_PATH);
        return false;
    }

    return true;
}
