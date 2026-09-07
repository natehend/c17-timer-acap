#include "webui.h"

#include <ctype.h>
#include <gio/gio.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#include "app_config.h"
#include "axparam_util.h"
#include "buttons.h"
#include "display.h"
#include "mediaclip.h"
#include "axis_logo.h"
#include "panel_image.h"
#include "timer.h"

static struct {
    AXParameter* axparam;
    timer_config_t* cfg;
    void (*on_config_saved)(void);
    int port;
} g;

/***** Small string helpers *****************************************************/

// Escapes a string for safe use in HTML text or an attribute value.
// Every user-typed value on these pages (display text, clip names) goes
// through this - without it an apostrophe in "Nate's timer" would close
// a value='...' attribute early and break the markup.
static void html_escape(const char* in, char* out, size_t outlen) {
    size_t o = 0;
    if (!in) {
        out[0] = '\0';
        return;
    }
    for (size_t i = 0; in[i] != '\0' && o + 7 < outlen; i++) {
        switch (in[i]) {
        case '&':
            o += (size_t)g_strlcpy(out + o, "&amp;", outlen - o);
            break;
        case '\'':
            o += (size_t)g_strlcpy(out + o, "&#39;", outlen - o);
            break;
        case '"':
            o += (size_t)g_strlcpy(out + o, "&quot;", outlen - o);
            break;
        case '<':
            o += (size_t)g_strlcpy(out + o, "&lt;", outlen - o);
            break;
        case '>':
            o += (size_t)g_strlcpy(out + o, "&gt;", outlen - o);
            break;
        default:
            out[o++] = in[i];
        }
    }
    out[o] = '\0';
}

static void url_decode(const char* in, char* out, size_t outlen) {
    size_t o = 0;
    for (size_t i = 0; in[i] != '\0' && o + 1 < outlen; i++) {
        if (in[i] == '+') {
            out[o++] = ' ';
        } else if (in[i] == '%' && isxdigit((unsigned char)in[i + 1]) &&
                  isxdigit((unsigned char)in[i + 2])) {
            char hex[3] = {in[i + 1], in[i + 2], '\0'};
            out[o++]    = (char)strtol(hex, NULL, 16);
            i += 2;
        } else {
            out[o++] = in[i];
        }
    }
    out[o] = '\0';
}

// Finds `key` in a "k=v&k2=v2" string (a query string or a
// form-urlencoded POST body - same shape) and writes its URL-decoded
// value into out. Returns true if the key was present at all, which is
// distinct from its value being empty: the settings forms rely on that
// difference to tell "left blank" from "not part of this form".
static bool query_get(const char* qs, const char* key, char* out, size_t outlen) {
    out[0] = '\0';
    if (!qs)
        return false;

    size_t keylen = strlen(key);
    const char* p = qs;
    while (p && *p) {
        const char* amp = strchr(p, '&');
        size_t seglen   = amp ? (size_t)(amp - p) : strlen(p);
        if (seglen >= keylen + 1 && p[keylen] == '=' && strncmp(p, key, keylen) == 0) {
            char raw[512];
            size_t vlen = seglen - keylen - 1;
            if (vlen >= sizeof(raw))
                vlen = sizeof(raw) - 1;
            memcpy(raw, p + keylen + 1, vlen);
            raw[vlen] = '\0';
            url_decode(raw, out, outlen);
            return true;
        }
        p = amp ? amp + 1 : NULL;
    }
    return false;
}

// Accepts either a plain number of seconds ("300") or a clock-style
// duration ("5:00", "1:05:00"), because both are natural things to type
// into a duration box or an API call, and refusing one would just be a
// papercut.
static int parse_duration(const char* s) {
    if (!s || !s[0])
        return 0;

    bool negative = (s[0] == '-');
    if (negative)
        s++;

    if (!strchr(s, ':'))
        return negative ? -atoi(s) : atoi(s); // NOLINT

    int parts[3]  = {0, 0, 0};
    int n         = 0;
    const char* p = s;
    while (p && *p && n < 3) {
        parts[n++]      = atoi(p); // NOLINT
        const char* col = strchr(p, ':');
        p               = col ? col + 1 : NULL;
    }

    int total;
    if (n == 3)
        total = parts[0] * 3600 + parts[1] * 60 + parts[2];
    else if (n == 2)
        total = parts[0] * 60 + parts[1];
    else
        total = parts[0];

    return negative ? -total : total;
}

/***** Shared page chrome *******************************************************/
//
// One CSS block for every page. The visual language stays close to the
// C8310 Customizer - same neutral ground, white rounded plates, uppercase
// gray section labels, var(--accent) accent - but the shell is now a left
// navigation column plus a content area whose cards flow horizontally and
// wrap, which is also how AXIS Audio Manager Pro's own console is laid
// out.
//
// Appended with g_string_append(), never g_string_append_printf(), so the
// many % characters in these rules don't have to be doubled up - an easy
// way to corrupt a stylesheet by accident.
static const char PAGE_CSS[] =
    "<style>"
    // ---------------------------------------------------------------
    // Palette, ported from the AAMP API Request Builder ACAP so the two
    // apps read as one family: Axis yellow rather than blue, and the
    // same light/dark values, taken from the Axis device's own admin UI.
    //
    // Everything the UI paints goes through these. The display preview
    // deliberately does NOT - its frame is always black and its screen
    // colors come from the profile being previewed, so it looks the same
    // in both themes. That is the whole point of a preview.
    // ---------------------------------------------------------------
    ":root{"
    "--accent:#ffcc33;"
    "--accent-dark:#e6b800;"
    // Accent used as TEXT rather than as a fill. Yellow on white is
    // unreadable, so light mode steps down to the darker tone and only
    // dark mode uses the bright one.
    "--accent-text:#e6b800;"
    "--text-on-accent:#242424;"
    "--accent-tint:rgba(255,204,51,.22);"
    "--accent-wash:rgba(255,204,51,.10);"
    "--focus-ring:rgba(255,204,51,.45);"
    "--hit-hover:rgba(255,204,51,.30);"
    "--hit-active:rgba(255,204,51,.48);"
    "--nav-active-text:#242424;"
    "--border:#e0e0e0;"
    "--divider:#eeeeee;"
    "--bg-page:#ffffff;"
    // The savebar fades the page out from transparent; a bare
    // `transparent` would fade through gray, so the zero-alpha form of
    // the page color is spelled out per theme.
    "--bg-page-fade:rgba(255,255,255,0);"
    "--bg-surface:#fcfcfc;"
    "--bg-soft:#ebebeb;"
    "--bg-code:#f5f5f5;"
    "--field-bg:#fcfcfc;"
    // A button has to read against the card it sits on, so it gets
    // its own tone rather than sharing --bg-surface with the card.
    "--btn-bg:#ffffff;"
    "--btn-border:#e0e0e0;"
    "--text-primary:#242424;"
    "--text-muted:#616161;"
    "--shadow-card:rgba(0,0,0,.10);"
    "--danger:#b3261e;"
    "--danger-border:#f0d7d3;"
    "--danger-soft:#fdecea;"
    "--warn-bg:#fdf0e3;"
    "--warn-text:#8a4b12;"
    "}"
    "html.dark-mode{"
    "--accent-text:#ffcc33;"
    "--nav-active-text:#ffcc33;"
    "--border:#3a3a3a;"
    "--divider:#333333;"
    "--bg-page:#1f1f1f;"
    "--bg-page-fade:rgba(31,31,31,0);"
    "--bg-surface:#3d3d3d;"
    "--bg-soft:#292929;"
    "--bg-code:#141414;"
    "--field-bg:#292929;"
    "--btn-bg:#2f2f2f;"
    "--btn-border:#565656;"
    "--text-primary:#d6d6d6;"
    "--text-muted:#adadad;"
    "--shadow-card:rgba(0,0,0,.55);"
    "--danger:#ff6259;"
    "--danger-border:rgba(255,98,89,.35);"
    "--danger-soft:rgba(255,98,89,.15);"
    "--warn-bg:rgba(255,204,51,.14);"
    "--warn-text:#ffcc33;"
    "}"
    // Native controls - dropdowns, spinners, the color picker's own
    // chrome - follow the theme too.
    "html.dark-mode{color-scheme:dark}"
    // Floats in the top right of the viewport, the same placement and
    // shape the Request Builder uses. Fixed rather than in the nav so it
    // stays reachable on a long settings page.
    ".themetoggle{position:fixed;top:1em;right:1em;z-index:2500;width:2.3em;height:2.3em;"
    "padding:0;margin:0;border-radius:50%;border:1px solid var(--border);"
    "background:var(--bg-surface);color:var(--text-muted);display:inline-flex;"
    "align-items:center;justify-content:center;cursor:pointer;"
    "box-shadow:0 1px 4px var(--shadow-card)}"
    ".themetoggle:hover{background:var(--bg-soft);color:var(--accent-text)}"
    ".themetoggle svg{width:1.25em;height:1.25em}"
    "*{box-sizing:border-box}"
    "body{background:var(--bg-page);font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',"
    "Roboto,Helvetica,Arial,sans-serif;margin:0;padding:26px 20px;color:var(--text-primary)}"
    ".shell{display:flex;gap:20px;align-items:flex-start;width:100%;max-width:1240px;"
    "margin:0 auto}"

    // --- Left navigation ---------------------------------------------
    ".nav{flex:0 0 236px;position:sticky;top:26px}"
    ".navplate{background:var(--bg-surface);border-radius:18px;box-shadow:0 10px 30px var(--shadow-card);"
    "padding:16px 12px}"
    ".navtitle{font-size:.8rem;font-weight:700;letter-spacing:.06em;text-transform:uppercase;"
    "color:var(--text-primary);padding:2px 10px 12px}"
    ".navsection{font-size:.64rem;font-weight:600;letter-spacing:.09em;text-transform:uppercase;"
    "color:var(--text-muted);padding:14px 10px 5px}"
    ".nav a{display:block;padding:8px 10px;border-radius:9px;color:var(--text-primary);font-size:.86rem;"
    "text-decoration:none;line-height:1.2}"
    ".nav a:hover{background:var(--bg-soft);color:var(--text-primary)}"
    ".nav a.on{background:var(--accent-tint);color:var(--nav-active-text);font-weight:600}"

    // --- Content and cards --------------------------------------------
    ".main{flex:1;min-width:0}"
    "h1{font-size:1.15rem;font-weight:600;margin:2px 0 4px;color:var(--text-primary)}"
    ".lede{font-size:.82rem;color:var(--text-muted);margin:0 0 18px;line-height:1.5}"
    // The card row: cards sit side by side and wrap onto a new line as the
    // window narrows. flex-basis 340px with grow means one card fills the
    // row, two split it, three wrap - no breakpoints to maintain.
    // Grid rather than flex-wrap, for two reasons that flex can't give at
    // once: every card occupies exactly one column (a lone card is one
    // column wide, not stretched across the row - which is what a flex
    // item with grow:1 would do), and cards in a row are automatically
    // the same height. auto-fill with a 340px minimum picks the column
    // count from the available width, so the layout still reflows on its
    // own with no breakpoints to maintain.
    ".cards{display:grid;grid-template-columns:repeat(auto-fill,minmax(340px,1fr));"
    "gap:18px;align-items:stretch}"
    // Equal-height ROWS, not just equal heights within a row. Only where
    // there is an add tile - i.e. the color changes page - because that
    // tile usually lands alone on a row of its own, where nothing else
    // would give it a height to match. In an auto-height grid, 1fr rows
    // all take the size of the tallest, which is exactly what is wanted
    // here and would only add empty space on the pages that don't have
    // one.
    ".cards:has(.addcard){grid-auto-rows:1fr}"
    ".card{background:var(--bg-surface);border-radius:18px;box-shadow:0 10px 30px var(--shadow-card);"
    "padding:20px 22px;min-width:0;overflow:hidden;display:flex;flex-direction:column}"
    ".card.wide{grid-column:1/-1}"
    // A full-size card rather than a button in a box, so it sits in the
    // grid as a peer of the color changes it adds. Dashed and
    // background-less so it still reads as an affordance, not content.
    ".card.addcard{background:transparent;box-shadow:none;border:2px dashed var(--btn-border);"
    "align-items:center;justify-content:center;text-align:center;min-height:220px;"
    "cursor:pointer;transition:border-color .12s,background .12s}"
    ".card.addcard:hover{border-color:var(--accent);background:var(--accent-wash)}"
    ".card.addcard .plus{font-size:2.6rem;font-weight:300;color:var(--text-muted);line-height:1}"
    ".card.addcard:hover .plus{color:var(--accent-text)}"
    ".card.addcard .addlabel{margin-top:10px;font-size:.86rem;font-weight:600;color:var(--text-muted)}"
    ".card.addcard:hover .addlabel{color:var(--accent-text)}"
    ".card.addcard button{all:unset;display:flex;flex-direction:column;align-items:center;"
    "justify-content:center;width:100%;height:100%;cursor:pointer}"
    // A card's heading owns the gap beneath it, and its note owns the gap
    // beneath that. Everything that can sit directly under either takes
    // margin-top:0 (below), so the space under a title is the same on
    // every card whatever follows it - a label, a note, a color row, a
    // dropdown. Previously each of those brought its own top margin,
    // which came to 5px under a heading in some cards and 16px in
    // others.
    ".card h2{font-size:.9rem;font-weight:600;color:var(--text-primary);margin:0 0 14px}"
    ".card .note{font-size:.75rem;color:var(--text-muted);margin:0 0 14px;line-height:1.5}"
    ".card .note:last-child{margin-bottom:0}"
    "h2+label,h2+.note,h2+.row,h2+.sub,h2+select,h2+input,h2+textarea,"
    ".note+label,.note+.row,.note+select,.note+input,.note+textarea{margin-top:0}"

    // --- Form controls -------------------------------------------------
    "label{display:block;font-size:.74rem;color:var(--text-muted);margin:16px 0 3px}"
    // Inside a two-up row the columns already sit under a shared gap, so
    // their own labels shouldn't add a second one.
    ".row>div>label{margin-top:0}"
    "input[type=text],input[type=password],input[type=number],select{"
    "width:100%;padding:9px 10px;border:1px solid var(--border);border-radius:8px;font:inherit;"
    "font-size:.92rem;background:var(--field-bg);color:var(--text-primary)}"
    "input:focus,select:focus{outline:2px solid var(--focus-ring);border-color:var(--accent)}"
    // The box is a form field like any other and follows the theme. Only
    // the swatch inside it is off limits - and that is the input's value,
    // which no stylesheet touches.
    "input[type=color]{width:100%;height:38px;padding:3px;border:1px solid var(--border);"
    "border-radius:8px;background:var(--field-bg);cursor:pointer}"
    // A text field that accepts modifiers carries a + inside its right
    // edge, and the list of modifiers opens INLINE beneath it rather than
    // as a floating menu: cards are overflow:hidden, so a dropdown would
    // be clipped by the card it sits in.
    ".fieldwrap{position:relative}"
    ".fieldwrap input[type=text]{padding-right:40px}"
    ".addmod{position:absolute;top:5px;right:6px;width:28px;height:28px;display:flex;"
    "align-items:center;justify-content:center;padding:0;border:1px solid var(--btn-border);"
    "border-radius:7px;background:var(--btn-bg);color:var(--text-muted);font:inherit;"
    "font-size:1.05rem;line-height:1;cursor:pointer}"
    ".addmod:hover{background:var(--bg-soft);color:var(--text-primary)}"
    ".addmod.on{background:var(--accent);border-color:var(--accent);"
    "color:var(--text-on-accent)}"
    ".modmenu{display:none;margin-top:6px;padding:7px;border:1px solid var(--border);"
    "border-radius:10px;background:var(--bg-soft)}"
    ".modmenu.open{display:flex;flex-wrap:wrap;gap:5px}"
    ".modmenu button{border:1px solid var(--btn-border);background:var(--btn-bg);"
    "border-radius:7px;padding:5px 8px;cursor:pointer;color:var(--text-primary);"
    "font:12px/1.2 ui-monospace,SFMono-Regular,Menlo,monospace}"
    ".modmenu button:hover{background:var(--accent-tint);border-color:var(--accent)}"
    ".sub{font-size:.71rem;color:var(--text-muted);margin:5px 0 0;line-height:1.45}"
    ".row{display:flex;gap:8px;margin-top:14px}"
    ".row>div{flex:1;min-width:0}"
    ".sublabel{display:block;margin:4px 0 0;font-size:.68rem;color:var(--text-muted);text-align:center}"
    ".row input{text-align:center}"
    ".btn{padding:10px 14px;border:1px solid var(--btn-border);border-radius:10px;background:var(--btn-bg);"
    "font:inherit;font-size:.86rem;font-weight:600;color:var(--text-primary);cursor:pointer}"
    ".btn:hover{background:var(--bg-soft)}"
    ".btn.primary{background:var(--accent);border-color:var(--accent);color:var(--text-on-accent)}"
    ".btn.primary:hover{background:var(--accent-dark)}"
    ".btn.full{width:100%}"
    ".btn.danger{color:var(--danger);border-color:var(--danger-border)}"
    ".btn.danger:hover{background:var(--danger-soft)}"
    ".savebar{position:sticky;bottom:0;margin-top:18px;padding:14px 0 2px;"
    "background:linear-gradient(180deg,var(--bg-page-fade) 0%,var(--bg-page) 42%)}"
    ".save{width:100%;padding:13px 0;border:none;border-radius:11px;background:var(--accent);"
    "color:var(--text-on-accent);font:inherit;font-size:.98rem;font-weight:600;cursor:pointer}"
    ".save:hover{background:var(--accent-dark)}"
    ".warn{margin:0 0 16px;padding:10px 13px;border-radius:9px;background:var(--warn-bg);color:var(--warn-text);"
    "font-size:.8rem;line-height:1.5}"

    // --- The display preview -------------------------------------------
    //
    // Geometry lifted from AXIS Audio Manager Pro's own visual-profile
    // preview, measured from a live AAMP instance so this reads as the
    // same component rather than a lookalike:
    //   frame        500 x 179 px, 6px radius, 8px padding
    //   text window  390 x 100 at (55, 40)  ->  11% / 22.35% / 78% / 55.87%
    //   text         50px in a 100px window -> 50% of the window's height
    //   AXIS logo    50 x 20, centered, 10px from the bottom
    // Everything is expressed as a percentage of the frame so it scales
    // to whatever width a card gives it.
    //
    // Note the frame itself takes the configured background color - the
    // "black frame" in AAMP is just what a #111111 background looks like.
    // The frame is always black, whatever the profile's background is,
    // with the AXIS lockup sitting on it. The lit area is the text window
    // inside, and that is what takes the background color.
    //
    // This is exactly how AAMP renders it - confirmed against a profile
    // with a red background, where the black frame and the red window are
    // plainly two different things. It was easy to miss on a profile
    // whose background is itself near-black, which is what made an
    // earlier version of this paint the whole frame instead.
    //
    // Geometry measured from a live AAMP instance:
    //   frame        500 x 179, 6px radius
    //   text window  390 x 100 at (55, 40) -> 11% / 22.35% / 78% / 55.87%
    //   text         50px in a 100px window -> 50% of the window's height
    //   AXIS lockup  50 x 20, centered, 10px from the bottom
    ".device{position:relative;width:100%;aspect-ratio:500/179;border-radius:6px;"
    "overflow:hidden;background:#000000}"
    ".screen{position:absolute;left:11%;top:22.35%;width:78%;height:55.87%;overflow:hidden;"
    "display:flex;align-items:center;justify-content:center;container-type:size;"
    "background:#000000}"
    // `pre` rather than `nowrap`: honors the newline between two paused
    // lines while still refusing to wrap a long single line.
    ".screen .txt{white-space:pre;text-align:center;font-weight:400;line-height:1.3;"
    "font-family:'Segoe UI','Segoe UI Web (West European)',-apple-system,BlinkMacSystemFont,"
    "Roboto,'Helvetica Neue',sans-serif}"
    // ONE scale, used by every preview in the app.
    //
    // textSize on the device is a single property - large, medium or
    // small - so a "medium" line is the same height whether it is on its
    // own or stacked with another. An earlier version had a second set of
    // sizes for stacked text, which made the same message render
    // differently on the home page than on its settings page.
    //
    // Calibrated against the hardware, not against AAMP's preview. In a
    // settings card whose text window measures 87.8px, the three sizes
    // read correctly at 62px, 32px and 22px. With the 1.3 leading below,
    // one large line comes to 92% of the window, two medium to 95% and
    // three small to 98%.
    ".screen.sz-large .txt{font-size:70.6cqh}"
    ".screen.sz-medium .txt{font-size:36.4cqh}"
    ".screen.sz-small .txt{font-size:25.1cqh}"
    // The size reserves its full grid of rows - 1 large, 2 medium, 3
    // small - and text fills it from the top. One medium line therefore
    // sits in the top row with the second row empty below it, which is
    // what the hardware does; centering it was wrong.
    //
    // Each min-height is that row count times the size above times the
    // 1.3 leading, so at the full count the box is exactly the text and
    // nothing moves. A vertical scroll is excluded: there the box has to
    // be the height of the text itself, because that is what the travel
    // distance is measured from.
    ".screen .txt{display:block}"
    ".screen:not(.wrap).sz-large .txt{min-height:91.8cqh}"
    ".screen:not(.wrap).sz-medium .txt{min-height:94.6cqh}"
    ".screen:not(.wrap).sz-small .txt{min-height:97.9cqh}"
    // The artwork is already a muted gray, so it needs no extra dimming
    // to sit correctly on the black bezel.
    ".axislogo{position:absolute;left:50%;transform:translateX(-50%);bottom:5.6%;width:10%;"
    "height:auto;display:block}"
    ".preview-label{font-size:.7rem;color:var(--text-muted);margin:8px 0 0;text-align:center}"
    // Scrolling. AAMP animates two identical copies of the text so the
    // marquee is seamless; this animates `left` rather than `transform`
    // because a percentage there resolves against the text window - what
    // "off screen" actually means - leaving transform free to hold the
    // vertical centering.
    // Scrolling.
    //
    // The travel has to be measured against the TEXT, not the container:
    // an earlier version animated `left` from 100% to -100% of the
    // screen, so anything wider (or taller) than the screen was still
    // partly visible when the animation restarted - it looked like the
    // scroll was being cut off, because it was.
    //
    // Percentages in `transform` resolve against the element's own size,
    // and container query units against the screen, so one pair of them
    // expresses "start just outside one edge, finish once the far edge
    // has cleared the other" for text of any length:
    //
    //   horizontal  from translateX(100cqw)  to translateX(-100%)
    //   vertical    from translateY(100cqh)  to translateY(-100%)
    //
    // The cross-axis half of each transform is the centering, which has to
    // travel along in the same declaration or it would be overwritten.
    ".screen.scroll .txt{position:absolute;animation-iteration-count:infinite;"
    "animation-timing-function:linear}"
    // A vertical scroll gets one line and the display breaks it between
    // words to fit the width. Constraining the box to the screen width
    // and letting it wrap reproduces that: at large, "00:14 REMAINING"
    // falls onto two rows because REMAINING is the longest word that
    // fits, and at medium it stays on one.
    //
    // width, not max-width: the scrolling .txt is absolutely positioned
    // at left:50%, so its available width - and therefore where it wraps
    // - would otherwise be only the half of the screen to the right of
    // that anchor. An explicit 100cqw makes it exactly the screen width,
    // which the -50% in the keyframes then re-centers.
    ".screen.wrap .txt{white-space:pre-wrap;width:100cqw;overflow-wrap:normal}"
    // Horizontal scrolling left-aligns its rows on the device rather
    // than centering them, so the preview does too. The box stays
    // centered - it is the full row grid, and the text sits at its top.
    ".screen.scroll-rtl .txt,.screen.scroll-ltr .txt{left:0;top:50%;text-align:left}"
    ".screen.scroll-btt .txt{left:50%;top:0}"
    "@keyframes scroll-rtl{from{transform:translate(100cqw,-50%)}"
    "to{transform:translate(-100%,-50%)}}"
    "@keyframes scroll-ltr{from{transform:translate(-100%,-50%)}"
    "to{transform:translate(100cqw,-50%)}}"
    "@keyframes scroll-btt{from{transform:translate(-50%,100cqh)}"
    "to{transform:translate(-50%,-100%)}}"
    ".screen.scroll-rtl .txt{animation-name:scroll-rtl}"
    ".screen.scroll-ltr .txt{animation-name:scroll-ltr}"
    ".screen.scroll-btt .txt{animation-name:scroll-btt}"

    // --- Home page ------------------------------------------------------
    ".clock{text-align:center;font-size:3.6rem;font-weight:700;font-variant-numeric:tabular-nums;"
    "color:var(--text-primary);margin:4px 0 0;line-height:1.05}"
    ".statepill{text-align:center;font-size:.7rem;text-transform:uppercase;letter-spacing:.08em;"
    "color:var(--text-muted);margin:4px 0 16px}"
    ".ctrls{display:flex;gap:8px;margin-top:12px}"
    ".ctrls form{flex:1;margin:0}"
    ".ctrls .btn{width:100%}"
    ".quick{display:flex;flex-wrap:wrap;gap:8px;margin-top:8px}"
    ".quick form{flex:1 1 calc(50% - 4px);margin:0}"
    ".quick .btn{width:100%}"

    // --- Virtual C8310 ---------------------------------------------------
    ".photo-wrap{position:relative;width:100%;max-width:270px;margin:0 auto;line-height:0}"
    ".photo-wrap img{width:100%;height:auto;display:block;user-select:none;-webkit-user-drag:none}"
    ".photo-wrap form{margin:0;display:contents}"
    ".hit{position:absolute;border:none;padding:0;margin:0;background:transparent;"
    "cursor:pointer;border-radius:10%;transition:background .12s}"
    ".hit:hover{background:var(--hit-hover)}"
    ".hit:active{background:var(--hit-active)}"
    ".btnlist{margin:14px 0 0;font-size:.75rem;color:var(--text-muted);line-height:1.7}"
    ".btnlist b{color:var(--text-primary);font-weight:600}"

    // --- API page ---------------------------------------------------------
    // Two columns, collapsing to one only when a column would be too
    // narrow for a URL to be readable at all.
    ".apigrid{display:grid;grid-template-columns:1fr 1fr;gap:10px;min-width:0}"
    "@media (max-width:1000px){.apigrid{grid-template-columns:1fr}}"
    // min-width:0 all the way down: a grid or flex item defaults to
    // min-width:auto, which refuses to shrink below its content, so
    // without these a long URL widens its column and pushes the card past
    // its own bounds instead of scrolling inside the code box.
    ".api{border:1px solid var(--divider);border-radius:11px;padding:11px 13px;"
    "display:flex;flex-direction:column;min-width:0;overflow:hidden}"
    ".api .urlrow{margin-top:auto;min-width:0}"
    ".api .what{font-size:.81rem;font-weight:600;color:var(--text-primary);margin-bottom:2px}"
    ".api .why{font-size:.71rem;color:var(--text-muted);margin-bottom:8px;line-height:1.45}"
    ".api .urlrow{display:flex;gap:6px;align-items:stretch}"
    // A long URL scrolls inside its own box rather than wrapping or being
    // cut off. The scrollbar is styled to stay visible: macOS hides
    // overlay scrollbars until something scrolls, which makes a truncated
    // URL look like the whole URL.
    ".api code{flex:1;min-width:0;display:block;background:var(--bg-code);border-radius:7px;"
    "padding:8px 9px 6px;font:12px/1.4 ui-monospace,SFMono-Regular,Menlo,monospace;"
    "color:var(--text-primary);overflow-x:auto;white-space:pre;scrollbar-width:thin;"
    "scrollbar-color:var(--border) transparent}"
    ".api code::-webkit-scrollbar{height:6px}"
    ".api code::-webkit-scrollbar-track{background:transparent}"
    ".api code::-webkit-scrollbar-thumb{background:var(--border);border-radius:3px}"
    ".api code::-webkit-scrollbar-thumb:hover{background:var(--text-muted)}"
    ".copy{flex:0 0 auto;padding:0 12px;border:1px solid var(--btn-border);border-radius:7px;background:var(--btn-bg);"
    "font:inherit;font-size:.73rem;font-weight:600;color:var(--text-primary);cursor:pointer}"
    ".copy:hover{background:var(--bg-soft)}"
    ".disclaimer{margin:14px 4px 0;color:var(--text-muted);font-size:.64rem;line-height:1.5}"

    // Below this width the nav can't sit beside the content without
    // squeezing the cards into a single narrow column, so it moves on top
    // and lays its links out in a row.
    "@media (max-width:800px){"
    ".shell{flex-direction:column}"
    ".nav{position:static;flex:1 1 auto;width:100%}"
    ".navplate{display:flex;flex-wrap:wrap;gap:4px;align-items:center;padding:12px}"
    ".navplate{padding-right:56px}"
    ".navtitle{width:100%;padding-bottom:6px}"
    ".navsection{padding:6px 8px 6px 2px}"
    "}"
    "</style>";

