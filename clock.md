# Otto Idle Clock

A full-screen clock that takes over from the `neutral.gif` idle animation after
the device has been continuously idle for a while, and instantly hands back
to the normal emoji/GIF view the moment anything happens. All changes live in
two files, both board-specific to otto-robot (no core/shared code touched):

- `main/boards/otto-robot/otto_emoji_display.h`
- `main/boards/otto-robot/otto_emoji_display.cc`

**Currently active: analog.** Both a digital clock (time + date text) and an
analog clock (face + rotating hands) are fully built and live in the code at
the same time. Only one is ever shown — right now that's the analog one,
picked by which widgets are hidden in `SetupIdleClockUI()`. See
[Switching between digital and analog](#switching-between-digital-and-analog)
below.

## Behaviour

1. **Idle, first ~30 seconds:** unchanged — `neutral.gif` keeps playing, same
   as before this feature existed.
2. **Idle continuously for 30+ seconds:** the emoji/GIF view is hidden and a
   full-screen clock takes over (currently the analog face).
3. **Any activity** (wake word, button press, voice, an MCP command — anything
   that changes `SetStatus()`/`SetEmotion()`/shows a camera preview while the
   device has actually left the idle state) hides the clock immediately and
   restores the emoji/GIF view, resuming GIF playback.

The 30-second delay is `OttoEmojiDisplay::kIdleClockDelayMs` in
`otto_emoji_display.h`.

## How it's wired in

- `SetupIdleClockUI()` — called once from `SetupUI()`, builds a full-screen
  `clock_container_` (black background), then creates **both** clock faces
  inside it: the digital labels (`clock_time_label_`, `clock_date_label_`,
  currently hidden) and, via `SetupAnalogClockUI()`, the analog face
  (`analog_face_`, `analog_hour_hand_`, `analog_minute_hand_`, currently
  shown). `clock_container_` itself starts hidden either way.
- `ShowIdleClock()` / `HideIdleClock()` — the actual full-screen toggle.
  Mirrors the existing `SetPreviewImage()` pattern exactly: hide `emoji_box_`
  and stop `gif_controller_` when showing the clock; restore both when
  hiding it. Doesn't care which clock face is inside — that's decided once,
  at setup time, by which widgets are hidden.
- `UpdateStatusBar()` (overridden) — called once per second by the existing
  app-wide clock tick. Tracks how long the device has been continuously idle
  (`idle_since_`) and calls `ShowIdleClock()` once that crosses the 30s
  threshold. Also calls `UpdateIdleClockTime()` every tick while the clock is
  showing.
- `UpdateIdleClockTime()` — reads the current time once, updates **both**
  clock faces every tick regardless of which is visible: sets the digital
  labels' text, and calls `UpdateAnalogClockHands(hour, minute)` to recompute
  the hand angles. Keeping the hidden one updated too means switching which
  is active is instant and always in sync — no stale digits/hands the first
  time you flip back.
- `UpdateAnalogClockHands(hour, minute)` — computes the hour/minute hand
  endpoints with plain trigonometry (angle clockwise from 12 o'clock) and
  calls `lv_line_set_points()`. No image asset, no font — just two vector
  lines recalculated from the display center each update.
- `SetStatus()`, `SetEmotion()`, `SetPreviewImage()` (all overridden) — each
  hides the clock immediately if it's showing and the device has genuinely
  left the idle state. This is what makes the hand-back feel instant instead
  of waiting up to a second for the next status-bar tick.

## Bugs hit along the way

### 1. Clock flashing for a second every 30 seconds

First attempt hid the clock on *every* `SetStatus()`/`SetEmotion()` call,
unconditionally. That broke almost immediately: the base class
(`LvglDisplay::UpdateStatusBar()`) has a quirk where it re-calls
`SetStatus()` with the same `HH:MM` text on *every single tick* while idle
(a `last_status_update_time_` bookkeeping bug that never advances past its
initial 10 seconds — not something we touched, and out of scope to fix here).
That meant the very next tick after the clock appeared would immediately hide
it again and reset the 30-second timer — symptom: clock flashes for ~1 second
every 30 seconds, forever.

**Fix:** only treat `SetStatus()`/`SetEmotion()` as real activity when
`Application::GetInstance().GetDeviceState() != kDeviceStateIdle` — i.e. the
device has actually transitioned away from idle, not just that the base class
touched the label while still idle. See the comments above each of those
three overrides in `otto_emoji_display.cc` for the exact reasoning.

### 2. Digital clock text clipped off-screen

The time label was scaled 3x (`768`, see below), which rendered wider than
the 128px screen for `"HH:MM"`. Fixed by dropping to 2x (`512`).

### 3. Build error adding the analog hands

`lv_line_set_points()` takes an array of `lv_point_precise_t`, whose
underlying numeric type depends on the LVGL build config — in this project's
build it's `int32_t`, not `float` (`LV_USE_FLOAT` is off). The hand-angle math
is naturally done in `float`, so assigning those computed coordinates
straight into a point struct triggered a `-Werror=narrowing` build failure
(implicit float→int32 narrowing isn't allowed).

**Fix:** round and cast explicitly before constructing each point —
see the `to_precise()` lambda (`lroundf(v)` then `static_cast<lv_value_precise_t>`)
at the top of `UpdateAnalogClockHands()`.

## Customizing the look

### Digital clock — font size (time and date)

In `SetupIdleClockUI()`. There's no large font asset involved — the normal
text font is just scaled up visually. LVGL's scale style uses a fixed-point
convention where **256 = 100%** (i.e. actual size):

```cpp
lv_obj_set_style_transform_scale(clock_time_label_, 512, 0);  // 512/256 = 2x size
```

- To make the **clock (time)** bigger or smaller, change that `512`:
  - `384` → 1.5x
  - `640` → 2.5x
  - `768` → 3x (this is what clipped the minutes off-screen originally — stay
    below this on a 128px-wide display, or re-check it fits if you change the
    date/time format)
- The **date label currently has no scale applied** (renders at the theme's
  normal, unscaled text size). To make it bigger/smaller too, add the same
  kind of line right after it's created:
  ```cpp
  lv_obj_set_style_transform_scale(clock_date_label_, 320, 0);  // e.g. 1.25x
  ```

**Important:** scaling only changes how big the text is *drawn* — it does not
change the label's layout box used for centering, so a bigger scale needs a
bigger safety margin from the screen edges. Re-check on the actual device
that "HH:MM" doesn't clip again if you go above 2x.

### Digital clock — shifting the digits (or the date) left/right/up/down

Both labels are centered via `lv_obj_align(..., LV_ALIGN_CENTER, x, y)`. The
second and third arguments are pixel offsets *from* that centered position —
positive `x` moves right, negative `x` moves left; positive `y` moves down,
negative `y` moves up.

```cpp
lv_obj_align(clock_time_label_, LV_ALIGN_CENTER, 0, -10);  // time: centered, 10px above screen center
lv_obj_align(clock_date_label_, LV_ALIGN_CENTER, 0, 30);   // date: centered, 30px below screen center
```

- To nudge the **time** a few pixels right: change `0` (the x argument) to
  e.g. `5` or `10`. Negative values shift it left.
- To nudge the **date** left/right the same way: change its `x` argument.
- The `y` values (`-10` and `30`) are what currently separates the time from
  the date vertically — adjust both together if you change either label's
  font scale, since a bigger/smaller label needs more/less vertical spacing
  to avoid overlapping the other one.

### Analog clock — face size and tick appearance

In `SetupAnalogClockUI()`.

```cpp
const int face_size = (width_ < height_ ? width_ : height_) - 8;  // face diameter, in pixels
...
lv_scale_set_total_tick_count(analog_face_, 60);   // one tick per minute
lv_scale_set_major_tick_every(analog_face_, 5);    // every 5th tick is a major (hour) tick
lv_obj_set_style_line_width(analog_face_, 2, LV_PART_INDICATOR);  // major tick thickness
lv_obj_set_style_line_width(analog_face_, 1, LV_PART_ITEMS);      // minor tick thickness
```

- To shrink the face (more margin from the screen edge), increase the `- 8`
  margin subtracted from `face_size`, e.g. `- 16`.
- To make major/minor ticks thicker or thinner, change the two
  `lv_obj_set_style_line_width(...)` values above.
- To change tick color, change the `lv_color_white()` arguments to
  `lv_obj_set_style_line_color(analog_face_, ..., LV_PART_INDICATOR)` /
  `LV_PART_ITEMS`.
- `lv_scale_set_rotation(analog_face_, 270)` is what makes tick `0` land at
  12 o'clock instead of 3 o'clock (LVGL's scale angle origin is normally the
  3 o'clock/east position) — don't change this unless you want the face
  rotated.

### Analog clock — hand length and thickness

In `UpdateAnalogClockHands()`:

```cpp
const float hour_len = shortest_side * 0.22f;    // hour hand length, as a fraction of screen size
const float minute_len = shortest_side * 0.34f;  // minute hand length
```

- Increase/decrease the `0.22f`/`0.34f` fractions to make that hand longer or
  shorter relative to the screen. Keep `minute_len` noticeably larger than
  `hour_len` (the usual clock convention) and both comfortably under `0.5`
  (the face radius) so the hands don't poke past the tick marks.
- Hand thickness is set separately in `SetupAnalogClockUI()`:
  ```cpp
  lv_obj_set_style_line_width(analog_hour_hand_, 3, 0);
  lv_obj_set_style_line_width(analog_minute_hand_, 2, 0);
  ```
- Hand color: `lv_obj_set_style_line_color(analog_hour_hand_/analog_minute_hand_, ..., 0)`,
  same two lines.

There's no second hand currently (only hour + minute) — adding one would mean
a third `lv_line`, a third points array, and extending
`UpdateAnalogClockHands()`'s signature to also take seconds.

## Switching between digital and analog

Which one shows is decided purely by which widgets are hidden right after
they're created, in `SetupIdleClockUI()`:

```cpp
lv_obj_add_flag(clock_time_label_, LV_OBJ_FLAG_HIDDEN);
lv_obj_add_flag(clock_date_label_, LV_OBJ_FLAG_HIDDEN);
```

To go back to the digital clock: remove those two lines (so the digital
labels show), and instead hide the analog widgets after `SetupAnalogClockUI()`
is called:

```cpp
lv_obj_add_flag(analog_face_, LV_OBJ_FLAG_HIDDEN);
lv_obj_add_flag(analog_hour_hand_, LV_OBJ_FLAG_HIDDEN);
lv_obj_add_flag(analog_minute_hand_, LV_OBJ_FLAG_HIDDEN);
```

Both faces keep getting their content updated every tick regardless of which
is hidden (see `UpdateIdleClockTime()`), so there's no stale state either way
— flipping which is hidden is the entire change needed to switch look.

## Rebuild and test

```sh
python3 scripts/build.py otto-robot --name otto-robot --language en-US --wake-word wn9_jarvis_tts
```

Then flash and let the device sit idle for 30+ seconds to see the result —
there's no way to preview any of this off-device since it's plain LVGL
layout and vector drawing, not a rendered asset.
