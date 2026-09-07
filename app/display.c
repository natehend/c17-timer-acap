#include "display.h"

#include <glib.h>
#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#include "local_vapix.h"
#include "timer.h"

#define DISPLAY_BASE "/config/rest/speaker-display-notification/v1"

static bool g_last_ok = true;
static char g_last_error[256] = {0};

// A running countdown posts once a second. If the display is refusing
// those - wrong device, API not present, permissions - that would be one
// ERR line per second forever. Log the first failure in full, then at
// most one line a minute after that, and log the recovery too so the log
// says when it started working again rather than just going quiet.
static gint64 g_last_error_log_us = 0;
#define ERROR_LOG_INTERVAL_US (60 * 1000000LL)

static void note_failure(const char* what, const char* err) {
    bool was_ok = g_last_ok;
    g_last_ok   = false;
    snprintf(g_last_error, sizeof(g_last_error), "%s", err ? err : "unknown error");

    gint64 now = g_get_monotonic_time();
    if (was_ok || now - g_last_error_log_us > ERROR_LOG_INTERVAL_US) {
        g_last_error_log_us = now;
        syslog(LOG_ERR,
              "display: %s failed: %s "
              "(is this a C17 Series display speaker? the Speaker display notification "
              "API only exists on devices with a display)",
              what,
              g_last_error);
    }
}

static void note_success(void) {
    if (!g_last_ok)
        syslog(LOG_INFO, "display: recovered, notifications are being accepted again");
    g_last_ok        = true;
    g_last_error[0]  = '\0';
}

bool display_last_ok(void) {
    return g_last_ok;
}

const char* display_last_error(void) {
    return g_last_error;
}

// The API validates textColor/backgroundColor against ^#[0-9a-fA-F]{6}$
// and rejects anything else outright, so a color that never made it
// through the settings form intact (empty, truncated, missing its hash)
// would fail every single post. Normalize here instead: this is the last
// point before the wire, so it catches a bad value however it got in -
// including from a hand-edited config.json.
static void sanitize_color(const char* in, const char* fallback, char* out, size_t outlen) {
    if (in && in[0] == '#' && strlen(in) == 7) {
        bool hex = true;
        for (int i = 1; i < 7; i++)
            if (!g_ascii_isxdigit(in[i]))
                hex = false;
        if (hex) {
            g_strlcpy(out, in, outlen);
            return;
        }
    }
    g_strlcpy(out, fallback, outlen);
}

static const char* sanitize_text_size(const char* in) {
    if (in) {
        if (strcmp(in, "small") == 0 || strcmp(in, "medium") == 0 || strcmp(in, "large") == 0)
            return in;
    }
    return "large";
}

static const char* sanitize_scroll_direction(const char* in) {
    if (in) {
        if (strcmp(in, "fromRightToLeft") == 0 || strcmp(in, "fromLeftToRight") == 0 ||
            strcmp(in, "fromBottomToTop") == 0)
            return in;
    }
    return "fromRightToLeft";
}

bool display_show(const char* message,
                  const char* text_color,
                  const char* bg_color,
                  const char* text_size,
                  const char* scroll_direction,
                  int scroll_speed,
                  int duration_ms) {
    char text_c[8], bg_c[8];
    sanitize_color(text_color, "#ffffff", text_c, sizeof(text_c));
    sanitize_color(bg_color, "#000000", bg_c, sizeof(bg_c));

    if (scroll_speed < 0)
        scroll_speed = 0;
    if (scroll_speed > 10)
        scroll_speed = 10;

    // The API caps message at 1000 characters. Truncate rather than let
    // the device reject the whole post over a long template.
    char msg[1024];
    g_strlcpy(msg, message ? message : "", sizeof(msg));
    if (strlen(msg) > 1000)
        msg[1000] = '\0';

    // Built with jansson rather than snprintf so a message containing a
    // quote, backslash or non-ASCII character is escaped correctly
    // instead of producing invalid JSON the device would reject.
    json_t* data = json_object();
    json_object_set_new(data, "message", json_string(msg));
    json_object_set_new(data, "textColor", json_string(text_c));
    json_object_set_new(data, "backgroundColor", json_string(bg_c));
    json_object_set_new(data, "textSize", json_string(sanitize_text_size(text_size)));
    json_object_set_new(data, "scrollDirection", json_string(sanitize_scroll_direction(scroll_direction)));
    json_object_set_new(data, "scrollSpeed", json_integer(scroll_speed));

    if (duration_ms > 0) {
        json_t* duration = json_object();
        json_object_set_new(duration, "type", json_string("time"));
        json_object_set_new(duration, "value", json_integer(duration_ms));
        json_object_set_new(data, "duration", duration);
    }
    // duration omitted entirely = indefinite. Used for the finished
    // message when its configured display time is 0 ("leave it up").

    json_t* root = json_object();
    json_object_set_new(root, "data", data);
    char* body = json_dumps(root, JSON_COMPACT);
    json_decref(root);
    if (!body) {
        note_failure("notification", "could not serialize request body");
        return false;
    }

    char err[256] = {0};
    char* resp    = local_vapix_post_json(DISPLAY_BASE "/simple", body, err, sizeof(err));
    free(body);

    if (!resp) {
        note_failure("notification", err);
        return false;
    }
    free(resp);
    note_success();
    return true;
}

