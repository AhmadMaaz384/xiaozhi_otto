# Changes Made

Complete, exact list of every file created or modified in this session — three source files plus the initial `CLAUDE.md` from `/init`.

## 1. `CLAUDE.md` (new file, root of repo)

Created at the very start via `/init` — repo-wide guidance file (commands, architecture, required rules) for future Claude Code sessions. Unrelated to the otto-robot debugging work that followed.

## 2. `main/boards/otto-robot/otto_robot.cc` — disable false low-battery alert

```cpp
virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
    // TODO: no battery is wired up yet on this unit - reporting readings from a
    // floating ADC pin trips the low-battery popup/alert sound on every boot.
    // Return false (matches Board::GetBatteryLevel()'s no-battery default) until
    // a battery is connected; then restore the body below instead of this stub.
    return false;
    // charging = power_manager_->IsCharging();
    // discharging = !charging;
    // level = power_manager_->GetBatteryLevel();
    // return true;
}
```

**Why:** with no battery connected, the floating ADC read as 0% + `discharging=true` was tripping the shared low-battery popup/sound on every boot. Original body is commented out, not deleted — revert is a two-line swap once a battery is wired up.

## 3. `main/display/lvgl_display/gif/lvgl_gif.cc` — crash guard in `NextFrame()`

Added a new branch right after the existing "animation finished" check:

```cpp
if(has_next < 0) {
    // Malformed frame (e.g. out-of-bounds coordinates) - gd_get_frame() already
    // logged the reason. gif_->fx/fy/fw/fh may now hold the invalid geometry from
    // the failed parse, so stop here instead of falling through to gd_render_frame(),
    // which would render/dispose using that stale geometry and write past the
    // canvas buffer.
    playing_ = false;
    if (timer_) {
        lv_timer_pause(timer_);
    }
    ESP_LOGE(TAG, "GIF decode error, stopping animation");
    return;
}
```

**Why:** `gd_get_frame()`'s error return (`-1`) was never checked — execution fell through to `gd_render_frame()` with corrupted frame geometry, writing past the canvas buffer and crashing the device (the Guru Meditation panic caught live on serial). This is the safety net; #4 below is the actual root-cause fix.

## 4. `main/display/lvgl_display/gif/gifdec.c` — the actual root-cause fix

Added one line inside `read_image_data()`'s clear-code branch:

```c
if(key == clear) {
    key_size = init_key_size;
    table->nentries = (1 << (key_size - 1)) + 2;
    table_is_full = 0;
    // A clear code can arrive in the same pass that add_entry() just
    // returned 1 (table crossed a power-of-two size) for the *previous*
    // key. Without resetting ret here, the stale ret==1 below would
    // widen key_size right after this reset, misaligning every bit read
    // for the rest of the frame and corrupting the decode.
    ret = 0;
}
```

**Why:** the actual LZW decoder bug — verified against real GIF bytes that this exact one-line omission was corrupting decode state on all 21 emoji GIFs (traced and confirmed via a faithful Python port before touching the real file).

## Not code changes, but worth noting for completeness

- Build/flash commands used `--language en-US --wake-word wn9_jarvis_tts` — these are runtime build configuration flags (regenerate `sdkconfig`/`build/` each time), not hand-edited source files.
- The 128×128 GIF conversion, `config.h`'s `DISPLAY_WIDTH`/`DISPLAY_HEIGHT` change, and `converter.py` were all prior work done by the user before this session — not touched here.
