# C17 Timer (appName: c17timer)

> This is an independent, community-developed ACAP package created to
> show what can be done with Axis devices when you think outside the
> box. It is not an official Axis product and is not affiliated with,
> endorsed by, or supported by Axis Communications AB. Use at your own
> risk.

A countdown timer for the **AXIS C17 Series Network Display Speaker**
(e.g. the **AXIS C1710**). The countdown runs on the speaker's own
1920&times;480 display, changes color as configured thresholds are
crossed, and plays one of the speaker's audio clips when it finishes.

Three ways to drive it, all controlling the same single timer:

1. **The buttons on an AXIS C8310 Volume Controller** wired into the
   speaker's I/O connector.
2. **An HTTP API** - plain GET URLs, listed in the app itself with Copy
   buttons.
3. **The device's own events engine** - the timer appears as conditions,
   and rules start and stop it through an HTTP notification action.

Built on the same foundation as this project's sibling **C8310
Customizer**: same I/O-port button handling, same embedded web server
architecture, same UI language, same build and versioning process.

## Why the C17, specifically

The C1710's datasheet settles the two things this app depends on:

- **The AXIS C8310 Volume Controller is a listed accessory** for it, and
  the speaker has an I/O terminal block (4-pin 2.5 mm, 2 supervised
  configurable I/Os) for the C8310 to plug into.
- **The display is 8.8", LCD, full color, 1920&times;480**, with single-line
  text readable to 12 m. Every preview in this app is locked to that
  exact 4:1 aspect ratio.