// The AXIS Communications lockup that sits under the text on the real
// device, and in AAMP's preview. Drawn as inline SVG rather than linked,
// because a strict Content-Security-Policy on the proxied path blocks
// external assets - and because an SVG stays crisp at any card width.
//
// This is a close reconstruction, not the official artwork file. If you
// want the real asset, replace the markup here with an <img> whose src is
// a base64 data: URI of it; nothing else needs to change.
// Fallback lockup, used only while axis_logo.h carries no artwork (see
// that file). Redrawn from the real mark: the wordmark with its
// registered symbol, the triangle, and the letterspaced COMMUNICATIONS
// line beneath, all on the same 748 x 290 proportions as the original.
static const char AXIS_LOGO_SVG[] =
    "<svg class='axislogo' viewBox='0 0 748 290' xmlns='http://www.w3.org/2000/svg' "
    "aria-label='AXIS Communications' role='img'>"
    "<g fill='#ffffff'>"
    "<text x='14' y='198' font-family='Helvetica,Arial,sans-serif' font-size='200' "
    "font-weight='700' letter-spacing='-4'>AXIS</text>"
    "<text x='470' y='74' font-family='Helvetica,Arial,sans-serif' font-size='46'>&#174;</text>"
    "<path d='M604 6 L744 198 L464 198 Z'/>"
    "<text x='16' y='284' font-family='Helvetica,Arial,sans-serif' font-size='52' "
    "font-weight='600' letter-spacing='15.4'>COMMUNICATIONS</text>"
    "</g></svg>";

// Emits the lockup: the real artwork when one has been embedded, the SVG
// reconstruction otherwise. The test folds to a constant at compile time.
static void append_axis_logo(GString* body) {
    if (AXIS_LOGO_B64[0])
        g_string_append(body,
                        "<img class='axislogo' alt='AXIS Communications' "
                        "src='data:image/png;base64," AXIS_LOGO_B64 "'>");
    else
        g_string_append(body, AXIS_LOGO_SVG);
}


static const char DISCLAIMER_HTML[] =
    "<p class='disclaimer'>This is an independent, community-developed ACAP package "
    "created to show what can be done with Axis devices when you think outside the box. "
    "It is not an official Axis product and is not affiliated with, endorsed by, or "
    "supported by Axis Communications AB. Use at your own risk.</p>";

/***** Client-side scripts ******************************************************/
//
// All served as same-origin external scripts via query-param routes
// (?action=poll-js and friends), never inlined - see webui.h. The same
// rule rules out inline onchange="..." attributes, which is why every
// interactive element is wired up with addEventListener from here rather
// than from markup.

