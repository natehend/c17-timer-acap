/**
 * local_config - the app's real configuration (timer behavior, button
 * assignments, display colors/thresholds, the finished message, the
 * media clip), persisted as a plain JSON file under this ACAP's
 * localdata/ directory rather than AXParameter.
 *
 * Why not AXParameter: any paramConfig field always shows up somewhere
 * in the native ACAP Settings dialog - "hidden:" only tucks it behind
 * the "Show hidden parameters" toggle, it doesn't remove it. The only
 * way to keep a setting out of that dialog entirely is not to declare it
 * in manifest.json at all, which also means AXParameter can't store it.
 * Since all of these already have a proper editing UI on the web
 * Settings page, they live in their own file instead. (Same reasoning,
 * and same shape, as the C8310 Customizer's localdata/devices.json.)
 *
 * localdata/ is the Axis-documented location for exactly this: content
 * there survives an application upgrade, unlike the rest of the package
 * directory which gets replaced. Their docs warn against *continuous*
 * writes to it (it's flash with a limited per-block write count) - not a
 * concern here, since this is only written when someone saves the
 * settings page, a human-paced action. The running timer's own state is
 * never written here.
 */
#ifndef LOCAL_CONFIG_H
#define LOCAL_CONFIG_H

#include <stdbool.h>

#define LOCAL_CONFIG_DIR  "localdata"
#define LOCAL_CONFIG_PATH LOCAL_CONFIG_DIR "/config.json"

// One "normal" display state plus up to 5 color-change thresholds.
// Bounded rather than unlimited because each threshold also gets its own
// Rule engine condition slot (see event.h) - those are declared once at
// startup and can't grow at runtime, so the config has to agree on a
// maximum with them.
#define MAX_DISPLAY_STATES 6
#define MAX_THRESHOLDS     (MAX_DISPLAY_STATES - 1)

// Buttons 1, 2, 3 and Mute are freely assignable; Volume up/down are
// fixed to +/- one step, so they have no entry here.
#define ASSIGNABLE_BUTTONS 4

typedef enum {
    BTN_ACTION_NONE = 0,
    BTN_ACTION_START_PAUSE,
    BTN_ACTION_START,
    BTN_ACTION_PAUSE,
    BTN_ACTION_STOP_RESET,
    BTN_ACTION_QUICK, // starts a timer of value_seconds immediately
    BTN_ACTION_ADD,   // adds value_seconds to the timer
    BTN_ACTION_TRIGGER, // fires only the Rule engine button event
    BTN_ACTION_FINISH,  // jumps to 0 and plays the finished message
    BTN_ACTION_COUNT
} button_action_t;

// Stable strings used in the JSON file and in the settings form's
// <select> values, so renaming a label in the UI can never silently
// change what a saved config means.
const char* button_action_id(button_action_t a);
button_action_t button_action_from_id(const char* id);
// Human-readable label for the settings page dropdown.
const char* button_action_label(button_action_t a);
// True if this action's value_seconds field is meaningful (and so should
// be shown in the settings form).
bool button_action_uses_value(button_action_t a);

typedef struct {
    button_action_t action;
    int value_seconds; // only meaningful for QUICK / ADD
} button_cfg_t;

// One audio clip selection. Used for the sound a timer starts with, the
// sound it finishes with, and the sound each threshold plays as it
// is crossed.
typedef struct {
    int id;        // -1 = play nothing
    char name[96]; // cached label, so the settings page can still name the
                   // clip if the device is slow to answer a list request
    int repeat;    // 0 = play once, -1 = repeat forever
    int volume;    // 0-1000, as a percentage
} clip_cfg_t;

typedef struct {
    // -1 marks the "normal" state (states[0]), which applies from the
    // start of the countdown. Every other entry means "once this many
    // seconds or fewer remain, switch to these colors".
    int at_seconds;
    char bg_color[8];    // "#RRGGBB", the format the display API requires
    char text_color[8];  // "#RRGGBB"
    char text[128];      // template; "{time}" is substituted (see timer_format_template)

    // Played as the countdown crosses into this state. Only meaningful
    // for the thresholds (states[1..]); states[0] is entered by starting
    // the timer, which has its own start_clip.
    clip_cfg_t clip;
} display_state_t;


