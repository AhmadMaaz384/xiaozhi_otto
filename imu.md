# Otto Shake Detection (MPU6050)

Physically shaking otto triggers an instant local reaction: the emoji switches
to a "shocked" face, a vibration sound plays, and the legs do a quick shake
burst — all fully offline, no server/LLM round-trip involved. Three files are
touched, all board-specific to otto-robot (no core/shared code changed):

- `main/boards/otto-robot/mpu6050.h` (new)
- `main/boards/otto-robot/otto_robot.cc`
- `main/boards/otto-robot/otto_controller.cc`

## Behaviour

1. At boot, `OttoRobot` probes for a real MPU6050 on the I2C pins reserved in
   `config.h` (`i2c_sda_pin`/`i2c_scl_pin`, currently GPIO17/18).
2. **If nothing answers** (no sensor wired up yet): logs a warning and the
   feature stays off. This is a normal, supported state, not an error — same
   as camera/battery being optional elsewhere on this board.
3. **If a real MPU6050 responds**: logs `"MPU6050 detected, starting shake
   detection"` and starts a background task that polls the accelerometer
   every ~80ms.
4. Whenever the combined movement between two consecutive samples exceeds a
   threshold (see [Adjusting shake sensitivity](#adjusting-shake-sensitivity)
   below), and at least 2 seconds have passed since the last trigger, it
   fires the response: emotion → `"shocked"`, plays
   `Lang::Sounds::OGG_VIBRATION`, and queues a `shake_leg` servo action
   (3 steps, speed 400, same as if `self.otto.action` had been called with
   `action: "shake_leg"`).

Confirmed working on real hardware — see the September 2026 testing session:
multiple real shakes each correctly triggered all three parts of the
response, no crashes, no false triggers except one suspected case (see
[Known consideration](#known-consideration-speaker-vibration) below).

## How it's wired in

### `mpu6050.h` — the driver

A minimal class, `Mpu6050 : public I2cDevice` (reusing
`main/boards/common/i2c_device.h`, the same generic I2C register
read/write helper other board peripherals use — no external managed
component needed):

```cpp
Mpu6050(i2c_master_bus_handle_t i2c_bus, uint8_t addr = kDefaultAddress)
    : I2cDevice(i2c_bus, addr) {
    WriteReg(0x6B, 0x00);  // PWR_MGMT_1: clear SLEEP bit
}

void ReadAccel(int16_t& ax, int16_t& ay, int16_t& az) {
    uint8_t buf[6];
    ReadRegs(0x3B, buf, sizeof(buf));  // ACCEL_XOUT_H .. ACCEL_ZOUT_L
    ...
}
```

No gyro, no DMP, no interrupt/FIFO support — just enough to read raw
accelerometer samples for delta-based shake detection.

### `otto_robot.cc` — bus creation, safe probing, and the detection task

**`InitializeMpu6050()`** (private method on `OttoRobot`, called from the
constructor right after `InitializeOttoController()`):

1. Skips entirely if `i2c_sda_pin`/`i2c_scl_pin` are `GPIO_NUM_NC` (treats the
   sensor as an optional capability, per this board's usual convention for
   camera/battery/etc.).
2. Creates a **separate** I2C bus/port (`I2C_NUM_1`) from the one used for
   camera detection (`i2c_bus_`, `I2C_NUM_0`) — they're unrelated and
   shouldn't share a port.
3. **Probes manually first**, via a raw `i2c_master_bus_add_device()` +
   `i2c_master_transmit_receive()` against the `WHO_AM_I` register (`0x75`,
   expects `0x68` back) — checking the return code by hand, *not* going
   through `I2cDevice` yet.
4. Only constructs the real `Mpu6050` object (which uses `I2cDevice`'s
   `ESP_ERROR_CHECK`-based methods) once that probe has already confirmed a
   real device is there.
5. If detected: starts `OttoShakeDetectionTask` via `xTaskCreate` (3072-byte
   stack, priority 3), passing the `Mpu6050*` directly as the task argument.

**Why the manual probe matters — this is the one thing to never skip if you
touch this code:** `I2cDevice::WriteReg()`/`ReadRegs()` wrap every I2C call in
`ESP_ERROR_CHECK`, which **panics and reboots the whole firmware** if nothing
acknowledges on the bus. Since an MPU6050 not being physically wired up yet is
a completely normal state for this board (same as no camera, no battery), the
driver object must never be constructed until presence is already confirmed
through a call that tolerates failure gracefully.

**`OttoShakeDetectionTask()`** (a plain function in an anonymous namespace,
not a class method — deliberately: it only needs the `Mpu6050*`, not the
whole `OttoRobot`, so it doesn't require touching the class declaration):
delta-threshold detector, same approach as `espressif/esp-vocat`'s BMI270
implementation in this same codebase — read accel, compare to the previous
sample, fire on a large-enough combined delta with a cooldown so one shake
doesn't retrigger dozens of times in a row. Runs on its own task, so the
response is dispatched via `Application::GetInstance().Schedule(...)` rather
than touching `Display`/`Application` state directly from this task.

### `otto_controller.cc` — triggering the servo response

Same extern-hook pattern already used for `SetOttoServosAttached`
(releasing/restoring servo torque for the idle clock):

```cpp
// Public method on OttoController
void TriggerShakeResponse() {
    if (is_action_in_progress_) {
        return;
    }
    QueueAction(ACTION_SHAKE_LEG, 3, 400, 1, 0);
}

// Free function otto_robot.cc calls into
void TriggerOttoShake() {
    if (g_otto_controller != nullptr) {
        g_otto_controller->TriggerShakeResponse();
    }
}
```

Silently dropped if an action is already running, same as any other action
queued during that window would be.

## Customizing

### Adjusting shake sensitivity

In `otto_robot.cc`, inside `OttoShakeDetectionTask()`:

```cpp
constexpr int kShakeDeltaThreshold = 20000;
constexpr int64_t kShakeCooldownMs = 2000;
```

- **`kShakeDeltaThreshold`** is the "how hard" knob — compared against
  `dx + dy + dz`, the combined change in raw accelerometer reading between
  two consecutive ~80ms samples.
  - **Lower** → triggers on gentler movement (more sensitive). Try
    `12000`–`15000` for a light-tap trigger.
  - **Higher** → needs a harder shake (less sensitive). Try `30000`–`35000`
    if it's firing too easily (e.g. from the speaker-vibration case below).
- **`kShakeCooldownMs`** is unrelated to intensity — it's the minimum time
  between two triggers, so one continuous shake doesn't fire the response
  repeatedly. Raise it if a single shake is triggering more than once;
  lower it if you want back-to-back shakes to each register separately.

### Changing what happens on trigger

Inside the `Schedule()` callback in `OttoShakeDetectionTask()`:

```cpp
Application::GetInstance().Schedule([]() {
    Board::GetInstance().GetDisplay()->SetEmotion("shocked");
    Application::GetInstance().PlaySound(Lang::Sounds::OGG_VIBRATION);
    TriggerOttoShake();
});
```

- **Emotion** — swap `"shocked"` for any other name from the otto-gif
  collection (see `clock.md`'s sibling doc / the earlier emoji-GIF work for
  the full list of 21 names).
- **Sound** — swap `Lang::Sounds::OGG_VIBRATION` for any other entry in
  `main/assets/lang_config.h`'s `Lang::Sounds` namespace (e.g.
  `OGG_POPUP`, `OGG_EXCLAMATION`).
- **Servo response** — change the `QueueAction(ACTION_SHAKE_LEG, 3, 400, 1, 0)`
  call in `OttoController::TriggerShakeResponse()` (`otto_controller.cc`) to
  any other action type/parameters — same `steps/speed/direction/amount`
  arguments documented for `self.otto.action` in this board's own README.

### Changing the I2C pins

`config.h`'s `NON_CAMERA_VERSION_CONFIG.i2c_sda_pin`/`i2c_scl_pin` (currently
GPIO17/18) is the only place the pin choice lives — `InitializeMpu6050()`
reads them from `hw_config_` directly, nothing else needs to change if you
move the sensor to different pins (as long as the new pins don't collide with
anything else — see the pin-inventory discussion earlier in this project for
what's already spoken for on this board).

## Known consideration: speaker vibration

During testing, one shake-trigger fired right as audio output turned on
(immediately after `AudioCodec: Set output enable to true`). If the MPU6050
ends up mounted near the speaker or chassis, speaker vibration itself could
occasionally read as a "shake." If this turns out to happen during normal
speech/sound playback (not from actually moving the robot), two options:
raise `kShakeDeltaThreshold`, or add a guard in `OttoShakeDetectionTask()` to
skip detection while `Application::GetInstance().GetDeviceState() ==
kDeviceStateSpeaking`.

## Rebuild and test

```sh
python3 scripts/build.py otto-robot --name otto-robot --language en-US --wake-word wn9_jarvis_tts
```

Then flash and check the boot log for either `"MPU6050 detected, starting
shake detection"` or the graceful `"No MPU6050 detected... shake detection
disabled"` warning — confirms the sensor probe result before testing the
actual shake response by hand.
