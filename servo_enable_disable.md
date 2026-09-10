# Otto Servo Enable/Disable (synced to the idle clock)

Releases servo holding torque while the full-screen idle clock (`clock.md`) is
showing, and restores it the instant real activity resumes. This is the
software-only half of a larger power-saving idea (see
[Not done yet: the MOSFET rail cutoff](#not-done-yet-the-mosfet-rail-cutoff)
below) — for now it only stops/starts the PWM signal, it does not cut the
servos' power rail. All changes are board-specific to otto-robot (no
core/shared code touched):

- `main/boards/otto-robot/otto_controller.cc`
- `main/boards/otto-robot/otto_emoji_display.cc`
- `main/boards/otto-robot/oscillator.h`
- `main/boards/otto-robot/oscillator.cc`

## Behaviour

1. **Idle clock appears** (30+ seconds continuously idle — see `clock.md`):
   all 6 servos detach. `ledc_stop()` stops the PWM pulse train, so the
   servos release holding torque instead of actively fighting to hold
   position for no reason while nothing is happening.
2. **Idle clock disappears** (any real activity resumes): all 6 servos
   re-attach on their original LEDC channels, and the robot re-homes —
   giving a visible, physical confirmation that they're actually alive again,
   not just silently re-configured.
3. **Skipped entirely while an action is running** (`is_action_in_progress_`):
   this only ever fires during genuine idle, so in practice it never
   overlaps with `ActionTask` actively driving the servos, but the guard is
   there regardless — it stops the idle-clock code from re-configuring the
   LEDC channels out from under a motion in progress.

Detach only stops PWM output — it doesn't cut the servos' power rail, so it
doesn't actually save current/voltage yet, just holding torque. That's the
next step, not part of this change (see below).

## How it's wired in