typedef struct {
    // --- Timer behavior ---
    // Volume up and Volume down carry their own amounts rather than
    // sharing one: a five-minute nudge up and a one-minute trim down is
    // a perfectly reasonable way to want a timer to behave, and a single
    // shared step can't express it.
    int step_up_seconds;
    int step_down_seconds;
    int default_seconds; // the duration the timer holds at startup
    int max_seconds;     // upper clamp for Volume up / add-time

    // --- Buttons 1, 2, 3, Mute (in that order) ---
    button_cfg_t buttons[ASSIGNABLE_BUTTONS];

    // --- Countdown display ---
    //
    // The countdown has no text-size or scrolling settings of its own: it
    // is always large and always static. Scrolling genuinely cannot work
    // for it (a running countdown re-posts every second and every post
    // restarts the notification's animation), and offering a size choice
    // for a line that is only ever a few digits was clutter. The finished
    // message is posted once, so it keeps its own full set below.
    int state_count; // >= 1; states[0] is the normal state
    display_state_t states[MAX_DISPLAY_STATES];

    // Text shown while a timer is paused, as two lines. Colors stay
    // whatever the countdown had reached; only the wording changes, to
    // say it is not running. Both lines support the usual placeholders
    // plus {paused_at}, {paused_at_12} and {paused_at_24} - the clock
    // time the pause happened. Leave line two empty for a single line.
    //
    // --- Paused message ---
    //
    // Lines are joined with newlines into the one "message" the display
    // API takes. A vertical scroll direction (fromBottomToTop) is what
    // selects the display's multi-line layout; a horizontal one runs the
    // lines together on a single row.
    //
    // `lines` decides how many of the line fields are used - text in a
    // line beyond it is ignored, though it stays in the config.
    //
    // Text size is its own setting, NOT derived from the line count.
    //
    // The two interact differently depending on direction, both confirmed
    // on hardware: a horizontal scroll keeps the configured lines as rows
    // and left-aligns them, while a vertical scroll re-wraps the text to
    // fit - so there the size is what decides how many rows appear. The
    // line count governs what text is sent either way.
    //
    // Unlike the countdown this frame is posted once, so it is free to
    // scroll, and it carries its own colors rather than inheriting
    // whichever threshold the countdown had reached.
    int paused_lines; // 1-3
    char paused_text_size[8]; // "small" | "medium" | "large"
    char paused_line1[128];
    char paused_line2[128];
    char paused_line3[128];
    char paused_bg[8];
    char paused_text_color[8];
    char paused_scroll_direction[24];
    int paused_scroll_speed;
    clip_cfg_t paused_clip; // played when a timer is paused
    clip_cfg_t resume_clip; // played when a paused timer is resumed

    // --- Finished message ---
    //
    // Same shape as the paused message above, plus how long it stays up.
    int finished_lines; // 1-3
    char finished_text_size[8];
    char finished_line1[128];
    char finished_line2[128];
    char finished_line3[128];
    char finished_bg[8];
    char finished_text_color[8];
    char finished_scroll_direction[24];
    int finished_scroll_speed;
    int finished_display_seconds;

    // --- Sounds ---
    clip_cfg_t start_clip;  // played when a timer starts
    clip_cfg_t finish_clip; // played when a timer reaches zero

    // Whether to hand the display back (POST /stop) once the timer has
    // nothing left to show. The Speaker display notification API
    // explicitly does not support coexistence - whoever wrote to it last
    // owns the screen - so leaving this on is what lets AAMP, the rule
    // engine and anything else use the display between timers.
    bool release_display_when_idle;

    // Whether a *paused* timer also counts as "nothing to show". Off by
    // default: pausing is a deliberate hold, and blanking the screen the
    // moment someone pauses reads as the timer having been canceled.
    // A paused frame is posted with no expiry so it genuinely stays up,
    // rather than lapsing a few seconds later.
    bool release_display_when_paused;
} timer_config_t;

// Loads LOCAL_CONFIG_PATH into *cfg. Any field missing from the file
// keeps its built-in default, so a config written by an older version of
// this app upgrades cleanly instead of losing settings. A missing file
// entirely (fresh install) just yields the defaults.
void local_config_load(timer_config_t* cfg);

// Fills *cfg with the built-in defaults without touching the filesystem.
void local_config_defaults(timer_config_t* cfg);

// Atomically writes *cfg to LOCAL_CONFIG_PATH (temp file, then rename -
// rename() is atomic on the same filesystem, so a crash or power loss
// mid-write can never leave a half-written config at the real path).
bool local_config_save(const timer_config_t* cfg);

#endif // LOCAL_CONFIG_H