bool display_stop(void) {
    char err[256] = {0};
    char* resp    = local_vapix_post_json(DISPLAY_BASE "/stop", "{\"data\":{}}", err, sizeof(err));
    if (!resp) {
        note_failure("stop", err);
        return false;
    }
    free(resp);
    note_success();
    return true;
}

// How many rows the display can actually show, measured on hardware.
//
// The text size decides it: large fits one row, medium two, small three.
// Asking for more than that does not get you more rows, it just loses
// the ones that do not fit - so the count is clamped here rather than
// sent and quietly dropped.
//
// A vertical scroll is a different shape entirely: it takes a single
// line and inserts its own breaks between words to fit the width, so
// configured rows mean nothing to it. At large, "00:14 REMAINING" comes
// out as two rows because REMAINING is the longest word that fits; at
// medium the whole thing fits on one.
//
// Clamped here, at the last point before the wire, so it holds however
// the config was set - the settings form, the API, or a hand-edited
// config.json.
int display_effective_lines(const char* text_size, const char* scroll_direction, int configured) {
    int cap;
    if (strcmp(sanitize_scroll_direction(scroll_direction), "fromBottomToTop") == 0) {
        cap = 1;
    } else {
        const char* size = sanitize_text_size(text_size);
        cap = (strcmp(size, "small") == 0) ? 3 : (strcmp(size, "medium") == 0) ? 2 : 1;
    }
    if (configured < 1)
        return 1;
    return configured > cap ? cap : configured;
}

// The display treats every newline in `message` as a row break, so the
// only ones that should ever reach it are the separators build_lines
// puts BETWEEN configured lines. A stray CR or LF inside a single line
// silently adds a row: with one line configured the text then renders
// against the top of the screen with an empty row beneath it, which
// reads as "the text is not vertically centered".
//
// A text input cannot type a newline, but url_decode turns %0A into a
// real one, so an API call or a hand-edited config.json can put one in
// the stored template. Fold control characters to spaces and trim the
// ends, which makes the configured line count the only thing that
// decides how many rows the display gets.
//
// Returns true if it changed anything, so the caller can say so once in
// the log rather than leaving a silently-corrected config invisible.
static bool sanitize_line(char* s) {
    bool changed = false;
    for (char* p = s; *p; p++) {
        if (*p == '\n' || *p == '\r' || *p == '\t' || *p == '\v' || *p == '\f') {
            *p      = ' ';
            changed = true;
        }
    }
    size_t before = strlen(s);
    g_strstrip(s);
    return changed || strlen(s) != before;
}

static void warn_stripped_once(const char* where) {
    static bool warned = false;
    if (warned)
        return;
    warned = true;
    syslog(LOG_WARNING,
          "display: %s contained a line break or stray whitespace; it was folded to a "
          "space so the configured line count still decides the number of rows",
          where);
}

// Builds a multi-line display message from up to three templates.
//
// `lines` is how many rows the display gets. Only that many templates
// are used - text left in a line beyond the count is ignored, not folded
// onto the last row. It stays in the config, so raising the count brings
// it straight back.
//
// Empty templates within the count are skipped, so a gap in the middle
// doesn't leave a blank row, and each rendered line is stripped of its
// own line breaks first - so the message carries exactly (lines - 1)
// newlines at most, never more.
//
// Text size is NOT decided here: on a vertical scroll the display adds
// its own line breaks to fit, so how many rows appear is its decision -
// the configured size is what governs the layout, and it is passed
// through untouched.
static void build_lines(const char* const templates[3],
                        int lines,
                        int remaining,
                        char* message,
                        size_t message_len) {
    if (lines < 1)
        lines = 1;
    if (lines > 3)
        lines = 3;

    message[0] = '\0';
    for (int i = 0; i < lines; i++) {
        char rendered[256];
        timer_format_template(templates[i], remaining, rendered, sizeof(rendered));
        if (sanitize_line(rendered))
            warn_stripped_once("a configured line");
        if (!rendered[0])
            continue;
        if (message[0])
            g_strlcat(message, "\n", message_len);
        g_strlcat(message, rendered, message_len);
    }
}