The C1710 runs an **NXP i.MX 8M Mini**, which is **aarch64** - so
`C17_Timer_<version>_aarch64.eap` is the build to install on a C17. The
whole C17 Series (C1710, C1720) is aarch64, per Axis's device list under
[Prepare your devices](https://help.axis.com/en-us/axis-audio-manager-pro),
item 4, so aarch64 is the only architecture this app is built for.

## How it actually works

The C8310 is not a network device. It has one cable into the host
speaker's I/O connector, and the host's firmware exposes each button as
its own numbered digital input port - the same ports you see under
**System > Events > Rules > I/O > Digital input is active > Port**. This
app subscribes to those ports.

The display is driven through the **Speaker display notification API**:

```
POST /config/rest/speaker-display-notification/v1/simple
POST /config/rest/speaker-display-notification/v1/stop
```

called on the local device over `127.0.0.12` using the app's own VAPIX
service account, which Axis documents as having **admin** access - the
level this API requires. Audio clips are listed via
`param.cgi?action=list&group=MediaClip` and played with `playclip.cgi`.

### Two display API behaviors that shaped the whole design

**1. It does not support coexistence.** Whoever posted last owns the
screen. So this app takes the display only while it has something to
show, and hands it straight back (`/stop`) the moment it doesn't - which
is what lets AAMP, the rule engine and anything else use the same screen
between timers. That's the **Display sharing** card under Display, and it
defaults to releasing. Turn it off and the last frame is re-posted with
no expiry so it genuinely stays up, rather than quietly timing out.

**A paused timer keeps the screen**, and has its own setting next to it.
This needed care: a running countdown re-posts every second, so a short
5-second hold that overlaps the next post is right for it - but a paused
timer posts once and then nothing happens at all, so the same short hold
let the notification lapse and blanked the screen a few seconds into the
pause. That looked exactly like the display being released. Paused frames
are posted with **no expiry** instead and stay up until something
changes.

The app also never posts anything at startup. A freshly installed or
restarted app has no business clearing a screen it never wrote to.

**2. Every post replaces the current notification and restarts its
animation.** A running countdown re-posts once a second, so **a
scrolling countdown can never get past its first few characters** - it
restarts every second. The countdown's scroll speed therefore defaults
to 0 (static), and the settings page says so, loudly, if you raise it.
The finished message is posted once and has its own separate scroll
settings, so it can scroll freely.

Each countdown post asks to stay up for 5 seconds - comfortably longer
than the one-second repost interval. The overlap means the screen never
blanks between posts, and if this app dies mid-countdown the display
clears itself a few seconds later instead of leaving a frozen number on
a wall.

## The timer

One timer, not several. Quick-timer buttons set and start this same
timer rather than creating more, which is the only model the hardware
can actually represent - there is one display.

**Duration vs. remaining** is the one distinction worth having straight:

- **Duration** is what the timer counts down *from*. Changed by Volume
  up/down while nothing is running, by `?api=set`, and by a quick-timer
  button.
- **Remaining** is where the current run is up to.
- **Volume up/down while running** extends or shortens the current run
  only, leaving the duration alone.
- **The timer always comes to rest at the configured default duration.**
  Whenever it stops running - by finishing, or by Stop & reset - both the
  duration and the remaining time go back to that default, not to
  whatever the last run happened to be. A quick-timer button, a dialled-in
  adjustment or an API `set` therefore last exactly as long as the run
  they belong to, and the next person finds the timer in the same known
  state. (Up to 0.1.7 it stayed at the last-used duration, which made the
  Default duration setting little more than a startup value.)
- **One deliberate exception:** pressing Start *while the finished message
  is still on screen* re-runs the duration that just elapsed, because "run
  that again" is plainly what it means there. Once the message clears, the
  default applies as usual.
- **Volume up and Volume down carry separate amounts** (both default to
  one minute). A five-minute nudge up with a one-minute trim down is a
  perfectly reasonable way to want a timer to behave, and a single shared
  step cannot express it. A config written before they were separate
  carries one `step_seconds`, which is read into both on upgrade so
  nothing changes behavior by itself.
- The duration starts each process life at the configured **Default
  duration**. It is deliberately not persisted across restarts: writing
  `localdata/` on every button press is exactly the continuous flash
  write Axis's own docs warn against.

Timing is monotonic (`g_get_monotonic_time`), never wall clock, so an
NTP step mid-countdown can't make a 5 minute timer finish early or run
for an hour. The displayed value is rounded **up**, so a running timer
shows "1" for the whole of its final second and only reads 0 once it has
genuinely finished.

**Starting a zero-length timer is refused** rather than firing the alarm
instantly - a button that visibly does nothing is much better than one
that surprises a room.

### The idle nudge

Volume up/down while the timer is idle briefly puts the new duration on
the display (3.5 seconds, in the normal colors) and then hands the
screen back. Without this the C8310's up and down buttons would have no
visible effect at all until someone pressed start - you would be setting
a timer blind, standing in front of a dark screen.

## Buttons

Volume up and Volume down are fixed to +/- one step. **Button 1, Button
2, Button 3 and Mute** are each assignable to:

| Action | What it does |
| --- | --- |
| Nothing | Only fires the Rule engine condition |
| Start / Pause | Whichever applies |
| Start only | |
| Pause only | |
| Stop and reset | |
| Quick timer | Sets its own duration and starts immediately |
| Add time | Adds its own value to the timer |
| Rule engine trigger only | Same as Nothing, named for intent |
| Finish now | Jumps to 0 and plays the finished message |

Defaults: Button 1 is a 5 minute quick timer, Button 2 is 10 minutes,
Button 3 is Start/Pause, Mute is Stop and reset. A fresh install is
usable without opening settings at all.

**Every button fires its own Rule engine condition on every press**,
whatever it is otherwise assigned to - so a button can drive an
unrelated automation at the same time as running the timer.

### Port numbers

Defaults are `1000`-`1005` for Button 1/2/3, Volume up, Volume down and
Mute - the numbering confirmed on a real C8310 pairing during the C8310
Customizer's development. That project also found this numbering is
**not guaranteed to be identical on every unit**, so this app subscribes
to every I/O port with no port filter, purely for diagnostics, and logs

```
I/O port debug: port=1000 state=active
```

for every press on every port. Press each physical button once, read the
app log, and set the matching `*Port` override in the app's native
Settings page (hidden parameters) if they don't match. Overrides take
effect on app restart. Setting a port to `-1` instead asks for
auto-detection from the port's *nice name* - only useful if you have
actually typed names into each port's Name field on the device's I/O
ports page, since the gray text shown there by default is placeholder
hint text, not a saved value.

## Display colors

The **normal** colors apply from the start of a countdown. On top of that
you can add up to **five color changes**, each with its own "when this
much time is left", background color, text color and text. Defaults:
green, amber at 30 seconds left, red at 10.

Thresholds are **sorted largest-first when saved**, and this matters
for correctness rather than tidiness: the timer takes the *last* matching
row as it walks the list, so `[30s, 10s]` correctly lands a timer at 5
seconds on the 10s row, while `[10s, 30s]` would wrongly land it on the
30s one. Sorting on save means the order you happened to add rows in can
never change what the display does.

**The countdown has no text-size or scrolling settings.** It is always
large and always static. Scrolling genuinely cannot work for it - a
running countdown re-posts every second and every post restarts the
notification's animation - and a size choice for a line that is only ever
a few digits was clutter. The finished message is sent once, so it keeps
its own full set: size, direction and speed.

**The paused and finished messages share one shape.** Each has an
**Appearance** card - number of lines, colors, scroll direction and speed,
with the live preview - and a **Message** card whose line fields appear or
disappear as the line count changes. Appearance comes first because the
line count decides how many message fields there are.

**Text size is not a setting.** It follows the line count, because the
display only fits so much: large for one line, medium for two, small for
three. A choice that could only be set wrong was worse than deciding it.

**Multiple lines need a vertical scroll direction** (`fromBottomToTop`).
The lines are joined with newlines into the single `message` field the
API takes. A horizontal direction keeps them as separate rows and
left-aligns them; a vertical one takes a single line and inserts its own
breaks between words.

Defaults: **paused** is two lines, medium, no scroll, showing
`PAUSED AT {paused_at_12}` and `{time} REMAINING`. **Finished** is one
line, large, right to left at speed 5, showing `TIME'S UP`.

**The line count truncates.** Only the lines it covers are sent - text in
a line beyond it is ignored. It stays in the config rather than being
deleted, so raising the count brings it straight back. (An intermediate
version folded the surplus onto the last row instead, which was wrong:
hiding a line should hide it.)

**What the display does with a line that is too long is its own
business**, and worth knowing when composing one: with scrolling on it
wraps the line onto further rows; with scrolling off it shows what fits
and cuts the rest.

Text is a template. `{time}` is the formatted countdown; `{hh}`, `{mm}`,
`{ss}` and `{total}` are available if you want to build the layout
yourself, and `{paused_at}` gives the clock time of the current pause
(empty when not paused, so it never shows a stale or invented time). An unknown `{placeholder}` is left exactly as typed rather than
silently deleted, so a typo is visible on screen instead of vanishing.

### The preview

Every color has a live preview, updating as you drag a color picker or
type text.

**The preview is a copy of AXIS Audio Manager Pro's own visual-profile
preview**, measured from a live AAMP instance rather than approximated,
so the two read as the same component:

| | AAMP | Here |
| --- | --- | --- |
| Frame | 500 x 179, 6px radius, 8px padding | same ratio (2.793:1), same radius |
| Text window | 390 x 100 at (55, 40) | 11% / 22.35% / 78% / 55.87% of the frame |
| Text | 50px in a 100px window, weight 400 | sized to the device, not to AAMP - see below |
| Font | `"Segoe UI", "Segoe UI Web (West European)", ...` | the same stack |
| AXIS lockup | 50 x 20, centered, 10px from the bottom | 10% wide, centered, 5.6% from the bottom |
| Frame | fixed black, whatever the background is | same |
| Background | fills the **text window**, not the frame | same |

Everything is a percentage of the frame, so it scales to whatever width a
card gives it, and the rendered result was measured back against those
numbers to confirm they land exactly.

**Text size and scroll speed are the places this deliberately departs
from AAMP,** because the preview's job is to predict the hardware rather
than to match another tool's preview. Both were calibrated against a real
C17 and then measured back in a browser.

**One size scale, used by every preview in the app.** `textSize` on the
device is a single property, so a "medium" line is the same height
whether it stands alone or is stacked with another:

| Size | Preview | At an 87.8px text window |
| --- | --- | --- |
| large | 70.6% of the window | 62px |
| medium | 36.4% | 32px |
| small | 25.1% | 22px |

Line height is 1.3 throughout, so one large line fills 92% of the window,
two medium 95% and three small 98%. The size follows the line count -
large for one, medium for two, small for three - derived identically in
three places: `display_resolve()` for what the device is sent, the C for
the first render, and the script for live edits. An earlier version had a
second set of sizes for stacked text and let each page work the size out
its own way, which made the same message render differently on the home
page than on its settings page.

**The scroll travel is measured against the text, not the screen.** An
earlier version animated `left` between +/-100% of the screen, so anything
wider or taller than it was still partly visible when the animation
restarted - the scroll looked cut off because it was. Percentages in
`transform` resolve against the element's own size and container query
units against the screen, so one pair expresses "start just outside one
edge, finish once the far edge has cleared the other" for text of any
length.

Scroll duration is `48 / (1 + 3.8 x speed)` seconds - 10.0s at speed 1,
**2.4s at speed 5** (the point checked against the device), 1.2s at 10. A
linear-in-duration curve could not reach 2.4s by the middle of the range
without going negative at the top, so the fit is linear in rate instead.
Both defaults are speed 5.

The preview is also trustworthy in a specific way: it is not a separate
guess at what the display is doing. `display_resolve()` in `display.c` is
the single function that works out what belongs on the screen, and
**both** the code that posts to the real device and the code that feeds
the browser preview call it. The home page's preview is driven from the
status poll, so it mirrors the actual screen twice a second - including
changes made from the physical buttons, an API call or a rule.

## Sounds

Every point the timer passes can sound a clip, chosen from whatever is
already on the device (its own **Audio > Clips** page - this app only
reads and plays, it will not upload or delete anyone's audio). Each
picker sits on the page for the thing it belongs to rather than in a
separate list:

- **Timer - Paused** - played once when a pause starts.
- **Timer - Normal** - the sound a timer *starts* with. Played on a fresh
  start, deliberately *not* on a resume: a chime every time someone
  unpauses would get old fast, while a start tone is genuinely useful for
  telling a room the clock is running.
- **Timer - Thresholds** - each color change can sound its own clip as the
  countdown crosses into it. A warning chime at 30 seconds and a sharper
  one at 10, say. Leave it as None for a silent color change.
- **Timer - Finished** - played the moment the countdown reaches zero,
  alongside the finished message. Stop & reset also stops it.

Each has its own repeat count (0 for once, -1 for forever) and volume.

> **Fixed in 0.1.2:** clip selection appeared not to save, and playback
> hit the wrong clip. The clip list parser searched for `".M"` in
> `root.MediaClip.M0.Name` and matched the `.M` of `.MediaClip` itself,
> four characters in - so **every clip parsed as id 0**. The dropdown
> showed the right names against identical values, the browser therefore
> re-selected the same entry every time, and playback targeted clip 0.
> Confirmed against the live device before and after the fix, and there
> is now a regression test over a realistic `param.cgi` response
> (including non-contiguous and two-digit ids).

## The HTTP API

Every verb is a plain GET returning JSON. Nothing needs a body or a
special content type.

| URL | What it does |
| --- | --- |
| `?api=status` | Current state, remaining, duration, and the exact frame the display is showing |
| `?api=start` | Start, or resume from paused |
| `?api=pause` | Freeze where it is |
| `?api=toggle` | Whichever of the two applies |
| `?api=stop` | Stop and reset (`?api=reset` is a synonym) |
| `?api=set&seconds=300` | Set the duration |
| `?api=add&seconds=60` | Add to the timer (negative to subtract) |
| `?api=quick&seconds=300` | Set and start immediately |
| `?api=button&n=3` | Press a C8310 button (1-3, 4 = Volume up, 5 = Volume down, 6 = Mute) |

Durations accept either seconds (`300`) or clock time (`5:00`); `time=`
works as an alias for `seconds=`.

Responses carry `ok` and `changed` separately, so "started it" and
"there was nothing to start" are distinguishable rather than both
reporting success:

```json
{"ok":true,"changed":false,"message":"already running","state":"running",
 "remaining":"04:47","remaining_seconds":287,"duration":"05:00",
 "duration_seconds":300,"toggle_label":"Pause","display_active":true,
 "display_ok":true,"display_error":"","text":"04:47","fg":"#ffffff",
 "bg":"#107c10","size":"large","dir":"fromRightToLeft","speed":0,
 "display_state":0}
```

The app's **API** page lists all of these with Copy buttons and a
dropdown that switches every URL between two forms.

### One address, always authenticated

```
https://<device-ip>/local/c17timer/ui?api=start
```

That is the only address - for another system, for curl, and for a rule
on this speaker calling its own device. The app's server is loopback-only
(see "Web UI" above), so there is no unauthenticated alternative to drop
down to.

Two things a client has to get right, both confirmed against a real
device, and both of which look like "the API is broken" from a client
that does not expect them:

- **Basic authentication with a device account.** With no credentials the
  device answers `401 Unauthorized` with
  `WWW-Authenticate: Basic realm="AXIS_..."`, from Apache, before the
  request reaches this app at all. The app keeps no separate password of
  its own.
- **The device certificate.** Axis devices present their own self-signed
  device-ID certificate (issuer "Axis device ID Intermediate CA ECC 3"),
  so a verifying client rejects it with `SELF_SIGNED_CERT_IN_CHAIN` -
  separately from, and in addition to, the login. Either disable
  certificate verification for that host (Postman's *Disable SSL
  certificate verification*, or `curl -k`) or trust the Axis device ID
  CA.

```sh
curl -k -u <user>:<pass> 'https://<device-ip>/local/c17timer/ui?api=quick&seconds=300'
```

> **Use `https`, not `http` - the scheme decides the auth scheme.**
> Confirmed on a real device: the same request with the same credentials
> succeeds over `https` and is rejected over `http`.
>
> | Scheme | Device offers | Basic accepted |
> | --- | --- | --- |
> | `https` | `WWW-Authenticate: Basic` | yes |
> | `http` | `WWW-Authenticate: Digest` | **no** |
>
> That is the device protecting you rather than being awkward: Basic puts
> the password on the wire in a trivially reversible form, so it is only
> offered where TLS is already protecting it. Sending Basic over `http`
> gets a 401 with perfectly good credentials, which reads like a wrong
> password. If you must use `http`, switch the client to Digest
> (`curl --digest`, or Digest Auth in Postman).

**A rule on the device authenticates too.** The rule engine's HTTP
notification action has username and password fields; fill them with a
device account. The device calling itself is not a special case here, and
deliberately so - one address, one set of rules about it. If the rule
engine refuses the device's own certificate, the `http://` form stays
inside the device - but see the scheme note above: it needs Digest, not
Basic.

**The URLs on the API page are built in the browser, not by the app.**
They have to be: behind the reverse proxy, Apache forwards to this app's
own port, so the `Host:` header the app sees is its own proxy target
rather than whatever address anyone actually typed - which is exactly
what made an earlier version print `localhost` in URLs meant for other
machines. `API_JS` rebuilds them from
`location.origin + location.pathname` on load. The server renders a best
guess (preferring `X-Forwarded-Host` over `Host`) purely as a fallback
for a browser with scripting off.

## Events engine

### Conditions (the timer driving a rule)

Declared at startup, and found under **Application** in the rule engine's
condition picker. "Application" is the device's own fixed heading for
every third-party app - not something an app can rename.

```
Application > C17 Timer: Timer event
                Event: Started | Paused | Resumed | Stopped | Finished
Application > C17 Timer: Timer running
                (stateful - true for the whole time a timer is counting)
Application > C17 Timer: Threshold reached
                Threshold: "Threshold 1 - 0:30 left", "Threshold 2 - 0:10 left", ...
Application > C17 Timer: Button pressed
                Button: Button 1..3 | Volume up | Volume down | Mute
```

**Timer running** is the one to pair with the rule engine's
"...while the rule is active" action variants - hold an output on for a
whole countdown, record audio for its duration.

**The threshold conditions are named after the times they fire at** -
"Threshold 1 - 0:30 left" rather than a bare number - so the dropdown
says what it will actually do. The names are rebuilt whenever the color
changes are saved. That re-declaration is safe: a rule binds to the
event's topic and to its source key's *value* (`threshold=1`), neither of
which changes - only the label attached to that value does. An
unconfigured slot reads "Threshold 4 - not set". The number stays in the
name on purpose: it is the part that never goes stale, and it matches the
numbering on the Display - Color Changes page and in the app log.

Their real shape, for reference:

```
tnsaxis:topic0 = CameraApplicationPlatform
tnsaxis:topic1 = C17Timer            (nice name: "C17 Timer")
tnsaxis:topic2 = TimerEvent | TimerRunning | ThresholdReached | ButtonPress
event     = started | paused | resumed | stopped | finished   (source key)
threshold = 1..5                                              (source key)
button    = button1..3 | volup | voldown | mute               (source key)
state     = boolean                                           (data key)
```

Every declaration is stateful and carries a boolean `state` data key,
which the rule engine surfaces as a checkbox under the dropdown. **That
checkbox is unavoidable** - the C8310 Customizer confirmed on real
hardware that splitting press/release into separate stateless
declarations does not remove it. So this app uses it the same way that
project settled on: checked means `state` is TRUE, i.e. the moment the
thing happened. Momentary events send TRUE immediately followed by
FALSE; **Timer running** holds TRUE for a whole run.

### Actions (a rule driving the timer)

**An ACAP cannot register a rule engine action.** Apps can declare
events, which become *conditions*; the *action* list belongs to the
platform (play audio clip, speaker display notification, toggle I/O,
send MQTT, HTTP notification, and so on) and third-party apps cannot add
to it. That is a platform limit, not something this app chose.

So a rule drives the timer through the built-in HTTP notification
action:

```
System > Events > Rules > Add a rule
  Condition: whatever should start the timer
  Action:    Notification > Send notification through HTTP
  URL:       https://<device-ip>/local/c17timer/ui?api=start
  Username:  a device account
  Password:  its password
```

The device calling itself still authenticates - there is no
unauthenticated route, by design. If the rule engine refuses the device's
own certificate, the `http://` form stays inside the device, but it
requires **Digest** rather than Basic - see "One address, always
authenticated".

## Web UI

One address, and it always requires a device login:

```
https://<device-ip>/local/c17timer/ui
```

**The app's own server binds to loopback only.** That is what makes "you
must log in" true rather than merely encouraged: the sole route to it is
Apache's reverse proxy, which is declared `access: "admin"`, so every
page and every API call inherits the device's own HTTPS and login. There
is no unauthenticated way in from the network, and nothing in this app
does its own authentication because nothing unauthenticated can arrive.

Earlier versions (up to 0.1.5) listened on every interface and guarded
that port with an optional password of the app's own, which meant the
default configuration was reachable by anyone who could route to the
device. Both **Web Ui Port** and **Web Ui Password** are gone: the port
is now private plumbing between Apache and the app (fixed at 8082 to
match the manifest's proxy target, so a runtime change could only ever
break the one route in), and the password is superseded by the device
login. Both parameters are deleted from the device's parameter store on
first startup after an upgrade, so they stop appearing under **Show
hidden parameters**.

The server is a plain `GSocketService` inside the daemon's own process.
This deliberately does **not** go through Apache's FastCGI or
`httpConfig` CGI paths: the C8310 Customizer tried both and each failed
silently and untraceably on real hardware. Running in-process also means
the web UI shares live timer state with the physical buttons with no IPC
and nothing to keep in sync.

### Layout

A left navigation column and a content area whose cards flow horizontally
and wrap as the window narrows - the same shape as AXIS Audio Manager
Pro's own console:

```
Home                the timer and the virtual C8310
TIMER
  Duration          the default and maximum duration
  Normal            colors from the start of a countdown, and the starting sound
  Paused            what shows and plays while paused
  Thresholds        each color change, its sound, and when it fires
  Finished          the end message and the sound it makes
SETTINGS
  C8310             what each button does, and the volume steps
  Display Sharing   when to hand the screen back
INTEGRATE
  API               every request, with copy buttons
  Rules             how this and the events engine fit together
```

The timer's own pages sit under a heading of their own, so each is named
for what it configures rather than repeating a "Timer - " prefix. Page
ids are unchanged, so older links still resolve.


There are no accordions anywhere - every setting on a page is visible at
once, and each page is short enough to take in at a glance. The
disclaimer sits under the nav column rather than across the foot of the
page: it belongs to the app rather than to whichever page is open, and
down there it never shifts as pages change length. Below 800px the nav
moves above the content and lays its links out in a row.

The card container is a **CSS grid**, not flex-wrap, for two things flex
cannot give at once: every card occupies exactly one column - so a lone
card (the "Add a threshold" tile, say) is one column wide rather than
stretched across the row - and cards sharing a row are automatically the
same height. `auto-fill` with a 340px minimum picks the column count from
the available width, so it still reflows on its own with no breakpoints
to maintain.

Each settings page posts **only its own fields**, carrying a hidden
`section` marker. That marker is load-bearing rather than decorative:
without it, a page that simply doesn't contain the color-change fields
would be indistinguishable from one submitting zero color changes, and
saving the Timer page would wipe the Display page's settings.

**Saving keeps your place.** A save is a POST that re-renders the page,
which otherwise dumps you back at the top - annoying when the thing you
just edited was near the bottom. The scroll offset is stashed on submit
and restored on the next load, keyed per page, then cleared so a later
plain navigation still starts at the top.

The server is a plain `GSocketService` inside the daemon's own process.
This deliberately does **not** go through Apache's FastCGI or
`httpConfig` CGI paths: the C8310 Customizer tried both and each failed
silently and untraceably on real hardware. Running in-process also means
the web UI shares live timer state with the physical buttons with no IPC
and nothing to keep in sync.

### Three inherited gotchas, all confirmed on real hardware

These are why the URLs in this app look the way they do. All three were
paid for during the C8310 Customizer's development; this app is built to
avoid them from the start rather than rediscovering them.

1. **Apache's reverseProxy matches its apiPath exactly - no subtree.** A
   request to `/local/c17timer/ui/settings` never reaches the app at all;
   Apache answers with its own 404 first. So every page and every API
   verb is a **query parameter on the one exact path**
   (`?page=settings`, `?api=start`), never a second path segment.
2. **Apache forwards the full original path unmodified**, producing a
   request line like `GET //local/c17timer/ui` - the doubled slash coming
   from the proxy target's own trailing `/` plus the forwarded path's
   leading one. `on_incoming()` collapses that and strips the whole mount
   prefix rather than assuming Apache already did.
3. **Pages reached through the proxy inherit Apache's
   Content-Security-Policy** (`script-src 'self'`), which silently blocks
   inline `<script>` blocks *and* inline `onchange="..."` attributes -
   no error in the page, the script just never runs. All JavaScript is
   therefore served as **same-origin external scripts**, referenced by
   **query string** (`?action=poll-js`) rather than a relative filename:
   the proxied URL has no trailing slash, so a bare filename would
   resolve against the parent directory and 404.

### A note on the request log

This app logs the request **path but never the query string**. A Web Ui
Password travels in `?key=`, and that log line goes straight into the
device's syslog. Settings save via POST for the same reason.

## Configuration and storage

Two mechanisms, deliberately:

- **Everything you actually configure** - timer step and defaults, button
  assignments, display colors and thresholds, the finished message, the
  two audio clips, display sharing - lives in `localdata/config.json`, not
  AXParameter, and is not declared in `manifest.json` at all. That means
  it cannot appear in the native three-dot **Settings** dialog under any
  circumstance, including **Show hidden parameters** - there is nothing
  there to show. The web UI's Settings page is the only place to edit it.
  `localdata/` is Axis's documented location for application data and
  survives app upgrades, unlike the rest of the package directory.
  Writes go through a temp file then a rename, so a crash or power loss
  mid-save can never leave a half-written config.
- **The six C8310 port overrides** are the only remaining AXParameter
  values, hidden in the native Settings dialog. Web Ui Port and Web Ui
  Password used to live there too and are gone - see "Web UI".

Any field missing from `config.json` keeps its built-in default, so a
config written by an older version of this app upgrades cleanly rather
than losing settings. The single finish clip that older configs stored as
flat `clip_*` keys is read into the finish clip before the newer nested
`start_clip`/`resume_clip`/`finish_clip` objects are applied, so an upgrade keeps
whatever sound was already set.

Settings changes take effect immediately - no restart, and the physical
buttons pick them up too, since everything reads the same live config
struct. The six port overrides need an app restart: the I/O
subscriptions are established once at startup.

## Project layout

```
c17-timer-acap/
├── build.command            # Double-click to build both architectures
├── embed-logo.sh            # Regenerates app/axis_logo.h from axis-logo.png
├── axis-logo.png            # The AXIS lockup shown in the display preview
├── Dockerfile
├── README.md
├── QUICKSTART.txt
├── older-versions/          # Created automatically by build.command
└── app/
    ├── manifest.json        # App identity, parameters, reverse proxy, D-Bus
    ├── LICENSE
    ├── Makefile
    ├── main.c               # Wiring; every side effect hangs off on_timer_event()
    ├── timer.c/h            # Countdown state machine - state and timing only
    ├── display.c/h          # Display notification client + display_resolve()
    ├── mediaclip.c/h        # Listing and playing the device's audio clips
    ├── event.c/h            # The custom rule engine conditions
    ├── buttons.c/h          # C8310 I/O subscriptions and button actions
    ├── webui.c/h            # The whole UI (nav, four settings pages) and HTTP API
    ├── local_config.c/h     # localdata/config.json
    ├── local_vapix.c/h      # Shared plumbing for this device's own VAPIX
    ├── io_discovery.c/h     # Auto-detecting button port numbers
    ├── app_config.c/h       # The AXParameter-backed settings
    ├── axparam_util.c/h     # AXParameter read/write helpers
    ├── panel_image.h        # The C8310 product photo, base64 (used with permission)
    ├── axis_logo.h          # GENERATED from axis-logo.png by embed-logo.sh
    └── html/index.html      # settingPage stub that redirects to the real UI
```

`timer.c` has no dependency on the display, the event system, the clip
player or the web UI. It owns state and timing and reports what happened
through one observer callback that `main.c` registers - so the rules
about *when* the display repaints, a clip plays or a condition fires all
live in one readable place instead of being scattered through the
modules that perform them.

## Building

Requires Docker and the official Axis ACAP Native SDK image. The
Dockerfile takes `ARCH` as a build argument; nothing in `app/` is
architecture-specific.

### Easiest: double-click `build.command`

It regenerates `app/axis_logo.h` from `axis-logo.png`, builds both
architectures, drops both `.eap` files into the project root, and then
bumps the patch version in `manifest.json` ready for the next build - so
neither the version number nor the embedded artwork needs touching by
hand.

**Older builds are never lost.** Before each build, any `.eap` already
sitting in the project root is moved into `older-versions/`, so the
project root only ever shows the newest build at a glance while the last
known-working one is still there to fall back on. (Same layout and same
script as this project's sibling **NWS Weather Alerts** ACAP.)

### Manual, one command per architecture

**aarch64** (the C17 Series - this is the one you want):

```sh
docker build --platform=linux/amd64 --build-arg ARCH=aarch64 --tag c17timer-aarch64 . && docker create --name c17-extract --platform=linux/amd64 c17timer-aarch64 && docker cp c17-extract:/opt/app/C17_Timer_0_1_0_aarch64.eap ./C17_Timer_0_1_0_aarch64.eap && docker rm c17-extract
```

Adjust the filename to the current version in `manifest.json`, or just
use `build.command`, which discovers it itself.

## What has and hasn't been verified

**Verified:**

- Builds clean for both architectures with the project's full warning set
  (`-Wall -Wextra -Wformat=2 -Wbad-function-cast -Wstrict-prototypes`
  and the rest) - no warnings.
- **130 behavioral checks** pass against native harnesses:
  - *Timer state machine (85)*, with a virtual clock: rounding at second
    boundaries, pause freezing the countdown, resume reporting as resumed
    rather than started, adjusting while running moving only the run,
    adjusting while idle moving the duration, clamping at zero and at the
    maximum, refusing a zero-length start, threshold selection including
    out-of-order rows, the finish and auto-clear sequence, and quick
    timers restarting cleanly mid-run, the timer coming to rest at the
    configured default after both a finish and a Stop & reset, and the
    `{paused_at}` placeholder filling only once a pause has happened.
  - *Input parsing and HTML escaping (35)*: `5:00` / `300` / `1:05:00` /
    negative durations, query parsing including the present-but-empty vs.
    absent distinction and prefix mismatches, URL decoding, all five HTML
    escapes, and truncation never emitting a partial entity.
  - *Media clip list parsing (10)*, guarding the id bug described under
    Sounds: non-contiguous ids, two-digit ids, all ids distinct, and a
    blank-named clip still getting something selectable.
- All three client scripts parse (`node --check`), and every element id
  the JavaScript looks up is cross-referenced against what the C actually
  emits - including a check that no id removed in a rewrite is still
  referenced.
- **The preview geometry was measured back against AAMP's own numbers**
  in a browser and matches to two decimal places on every dimension - see
  the table under "The preview" - including that the frame stays black
  while the background color fills the text window, checked against a
  real AAMP profile using a red background.
- **The layout was measured in a browser**: cards in a row come out the
  same height, the "Add a threshold" tile is exactly one column wide
  and left-aligned with the first column, the API list is two columns,
  long URLs scroll inside their own box rather than being cut off, and
  nothing overflows horizontally at any width.
- The clip id bug was confirmed on the live device (every dropdown option
  carried `value="0"`) before fixing.

**Verified on the real device:**

- **No flicker.** The once-a-second repost that drives the countdown was
  watched on real hardware and shows no flashing - which was the single
  biggest open risk in the design, since every post replaces the previous
  notification. `DISPLAY_HOLD_MS` overlapping the repost interval does
  what it was meant to.
- **The Speaker display notification API accepts this app's posts.** A
  `?api=quick&seconds=10` call answered `"display_ok":true` alongside
  `"display_active":true` and the resolved frame. That is meaningful
  rather than a default: `timer_quick()` runs the observer chain
  synchronously - started event, then `paint_display()`, then
  `display_show()` - all before the JSON response is serialized, and
  `display_ok` only reads true when no failure was recorded for that
  post. So the POST returned 2xx. It does not prove the pixels look
  right, but the call path works end to end.
- **The API over HTTPS with Basic auth**, returning 200 and actually
  starting the timer.

**Not verified - needs a real C17:**

- **Whether the colors and thresholds read well at a distance.** The API
  accepts the posts and the display doesn't flicker; what's left is
  judgment about legibility - text size, color contrast and how early
  the warning thresholds should fire for a room to notice them.
- **Whether a one-per-second repost looks clean**, or visibly flickers as
  each notification replaces the last. This is the single most likely
  thing to need adjusting. If it flickers, the knobs are
  `DISPLAY_HOLD_MS` in `display.h` and the tick behavior in
  `recompute()` in `timer.c` - for example only reposting every 5 seconds
  above a minute remaining, and every second below it.
- **Media clip playback.** The id parsing is now correct and tested, but
  `playclip.cgi` with `repeat` and `volume` still hasn't been heard
  coming out of a speaker.
- **Re-declaring the threshold conditions** to rename them. The reasoning
  is sound - a rule binds to the source key's value, not to the
  declaration - but it hasn't been proved by editing a color change on a
  device with a rule already bound to that threshold. If a rule does come
  unbound, the fix is to stop re-declaring and put the time in the
  condition's description instead of its name.
- **C8310 port numbers on a C1710 specifically.** The 1000-1005 defaults
  come from a C1210 pairing. The diagnostic log line described under
  "Port numbers" is there to settle this in one press per button.

## Known limitations

- **One timer.** By design - there is one display.
- **The countdown cannot scroll**, and has no text-size setting either -
  see "Display colors". The finished message has both.
- **Up to five color changes.** Bounded because each one also gets a
  rule engine condition slot, and those are declared once at startup and
  can't grow at runtime.
- **The start sound does not play on resume**, only on a fresh start.
  Deliberate - see "Sounds".
- **Everything needs an admin device login**, including just watching the
  countdown, because `access: "admin"` covers the whole
  `/local/c17timer/ui` path and that is now the only route in. The
  reverse proxy offers no unauthenticated level - admin, operator and
  viewer all require a login - and the whole UI, settings included, sits
  behind that one path, so admin is the right level for it.
- **A device with a system HTTP proxy configured** (System > Network)
  hands `http_proxy` to every app it starts, and libcurl honors it by
  default - so the app's own calls to the device's loopback address
  (`127.0.0.12`, for the display and media clips) went out to the proxy
  instead, which cannot reach it and answers **503**. The pages still
  load and the buttons still work; only the display and the sounds fail,
  and the app log shows `display: notification failed: HTTP 503` with a
  Squid (or similar) error page in the body. Fixed in 0.1.48: local calls
  bypass the proxy unconditionally, and the app logs the proxy it found
  (credentials redacted) the first time it makes one.
- **The web UI port is fixed at 8082** to match the manifest's proxy
  target. It is loopback-only and nobody types it, but a clash with
  something else on the device would need both changed together and a
  rebuild.
- **Installing this alongside the C8310 Customizer** works - they use
  different loopback ports (8082 vs 8081) and both can subscribe to the
  same I/O ports - but both apps will then react to the same button presses.
  Put each button in a mode where only one app acts on it, or install
  only one.