// Shared by the home page's poller and the settings pages' live previews,
// so the browser preview and the real display can't drift apart in how
// they interpret a config.
#define PREVIEW_JS_COMMON                                                                          \
    /* Mirrors display_effective_lines() in display.c. Large shows one   */                        \
    /* row, medium two, small three; a vertical scroll is always one     */                        \
    /* line that the display breaks between words itself. Kept in step   */                        \
    /* with the C so the preview cannot promise a row the device will    */                        \
    /* not show.                                                          */                       \
    "function capFor(size,dir){"                                                                   \
    "if(dir==='fromBottomToTop')return 1;"                                                         \
    "return size==='small'?3:size==='medium'?2:1;}"                                                \
    "function effLines(n,size,dir){"                                                               \
    "var c=capFor(size,dir);if(!(n>=1))n=1;return n>c?c:n;}"                                       \
    /* Scroll timing is a RATE, not a duration.                          */                        \
    /*                                                                    */                        \
    /* The animation always travels the screen plus the whole text - so   */                        \
    /* a fixed duration makes a taller or longer message cover more       */                        \
    /* ground in the same time, which reads as it speeding up. Duration   */                        \
    /* is computed from the measured travel instead, so three lines take  */                        \
    /* half again as long as two and the text moves at one speed.         */                        \
    /*                                                                    */                        \
    /* Measured on the speaker: duration is inversely proportional to    */                        \
    /* the speed setting, so PACE_H1 / PACE_V1 are the seconds one        */                        \
    /* reference travel takes at speed 1 and every other speed divides    */                        \
    /* it. Horizontal reads 25.6 exactly at speeds 5 and 10; vertical     */                        \
    /* reads 19.05 and 18.80, taken as 18.9.                              */                        \
    /*                                                                    */                        \
    /* The reference travels are one screen plus the text: 2 screen       */                        \
    /* widths for a screen-wide horizontal line, and 1.946 screen         */                        \
    /* heights for two medium rows scrolling vertically.                  */                        \
    "var PACE_VREF=1.946,PACE_HREF=2.0,PACE_V1=18.9,PACE_H1=25.6;"                                  \
    "function pace(box){"                                                                          \
    "var t=box.querySelector('.txt');if(!t)return;"                                                \
    "var sp=parseFloat(box.dataset.spd||'0');"                                                     \
    "if(!(sp>0)){t.style.animationDuration='';return;}"                                            \
    "var vert=box.classList.contains('scroll-btt');"                                               \
    "var span=vert?box.clientHeight:box.clientWidth;if(!span)return;"                              \
    "var text=vert?t.offsetHeight:t.offsetWidth;"                                                  \
    "var travel=1+text/span;"                                                                      \
    "var d=travel/(vert?PACE_VREF:PACE_HREF)*(vert?PACE_V1:PACE_H1)/sp;"                           \
    "t.style.animationDuration=d.toFixed(2)+'s';"                                                  \
    "}"                                                                                            \
    "function fitPreview(box){"                                                                    \
    "var t=box.querySelector('.txt');if(!t)return;"                                                \
    "var h=box.clientHeight;if(!h)return;"                                                         \
    /* Matches the .screen.sz-* rules: large is AAMP's own 50% of the   */                         \
    /* text window's height, the other two step down from it.           */                         \
    /* Matches the .screen.sz-* rules above, which are the one scale    */                         \
    /* every preview uses.                                               */                         \
    "var pct=box.classList.contains('sz-small')?0.251:"                                            \
    "box.classList.contains('sz-medium')?0.364:0.706;"                                             \
    "t.style.fontSize=Math.round(h*pct)+'px';"                                                     \
    /* The text box is only its final size once the font is set, and    */                         \
    /* the travel is measured from that box.                            */                        \
    "pace(box);"                                                                                   \
    "}"                                                                                            \
    "function applyPreview(box,o){"                                                                \
    "var t=box.querySelector('.txt');if(!t)return;"                                                \
    /* The frame is the device's bezel and stays black; only the screen  */                        \
    /* area takes the configured background color.                       */                        \
    "if(o.bg)box.style.background=o.bg;"                                                           \
    "if(o.fg)t.style.color=o.fg;"                                                                  \
    "if(o.text!=null)t.textContent=o.text;"                                                        \
    /* The caller supplies the size the device will be sent - derived   */                         \
    /* from the line count on a settings page, read from the status     */                         \
    /* poll on the home page. Same value either way, so the same        */                         \
    /* message renders identically wherever it is previewed.            */                         \
    "if(o.size){box.classList.remove('sz-small','sz-medium','sz-large');"                          \
    "box.classList.add('sz-'+o.size);}"                                                            \
    /* Wrapping follows the direction, not the speed - see the .wrap    */                         \
    /* rule in PAGE_CSS.                                                */                         \
    "if(o.dir!=null)box.classList.toggle('wrap',o.dir==='fromBottomToTop');"                       \
    "if(o.speed!=null){"                                                                           \
    "box.dataset.spd=o.speed;"                                                                     \
    "box.classList.remove('scroll','scroll-rtl','scroll-ltr','scroll-btt');"                       \
    "if(o.speed>0){box.classList.add('scroll');"                                                   \
    "box.classList.add(o.dir==='fromLeftToRight'?'scroll-ltr':"                                    \
    "o.dir==='fromBottomToTop'?'scroll-btt':'scroll-rtl');"                                        \
    /* The duration itself is pace()'s job - it needs the laid-out text */                         \
    /* to measure, so it runs from fitPreview() once the font is set.   */                         \
    "}else{t.style.animationDuration='';}"                                                         \
    "}"                                                                                            \
    "fitPreview(box);"                                                                             \
    "}"

// Home page. Polls the JSON status endpoint twice a second so the clock,
// the state line and the live display preview all track the real timer -
// including changes made from the physical C8310 buttons, an API call or
// a rule, none of which this page would otherwise hear about. The server
// can't push to an already-open tab, so this is a plain pull.
static const char POLL_JS[] =
    "(function(){"
    PREVIEW_JS_COMMON
    "var clock=document.getElementById('clock');"
    "var pill=document.getElementById('statepill');"
    "var box=document.getElementById('livescreen');"
    "var plabel=document.getElementById('previewlabel');"
    "var toggle=document.getElementById('togglelabel');"
    "var warn=document.getElementById('displaywarn');"
    "function poll(){"
    "fetch('?action=status',{cache:'no-store'})"
    ".then(function(r){return r.json();})"
    ".then(function(d){"
    "if(clock)clock.textContent=d.remaining;"
    "if(pill)pill.innerHTML=d.state_html;"
    "if(toggle)toggle.textContent=d.toggle_label;"
    "if(box)applyPreview(box,{bg:d.bg,fg:d.fg,text:d.text,size:d.size,dir:d.dir,speed:d.speed});"
    // The bezel is always there; an idle screen just goes white, which
    // reads as "off" without dimming the whole preview into something
    // that looks broken. The caption says so in words too.
    "if(plabel)plabel.textContent=d.display_active"
    "?'The C17 display':'Display released - nothing shown by this app';"
    "if(warn){warn.style.display=d.display_ok?'none':'';"
    "if(!d.display_ok)warn.textContent=d.display_error;}"
    "}).catch(function(){});"
    "}"
    "poll();"
    "setInterval(poll,500);"
    "window.addEventListener('resize',function(){if(box)fitPreview(box);});"
    "})();";

// Settings pages. Four jobs, all of which need JavaScript because the
// alternative is a save-and-reload round trip for every keystroke:
//   1. Show or hide each button's value box as its action changes.
//   2. Repaint every display preview live from its inputs.
//   3. Add and remove color-change cards without a round trip.
//   4. Put the page back where it was after a save.
static const char SETTINGS_JS[] =
    "(function(){"
    PREVIEW_JS_COMMON
    "function byId(id){return document.getElementById(id);}"

    // --- restore the scroll position across a save ---
    // A settings save is a POST that re-renders the page, which otherwise
    // dumps you back at the top - annoying when the thing you just edited
    // was near the bottom. Stash the offset on submit, restore it on the
    // next load, and clear it so a later plain navigation still starts at
    // the top. Keyed per page so one page's offset can't be applied to
    // another's.
    "var SKEY='c17timer:scroll:'+location.search;"
    "try{var y=sessionStorage.getItem(SKEY);"
    "if(y!==null){sessionStorage.removeItem(SKEY);"
    "window.requestAnimationFrame(function(){window.scrollTo(0,parseInt(y,10)||0);});}}catch(e){}"
    // The Save button says "Saved" on the render that follows a save;
    // hand the label back after five seconds. The confirmation lands
    // where the click did, and nothing shifts the layout.
    "var savedBtn=document.querySelector('.save[data-saved]');"
    "if(savedBtn){setTimeout(function(){"
    "savedBtn.textContent='Save';savedBtn.removeAttribute('data-saved');},5000);}"

    "var form=document.querySelector('form[data-settings]');"
    "if(form)form.addEventListener('submit',function(){"
    "try{sessionStorage.setItem(SKEY,String(window.scrollY));}catch(e){}"
    "});"

    // --- buttons ---
    "function syncButton(sel){"
    "var slot=sel.dataset.slot;"
    "var wrap=byId('btnvalue'+slot);"
    "if(wrap)wrap.style.display=(sel.value==='quick'||sel.value==='add')?'':'none';"
    "}"
    "var bsel=document.querySelectorAll('select[data-slot]');"
    "for(var i=0;i<bsel.length;i++){(function(s){"
    "syncButton(s);s.addEventListener('change',function(){syncButton(s);});"
    "})(bsel[i]);}"

    // --- previews ---
    // Each preview names the input ids that feed it in data- attributes,
    // so this loop stays generic: the normal colors, every color change
    // and the finished message all wire up the same way.
    "function repaint(box){"
    "function val(id){var e=id?byId(id):null;return e?e.value:null;}"
    /* Mirrors build_lines() in display.c: only the first n lines are   */
    /* used, and text beyond the count is ignored rather than folded    */
    /* onto the last row.                                               */
    "function joinN(n){var out=[];"
    "for(var i=1;i<=n&&i<arguments.length;i++){var v=arguments[i];if(v)out.push(v);}"
    "return out.join('\\n');}"
    "applyPreview(box,{"
    "bg:val(box.dataset.bg),fg:val(box.dataset.fg),"
    "size:val(box.dataset.size),"
    "text:joinN(effLines(parseInt(val(box.dataset.lines)||'1',10),"
    "val(box.dataset.size),val(box.dataset.dir)),"
    "val(box.dataset.text),val(box.dataset.text2),val(box.dataset.text3))"
    ".replace(/\\{time\\}/g,box.dataset.sample||'00:00')"
    ".replace(/\\{hh\\}/g,'0').replace(/\\{mm\\}/g,'00').replace(/\\{ss\\}/g,'00')"
    ".replace(/\\{total\\}/g,'0')"
    ".replace(/\\{paused_at_12\\}/g,'2:32 PM').replace(/\\{paused_at_24\\}/g,'14:32'),"
    "dir:val(box.dataset.dir),"
    "speed:parseInt(val(box.dataset.speed)||'0',10)});"
    "}"
    "var boxes=document.querySelectorAll('.screen[data-bg]');"
    "function repaintAll(){for(var i=0;i<boxes.length;i++)repaint(boxes[i]);}"
    "for(var b=0;b<boxes.length;b++){(function(box){"
    "['bg','fg','text','text2','text3','lines','size','dir','speed'].forEach(function(k){"
    "var e=box.dataset[k]?byId(box.dataset[k]):null;"
    "if(e){e.addEventListener('input',function(){repaint(box);});"
    "e.addEventListener('change',function(){repaint(box);});}"
    "});"
    "repaint(box);"
    "})(boxes[b]);}"
    "window.addEventListener('resize',repaintAll);"

    // --- modifier chips ---
    // The + inside a text field opens that field's chip list; a chip
    // drops its token in at the cursor rather than replacing the field,
    // so it composes with text already typed. The synthetic input event
    // is what makes the preview repaint, since nothing else knows the
    // value changed.
    "function closeMods(except){"
    "var open=document.querySelectorAll('.modmenu.open');"
    "for(var i=0;i<open.length;i++)if(open[i]!==except)open[i].classList.remove('open');"
    "var on=document.querySelectorAll('.addmod.on');"
    "for(var j=0;j<on.length;j++)on[j].classList.remove('on');"
    "}"
    "var addmods=document.querySelectorAll('.addmod');"
    "for(var ai=0;ai<addmods.length;ai++){(function(b){"
    "b.addEventListener('click',function(){"
    "var m=byId(b.dataset.mods+'mods');if(!m)return;"
    "var wasOpen=m.classList.contains('open');"
    "closeMods(null);"
    "if(!wasOpen){m.classList.add('open');b.classList.add('on');}"
    "});})(addmods[ai]);}"
    "var chips=document.querySelectorAll('.modmenu button[data-ins]');"
    "for(var ci=0;ci<chips.length;ci++){(function(b){"
    "b.addEventListener('click',function(){"
    "var f=byId(b.dataset.field);if(!f)return;"
    "var tok=b.dataset.ins;"
    "var a=f.selectionStart,z=f.selectionEnd;"
    "if(typeof a!=='number'||typeof z!=='number'){a=f.value.length;z=a;}"
    "f.value=f.value.slice(0,a)+tok+f.value.slice(z);"
    "var at=a+tok.length;"
    "f.focus();"
    "try{f.setSelectionRange(at,at);}catch(err){}"
    "f.dispatchEvent(new Event('input',{bubbles:true}));"
    "});})(chips[ci]);}"
    // Anywhere outside a field closes whatever is open.
    "document.addEventListener('click',function(e){"
    "var t=e.target;"
    "while(t&&t!==document){"
    "if(t.className&&String(t.className).indexOf('fieldwrap')>=0)return;"
    "t=t.parentNode;}"
    "closeMods(null);"
    "});"

    // --- the size and direction decide the line count on offer ---
    // Counts the chosen combination cannot show are hidden rather than
    // left selectable, and a count already above the new maximum is
    // clamped down to it. The text in a row that disappears is left in
    // its input, so widening the size brings it straight back.
    "function syncLines(sel,reset){"
    "var sf=sel.dataset.sizefield,df=sel.dataset.dirfield;"
    "var se=sf?byId(sf):null,de=df?byId(df):null;"
    "var size=se?se.value:null,dir=de?de.value:null;"
    "var cap=capFor(size,dir);"
    // Clear the restrictions BEFORE choosing the value: the count being
    // moved to may still be hidden and disabled from the previous size,
    // and assigning a select to a disabled option is not reliable.
    "for(var o=0;o<sel.options.length;o++){"
    "sel.options[o].hidden=false;sel.options[o].disabled=false;}"
    // Changing the size or direction sets the count to the new maximum -
    // picking small means you want its three rows - and you can then
    // lower it. Picking a count directly leaves it alone, and so does
    // the first render, which has to honor what was saved.
    "var n=reset?cap:(parseInt(sel.value,10)||1);"
    "if(n>cap)n=cap;"
    "sel.value=String(n);"
    "for(var o2=0;o2<sel.options.length;o2++){"
    "var over=(parseInt(sel.options[o2].value,10)||1)>cap;"
    "sel.options[o2].hidden=over;sel.options[o2].disabled=over;}"
    "for(var i=1;i<=3;i++){"
    "var row=byId(sel.id+'Row'+i);"
    "if(row)row.style.display=(i<=n)?'':'none';"
    "}}"
    "var lsel=document.querySelectorAll('select[data-lines]');"
    "for(var li=0;li<lsel.length;li++){(function(sel){"
    "syncLines(sel,false);"
    "sel.addEventListener('change',function(){syncLines(sel,false);});"
    // Size and direction feed the same rule, and reset the count to the
    // maximum the new combination allows.
    "[sel.dataset.sizefield,sel.dataset.dirfield].forEach(function(id){"
    "var e=id?byId(id):null;"
    "if(e)e.addEventListener('change',function(){syncLines(sel,true);});});"
    "})(lsel[li]);}"

    // --- add / remove color-change cards ---
    "function syncRows(){"
    "var next=null;"
    "for(var i=1;i<=" G_STRINGIFY(MAX_THRESHOLDS) ";i++){"
    "var used=byId('ThresholdUsed'+i);var row=byId('thresholdcard'+i);"
    "if(!used||!row)continue;"
    "if(used.value==='1'){row.style.display='';}"
    "else{row.style.display='none';if(next===null)next=i;}"
    "}"
    // Hide the CARD, not the button inside it: the dashed border belongs
    // to the section, so hiding only the button left an empty dashed box
    // sitting in the grid once all the thresholds were used.
    "var add=byId('addthresholdcard');"
    "if(add)add.style.display=(next===null)?'none':'';"
    "return next;"
    "}"
    "var add=byId('addthreshold');"
    "if(add)add.addEventListener('click',function(e){"
    "e.preventDefault();var n=syncRows();"
    "if(n!==null){byId('ThresholdUsed'+n).value='1';syncRows();"
    "var box=byId('thresholdscreen'+n);if(box){repaint(box);fitPreview(box);}}"
    "});"
    "var rem=document.querySelectorAll('button[data-remove]');"
    "for(var r=0;r<rem.length;r++){(function(btn){"
    "btn.addEventListener('click',function(e){"
    "e.preventDefault();byId('ThresholdUsed'+btn.dataset.remove).value='0';syncRows();"
    "});"
    "})(rem[r]);}"
    "syncRows();"
    "})();";

// Theme. Loaded from <head> and deliberately render-blocking: it has to
// set the class on <html> before the body paints, or the page flashes
// light before turning dark.
//
// Where the choice comes from, most specific first:
//   1. this tab's own toggle    (sessionStorage, per-tab like the
//                                Request Builder's - a theme picked to
//                                read one page is rarely meant to stick)
//   2. the device's admin theme (localStorage 'theme' or the cookie -
//                                same origin, so this page is really an
//                                extension of that UI)
//   3. the browser/OS preference
//
// It keeps following 2 and 3 live until the toggle is used.
static const char THEME_JS[] =
    "(function(){"
    "function unwrap(raw){if(raw==null)return null;"
    "try{var p=JSON.parse(raw);if(typeof p==='string')return p;}catch(e){}return raw;}"
    "function wanted(){"
    "try{"
    "var own=sessionStorage.getItem('c17TimerTheme');"
    "if(own==='dark'||own==='light')return own==='dark';"
    "var dev=unwrap(localStorage.getItem('theme'));"
    "if(dev!=='dark'&&dev!=='light'){"
    "var m=document.cookie.match(/(?:^|;\\s*)theme=([^;]*)/);"
    "dev=m?unwrap(decodeURIComponent(m[1])):null;}"
    "if(dev==='dark'||dev==='light')return dev==='dark';"
    "return !!(window.matchMedia&&window.matchMedia('(prefers-color-scheme: dark)').matches);"
    "}catch(e){return false;}}"
    "function apply(dark){document.documentElement.classList.toggle('dark-mode',dark);}"
    "function isDark(){return document.documentElement.classList.contains('dark-mode');}"
    "apply(wanted());"
    // The button is further down the document than this script, so its
    // wiring waits for the parse to finish. The class above does not.
    "document.addEventListener('DOMContentLoaded',function(){"
    "var btn=document.getElementById('themetoggle');"
    "function label(){if(!btn)return;"
    "var t=isDark()?'Switch to light mode':'Switch to dark mode';"
    "btn.title=t;btn.setAttribute('aria-label',t);}"
    "label();"
    "if(btn)btn.addEventListener('click',function(){"
    "apply(!isDark());label();"
    "try{sessionStorage.setItem('c17TimerTheme',isDark()?'dark':'light');}catch(e){}"
    "});"
    "try{window.matchMedia('(prefers-color-scheme: dark)')"
    ".addEventListener('change',function(e){"
    "try{if(sessionStorage.getItem('c17TimerTheme'))return;}catch(err){}"
    "apply(e.matches);label();});}catch(e){}"
    // The device's own UI writing a new theme in another tab.
    "window.addEventListener('storage',function(e){"
    "if(e.key!=='theme')return;"
    "var t=unwrap(e.newValue);"
    "if(t!=='dark'&&t!=='light')return;"
    "apply(t==='dark');label();"
    "try{sessionStorage.setItem('c17TimerTheme',t);}catch(err){}"
    "});"
    "});"
    "})();";