bool display_resolve(const timer_config_t* cfg, display_frame_t* out) {
    memset(out, 0, sizeof(*out));
    if (!cfg)
        return false;

    timer_state_t state = timer_get_state();

    if (state == TIMER_FINISHED) {
        // Posted once and then left alone, so unlike the countdown it is
        // free to scroll.
        out->active = true;
        const char* templates[3] = {cfg->finished_line1, cfg->finished_line2, cfg->finished_line3};
        build_lines(templates,
                    display_effective_lines(cfg->finished_text_size,
                                            cfg->finished_scroll_direction,
                                            cfg->finished_lines),
                    0,
                    out->message,
                    sizeof(out->message));
        g_strlcpy(out->text_size, cfg->finished_text_size, sizeof(out->text_size));
        g_strlcpy(out->text_color, cfg->finished_text_color, sizeof(out->text_color));
        g_strlcpy(out->bg_color, cfg->finished_bg, sizeof(out->bg_color));
        g_strlcpy(out->scroll_direction, cfg->finished_scroll_direction, sizeof(out->scroll_direction));
        out->scroll_speed = cfg->finished_scroll_speed;
        // Asked to stay up two seconds longer than the timer will
        // actually leave it there, so the state machine's own clear is
        // what removes it rather than the notification expiring first.
        out->duration_ms = cfg->finished_display_seconds * 1000 + 2000;
        return true;
    }

    if (state == TIMER_PAUSED && cfg->release_display_when_paused)
        return false;

    if (state == TIMER_RUNNING || state == TIMER_PAUSED) {
        int idx = timer_display_state_index();
        if (idx < 0 || idx >= cfg->state_count)
            idx = 0;
        const display_state_t* st = &cfg->states[idx];

        out->active = true;
        int remaining = timer_remaining_seconds();

        if (state == TIMER_PAUSED) {
            // A paused timer says so in words, with colors of its own so
            // it is unambiguous whichever threshold the countdown had
            // reached when it stopped.
            const char* templates[3] = {cfg->paused_line1, cfg->paused_line2, cfg->paused_line3};
            build_lines(templates,
                        display_effective_lines(cfg->paused_text_size,
                                                cfg->paused_scroll_direction,
                                                cfg->paused_lines),
                        remaining,
                        out->message,
                        sizeof(out->message));
            g_strlcpy(out->text_size, cfg->paused_text_size, sizeof(out->text_size));
            g_strlcpy(out->text_color, cfg->paused_text_color, sizeof(out->text_color));
            g_strlcpy(out->bg_color, cfg->paused_bg, sizeof(out->bg_color));
            g_strlcpy(out->scroll_direction,
                      cfg->paused_scroll_direction,
                      sizeof(out->scroll_direction));
            out->scroll_speed = cfg->paused_scroll_speed;
            out->duration_ms  = 0; // no expiry - a pause lasts until something changes
            return true;
        } else {
            timer_format_template(st->text, remaining, out->message, sizeof(out->message));
            if (sanitize_line(out->message))
                warn_stripped_once("the countdown text");
        }
        g_strlcpy(out->text_color, st->text_color, sizeof(out->text_color));
        g_strlcpy(out->bg_color, st->bg_color, sizeof(out->bg_color));
        // The countdown has no size or scroll settings of its own - see
        // the note in local_config.h. Always large, always static.
        g_strlcpy(out->text_size, "large", sizeof(out->text_size));
        g_strlcpy(out->scroll_direction, "fromRightToLeft", sizeof(out->scroll_direction));
        out->scroll_speed = 0;
        // A running timer re-posts every second, so a short hold that
        // overlaps the next post is right for it. A PAUSED timer posts
        // once and then nothing happens at all - so the same short hold
        // would let the notification lapse and blank the screen a few
        // seconds into the pause, which looked exactly like the display
        // being released. Paused frames are posted with no expiry
        // instead, and stay up until something changes.
        out->duration_ms = (state == TIMER_RUNNING) ? DISPLAY_HOLD_MS : 0;
        return true;
    }

    // TIMER_IDLE: nothing to show. The caller releases the screen so
    // AAMP, the Rule engine and anything else can use it - see the
    // coexistence note at the top of this file.
    return false;
}