- `OttoController::SetServosAttached(bool attached)` (new, in
  `otto_controller.cc`) — the actual attach/detach logic:
  - `false`: `otto_.DetachServos()`.
  - `true`: `otto_.AttachServos()`, then forces `otto_.SetRestState(false)`
    and queues `ACTION_HOME` (async, via the existing action queue/task —
    see [Bug 3](#3-servos-never-audibly-move-on-reattach-hometh-no-op) below
    for why both of those extra steps are necessary).
  - No-ops entirely if `is_action_in_progress_` is true.
- `SetOttoServosAttached(bool attached)` (new free function in
  `otto_controller.cc`, next to the existing `InitializeOttoController()`) —
  forwards to the global `g_otto_controller` if it exists. This is the only
  thing the display needs to know about; it never sees the `OttoController`
  class itself, same pattern already used for `InitializeOttoController()`.
- `OttoEmojiDisplay::ShowIdleClock()` / `HideIdleClock()` (`otto_emoji_display.cc`)
  — each now calls `SetOttoServosAttached(false)` / `(true)` right after
  actually toggling `clock_visible_`. Both only fire on a genuine state
  transition, since both functions already early-return when already in the
  target state (see `clock.md`).
- `Oscillator::has_ledc_channel_` (new member, `oscillator.h`/`oscillator.cc`)
  — makes each servo's `Attach()` reuse the same LEDC channel every time
  instead of pulling a new one from the shared round-robin counter on every
  call (see [Bug 1](#1-gpio-is-not-usable-maybe-conflict-with-others) below).
- `Oscillator::Detach()` (`oscillator.cc`) — now also calls
  `gpio_reset_pin()` after `ledc_stop()`, to actually release the GPIO
  reservation the LEDC driver takes and never lets go of on its own (see
  [Bug 2](#2-the-warning-came-back-even-with-a-reused-channel) below).

## Bugs hit along the way

This took three passes to get right — each fix was necessary but not
sufficient on its own; all three are needed together.

### 1. "GPIO is not usable, maybe conflict with others"

First attempt just called `otto_.DetachServos()` / `AttachServos()` directly.
Reflashing showed this on every re-attach, for all 6 servo pins:

```
W (146099) ledc: GPIO 42 is not usable, maybe conflict with others
W (146099) ledc: GPIO 1 is not usable, maybe conflict with others
...
```

Root cause: `Oscillator::Attach()` had `static int last_channel` as a
function-local static — **one counter shared by all 6 servos** — and handed
out a fresh LEDC channel from it on every single call, including a
re-attach. `Detach()` only called `ledc_stop()`, which doesn't release the
LEDC channel's binding to that GPIO. So on re-attach, each pin got assigned
a *different* channel than it had before, while the ESP-IDF LEDC driver
still considered the pin bound to the old one.

**Fix:** added `Oscillator::has_ledc_channel_` (`oscillator.h`) so a channel
is only ever pulled from the shared counter the *first* time a given
oscillator attaches; every later `Detach()`/`Attach()` cycle reuses that same
channel (`oscillator.cc`, inside `Attach()`).

### 2. The warning came back, even with a reused channel

Reflashing after fix 1 showed the exact same warning, for the exact same 6
pins. Channel reuse alone didn't help.

Read the actual ESP-IDF v6.0.2 LEDC driver source
(`components/esp_driver_ledc/src/ledc.c`) to find out why:

```c
static esp_err_t _ledc_set_pin(int gpio_num, bool out_inv, ledc_mode_t speed_mode, ledc_channel_t channel)
{
    uint64_t old_gpio_rsv_mask = esp_gpio_reserve(BIT64(gpio_num));
    if (old_gpio_rsv_mask & BIT64(gpio_num)) {
        ESP_LOGW(LEDC_TAG, "GPIO %d is not usable, maybe conflict with others", gpio_num);
    }
    gpio_matrix_output(gpio_num, ...);   // runs regardless of the warning
    ...
}
```

`esp_gpio_reserve()` is a generic, GPIO-number-scoped reservation registry
shared across all peripherals — it doesn't know or care about LEDC channels.
The bit gets set on the very first `ledc_channel_config()` call for that pin
and is **never cleared** by `ledc_stop()` (only a full "deconfigure" call,
which nothing here makes, clears it). So *any* future
`ledc_channel_config()` on that pin logs the warning forever, reused channel
or not.

Confirmed this is a known, permanent, deliberately-unfixed ESP-IDF
limitation by pulling the real issue threads via the GitHub API
([espressif/esp-idf#15621](https://github.com/espressif/esp-idf/issues/15621),
[#15666](https://github.com/espressif/esp-idf/issues/15666)). Espressif
engineer `songruo`, verbatim:

> "We are not likely to add `esp_gpio_revoke` in LEDC driver, but we have
> provided a workaround, please call `gpio_reset_pin`, it will do the
> revoke."
>
> "This is because LEDC driver is an old driver, the driver did not consider
> the channel release from the beginning. We don't want to take too much
> effort to add the channel release logic to the driver."

**Fix:** `Oscillator::Detach()` now calls `gpio_reset_pin((gpio_num_t)pin_)`
right after `ledc_stop()` — Espressif's own documented workaround, which
calls `esp_gpio_revoke()` internally. This is the fix that actually made the
warning go away.

**Side effect worth knowing:** `gpio_reset_pin()` doesn't just revoke the
reservation — it fully resets the pin (disables output, enables an internal
pull-up, disables pull-down, resets the IOMUX function-select). So for the
brief moment between `Detach()` and the next `Attach()`, the pin goes from
"actively driven low/idle" to "weakly pulled high." One reporter in the
GitHub thread, driving a speaker through a MOSFET on an LEDC pin, heard an
audible glitch from exactly this transition. For a servo signal line this is
expected to be harmless (no valid pulse train = the servo just ignores it),
but it's the reason for any tiny twitch right when the clock appears, if you
ever notice one.

### 3. Servos never audibly move on reattach (`Home()` no-op)

Even with the warning gone (confirming the LEDC/GPIO layer was now genuinely
fixed), the servos still didn't visibly do anything when the clock
disappeared.

Root cause: `Otto::Home(bool hands_down)` (`otto_movements.cc`) has an
"only if necessary" guard:

```cpp
void Otto::Home(bool hands_down) {
    if (is_otto_resting_ == false) {  // Go to rest position only if necessary
        ...
        MoveServos(home_time, homes);
        is_otto_resting_ = true;
    }
    vTaskDelay(pdMS_TO_TICKS(200));
}
```

Any real action's own cleanup already calls `Home()` when its queue empties,
which sets `is_otto_resting_ = true`. That's exactly the state the robot is
already in by the time it's been idle long enough for the clock to show. So
queuing `ACTION_HOME` after re-attaching was a complete no-op in the normal
case: `Home()` saw `is_otto_resting_ == true` and returned immediately
without ever calling `MoveServos()`/`Write()` on anything — leaving every
channel parked at 0% duty (the value `Attach()` always configures a channel
with) until some unrelated future action happened to move it.

**Fix:** `Otto` already exposes a public `SetRestState(bool)`
(`otto_movements.h`/`.cc`) for exactly this. `SetServosAttached(true)` now
calls `otto_.SetRestState(false)` immediately before queuing `ACTION_HOME`,
forcing it to actually execute instead of no-op.

## Thread-safety note (accepted, not fully closed)

`ShowIdleClock()`/`HideIdleClock()` run on the **main application task**
(`application.cc`'s `MAIN_EVENT_CLOCK_TICK` handler calls
`display->UpdateStatusBar()`), which is the safest place for this — no
`Application::Schedule()` needed, unlike a callback from an arbitrary
context. The one race this doesn't fully close: the separate `ActionTask`
FreeRTOS task also owns the same `Otto` object and its `Oscillator`s. The
`is_action_in_progress_` guard covers the common case, but it's a plain
`bool` read across tasks with no memory barrier or lock. Fine for this scale
of feature; worth revisiting with a proper lock/atomic if it ever misbehaves
in practice.

## Not done yet: the MOSFET rail cutoff

This entire feature only stops/starts the PWM signal (`ledc_stop()` /
re-`ledc_channel_config()`). The servos' power rail (V+) stays live the whole
time, and most servo driver ICs keep drawing idle current from a live rail
even with no PWM pulses arriving. Actually cutting current requires a
MOSFET-based power switch on the servo rail, gated by a spare GPIO, hung off
the same `SetServosAttached(bool)` call — sequenced as detach-PWM-first/
cut-power-second on the way down, and restore-power-first/re-attach-second
(with a short settle delay) on the way up, to avoid a glitch on the signal
line while the rail is transitioning. Not implemented — this file only
covers the software (PWM) half that's live today.

## Turning the feature off (without touching anything else)

The whole feature is just two call sites — `SetOttoServosAttached(false)` in
`ShowIdleClock()` and `SetOttoServosAttached(true)` in `HideIdleClock()` —
plus the logic behind them in `OttoController::SetServosAttached()`. Nothing
else in the idle clock, the action queue, or the oscillator code depends on
it running: the clock still shows/hides fine, and servos still work fine for
normal actions, whether or not this fires.

That makes it cheap to gate in exactly one place — the top of
`SetServosAttached(bool attached)` in `otto_controller.cc`:

```cpp
void SetServosAttached(bool attached) {
    if (!servo_power_save_enabled_) {
        return;
    }
    if (is_action_in_progress_) {
        return;
    }
    ...
}
```

- **Compile-time toggle:** a `#define`/Kconfig option in place of the member
  above, if it should never be runtime-changeable.
- **Runtime toggle:** a `bool servo_power_save_enabled_ = true;` member
  (flip it via an MCP tool, a button combo, an NVS setting, etc.) if you want
  it switchable without reflashing.

Either way, disabling it just makes the display's two calls no-ops — no
other code path needs to change.

## Rebuild and test

```sh
python3 scripts/build.py otto-robot --name otto-robot --language en-US --wake-word wn9_jarvis_tts
```

Flash, let the device go idle for 30+ seconds until the clock appears
(servos should go limp — no `ledc: GPIO ... not usable` warning), then
trigger any real activity and confirm the robot audibly/visibly re-homes as
the clock disappears.