// API page. Rewrites every URL when the base is switched between "another
// system" and "this device's own rules", and copies one to the clipboard.
static const char API_JS[] =
    "(function(){"
    "var codes=document.querySelectorAll('code[data-path]');"
    // The server cannot know the address anybody actually typed: behind
    // the reverse proxy, Apache forwards to this app's own port, so the
    // Host header it sees is the proxy target rather than the device's
    // address. The browser knows it, so the URLs are built here.
    // location.origin + location.pathname is exactly the prefix these
    // query strings hang off.
    "var base=location.origin+location.pathname;"
    "for(var i=0;i<codes.length;i++)codes[i].textContent=base+codes[i].dataset.path;"
    "var btns=document.querySelectorAll('button.copy');"
    "for(var i=0;i<btns.length;i++){(function(b){"
    "b.addEventListener('click',function(e){"
    "e.preventDefault();"
    "var code=document.getElementById(b.dataset.target);"
    "if(!code)return;"
    "var text=code.textContent;"
    "function done(){var o=b.textContent;b.textContent='Copied';"
    "setTimeout(function(){b.textContent=o;},1200);}"
    // The async Clipboard API needs a secure context. The proxied path is
    // HTTPS so it normally has one, but a device serving this over plain
    // HTTP would not - so there's a selection-based fallback rather than
    // a button that silently does nothing.
    "if(navigator.clipboard&&window.isSecureContext){"
    "navigator.clipboard.writeText(text).then(done).catch(function(){fallback(text,done);});"
    "}else{fallback(text,done);}"
    "});"
    "})(btns[i]);}"
    "function fallback(text,done){"
    "var ta=document.createElement('textarea');ta.value=text;"
    "ta.style.position='fixed';ta.style.opacity='0';"
    "document.body.appendChild(ta);ta.select();"
    "try{document.execCommand('copy');done();}catch(err){}"
    "document.body.removeChild(ta);"
    "}"
    "})();";

/***** Page shell ***************************************************************/

typedef struct {
    const char* id;    // ?page= value; "" is home
    const char* label;
} nav_item_t;

// The Settings group, in nav order. Display is four separate pages
// rather than one long scroll - each is short enough to take in at a
// glance, and saving one can't disturb another.
// The timer's own pages get a section of their own, so each item can be
// named for what it configures rather than carrying a "Timer - " prefix
// the heading already supplies. Page ids are unchanged, so existing
// links and the saved form sections still resolve.
static const nav_item_t NAV_TIMER[] = {
    {"timer", "Duration"},
    {"timer-normal", "Normal"},
    {"timer-paused", "Paused"},
    {"timer-thresholds", "Thresholds"},
    {"timer-finished", "Finished"},
};

// What is left is genuinely device configuration rather than timer
// behavior. C8310 first: it is the one people set up once, on install.
static const nav_item_t NAV_SETTINGS[] = {
    {"c8310", "C8310"},
    {"display-sharing", "Display Sharing"},
};

static const nav_item_t NAV_INTEGRATE[] = {
    {"api", "API"},
    {"rules", "Rules"},
};

static void append_head(GString* body, const char* title) {
    g_string_append(body,
                    "<!DOCTYPE html><html><head><meta charset=utf-8>"
                    "<meta name=viewport content='width=device-width, initial-scale=1'><title>");
    g_string_append(body, title);
    g_string_append(body, "</title>");
    g_string_append(body, PAGE_CSS);
    // Render-blocking on purpose - see THEME_JS. A separate URL rather
    // than an inline script because the reverse proxy's CSP is
    // script-src 'self', which drops inline script silently.
    g_string_append(body, "<script src='?action=theme-js'></script>");
    g_string_append(body, "</head><body>");
    // The half-filled circle is the same mark the Request Builder uses,
    // and it does not change with the theme - only the tooltip does,
    // which THEME_JS fills in once it knows which theme resolved.
    g_string_append(body,
                    "<button type=button class='themetoggle' id='themetoggle'>"
                    "<svg fill='currentColor' aria-hidden='true' viewBox='0 0 24 24' "
                    "xmlns='http://www.w3.org/2000/svg'><path d='M12,22 C17.5228475,22 22,17.5228475 "
                    "22,12 C22,6.4771525 17.5228475,2 12,2 C6.4771525,2 2,6.4771525 2,12 "
                    "C2,17.5228475 6.4771525,22 12,22 Z M12,20.5 L12,3.5 C16.6944204,3.5 20.5,"
                    "7.30557963 20.5,12 C20.5,16.6944204 16.6944204,20.5 12,20.5 Z'/></svg>"
                    "</button>");
    g_string_append(body, "<div class='shell'>");
}

// The left navigation, shown on every page with the current one marked.
static void append_nav(GString* body, const char* current) {
    g_string_append(body, "<nav class='nav'><div class='navplate'>");
    g_string_append(body, "<div class='navtitle'>C17 Timer</div>");

    g_string_append_printf(body,
                           "<a href='?' class='%s'>Home</a>",
                           strcmp(current, "home") == 0 ? "on" : "");

    static const struct {
        const char* heading;
        const nav_item_t* items;
        size_t count;
    } GROUPS[] = {
        {"Timer", NAV_TIMER, G_N_ELEMENTS(NAV_TIMER)},
        {"Settings", NAV_SETTINGS, G_N_ELEMENTS(NAV_SETTINGS)},
        {"Integrate", NAV_INTEGRATE, G_N_ELEMENTS(NAV_INTEGRATE)},
    };

    for (size_t g_i = 0; g_i < G_N_ELEMENTS(GROUPS); g_i++) {
        g_string_append_printf(body, "<div class='navsection'>%s</div>", GROUPS[g_i].heading);
        for (size_t i = 0; i < GROUPS[g_i].count; i++)
            g_string_append_printf(body,
                                   "<a href='?page=%s' class='%s'>%s</a>",
                                   GROUPS[g_i].items[i].id,
                                   strcmp(current, GROUPS[g_i].items[i].id) == 0 ? "on" : "",
                                   GROUPS[g_i].items[i].label);
    }

    // The disclaimer sits under the nav column rather than across the
    // foot of the page: it belongs to the app as a whole, not to whatever
    // page happens to be open, and down here it never pushes the content
    // around as pages change length.
    g_string_append(body, "</div>");
    g_string_append(body, DISCLAIMER_HTML);
    g_string_append(body, "</nav><main class='main'>");
}

static void append_foot(GString* body) {
    // The disclaimer is emitted by append_nav(), inside the nav column.
    g_string_append(body, "</main></div></body></html>");
}

// Emits one display preview: the AAMP-style frame, the text window inside
// it, and the AXIS lockup at the bottom.
//
// The data_* ids, when given, tell SETTINGS_JS which form inputs feed
// this preview so it can repaint live. The home page passes NULL for all
// of them and drives its preview from the status poll instead.
static void append_preview(GString* body,
                           const char* frame_id,
                           const char* screen_id,
                           const char* message,
                           const char* fg,
                           const char* bg,
                           const char* size,
                           const char* dir,
                           int speed,
                           const char* id_bg,
                           const char* id_fg,
                           const char* id_text,
                           const char* id_text2,
                           const char* id_text3,
                           const char* id_lines,
                           const char* id_size,
                           const char* id_dir,
                           const char* id_speed,
                           const char* sample_time) {
    char msg_esc[1024], fg_esc[16], bg_esc[16];
    html_escape(message, msg_esc, sizeof(msg_esc));
    html_escape(fg && fg[0] ? fg : "#ffffff", fg_esc, sizeof(fg_esc));
    html_escape(bg && bg[0] ? bg : "#111111", bg_esc, sizeof(bg_esc));

    const char* size_class = "sz-large";
    if (size && strcmp(size, "small") == 0)
        size_class = "sz-small";
    else if (size && strcmp(size, "medium") == 0)
        size_class = "sz-medium";


    // Wrapping follows the direction, not the speed: a vertical profile
    // is a single wrapped line whether or not it is moving.
    const char* wrap_class = (dir && strcmp(dir, "fromBottomToTop") == 0) ? " wrap" : "";

    const char* scroll_class = "";
    if (speed > 0) {
        if (dir && strcmp(dir, "fromLeftToRight") == 0)
            scroll_class = " scroll scroll-ltr";
        else if (dir && strcmp(dir, "fromBottomToTop") == 0)
            scroll_class = " scroll scroll-btt";
        else
            scroll_class = " scroll scroll-rtl";
    }

    // The frame stays black whatever the profile's background is - it
    // stands in for the device's bezel, and the AXIS lockup sits on it.
    g_string_append(body, "<div class='device'");
    if (frame_id)
        g_string_append_printf(body, " id='%s'", frame_id);
    g_string_append(body, ">");

    g_string_append_printf(body, "<div class='screen %s%s%s'", size_class, wrap_class, scroll_class);
    if (screen_id)
        g_string_append_printf(body, " id='%s'", screen_id);
    if (id_bg)
        g_string_append_printf(body, " data-bg='%s'", id_bg);
    if (id_fg)
        g_string_append_printf(body, " data-fg='%s'", id_fg);
    if (id_text)
        g_string_append_printf(body, " data-text='%s'", id_text);
    if (id_text2)
        g_string_append_printf(body, " data-text2='%s'", id_text2);
    if (id_text3)
        g_string_append_printf(body, " data-text3='%s'", id_text3);

    if (id_lines)
        g_string_append_printf(body, " data-lines='%s'", id_lines);
    if (id_size)
        g_string_append_printf(body, " data-size='%s'", id_size);
    if (id_dir)
        g_string_append_printf(body, " data-dir='%s'", id_dir);
    if (id_speed)
        g_string_append_printf(body, " data-speed='%s'", id_speed);
    if (sample_time)
        g_string_append_printf(body, " data-sample='%s'", sample_time);
    g_string_append_printf(body, " style='background:%s'>", bg_esc);

    g_string_append_printf(body, "<span class='txt' style='color:%s", fg_esc);
    // A rough duration for the frame or two before pace() runs and
    // replaces it with one measured from the laid-out text. Correct only
    // at the horizontal reference travel - close enough that nothing
    // visibly jumps, and it keeps the preview animating at all if
    // scripts are blocked. Keep in step with PACE_H1.
    if (speed > 0)
        g_string_append_printf(body, ";animation-duration:%.1fs", 25.6 / speed);
    g_string_append_printf(body, "'>%s</span></div>", msg_esc);

    append_axis_logo(body);
    g_string_append(body, "</div>");
}

/***** Status JSON **************************************************************/

// Appends `in` to `out` with the escaping a JSON string value needs.
// Hand-rolled rather than pulling jansson into this file for a handful of
// short fields.
static void append_json_escaped(GString* out, const char* in) {
    for (const char* p = in; *p; p++) {
        switch (*p) {
        case '"':
            g_string_append(out, "\\\"");
            break;
        case '\\':
            g_string_append(out, "\\\\");
            break;
        case '\n':
            g_string_append(out, "\\n");
            break;
        case '\r':
            g_string_append(out, "\\r");
            break;
        case '\t':
            g_string_append(out, "\\t");
            break;
        default:
            if ((unsigned char)*p < 0x20)
                g_string_append_printf(out, "\\u%04x", (unsigned char)*p);
            else
                g_string_append_c(out, *p);
        }
    }
}

static void append_json_field(GString* out, const char* key, const char* value, bool last) {
    g_string_append_printf(out, "\"%s\":\"", key);
    append_json_escaped(out, value);
    g_string_append(out, last ? "\"" : "\",");
}

// The single source of truth for "what is the timer doing" - used by the
// home page's poller, by every API verb's response, and by anything
// external reading state. One shape, one code path.
static void append_status_json(GString* body, bool changed, bool ok, const char* message) {
    timer_state_t st = timer_get_state();

    char remaining[16], duration[16];
    timer_format_time(timer_remaining_seconds(), remaining, sizeof(remaining));
    timer_format_time(timer_set_seconds(), duration, sizeof(duration));

    display_frame_t frame;
    bool active = display_resolve(g.cfg, &frame);

    const char* state_label = "Idle";
    switch (st) {
    case TIMER_RUNNING:
        state_label = "Running";
        break;
    case TIMER_PAUSED:
        state_label = "Paused";
        break;
    case TIMER_FINISHED:
        state_label = "Finished";
        break;
    default:
        break;
    }

    // The line under the clock says both what the timer is doing and what
    // it is set to - "Idle" alone leaves you guessing what pressing start
    // would actually run.
    char state_html[192];
    if (st == TIMER_FINISHED)
        snprintf(state_html, sizeof(state_html), "<b>Finished</b>");
    else
        snprintf(state_html, sizeof(state_html), "<b>%s</b> &middot; set to %s", state_label, duration);

    g_string_append_c(body, '{');
    g_string_append_printf(body, "\"ok\":%s,", ok ? "true" : "false");
    g_string_append_printf(body, "\"changed\":%s,", changed ? "true" : "false");
    append_json_field(body, "message", message ? message : "", false);
    append_json_field(body, "state", timer_state_name(st), false);
    append_json_field(body, "state_label", state_label, false);
    append_json_field(body, "state_html", state_html, false);
    append_json_field(body, "remaining", remaining, false);
    g_string_append_printf(body, "\"remaining_seconds\":%d,", timer_remaining_seconds());
    append_json_field(body, "duration", duration, false);
    g_string_append_printf(body, "\"duration_seconds\":%d,", timer_set_seconds());
    append_json_field(body, "toggle_label", st == TIMER_RUNNING ? "Pause" : "Start", false);

    g_string_append_printf(body, "\"display_active\":%s,", active ? "true" : "false");
    g_string_append_printf(body, "\"display_ok\":%s,", display_last_ok() ? "true" : "false");
    append_json_field(body, "display_error", display_last_error(), false);

    // The frame the display is showing (or would show) right now, so the
    // browser preview is the same calculation as the real screen.
    append_json_field(body, "text", active ? frame.message : "", false);
    append_json_field(body, "fg", frame.text_color[0] ? frame.text_color : "#ffffff", false);
    append_json_field(body, "bg", active && frame.bg_color[0] ? frame.bg_color : "#000000", false);
    append_json_field(body, "size", frame.text_size[0] ? frame.text_size : "large", false);
    append_json_field(body, "dir", frame.scroll_direction[0] ? frame.scroll_direction : "fromRightToLeft", false);
    g_string_append_printf(body, "\"speed\":%d,", frame.scroll_speed);
    g_string_append_printf(body, "\"display_state\":%d", timer_display_state_index());
    g_string_append_c(body, '}');
}

/***** Home *********************************************************************/

// Invisible hit-region rectangles overlaid on the real C8310 product
// photo (PANEL_IMAGE_B64, panel_image.h - used with permission),
// positioned as percentages of the photo's box so they track it at any
// render size. Measured from the source photo's own pixel coordinates and
// carried over unchanged from the C8310 Customizer, which uses the same
// photo. Indexed by the BTN_* enum so the loop below can't pair a
// rectangle with the wrong button.
typedef struct {
    const char* left;
    const char* top;
    const char* width;
    const char* height;
} hit_rect_t;

static const hit_rect_t HIT_RECT[BTN_COUNT] = {
    [BTN_1]       = {"28.41%", "43.37%", "21.34%", "10.33%"},
    [BTN_2]       = {"49.74%", "43.37%", "21.72%", "10.33%"},
    [BTN_3]       = {"28.41%", "53.70%", "21.34%", "10.22%"},
    [BTN_VOLUP]   = {"28.41%", "28.26%", "43.06%", "15.11%"},
    [BTN_VOLDOWN] = {"28.41%", "63.91%", "43.06%", "13.59%"},
    [BTN_MUTE]    = {"49.74%", "53.70%", "21.72%", "10.22%"},
};

static void render_home(GString* body) {
    append_head(body, "C17 Timer");
    append_nav(body, "home");

    g_string_append(body, "<h1>Timer</h1>");
    g_string_append(body,
                    "<p class='lede'>The countdown and what the display is showing. Updates "
                    "live, no matter what changed it.</p>");

    g_string_append_printf(body,
                           "<div class='warn' id='displaywarn' style='display:%s'>%s</div>",
                           display_last_ok() ? "none" : "",
                           display_last_ok() ? "" : display_last_error());

    g_string_append(body, "<div class='cards'>");

    // --- Timer card ---
    char remaining[16], duration[16];
    timer_format_time(timer_remaining_seconds(), remaining, sizeof(remaining));
    timer_format_time(timer_set_seconds(), duration, sizeof(duration));
    timer_state_t st = timer_get_state();

    g_string_append(body, "<section class='card'>");
    g_string_append_printf(body, "<div class='clock' id='clock'>%s</div>", remaining);
    if (st == TIMER_FINISHED)
        g_string_append(body, "<div class='statepill' id='statepill'><b>Finished</b></div>");
    else
        g_string_append_printf(body,
                               "<div class='statepill' id='statepill'><b>%s</b> &middot; set to %s</div>",
                               st == TIMER_RUNNING ? "Running" : (st == TIMER_PAUSED ? "Paused" : "Idle"),
                               duration);

    display_frame_t frame;
    bool active = display_resolve(g.cfg, &frame);
    // Released: a dark screen with no text, which is what the panel
    // itself looks like when nothing is driving it.
    append_preview(body,
                   "livedevice",
                   "livescreen",
                   active ? frame.message : "",
                   frame.text_color[0] ? frame.text_color : "#ffffff",
                   active && frame.bg_color[0] ? frame.bg_color : "#000000",
                   frame.text_size,
                   frame.scroll_direction,
                   frame.scroll_speed,
                   NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
    g_string_append_printf(body,
                           "<p class='preview-label' id='previewlabel'>%s</p>",
                           active ? "The C17&#39;s display"
                                  : "Display released - nothing shown by this app");

    // Start and Pause are one toggle whose label the poller keeps
    // current, rather than two buttons where one is always the wrong
    // thing to press.
    g_string_append_printf(body,
                           "<div class='ctrls'>"
                           "<form method=get action=''>"
                           "<input type=hidden name=action value=toggle>"
                           "<button class='btn primary' type=submit>"
                           "<span id='togglelabel'>%s</span></button></form>"
                           "<form method=get action=''>"
                           "<input type=hidden name=action value=stop>"
                           "<button class='btn' type=submit>Stop &amp; reset</button></form>"
                           "</div>",
                           st == TIMER_RUNNING ? "Pause" : "Start");

    char step_up[16], step_down[16];
    timer_format_time(g.cfg->step_up_seconds, step_up, sizeof(step_up));
    timer_format_time(g.cfg->step_down_seconds, step_down, sizeof(step_down));
    g_string_append_printf(body,
                           "<div class='ctrls'>"
                           "<form method=get action=''>"
                           "<input type=hidden name=action value=add>"
                           "<input type=hidden name=seconds value='-%d'>"
                           "<button class='btn' type=submit>&minus; %s</button></form>"
                           "<form method=get action=''>"
                           "<input type=hidden name=action value=add>"
                           "<input type=hidden name=seconds value='%d'>"
                           "<button class='btn' type=submit>+ %s</button></form>"
                           "</div>",
                           g.cfg->step_down_seconds, step_down,
                           g.cfg->step_up_seconds, step_up);

    // One shortcut per button actually assigned to a quick timer, so this
    // reflects the current button setup rather than being a second list
    // of presets to keep in sync.
    GString* quick = g_string_new(NULL);
    for (int i = 0; i < BTN_COUNT; i++) {
        int slot = buttons_config_slot(i);
        if (slot < 0 || g.cfg->buttons[slot].action != BTN_ACTION_QUICK)
            continue;
        char label[16];
        timer_format_time(g.cfg->buttons[slot].value_seconds, label, sizeof(label));
        g_string_append_printf(quick,
                               "<form method=get action=''>"
                               "<input type=hidden name=action value=quick>"
                               "<input type=hidden name=seconds value='%d'>"
                               "<button class='btn' type=submit>%s</button></form>",
                               g.cfg->buttons[slot].value_seconds, label);
    }
    if (quick->len > 0) {
        g_string_append(body, "<label>Quick timers</label><div class='quick'>");
        g_string_append_len(body, quick->str, (gssize)quick->len);
        g_string_append(body, "</div>");
    }
    g_string_free(quick, TRUE);
    g_string_append(body, "</section>");

    // --- Virtual C8310 card ---
    g_string_append(body, "<section class='card'>");
    g_string_append(body, "<h2>Virtual C8310</h2>");
    g_string_append(body,
                    "<p class='note'>Does exactly what pressing the physical button does.</p>");
    g_string_append(body, "<div class='photo-wrap'><img src='data:image/png;base64,");
    g_string_append(body, PANEL_IMAGE_B64);
    g_string_append(body, "' alt='AXIS C8310 volume controller'>");

    for (int i = 0; i < BTN_COUNT; i++) {
        char what[64], what_esc[128], label_esc[64];
        buttons_describe(i, what, sizeof(what));
        html_escape(what, what_esc, sizeof(what_esc));
        html_escape(BTN_DISPLAY_NAME[i], label_esc, sizeof(label_esc));
        g_string_append_printf(body,
                               "<form method=get action=''>"
                               "<input type=hidden name=action value=button>"
                               "<input type=hidden name=n value='%d'>"
                               "<button class='hit' type=submit "
                               "style='left:%s;top:%s;width:%s;height:%s' "
                               "title='%s: %s' aria-label='%s: %s'></button></form>",
                               i + 1,
                               HIT_RECT[i].left, HIT_RECT[i].top,
                               HIT_RECT[i].width, HIT_RECT[i].height,
                               label_esc, what_esc, label_esc, what_esc);
    }
    g_string_append(body, "</div>");

    // A written key underneath, so what each button does is readable
    // without hovering every region on the photo.
    g_string_append(body, "<div class='btnlist'>");
    for (int i = 0; i < BTN_COUNT; i++) {
        char what[64], what_esc[128];
        buttons_describe(i, what, sizeof(what));
        html_escape(what, what_esc, sizeof(what_esc));
        g_string_append_printf(body, "<b>%s</b> &middot; %s<br>", BTN_DISPLAY_NAME[i], what_esc);
    }
    g_string_append(body, "</div></section>");

    g_string_append(body, "</div>"); // .cards

    g_string_append(body, "<script src='?action=poll-js'></script>");
    append_foot(body);
}

/***** Settings: shared scaffolding *********************************************/

// Form field names, reused verbatim as element ids so the preview wiring
// in SETTINGS_JS can find each input by the same string the server used
// to render it. Keeping the two identical removes a whole class of "the
// preview silently stopped updating" bug.
static const char* const STATE_BG_FIELD[MAX_DISPLAY_STATES]   = {"State0Bg", "State1Bg", "State2Bg",
                                                                 "State3Bg", "State4Bg", "State5Bg"};
static const char* const STATE_FG_FIELD[MAX_DISPLAY_STATES]   = {"State0Fg", "State1Fg", "State2Fg",
                                                                 "State3Fg", "State4Fg", "State5Fg"};
static const char* const STATE_TEXT_FIELD[MAX_DISPLAY_STATES] = {"State0Text", "State1Text", "State2Text",
                                                                 "State3Text", "State4Text", "State5Text"};
static const char* const STATE_AT_FIELD[MAX_DISPLAY_STATES]   = {"State0At", "State1At", "State2At",
                                                                 "State3At", "State4At", "State5At"};

// Opens a settings page: shell, nav, heading, and the form every card on
// the page lives inside.
//
// The hidden `section` field is what lets one apply function serve four
// separate forms safely - without it, a page that simply doesn't contain
// the color-change fields would look identical to one submitting zero
// color changes, and saving the Timer page would wipe the Display page's
// settings.
static void settings_open(GString* body,
                          const char* page_id,
                          const char* title,
                          const char* lede) {
    char page_title[64];
    snprintf(page_title, sizeof(page_title), "C17 Timer - %s", title);
    append_head(body, page_title);
    append_nav(body, page_id);

    g_string_append_printf(body, "<h1>%s</h1>", title);
    if (lede && lede[0])
        g_string_append_printf(body, "<p class='lede'>%s</p>", lede);

    // Explicit action rather than an empty one. A POST form normally
    // resubmits to the current URL query string and all, but this app
    // routes pages through the query string specifically to work around
    // Apache's exact-apiPath-only proxy matching - too load-bearing to
    // leave to an edge case of browser behavior.
    g_string_append_printf(body,
                           "<form method=post action='?page=%s' data-settings>"
                           "<input type=hidden name=section value='%s'>"
                           "<div class='cards'>",
                           page_id, page_id);
}

// `saved` is true on the render that follows a save. The button says so
// itself rather than a banner appearing above the page - the confirmation
// lands where the click did, and nothing shifts the layout.
// SETTINGS_JS puts the label back after five seconds.
static void settings_close(GString* body, bool saved) {
    g_string_append_printf(body,
                           "</div>"
                           "<div class='savebar'><button class='save' type=submit%s>%s</button>"
                           "</div></form>",
                           saved ? " data-saved" : "",
                           saved ? "Saved" : "Save");
    g_string_append(body, "<script src='?action=settings-js'></script>");
    append_foot(body);
}

static void append_duration_input(GString* body,
                                  const char* label,
                                  const char* field,
                                  int seconds,
                                  const char* placeholder) {
    char value[16];
    timer_format_time(seconds, value, sizeof(value));
    g_string_append_printf(body,
                           "<label>%s</label>"
                           "<input type=text id='%s' name='%s' value='%s' placeholder='%s'>",
                           label, field, field, value, placeholder);
}

static void append_size_select(GString* body, const char* field, const char* current) {
    g_string_append_printf(body, "<select id='%s' name='%s'>", field, field);
    static const char* const ids[3]    = {"small", "medium", "large"};
    static const char* const labels[3] = {"Small", "Medium", "Large"};
    for (int i = 0; i < 3; i++)
        g_string_append_printf(body,
                               "<option value='%s'%s>%s</option>",
                               ids[i], strcmp(current, ids[i]) == 0 ? " selected" : "", labels[i]);
    g_string_append(body, "</select>");
}

// The size and direction fields are named on the element so SETTINGS_JS
// can offer only the counts that combination can actually show - see
// display_effective_lines().
static void append_lines_select(GString* body,
                                const char* field,
                                int current,
                                const char* size_field,
                                const char* dir_field) {
    g_string_append_printf(body,
                           "<select id='%s' name='%s' data-lines data-sizefield='%s' "
                           "data-dirfield='%s'>",
                           field, field, size_field ? size_field : "", dir_field ? dir_field : "");
    static const char* const labels[3] = {"1 line", "2 lines", "3 lines"};
    for (int i = 1; i <= 3; i++)
        g_string_append_printf(body,
                               "<option value='%d'%s>%s</option>",
                               i,
                               current == i ? " selected" : "",
                               labels[i - 1]);
    g_string_append(body, "</select>");
}

static void append_speed_select(GString* body, const char* field, int current) {
    g_string_append_printf(body, "<select id='%s' name='%s'>", field, field);
    for (int i = 0; i <= 10; i++) {
        const char* suffix = (i == 1) ? " (slow)" : (i == 10) ? " (fast)" : "";
        if (i == 0)
            g_string_append_printf(body,
                                   "<option value='0'%s>No scroll</option>",
                                   current == 0 ? " selected" : "");
        else
            g_string_append_printf(body,
                                   "<option value='%d'%s>%d%s</option>",
                                   i,
                                   current == i ? " selected" : "",
                                   i,
                                   suffix);
    }
    g_string_append(body, "</select>");
}

static void append_direction_select(GString* body, const char* field, const char* current) {
    g_string_append_printf(body, "<select id='%s' name='%s'>", field, field);
    static const char* const ids[3]    = {"fromRightToLeft", "fromLeftToRight", "fromBottomToTop"};
    static const char* const labels[3] = {"Right to left", "Left to right", "Bottom to top"};
    for (int i = 0; i < 3; i++)
        g_string_append_printf(body,
                               "<option value='%s'%s>%s</option>",
                               ids[i], strcmp(current, ids[i]) == 0 ? " selected" : "", labels[i]);
    g_string_append(body, "</select>");
}

// One color-state editor: two color pickers, the text template, and the
// preview those three feed. Used for the normal colors and for every
// color change.
// The template modifiers, offered as one-click chips beside every field
// that accepts them. The `what` text becomes the chip's tooltip - the
// token itself is the label, since that is what lands in the field.
typedef struct {
    const char* token;
    const char* what;
} modifier_t;

static const modifier_t MODIFIERS_TIME[] = {
    {"{time}", "Time left, as m:ss"},
    {"{hh}", "Hours left"},
    {"{mm}", "Minutes left"},
    {"{ss}", "Seconds left"},
    {"{total}", "Whole seconds left"},
};

// Only offered where a pause time actually means something. They render
// empty everywhere else, so putting them on the finished message would
// be offering a token that can only disappoint.
static const modifier_t MODIFIERS_PAUSED[] = {
    {"{paused_at_12}", "The time it was paused, 2:32 PM"},
    {"{paused_at_24}", "The time it was paused, 14:32"},
};

// A text input with a + inside it that opens the modifier list. `value`
// must already be HTML-escaped by the caller.
static void append_modifier_field(GString* body,
                                  const char* label,
                                  const char* field,
                                  const char* value,
                                  const char* placeholder,
                                  bool with_paused) {
    g_string_append_printf(body,
                           "<label>%s</label><div class='fieldwrap'>"
                           "<input type=text id='%s' name='%s' value='%s' placeholder='%s'>"
                           "<button type=button class='addmod' data-mods='%s' "
                           "aria-label='Insert a modifier' title='Insert a modifier'>+</button>"
                           "<div class='modmenu' id='%smods'>",
                           label, field, field, value, placeholder ? placeholder : "", field,
                           field);

    for (size_t i = 0; i < G_N_ELEMENTS(MODIFIERS_TIME); i++)
        g_string_append_printf(body,
                               "<button type=button data-ins='%s' data-field='%s' "
                               "title='%s'>%s</button>",
                               MODIFIERS_TIME[i].token, field, MODIFIERS_TIME[i].what,
                               MODIFIERS_TIME[i].token);

    if (with_paused)
        for (size_t i = 0; i < G_N_ELEMENTS(MODIFIERS_PAUSED); i++)
            g_string_append_printf(body,
                                   "<button type=button data-ins='%s' data-field='%s' "
                                   "title='%s'>%s</button>",
                                   MODIFIERS_PAUSED[i].token, field, MODIFIERS_PAUSED[i].what,
                                   MODIFIERS_PAUSED[i].token);

    g_string_append(body, "</div></div>");
}

static void append_state_fields(GString* body,
                                int idx,
                                const display_state_t* st,
                                const char* screen_id,
                                const char* sample_time) {
    char text_esc[256];
    html_escape(st->text, text_esc, sizeof(text_esc));

    g_string_append_printf(body,
                           "<div class='row'>"
                           "<div><label>Background</label>"
                           "<input type=color id='%s' name='%s' value='%s'></div>"
                           "<div><label>Text color</label>"
                           "<input type=color id='%s' name='%s' value='%s'></div>"
                           "</div>",
                           STATE_BG_FIELD[idx], STATE_BG_FIELD[idx], st->bg_color,
                           STATE_FG_FIELD[idx], STATE_FG_FIELD[idx], st->text_color);

    append_modifier_field(body, "Text", STATE_TEXT_FIELD[idx], text_esc, "{time}", false);

    char msg[512];
    timer_format_template(st->text, parse_duration(sample_time), msg, sizeof(msg));
    g_string_append(body, "<div style='margin-top:14px'>");
    // The countdown is always large and static (see local_config.h), so
    // no size/direction/speed inputs are wired to these previews.
    append_preview(body, NULL, screen_id, msg, st->text_color, st->bg_color,
                   "large", "fromRightToLeft", 0,
                   STATE_BG_FIELD[idx], STATE_FG_FIELD[idx], STATE_TEXT_FIELD[idx],
                   NULL, NULL, NULL, NULL, NULL, NULL, sample_time);
    g_string_append(body, "</div>");
}

/***** Settings: Timer **********************************************************/

static void render_settings_timer(bool saved, GString* body) {
    const timer_config_t* cfg = g.cfg;

    settings_open(body, "timer", "Timer",
                  "Durations accept <code>5:00</code> or a plain number of seconds.");

    g_string_append(body, "<section class='card'><h2>Default duration</h2>");
    g_string_append(body, "<p class='note'>What the timer counts down from.</p>");
    append_duration_input(body, "Duration", "DefaultSeconds", cfg->default_seconds, "5:00");
    g_string_append(body,
                    "<p class='sub'>The timer returns to this whenever it finishes or is "
                    "reset.</p>");
    g_string_append(body, "</section>");

    g_string_append(body, "<section class='card'><h2>Maximum duration</h2>");
    g_string_append(body, "<p class='note'>The upper limit for any timer.</p>");
    append_duration_input(body, "Maximum", "MaxSeconds", cfg->max_seconds, "9:59:59");
    g_string_append(body,
                    "<p class='sub'>Applies however the timer is set - buttons, quick timers or "
                    "the API.</p>");
    g_string_append(body, "</section>");

    settings_close(body, saved);
}

/***** Settings: C8310 **********************************************************/

static void render_settings_c8310(bool saved, GString* body) {
    const timer_config_t* cfg = g.cfg;

    settings_open(body, "c8310", "C8310",
                  "What each button does. Every button also fires its own rule engine condition "
                  "when pressed, whatever it is set to here.");

    static const int ASSIGNABLE_IDX[ASSIGNABLE_BUTTONS] = {BTN_1, BTN_2, BTN_3, BTN_MUTE};
    for (int slot = 0; slot < ASSIGNABLE_BUTTONS; slot++) {
        const button_cfg_t* b = &cfg->buttons[slot];

        g_string_append_printf(body,
                               "<section class='card'><h2>%s</h2>"
                               "<label>When pressed</label>"
                               "<select id='Btn%dAction' name='Btn%dAction' data-slot='%d'>",
                               BTN_DISPLAY_NAME[ASSIGNABLE_IDX[slot]], slot, slot, slot);
        for (int a = 0; a < BTN_ACTION_COUNT; a++) {
            button_action_t act = (button_action_t)a;
            g_string_append_printf(body,
                                   "<option value='%s'%s>%s</option>",
                                   button_action_id(act),
                                   b->action == act ? " selected" : "",
                                   button_action_label(act));
        }
        g_string_append(body, "</select>");

        char value[16];
        timer_format_time(b->value_seconds, value, sizeof(value));
        g_string_append_printf(body,
                               "<div id='btnvalue%d' style='display:%s'>"
                               "<label>Duration</label>"
                               "<input type=text id='btnvalinput%d' name='Btn%dValue' value='%s' "
                               "placeholder='5:00'></div>",
                               slot, button_action_uses_value(b->action) ? "" : "none",
                               slot, slot, value);
        g_string_append(body, "</section>");
    }

    // One card each, so they sit in the grid as peers of the four
    // assignable buttons rather than as a combined afterthought.
    g_string_append(body, "<section class='card'><h2>Volume up</h2>");
    g_string_append(body, "<p class='note'>Not assignable. Always adds time.</p>");
    append_duration_input(body, "Adds", "StepUpSeconds", cfg->step_up_seconds, "1:00");
    g_string_append(body,
                    "<p class='sub'>While a timer is running this extends that run only.</p>");
    g_string_append(body, "</section>");

    g_string_append(body, "<section class='card'><h2>Volume down</h2>");
    g_string_append(body, "<p class='note'>Not assignable. Always subtracts time.</p>");
    append_duration_input(body, "Subtracts", "StepDownSeconds", cfg->step_down_seconds, "1:00");
    g_string_append(body,
                    "<p class='sub'>While a timer is running this shortens that run only.</p>");
    g_string_append(body, "</section>");

    settings_close(body, saved);
}

// One clip picker: the device's own clips, plus repeat and volume.
// Rendered inline on three different pages - the sound a timer starts
// with, the sound each threshold makes as it is crossed, and the sound it
// finishes with - so the choice always sits next to the thing it belongs
// to rather than in a separate list somewhere else.
//
// `prefix` namespaces the field names ("Start", "Finish", "T1"...), and
// `clips` is the shared list so a page with several pickers only asks the
// device once.
static void append_clip_fields(GString* body,
                               const char* label,
                               const char* prefix,
                               const clip_cfg_t* clip,
                               GArray* clips) {
    g_string_append_printf(body, "<label>%s</label><select name='%sClipId'>", label, prefix);
    g_string_append_printf(body, "<option value='-1'%s>None</option>", clip->id < 0 ? " selected" : "");

    bool listed = false;
    if (clips) {
        for (guint i = 0; i < clips->len; i++) {
            media_clip_t* c = &g_array_index(clips, media_clip_t, i);
            char name_esc[192];
            html_escape(c->name, name_esc, sizeof(name_esc));
            g_string_append_printf(body,
                                   "<option value='%d'%s>%s</option>",
                                   c->id, c->id == clip->id ? " selected" : "", name_esc);
            if (c->id == clip->id)
                listed = true;
        }
    }
    // Keep a selected-but-missing clip in the list rather than silently
    // resetting the setting to None: the device may just be slow to
    // answer, and quietly dropping someone's chosen sound is worse than
    // showing it as unavailable.
    if (clip->id >= 0 && !listed) {
        char name_esc[192];
        html_escape(clip->name[0] ? clip->name : "Saved clip", name_esc, sizeof(name_esc));
        g_string_append_printf(body,
                               "<option value='%d' selected>%s (not found on this device)</option>",
                               clip->id, name_esc);
    }
    g_string_append(body, "</select>");

    g_string_append_printf(body,
                           "<div class='row'>"
                           "<div><label>Repeat</label>"
                           "<input type=number name='%sClipRepeat' min='-1' value='%d'>"
                           "<span class='sublabel'>0 = once, -1 = forever</span></div>"
                           "<div><label>Volume</label>"
                           "<input type=number name='%sClipVolume' min='0' max='1000' value='%d'>"
                           "<span class='sublabel'>percent</span></div>"
                           "</div>",
                           prefix, clip->repeat, prefix, clip->volume);
}

// Standard note for the clip pickers, so the same explanation of where
// clips come from doesn't drift between the three pages that carry one.
#define CLIP_SOURCE_NOTE                                                                           \
    "<p class='sub'>Clips come from the device's own Audio &gt; Clips page - upload them there "    \
    "and they appear here.</p>"

/***** Settings: Display ********************************************************/
//
// Four pages rather than one. Each is short enough to take in at a
// glance, and because every settings form posts its own `section` marker
// (see settings_open), saving one can't disturb the others.

// Shared preamble for the display pages, so the explanation of the text
// placeholders doesn't have to be repeated four times or, worse, drift.
#define DISPLAY_TEXT_HELP                                                                          \
    "Text supports <code>{time}</code>, <code>{hh}</code>, <code>{mm}</code>, <code>{ss}</code> "   \
    "and <code>{total}</code>."

static void render_settings_normal(bool saved, GString* body) {
    const timer_config_t* cfg = g.cfg;

    settings_open(body, "timer-normal", "Timer - Normal",
                  "Colors from the start of a countdown until the first threshold. "
                  DISPLAY_TEXT_HELP);

    g_string_append(body, "<section class='card'><h2>Normal colors</h2>");
    append_state_fields(body, 0, &cfg->states[0], "statescreen0", "5:00");
    g_string_append(body, "</section>");

    GArray* clips = mediaclip_list();
    g_string_append(body, "<section class='card'><h2>Sound when the timer starts</h2>");
    g_string_append(body,
                    "<p class='note'>Played on a fresh start, not on a resume.</p>");
    append_clip_fields(body, "Audio clip", "Start", &cfg->start_clip, clips);
    g_string_append(body, CLIP_SOURCE_NOTE "</section>");
    if (clips)
        g_array_unref(clips);

    settings_close(body, saved);
}

// The Appearance and Message cards, shared by the paused and finished
// pages: same controls, same order, different field names. Appearance
// comes first because the line count it carries decides how many message
// fields there are.
typedef struct {
    const char* lines;
    const char* size;
    const char* line1;
    const char* line2;
    const char* line3;
    const char* bg;
    const char* fg;
    const char* dir;
    const char* speed;
    const char* screen_id;
    // Whether {paused_at_*} belongs in this page's modifier list.
    bool paused_tokens;
} message_fields_t;

static void append_message_cards(GString* body,
                                 const message_fields_t* f,
                                 int lines,
                                 const char* text_size,
                                 const char* l1,
                                 const char* l2,
                                 const char* l3,
                                 const char* bg,
                                 const char* fg,
                                 const char* dir,
                                 int speed,
                                 const char* sample_time,
                                 const char* placeholder1,
                                 const char* placeholder2,
                                 // Finished only: how long the message stays up. NULL on the
                                 // paused page, where the message lasts until something changes.
                                 const char* timing_field,
                                 int timing_seconds) {
    g_string_append(body, "<section class='card'><h2>Appearance</h2>");
    // Size first: it decides how many lines are on offer below it.
    g_string_append(body, "<label>Text size</label>");
    append_size_select(body, f->size, text_size);
    g_string_append(body, "<label>Lines</label>");
    append_lines_select(body, f->lines, lines, f->size, f->dir);
    g_string_append(body,
                    "<p class='sub'>Small fits 3 lines, medium 2, large 1. Changing the size "
                    "sets the count to its maximum - lower it if you want fewer. Horizontal "
                    "scrolling keeps those lines as rows and left-aligns them.</p>");
    g_string_append(body, "<label>Scroll direction</label>");
    append_direction_select(body, f->dir, dir);
    g_string_append(body,
                    "<p class='sub'>Bottom to top is always one line - the display breaks it "
                    "between words to fit the width, so only the text size changes the layout.</p>");
    g_string_append(body, "<label>Scroll speed</label>");
    append_speed_select(body, f->speed, speed);
    // Colors last, so they sit directly above the preview that shows
    // them.
    g_string_append_printf(body,
                           "<div class='row'>"
                           "<div><label>Background</label>"
                           "<input type=color id='%s' name='%s' value='%s'></div>"
                           "<div><label>Text color</label>"
                           "<input type=color id='%s' name='%s' value='%s'></div>"
                           "</div>",
                           f->bg, f->bg, bg, f->fg, f->fg, fg);

    g_string_append(body, "<div style='margin-top:14px'>");
    append_preview(body, NULL, f->screen_id, "", fg, bg, text_size, dir, speed,
                   f->bg, f->fg, f->line1, f->line2, f->line3, f->lines, f->size, f->dir, f->speed,
                   sample_time);
    g_string_append(body, "</div></section>");

    char e1[256], e2[256], e3[256];
    html_escape(l1, e1, sizeof(e1));
    html_escape(l2, e2, sizeof(e2));
    html_escape(l3, e3, sizeof(e3));

    g_string_append(body, "<section class='card'><h2>Message</h2>");
    const char* const LINE_LABEL[3] = {"Line one", "Line two", "Line three"};
    const char* const line_field[3] = {f->line1, f->line2, f->line3};
    const char* const line_value[3] = {e1, e2, e3};
    const char* const line_hint[3]  = {placeholder1, placeholder2, ""};
    for (int i = 0; i < 3; i++) {
        g_string_append_printf(body, "<div id='%sRow%d'>", f->lines, i + 1);
        append_modifier_field(body, LINE_LABEL[i], line_field[i], line_value[i], line_hint[i],
                              f->paused_tokens);
        g_string_append(body, "</div>");
    }
    g_string_append(body,
                    "<p class='sub'><code>{paused_at_24}</code> gives <code>14:32</code>, "
                    "<code>{paused_at_12}</code> gives <code>2:32 PM</code>. Both are empty when "
                    "the timer is not paused.</p>");

    if (timing_field) {
        append_duration_input(body, "Show it for", timing_field, timing_seconds, "0:10");
        g_string_append(body, "<p class='sub'>0 skips the message.</p>");
    }

    g_string_append(body, "</section>");
}

static void render_settings_paused(bool saved, GString* body) {
    const timer_config_t* cfg = g.cfg;

    settings_open(body, "timer-paused", "Timer - Paused",
                  "What the display shows, and what plays, while a timer is paused. "
                  DISPLAY_TEXT_HELP);

    static const message_fields_t PAUSED = {
        "PausedLines", "PausedSize", "PausedLine1", "PausedLine2", "PausedLine3",
        "PausedBg",    "PausedFg",   "PausedDir",   "PausedSpeed", "pausedscreen", true};
    append_message_cards(body, &PAUSED, cfg->paused_lines, cfg->paused_text_size,
                         cfg->paused_line1, cfg->paused_line2,
                         cfg->paused_line3, cfg->paused_bg, cfg->paused_text_color,
                         cfg->paused_scroll_direction, cfg->paused_scroll_speed, "5:00",
                         "PAUSED AT {paused_at_12}", "{time} REMAINING", NULL, 0);

    GArray* clips = mediaclip_list();
    g_string_append(body, "<section class='card'><h2>Sound when the timer is paused</h2>");
    g_string_append(body, "<p class='note'>Played once, when the pause starts.</p>");
    append_clip_fields(body, "Audio clip", "Paused", &cfg->paused_clip, clips);
    g_string_append(body, CLIP_SOURCE_NOTE "</section>");

    g_string_append(body, "<section class='card'><h2>Sound when the timer is resumed</h2>");
    g_string_append(body, "<p class='note'>Played once, when the countdown picks back up.</p>");
    append_clip_fields(body, "Audio clip", "Resume", &cfg->resume_clip, clips);
    g_string_append(body, CLIP_SOURCE_NOTE "</section>");
    if (clips)
        g_array_unref(clips);

    settings_close(body, saved);
}

static void render_settings_thresholds(bool saved, GString* body) {
    const timer_config_t* cfg = g.cfg;

    settings_open(body, "timer-thresholds", "Timer - Thresholds",
                  "Change the display, and optionally play a clip, as the countdown passes each "
                  "point. Up to five, sorted on save. " DISPLAY_TEXT_HELP);

    GArray* clips = mediaclip_list();

    // Every slot is rendered whether in use or not, with a hidden Used
    // flag deciding visibility - so adding a color change is instant
    // rather than a save-and-reload round trip, and the server still gets
    // an unambiguous picture of which cards to keep.
    for (int i = 1; i <= MAX_THRESHOLDS; i++) {
        bool used                 = (i < cfg->state_count);
        const display_state_t* st = &cfg->states[i];

        // An unused slot still needs sensible values in its inputs, or
        // enabling it would produce a black-on-black card at 0 seconds.
        display_state_t blank;
        if (!used) {
            memset(&blank, 0, sizeof(blank));
            blank.at_seconds = 30;
            g_strlcpy(blank.bg_color, "#c00000", sizeof(blank.bg_color));
            g_strlcpy(blank.text_color, "#ffffff", sizeof(blank.text_color));
            g_strlcpy(blank.text, "{time}", sizeof(blank.text));
            st = &blank;
        }

        char at_value[16];
        timer_format_time(st->at_seconds, at_value, sizeof(at_value));

        g_string_append_printf(body,
                               "<input type=hidden id='ThresholdUsed%d' name='ThresholdUsed%d' "
                               "value='%s'>"
                               "<section class='card' id='thresholdcard%d' style='display:%s'>"
                               "<h2>Threshold %d</h2>",
                               i, i, used ? "1" : "0", i, used ? "" : "none", i);

        g_string_append_printf(body,
                               "<label>When this much time is left</label>"
                               "<input type=text id='%s' name='%s' value='%s' placeholder='0:30'>",
                               STATE_AT_FIELD[i], STATE_AT_FIELD[i], at_value);

        char screen_id[32];
        snprintf(screen_id, sizeof(screen_id), "thresholdscreen%d", i);
        append_state_fields(body, i, st, screen_id, at_value);

        char prefix[8];
        snprintf(prefix, sizeof(prefix), "T%d", i);
        g_string_append_printf(body, "<div id='tclip%d'>", i);
        append_clip_fields(body, "Audio clip", prefix, &st->clip, clips);
        g_string_append(body, "</div>");

        g_string_append_printf(body,
                               "<button type=button class='btn danger full' data-remove='%d' "
                               "style='margin-top:14px'>Remove</button></section>",
                               i);
    }
    if (clips)
        g_array_unref(clips);

    // A full-size card of its own, so it sits in the grid as a peer of
    // the color changes it adds rather than as a stray button underneath
    // them.
    g_string_append(body,
                    "<section class='card addcard' id='addthresholdcard'>"
                    "<button type=button id='addthreshold'>"
                    "<span class='plus'>+</span>"
                    "<span class='addlabel'>Add a threshold</span>"
                    "</button></section>");

    settings_close(body, saved);
}

static void render_settings_finished(bool saved, GString* body) {
    const timer_config_t* cfg = g.cfg;

    settings_open(body, "timer-finished", "Timer - Finished",
                  "Shown when the countdown reaches zero. This one can scroll. " DISPLAY_TEXT_HELP);

    static const message_fields_t FINISHED = {
        "FinishedLines", "FinishedSize", "FinishedLine1", "FinishedLine2", "FinishedLine3",
        "FinishedBg",    "FinishedFg",   "FinishedDir",   "FinishedSpeed", "finishedscreen",
        false};
    append_message_cards(body, &FINISHED, cfg->finished_lines, cfg->finished_text_size,
                         cfg->finished_line1,
                         cfg->finished_line2, cfg->finished_line3, cfg->finished_bg,
                         cfg->finished_text_color, cfg->finished_scroll_direction,
                         cfg->finished_scroll_speed, "0:00", "TIME\'S UP", "",
                         "FinishedSeconds", cfg->finished_display_seconds);

    GArray* clips = mediaclip_list();
    g_string_append(body, "<section class='card'><h2>Sound when the timer finishes</h2>");
    g_string_append(body,
                    "<p class='note'>Played when the countdown reaches zero. Stop &amp; reset "
                    "stops it.</p>");
    append_clip_fields(body, "Audio clip", "Finish", &cfg->finish_clip, clips);
    g_string_append(body, CLIP_SOURCE_NOTE "</section>");
    if (clips)
        g_array_unref(clips);

    settings_close(body, saved);
}

static void render_settings_sharing(bool saved, GString* body) {
    const timer_config_t* cfg = g.cfg;

    settings_open(body, "display-sharing", "Display Sharing",
                  "Only one thing can drive the screen at a time. These decide when this app hands "
                  "it back.");

    g_string_append_printf(body,
                           "<section class='card'><h2>When no timer is running</h2>"
                           "<p class='note'>Release clears the screen when there is nothing to "
                           "show. Keep leaves the last frame up.</p>"
                           "<label>Idle behavior</label>"
                           "<select name='ReleaseIdle'>"
                           "<option value='yes'%s>Release the display</option>"
                           "<option value='no'%s>Keep the last frame on screen</option>"
                           "</select></section>",
                           cfg->release_display_when_idle ? " selected" : "",
                           cfg->release_display_when_idle ? "" : " selected");

    g_string_append_printf(body,
                           "<section class='card'><h2>When a timer is paused</h2>"
                           "<p class='note'>A paused countdown stays on screen by default.</p>"
                           "<label>Paused behavior</label>"
                           "<select name='ReleasePaused'>"
                           "<option value='no'%s>Keep showing the countdown</option>"
                           "<option value='yes'%s>Release the display</option>"
                           "</select></section>",
                           cfg->release_display_when_paused ? "" : " selected",
                           cfg->release_display_when_paused ? " selected" : "");

    settings_close(body, saved);
}

/***** Settings: Sounds *********************************************************/

/***** API & rules page *********************************************************/

typedef struct {
    const char* path; // query string, e.g. "?api=start"
    const char* what;
    const char* why;
} api_entry_t;

static const api_entry_t API_ENTRIES[] = {
    {"?api=start", "Start", "Starts the timer, or resumes it if it was paused."},
    {"?api=pause", "Pause", "Freezes the countdown where it is. Start resumes from there."},
    {"?api=toggle", "Start or pause", "Whichever applies right now - one URL for a single button."},
    {"?api=stop", "Stop and reset", "Stops the timer and puts it back to its set duration."},
    {"?api=set&seconds=300", "Set the duration",
     "Sets what the timer counts down from. Accepts seconds (300) or clock time (5:00)."},
    {"?api=add&seconds=60", "Add time",
     "Adds to the timer. While it is running this extends the current run only."},
    {"?api=add&seconds=-60", "Subtract time", "The same call with a negative value."},
    {"?api=quick&seconds=300", "Start a timer now",
     "Sets the duration and starts immediately, whatever the timer was doing."},
    {"?api=button&n=3", "Press a C8310 button",
     "Does exactly what pressing that physical button does, including firing its condition. "
     "1-3 are the numbered buttons, 4 is Volume up, 5 is Volume down, 6 is Mute."},
    {"?api=status", "Read the current state",
     "Returns JSON: state, remaining, duration, and the exact frame the display is showing."},
};

static void render_api(const char* host, bool via_proxy, GString* body) {
    (void)via_proxy;

    // Best-effort prefix for the no-JavaScript case only. Behind the
    // reverse proxy the Host header is this app's own proxy target, not
    // the address anyone typed, so API_JS rebuilds these from
    // location.origin + location.pathname the moment the page loads.
    char base[512];
    snprintf(base,
             sizeof(base),
             "https://%s/local/%s/ui",
             host && host[0] ? host : "device-ip",
             APP_NAME);

    append_head(body, "C17 Timer - API");
    append_nav(body, "api");

    g_string_append(body, "<h1>API</h1>");
    g_string_append(body,
                    "<p class='lede'>Plain GET requests returning JSON. Durations accept seconds "
                    "(<code>300</code>) or clock time (<code>5:00</code>).</p>");

    g_string_append(body, "<div class='cards'>");

    g_string_append(body,
                    "<section class='card wide'>"
                    "<h2>Requests</h2>"
                    "<div class='apigrid'>");

    for (size_t i = 0; i < G_N_ELEMENTS(API_ENTRIES); i++) {
        char id[24];
        snprintf(id, sizeof(id), "api%zu", i);
        g_string_append_printf(body,
                               "<div class='api'>"
                               "<div class='what'>%s</div>"
                               "<div class='why'>%s</div>"
                               "<div class='urlrow'>"
                               "<code id='%s' data-path='%s'>%s%s</code>"
                               "<button type=button class='copy' data-target='%s'>Copy</button>"
                               "</div></div>",
                               API_ENTRIES[i].what, API_ENTRIES[i].why,
                               id, API_ENTRIES[i].path,
                               base, API_ENTRIES[i].path,
                               id);
    }
    g_string_append(body, "</div></section>");

    g_string_append(body, "</div>");
    g_string_append(body, "<script src='?action=api-js'></script>");
    append_foot(body);
}

/***** Rules page ***************************************************************/

static void render_rules(GString* body) {
    append_head(body, "C17 Timer - Rules");
    append_nav(body, "rules");

    g_string_append(body, "<h1>Rules</h1>");
    g_string_append(body,
                    "<p class='lede'>How the timer and the events engine drive each "
                    "other.</p>");

    g_string_append(body, "<div class='cards'>");

    g_string_append(body,
                    "<section class='card'><h2>Making the timer control a rule</h2>"
                    "<p class='note'>Found under <b>Application</b> in the condition "
                    "picker.</p>"
                    "<p class='sub'>"
                    "<b>C17 Timer: Timer event</b><br>"
                    "Started, Paused, Resumed, Stopped or Finished. A one-off pulse - use it to "
                    "play a clip, send a notification, flip an output.<br><br>"
                    "<b>C17 Timer: Timer running</b><br>"
                    "True while a timer is counting. Pair with the \"...while the rule is "
                    "active\" actions.<br><br>"
                    "<b>C17 Timer: Threshold reached</b><br>"
                    "Fires as the countdown crosses into each threshold, numbered in the order "
                    "they appear on the Timer - Thresholds page.<br><br>"
                    "<b>C17 Timer: Button pressed</b><br>"
                    "Any of the six C8310 buttons, physical or virtual, whatever that button is "
                    "otherwise set to do.</p></section>");

    g_string_append(body,
                    "<section class='card'><h2>Making a rule control the timer</h2>"
                    "<p class='note'>An ACAP can add conditions but not actions, so a rule "
                    "drives the timer through the HTTP notification action.</p>"
                    "<p class='sub'><b>System &gt; Events &gt; Rules &gt; Add a rule</b><br><br>"
                    "<b>Condition:</b> whatever should start the timer - a schedule, a digital "
                    "input, a call button, an MQTT message.<br>"
                    "<b>Action:</b> Notification &gt; Send notification through HTTP.<br>"
                    "<b>URL:</b> copy the verb you want from the API page.<br>"
                    "<b>Username and password:</b> a device account - the request is "
                    "authenticated even though the device is calling itself.</p>"
                    "</section>");

    g_string_append(body, "</div>");
    append_foot(body);
}

/***** Applying a saved settings form *******************************************/

// Reads a color field, keeping the existing value if the field is missing
// or not a well-formed #RRGGBB. <input type=color> always submits a valid
// value, so this is really about a hand-crafted POST or a truncated body -
// but the display API rejects a malformed color outright, so it is worth
// never letting one through.
static void apply_color(const char* post, const char* field, char* dest, size_t destlen) {
    char v[32];
    if (!query_get(post, field, v, sizeof(v)))
        return;
    if (v[0] != '#' || strlen(v) != 7)
        return;
    for (int i = 1; i < 7; i++)
        if (!g_ascii_isxdigit(v[i]))
            return;
    // Normalized to lower case so two colors differing only in case don't
    // look like a change in the saved file.
    g_strlcpy(dest, v, destlen);
    for (char* p = dest; *p; p++)
        *p = g_ascii_tolower(*p);
}

static void apply_text(const char* post, const char* field, char* dest, size_t destlen) {
    char v[512];
    if (query_get(post, field, v, sizeof(v)))
        g_strlcpy(dest, v, destlen);
}

static void apply_int(const char* post, const char* field, int* dest, int min, int max) {
    char v[32];
    if (!query_get(post, field, v, sizeof(v)) || !v[0])
        return;
    int n = atoi(v); // NOLINT
    if (n < min)
        n = min;
    if (n > max)
        n = max;
    *dest = n;
}

static void apply_duration(const char* post, const char* field, int* dest, int min, int max) {
    char v[32];
    if (!query_get(post, field, v, sizeof(v)) || !v[0])
        return;
    int n = parse_duration(v);
    if (n < min)
        n = min;
    if (n > max)
        n = max;
    *dest = n;
}

static void apply_bool(const char* post, const char* field, bool* dest, const char* true_value) {
    char v[16];
    if (query_get(post, field, v, sizeof(v)))
        *dest = (strcmp(v, true_value) == 0);
}

// Sorts color changes by remaining-time, largest first.
//
// This matters for correctness, not tidiness: the timer picks the LAST
// matching state as it walks the list, so with rows in the order
// [30s, 10s] a timer at 5 seconds correctly lands on the 10s row, while
// [10s, 30s] would wrongly land on the 30s one. Sorting here means the
// order rows happened to be added in can never change what the display
// does.
static int compare_states_desc(const void* a, const void* b) {
    const display_state_t* sa = a;
    const display_state_t* sb = b;
    if (sa->at_seconds == sb->at_seconds)
        return 0;
    return (sa->at_seconds > sb->at_seconds) ? -1 : 1;
}

static void apply_clip(const char* post, const char* prefix, clip_cfg_t* clip) {
    char field[32];
    snprintf(field, sizeof(field), "%sClipId", prefix);
    apply_int(post, field, &clip->id, -1, 9999);
    snprintf(field, sizeof(field), "%sClipRepeat", prefix);
    apply_int(post, field, &clip->repeat, -1, 999);
    snprintf(field, sizeof(field), "%sClipVolume", prefix);
    apply_int(post, field, &clip->volume, 0, 1000);

    // Cache the clip's name alongside its id, so the settings page can
    // still label it if the device is slow (or refuses) to list clips
    // next time.
    if (clip->id >= 0)
        mediaclip_name_for(clip->id, clip->name, sizeof(clip->name));
    else
        clip->name[0] = '\0';
}

// Applies one settings page's form. `section` says which page it came
// from, which is what keeps the four forms from overwriting each other -
// see settings_open() for why that matters.
static void apply_settings_post(const char* post) {
    timer_config_t* cfg = g.cfg;

    char section[24] = {0};
    query_get(post, "section", section, sizeof(section));

    if (strcmp(section, "timer") == 0) {
        apply_duration(post, "DefaultSeconds", &cfg->default_seconds, 0, 359999);
        apply_duration(post, "MaxSeconds", &cfg->max_seconds, 1, 359999);
        // A default longer than the maximum would leave the timer
        // permanently clamped below the value this page claims it is set
        // to.
        if (cfg->default_seconds > cfg->max_seconds)
            cfg->default_seconds = cfg->max_seconds;

    } else if (strcmp(section, "c8310") == 0) {
        apply_duration(post, "StepUpSeconds", &cfg->step_up_seconds, 1, 3600);
        apply_duration(post, "StepDownSeconds", &cfg->step_down_seconds, 1, 3600);
        for (int slot = 0; slot < ASSIGNABLE_BUTTONS; slot++) {
            char field[24], v[32];
            snprintf(field, sizeof(field), "Btn%dAction", slot);
            if (query_get(post, field, v, sizeof(v)))
                cfg->buttons[slot].action = button_action_from_id(v);
            snprintf(field, sizeof(field), "Btn%dValue", slot);
            apply_duration(post, field, &cfg->buttons[slot].value_seconds, 0, cfg->max_seconds);
        }

    } else if (strcmp(section, "timer-normal") == 0) {
        // states[0] is the normal state; its at_seconds is meaningless
        // and stays -1 whatever anything else says.
        apply_color(post, STATE_BG_FIELD[0], cfg->states[0].bg_color, sizeof(cfg->states[0].bg_color));
        apply_color(post, STATE_FG_FIELD[0], cfg->states[0].text_color, sizeof(cfg->states[0].text_color));
        apply_text(post, STATE_TEXT_FIELD[0], cfg->states[0].text, sizeof(cfg->states[0].text));
        apply_clip(post, "Start", &cfg->start_clip);
        cfg->states[0].at_seconds = -1;

    } else if (strcmp(section, "timer-paused") == 0) {
        apply_int(post, "PausedLines", &cfg->paused_lines, 1, 3);
        apply_text(post, "PausedSize", cfg->paused_text_size, sizeof(cfg->paused_text_size));
        apply_text(post, "PausedLine1", cfg->paused_line1, sizeof(cfg->paused_line1));
        apply_text(post, "PausedLine2", cfg->paused_line2, sizeof(cfg->paused_line2));
        apply_text(post, "PausedLine3", cfg->paused_line3, sizeof(cfg->paused_line3));
        apply_color(post, "PausedBg", cfg->paused_bg, sizeof(cfg->paused_bg));
        apply_color(post, "PausedFg", cfg->paused_text_color, sizeof(cfg->paused_text_color));
        apply_text(post, "PausedDir", cfg->paused_scroll_direction,
                   sizeof(cfg->paused_scroll_direction));
        apply_int(post, "PausedSpeed", &cfg->paused_scroll_speed, 0, 10);
        apply_clip(post, "Paused", &cfg->paused_clip);
        apply_clip(post, "Resume", &cfg->resume_clip);

    } else if (strcmp(section, "timer-thresholds") == 0) {
        // Collect whichever color-change cards came back marked in use,
        // then compact and sort them into states[1..]. Rebuilt from
        // scratch each save rather than edited in place, so removing a
        // card genuinely removes it instead of leaving a stale entry.
        display_state_t kept[MAX_THRESHOLDS];
        int kept_count = 0;
        for (int i = 1; i <= MAX_THRESHOLDS; i++) {
            char field[24], v[8];
            snprintf(field, sizeof(field), "ThresholdUsed%d", i);
            if (!query_get(post, field, v, sizeof(v)) || strcmp(v, "1") != 0)
                continue;

            display_state_t st;
            memset(&st, 0, sizeof(st));
            st.at_seconds = 30;
            g_strlcpy(st.bg_color, "#c00000", sizeof(st.bg_color));
            g_strlcpy(st.text_color, "#ffffff", sizeof(st.text_color));
            g_strlcpy(st.text, "{time}", sizeof(st.text));

            apply_duration(post, STATE_AT_FIELD[i], &st.at_seconds, 0, cfg->max_seconds);
            apply_color(post, STATE_BG_FIELD[i], st.bg_color, sizeof(st.bg_color));
            apply_color(post, STATE_FG_FIELD[i], st.text_color, sizeof(st.text_color));
            apply_text(post, STATE_TEXT_FIELD[i], st.text, sizeof(st.text));

            char prefix[8];
            snprintf(prefix, sizeof(prefix), "T%d", i);
            st.clip.id     = -1;
            st.clip.repeat = 0;
            st.clip.volume = 100;
            apply_clip(post, prefix, &st.clip);

            kept[kept_count++] = st;
        }
        qsort(kept, (size_t)kept_count, sizeof(kept[0]), compare_states_desc);
        for (int i = 0; i < kept_count; i++)
            cfg->states[i + 1] = kept[i];
        for (int i = kept_count + 1; i < MAX_DISPLAY_STATES; i++)
            memset(&cfg->states[i], 0, sizeof(cfg->states[i]));
        cfg->state_count = kept_count + 1;

    } else if (strcmp(section, "timer-finished") == 0) {
        apply_int(post, "FinishedLines", &cfg->finished_lines, 1, 3);
        apply_text(post, "FinishedSize", cfg->finished_text_size, sizeof(cfg->finished_text_size));
        apply_text(post, "FinishedLine1", cfg->finished_line1, sizeof(cfg->finished_line1));
        apply_text(post, "FinishedLine2", cfg->finished_line2, sizeof(cfg->finished_line2));
        apply_text(post, "FinishedLine3", cfg->finished_line3, sizeof(cfg->finished_line3));
        apply_color(post, "FinishedBg", cfg->finished_bg, sizeof(cfg->finished_bg));
        apply_color(post, "FinishedFg", cfg->finished_text_color, sizeof(cfg->finished_text_color));
        apply_text(post, "FinishedDir", cfg->finished_scroll_direction,
                   sizeof(cfg->finished_scroll_direction));
        apply_int(post, "FinishedSpeed", &cfg->finished_scroll_speed, 0, 10);
        apply_duration(post, "FinishedSeconds", &cfg->finished_display_seconds, 0, 3600);
        apply_clip(post, "Finish", &cfg->finish_clip);

    } else if (strcmp(section, "display-sharing") == 0) {
        apply_bool(post, "ReleaseIdle", &cfg->release_display_when_idle, "yes");
        apply_bool(post, "ReleasePaused", &cfg->release_display_when_paused, "yes");

    } else {
        syslog(LOG_WARNING, "Web UI: settings POST with unknown section '%s' - ignored", section);
        return;
    }

    local_config_save(cfg);
    if (g.on_config_saved)
        g.on_config_saved();
}

/***** Request dispatch *********************************************************/

// Runs one API verb and writes its JSON result. Shared by the ?api=
// routes and by the home page's own buttons, so a click and an API call
// can never diverge in what they actually do.
//
// `changed` distinguishes "did the thing" from "there was nothing to do" -
// starting an already-running timer answers ok:true, changed:false rather
// than pretending something happened.
static void run_verb(const char* verb, const char* query, GString* out, bool json_response) {
    bool ok             = true;
    bool changed        = false;
    const char* message = "";

    char sec_buf[32] = {0};
    // "seconds" is the documented parameter, but it accepts clock time
    // too, and "time" reads more naturally for that - so both work.
    if (!query_get(query, "seconds", sec_buf, sizeof(sec_buf)))
        query_get(query, "time", sec_buf, sizeof(sec_buf));
    int seconds = parse_duration(sec_buf);

    if (strcmp(verb, "start") == 0) {
        changed = timer_start();
        if (!changed)
            message = (timer_get_state() == TIMER_RUNNING) ? "already running"
                                                           : "nothing to start - the timer is at zero";
    } else if (strcmp(verb, "pause") == 0) {
        changed = timer_pause();
        if (!changed)
            message = "not running";
    } else if (strcmp(verb, "toggle") == 0) {
        changed = timer_toggle();
    } else if (strcmp(verb, "resume") == 0) {
        // Not advertised on the API page - "start" already resumes - but
        // accepted because it is the obvious thing to reach for.
        changed = timer_start();
    } else if (strcmp(verb, "finish") == 0) {
        // Not on the API page, but accepted so a rule can do what the
        // C8310's Finish button does.
        changed = timer_finish();
        if (!changed)
            message = "already finished";
    } else if (strcmp(verb, "stop") == 0 || strcmp(verb, "reset") == 0) {
        changed = timer_stop_reset();
        if (!changed)
            message = "already stopped and reset";
    } else if (strcmp(verb, "set") == 0) {
        if (!sec_buf[0]) {
            ok      = false;
            message = "set needs a seconds parameter, e.g. ?api=set&seconds=300";
        } else {
            changed = timer_set(seconds);
        }
    } else if (strcmp(verb, "add") == 0) {
        if (!sec_buf[0]) {
            ok      = false;
            message = "add needs a seconds parameter, e.g. ?api=add&seconds=60";
        } else {
            changed = timer_add(seconds);
        }
    } else if (strcmp(verb, "quick") == 0) {
        if (!sec_buf[0]) {
            ok      = false;
            message = "quick needs a seconds parameter, e.g. ?api=quick&seconds=300";
        } else {
            changed = timer_quick(seconds);
            if (!changed)
                message = "quick needs a duration greater than zero";
        }
    } else if (strcmp(verb, "button") == 0) {
        char n_buf[8] = {0};
        query_get(query, "n", n_buf, sizeof(n_buf));
        int n = atoi(n_buf); // NOLINT
        if (n < 1 || n > BTN_COUNT) {
            ok      = false;
            message = "button needs n=1..6";
        } else {
            buttons_press(n - 1, true);
            changed = true;
        }
    } else if (strcmp(verb, "status") == 0) {
        // Pure read - nothing changed, and that's the honest answer.
    } else {
        ok      = false;
        message = "unknown action";
    }

    if (json_response)
        append_status_json(out, changed, ok, message);
}

static void handle_request(const char* method,
                           const char* query,
                           const char* post_body,
                           const char* host,
                           bool via_proxy,
                           GString* body) {
    char page[32] = {0}, action[32] = {0}, api[32] = {0};
    bool is_post = (strcmp(method, "POST") == 0);

    query_get(query, "page", page, sizeof(page));
    query_get(query, "action", action, sizeof(action));
    query_get(query, "api", api, sizeof(api));

    // No authentication check here, and none needed: this server binds to
    // loopback only, so the sole route to it is Apache's reverse proxy,
    // which is declared admin-only and has already demanded a device
    // login before any of this runs. See webui_start().
    if (strcmp(action, "poll-js") == 0) {
        g_string_append(body, POLL_JS);
        return;
    }
    if (strcmp(action, "settings-js") == 0) {
        g_string_append(body, SETTINGS_JS);
        return;
    }
    if (strcmp(action, "api-js") == 0) {
        g_string_append(body, API_JS);
        return;
    }
    if (strcmp(action, "theme-js") == 0) {
        g_string_append(body, THEME_JS);
        return;
    }

    // --- JSON API ---
    if (api[0]) {
        run_verb(api, query, body, true);
        return;
    }
    if (strcmp(action, "status") == 0) {
        append_status_json(body, false, true, "");
        return;
    }

    // --- Settings pages ---
    bool saved = false;
    if (is_post && page[0]) {
        apply_settings_post(post_body);
        saved = true;
    }

    if (strcmp(page, "timer") == 0) {
        render_settings_timer(saved, body);
        return;
    }
    if (strcmp(page, "c8310") == 0) {
        render_settings_c8310(saved, body);
        return;
    }
    if (strcmp(page, "timer-normal") == 0) {
        render_settings_normal(saved, body);
        return;
    }
    if (strcmp(page, "timer-paused") == 0) {
        render_settings_paused(saved, body);
        return;
    }
    if (strcmp(page, "timer-thresholds") == 0) {
        render_settings_thresholds(saved, body);
        return;
    }
    if (strcmp(page, "timer-finished") == 0) {
        render_settings_finished(saved, body);
        return;
    }
    if (strcmp(page, "display-sharing") == 0) {
        render_settings_sharing(saved, body);
        return;
    }
    if (strcmp(page, "api") == 0) {
        render_api(host, via_proxy, body);
        return;
    }
    if (strcmp(page, "rules") == 0) {
        render_rules(body);
        return;
    }
    // Bookmarks from earlier layouts - send them somewhere useful rather
    // than silently showing Home. The display-* names were what these
    // pages were called before the display and timer settings were
    // merged, and "sounds" was a page of its own before each clip moved
    // next to the thing it belongs to.
    if (strcmp(page, "display-normal") == 0 || strcmp(page, "display") == 0 ||
        strcmp(page, "sounds") == 0) {
        render_settings_normal(saved, body);
        return;
    }
    if (strcmp(page, "display-colors") == 0) {
        render_settings_thresholds(saved, body);
        return;
    }
    if (strcmp(page, "display-finished") == 0) {
        render_settings_finished(saved, body);
        return;
    }
    if (strcmp(page, "settings") == 0) {
        render_settings_timer(saved, body);
        return;
    }

    // --- Home, plus its own buttons ---
    // A home-page button uses the same verbs as the API but re-renders the
    // page rather than answering JSON, so a click behaves like a normal
    // form submission.
    if (action[0])
        run_verb(action, query, body, false);

    render_home(body);
}

/***** HTTP server **************************************************************/

static gboolean on_incoming(GSocketService* service,
                            GSocketConnection* connection,
                            GObject* source_object,
                            gpointer user_data) {
    (void)service;
    (void)source_object;
    (void)user_data;

    GInputStream* in      = g_io_stream_get_input_stream(G_IO_STREAM(connection));
    GDataInputStream* din = g_data_input_stream_new(in);
    // IMPORTANT: GDataInputStream defaults to LF-only newlines. Real HTTP
    // is CRLF, so with the default each line arrives with a trailing '\r'
    // still attached - meaning the blank line that ends the headers comes
    // through as "\r", is never recognized as blank, and the drain loop
    // below blocks forever waiting for a line the browser will never send
    // (it is waiting on us). That's a genuine deadlock, and it cost the
    // C8310 Customizer a debugging session. Accept any newline convention.
    g_data_input_stream_set_newline_type(din, G_DATA_STREAM_NEWLINE_TYPE_ANY);

    gsize len          = 0;
    char* request_line = g_data_input_stream_read_line(din, &len, NULL, NULL);
    if (!request_line) {
        g_object_unref(din);
        return TRUE;
    }

    char method[8]    = {0};
    char target[1024] = {0};
    sscanf(request_line, "%7s %1023s", method, target); // NOLINT
    g_free(request_line);

    // Drain the remaining headers up to the blank line, picking out
    // Content-Length (for POST bodies) and Host (for the API page's
    // copy-paste URLs) on the way past.
    glong content_length     = 0;
    char host[256]           = {0};
    char forwarded_host[256] = {0};
    char* line;
    while ((line = g_data_input_stream_read_line(din, &len, NULL, NULL)) != NULL) {
        bool blank = (line[0] == '\0');
        // "Content-Length:" is 15 characters. Comparing 16 - as an earlier
        // version of the C8310 Customizer did - silently requires a 16th
        // byte matching the literal's own NUL, which no real header has, so
        // the match always failed, every POST body read as empty, and every
        // settings save silently blanked the config. Compare exactly 15.
        if (g_ascii_strncasecmp(line, "Content-Length:", 15) == 0)
            content_length = atol(line + 15); // NOLINT
        else if (g_ascii_strncasecmp(line, "X-Forwarded-Host:", 17) == 0)
            // Set by Apache's mod_proxy and closer to the address the
            // caller actually used than Host is on a proxied request,
            // where Host is this app's own proxy target. Only a fallback
            // either way - the API page rebuilds its URLs in the browser.
            g_strlcpy(forwarded_host, g_strstrip(line + 17), sizeof(forwarded_host));
        else if (g_ascii_strncasecmp(line, "Host:", 5) == 0)
            g_strlcpy(host, g_strstrip(line + 5), sizeof(host));
        g_free(line);
        if (blank)
            break;
    }

    // Form-urlencoded POST body, same shape as a query string so
    // query_get() handles it unchanged. Clamped to a sane size: the
    // settings forms are fixed-shape and never legitimately near this
    // large.
    char post_body[8192] = {0};
    if (content_length > 0) {
        gsize want = (gsize)content_length;
        if (want >= sizeof(post_body))
            want = sizeof(post_body) - 1;
        gsize got = 0;
        g_input_stream_read_all(G_INPUT_STREAM(din), post_body, want, &got, NULL, NULL);
        post_body[got] = '\0';
    }
    g_object_unref(din);

    const char* qmark = strchr(target, '?');
    char path[1024]   = {0};
    char query[1024]  = {0};
    if (qmark) {
        size_t plen = (size_t)(qmark - target);
        if (plen >= sizeof(path))
            plen = sizeof(path) - 1;
        memcpy(path, target, plen);
        path[plen] = '\0';
        g_strlcpy(query, qmark + 1, sizeof(query));
    } else {
        g_strlcpy(path, target, sizeof(path));
    }

    // Log the path but never the query string. Nothing secret travels
    // there any more, but a query string can carry whatever an integrator
    // puts in one, and this line goes straight into the device's syslog.
    syslog(LOG_INFO, "Web UI: %s %s", method, path);

    // manifest.json's reverseProxy mounts this server at
    // /local/c17timer/ui, and Apache forwards the whole original path
    // unchanged rather than stripping that prefix - producing a request
    // line like "GET //local/c17timer/ui" (the doubled slash coming from
    // the proxy target's own trailing "/" plus the forwarded path's
    // leading one). Recognize and strip it here. Requests that arrive
    // without the prefix - Apache's own health checks, say - fall through
    // unchanged.
    const char* route = path;
    bool via_proxy    = false;
    if (route[0] == '/' && route[1] == '/')
        route++;
    {
        static const char mount_prefix[] = "/local/" APP_NAME "/ui";
        size_t mount_len                 = sizeof(mount_prefix) - 1;
        if (strncmp(route, mount_prefix, mount_len) == 0 &&
            (route[mount_len] == '\0' || route[mount_len] == '/')) {
            route += mount_len;
            via_proxy = true;
            if (route[0] == '\0')
                route = "/";
        }
    }

    GString* body      = g_string_new(NULL);
    const char* status = "200 OK";

    if (strcmp(route, "/") == 0) {
        handle_request(method, query, post_body,
                       forwarded_host[0] ? forwarded_host : host, via_proxy, body);
    } else {
        // Browsers request /favicon.ico unprompted on every page load.
        // Answering it with the full page logic would run a timer verb per
        // page load - harmless but confusing in the log.
        status = "404 Not Found";
        g_string_append(body, "Not found.");
    }

    // The JSON and JavaScript routes are query-param routes on this same
    // "/" path, so the content type is worked out from the query string
    // here rather than threaded back out of handle_request().
    const char* content_type = "text/html; charset=utf-8";
    char api_check[32] = {0}, action_check[32] = {0};
    query_get(query, "api", api_check, sizeof(api_check));
    query_get(query, "action", action_check, sizeof(action_check));
    if (api_check[0] || strcmp(action_check, "status") == 0)
        content_type = "application/json; charset=utf-8";
    else if (strcmp(action_check, "poll-js") == 0 || strcmp(action_check, "settings-js") == 0 ||
             strcmp(action_check, "api-js") == 0 || strcmp(action_check, "theme-js") == 0)
        content_type = "text/javascript; charset=utf-8";

    GString* resp = g_string_new(NULL);
    g_string_append_printf(resp,
                           "HTTP/1.1 %s\r\n"
                           "Content-Type: %s\r\n"
                           "Content-Length: %zu\r\n"
                           "Cache-Control: no-store\r\n"
                           "Connection: close\r\n\r\n",
                           status, content_type, body->len);
    g_string_append_len(resp, body->str, (gssize)body->len);

    GOutputStream* out = g_io_stream_get_output_stream(G_IO_STREAM(connection));
    g_output_stream_write_all(out, resp->str, resp->len, NULL, NULL, NULL);

    g_string_free(body, TRUE);
    g_string_free(resp, TRUE);
    return TRUE;
}

void webui_start(AXParameter* axparam, timer_config_t* cfg, void (*on_config_saved)(void)) {
    g.axparam         = axparam;
    g.cfg             = cfg;
    g.on_config_saved = on_config_saved;
    g.port            = WEB_PORT;

    GSocketService* service = g_socket_service_new();
    GError* error           = NULL;

    // Bound to loopback, NOT to every interface.
    //
    // This is what makes "you must log in" true rather than merely
    // encouraged. The only thing that can reach this socket is Apache on
    // the same device, forwarding requests that already satisfied the
    // reverseProxy's admin access level - so there is no unauthenticated
    // way to reach the timer, its settings or its API from the network at
    // all. Earlier versions listened on every interface and guarded the
    // port with an optional password of the app's own, which meant the
    // default configuration was wide open to anyone who could route to
    // the device.
    GInetAddress* loopback  = g_inet_address_new_loopback(G_SOCKET_FAMILY_IPV4);
    GSocketAddress* address = g_inet_socket_address_new(loopback, (guint16)g.port);

    gboolean ok = g_socket_listener_add_address(G_SOCKET_LISTENER(service),
                                                address,
                                                G_SOCKET_TYPE_STREAM,
                                                G_SOCKET_PROTOCOL_TCP,
                                                NULL,
                                                NULL,
                                                &error);
    g_object_unref(address);
    g_object_unref(loopback);

    if (!ok) {
        syslog(LOG_ERR,
              "Web UI: failed to bind 127.0.0.1:%d: %s. The app's pages and API will not be "
              "reachable. This port is fixed to match manifest.json's reverseProxy target, so a "
              "clash here needs both changed together.",
              g.port,
              error ? error->message : "?");
        if (error)
            g_error_free(error);
        g_object_unref(service);
        return;
    }

    g_signal_connect(service, "incoming", G_CALLBACK(on_incoming), NULL);
    g_socket_service_start(service);
    syslog(LOG_INFO,
          "C17 Timer web UI listening on 127.0.0.1:%d - reachable only via /local/%s/ui, which "
          "requires a device login",
          g.port,
          APP_NAME);
}
