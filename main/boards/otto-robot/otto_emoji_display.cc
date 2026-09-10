#include "otto_emoji_display.h"

#include <esp_log.h>

#include <cmath>
#include <cstring>
#include <ctime>
#include <vector>

#include "application.h"
#include "assets.h"
#include "assets/lang_config.h"
#include "device_state.h"
#include "display/lvgl_display/emoji_collection.h"
#include "display/lvgl_display/lvgl_image.h"
#include "display/lvgl_display/lvgl_theme.h"

#define TAG "OttoEmojiDisplay"

// Defined in otto_controller.cc; lets the idle clock release/restore servo
// holding torque without the display needing to know about OttoController.
extern void SetOttoServosAttached(bool attached);

OttoEmojiDisplay::OttoEmojiDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                                   int width, int height, int offset_x, int offset_y, bool mirror_x,
                                   bool mirror_y, bool swap_xy)
    : SpiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y, mirror_x, mirror_y,
                    swap_xy) {}

void OttoEmojiDisplay::SetupUI() {
    // Prevent duplicate calls - parent SetupUI() will also check, but check here for early return
    if (setup_ui_called_) {
        ESP_LOGW(TAG, "SetupUI() called multiple times, skipping duplicate call");
        return;
    }

    // Call parent SetupUI() first to create all lvgl objects
    SpiLcdDisplay::SetupUI();

    // UI 对象创建完成后切换主题
    auto* dark_theme = LvglThemeManager::GetInstance().GetTheme("dark");
    if (dark_theme != nullptr) {
        SetTheme(dark_theme);
    }

    // Setup preview image after UI is initialized - release lock before calling SetEmotion
    // to avoid deadlock (SetEmotion also acquires DisplayLockGuard internally)
    {
        DisplayLockGuard lock(this);
        lv_obj_set_size(preview_image_, width_, height_);
    }

#if OTTO_IDLE_CLOCK_ENABLED
    SetupIdleClockUI();
#endif
    SetupTicTacToeUI();

    // Set default emotion after UI is initialized
    SetEmotion("staticstate");
}

void OttoEmojiDisplay::SetupPreviewImage() {
    DisplayLockGuard lock(this);
    if (preview_image_ == nullptr) {
        ESP_LOGW(TAG,
                 "SetupPreviewImage called but preview_image_ is nullptr (UI not initialized yet)");
        return;
    }
    lv_obj_set_size(preview_image_, width_, height_);
}

void OttoEmojiDisplay::InitializeOttoEmojis() {
    ESP_LOGI(TAG, "Otto表情初始化将由Assets系统处理");
    // 表情初始化已移至assets系统,通过DEFAULT_EMOJI_COLLECTION=otto-gif配置
    // assets.cc会从assets分区加载GIF表情并设置到theme
    // Note: Default emotion is now set in SetupUI() after LVGL objects are created
}

LV_FONT_DECLARE(OTTO_ICON_FONT);
void OttoEmojiDisplay::SetStatus(const char* status) {
    // Any status change while the device has actually left the idle state
    // means real activity - drop the idle clock immediately rather than
    // waiting for the next UpdateStatusBar() tick. Must happen before we take
    // our own lock below: HideIdleClock() acquires DisplayLockGuard itself
    // and the lock is not re-entrant.
    //
    // Gate on device state, not just "clock is visible": LvglDisplay's own
    // UpdateStatusBar() re-calls SetStatus() with the same HH:MM text on
    // every tick once idle (last_status_update_time_ never advances past its
    // initial 10s), so without this check the clock would hide itself again
    // one tick after ever showing.
    if (clock_visible_ && Application::GetInstance().GetDeviceState() != kDeviceStateIdle) {
        HideIdleClock();
    }

    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    auto text_font = lvgl_theme->text_font()->font();
    DisplayLockGuard lock(this);
    if (!status) {
        ESP_LOGE(TAG, "SetStatus: status is nullptr");
        return;
    }

    if (strcmp(status, Lang::Strings::LISTENING) == 0) {
        lv_obj_set_style_text_font(status_label_, &OTTO_ICON_FONT, 0);
        lv_label_set_text(status_label_, "\xEF\x84\xB0");  // U+F130 麦克风图标
        lv_obj_clear_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(network_label_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(battery_label_, LV_OBJ_FLAG_HIDDEN);
        return;
    } else if (strcmp(status, Lang::Strings::SPEAKING) == 0) {
        lv_obj_set_style_text_font(status_label_, &OTTO_ICON_FONT, 0);
        lv_label_set_text(status_label_, "\xEF\x80\xA8");  // U+F028 说话图标
        lv_obj_clear_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(network_label_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(battery_label_, LV_OBJ_FLAG_HIDDEN);
        return;
    } else if (strcmp(status, Lang::Strings::CONNECTING) == 0) {
        lv_obj_set_style_text_font(status_label_, &OTTO_ICON_FONT, 0);
        lv_label_set_text(status_label_, "\xEF\x83\x81");  // U+F0c1 连接图标
        lv_obj_clear_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
        return;
    } else if (strcmp(status, Lang::Strings::STANDBY) == 0) {
        lv_obj_set_style_text_font(status_label_, text_font, 0);
        lv_label_set_text(status_label_, "");
        lv_obj_clear_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(network_label_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(battery_label_, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_set_style_text_font(status_label_, text_font, 0);
    lv_label_set_text(status_label_, status);
}

void OttoEmojiDisplay::SetEmotion(const char* emotion) {
    // Same reasoning as SetStatus() above: only treat this as real activity
    // once the device has actually left the idle state. Neither call below
    // holds our lock itself, so no nesting/deadlock risk (HideIdleClock() and
    // the base SetEmotion() each acquire their own DisplayLockGuard
    // internally).
    if (clock_visible_ && Application::GetInstance().GetDeviceState() != kDeviceStateIdle) {
        HideIdleClock();
    }
    SpiLcdDisplay::SetEmotion(emotion);
}

void OttoEmojiDisplay::SetPreviewImage(std::unique_ptr<LvglImage> image) {
    // A real preview image also hides emoji_box_, so make sure the idle
    // clock isn't left showing underneath/alongside it. Must happen before
    // the lock below - HideIdleClock() takes its own, non-re-entrant lock.
    if (image != nullptr && clock_visible_) {
        HideIdleClock();
    }

    DisplayLockGuard lock(this);
    if (preview_image_ == nullptr) {
        ESP_LOGE(TAG, "Preview image is not initialized");
        return;
    }

    if (image == nullptr) {
        esp_timer_stop(preview_timer_);
        lv_obj_remove_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
        preview_image_cached_.reset();
        if (gif_controller_) {
            gif_controller_->Start();
        }
        return;
    }

    preview_image_cached_ = std::move(image);
    auto img_dsc = preview_image_cached_->image_dsc();
    // 设置图片源并显示预览图片
    lv_image_set_src(preview_image_, img_dsc);
    lv_image_set_rotation(preview_image_, 900);
    if (img_dsc->header.w > 0 && img_dsc->header.h > 0) {
        // zoom factor 1.0
        lv_image_set_scale(preview_image_, 256 * width_ / img_dsc->header.w);
    }

    // Hide emoji_box_
    if (gif_controller_) {
        gif_controller_->Stop();
    }
    lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
    esp_timer_stop(preview_timer_);
    ESP_ERROR_CHECK(esp_timer_start_once(preview_timer_, PREVIEW_IMAGE_DURATION_MS * 1000));
}

void OttoEmojiDisplay::SetupIdleClockUI() {
    DisplayLockGuard lock(this);
    lv_obj_t* screen = lv_screen_active();

    clock_container_ = lv_obj_create(screen);
    lv_obj_set_size(clock_container_, width_, height_);
    lv_obj_align(clock_container_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_pad_all(clock_container_, 0, 0);
    lv_obj_set_style_border_width(clock_container_, 0, 0);
    lv_obj_set_style_radius(clock_container_, 0, 0);
    // Explicit black background regardless of the active theme, so this
    // reads as a deliberate full-screen clock face rather than inheriting
    // whatever the surrounding UI happens to use.
    lv_obj_set_style_bg_color(clock_container_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(clock_container_, LV_OPA_COVER, 0);
    lv_obj_add_flag(clock_container_, LV_OBJ_FLAG_HIDDEN);

    // Only the active style's widgets are actually created. This used to
    // unconditionally build both faces and just hide whichever one wasn't
    // active, on the theory that flipping OTTO_IDLE_CLOCK_STYLE would then
    // never need anything else changed - but OTTO_IDLE_CLOCK_STYLE is a
    // compile-time macro, not a runtime switch, so there was never an
    // actual benefit to that: the inactive face's widgets can never be
    // shown without a rebuild anyway. (This was tried as one candidate fix
    // for a tearing bug that turned out to be an SPI clock speed issue, not
    // a widget one - see InitializeLcdDisplay() in otto_robot.cc for the
    // real root cause/fix. Kept anyway since it's simply less to build.)
#if OTTO_IDLE_CLOCK_STYLE == ANALOG
    SetupAnalogClockUI();
#else
    clock_time_label_ = lv_label_create(clock_container_);
    // "88:88" (not the "--:--" actually shown below) - widest realistic
    // digit combination, used only to measure a safe fixed width immediately
    // below. Some fonts don't give every digit the same advance width, so
    // measuring against dashes could freeze a box too narrow for e.g. "8".
    lv_label_set_text(clock_time_label_, "88:88");
    lv_obj_set_style_text_color(clock_time_label_, lv_color_white(), 0);
    // Scale the normal text font up instead of pulling in a new large font
    // asset - LVGL's 256 == 100% fixed-point scale convention. Kept modest
    // (2x) so "HH:MM" reliably fits within a 128px-wide screen; 3x clipped
    // the minutes off the edge.
    lv_obj_set_style_transform_scale(clock_time_label_, 512, 0);
    lv_obj_set_style_text_align(clock_time_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(clock_time_label_, LV_ALIGN_CENTER, 0, -10);
    // Freeze the label's own width instead of leaving it on LVGL's default
    // auto-content sizing (LV_SIZE_CONTENT), so a real text change can never
    // resize/reposition this transform-scaled widget. Another candidate fix
    // tried for the tearing bug mentioned above (didn't turn out to be the
    // cause either - see otto_robot.cc), but a reasonable defensive habit
    // for a transform-scaled label with dynamic content regardless, so it's
    // kept.
    lv_obj_update_layout(clock_time_label_);  // force sizing now, before freezing it
    lv_obj_set_width(clock_time_label_, lv_obj_get_width(clock_time_label_));
    lv_label_set_text(clock_time_label_, "--:--");  // now safe - width is fixed, no resize triggered

    clock_date_label_ = lv_label_create(clock_container_);
    lv_label_set_text(clock_date_label_, "");
    lv_obj_set_style_text_color(clock_date_label_, lv_color_white(), 0);
    lv_obj_align(clock_date_label_, LV_ALIGN_CENTER, 0, 30);
#endif
}

void OttoEmojiDisplay::SetupAnalogClockUI() {
    DisplayLockGuard lock(this);

    const int face_size = (width_ < height_ ? width_ : height_) - 8;

    analog_face_ = lv_scale_create(clock_container_);
    lv_obj_set_size(analog_face_, face_size, face_size);
    lv_obj_align(analog_face_, LV_ALIGN_CENTER, 0, 0);
    lv_scale_set_mode(analog_face_, LV_SCALE_MODE_ROUND_INNER);
    lv_scale_set_rotation(analog_face_, 270);  // 0 starts at 12 o'clock, not 3 o'clock
    lv_scale_set_angle_range(analog_face_, 360);
    lv_scale_set_range(analog_face_, 0, 60);
    lv_scale_set_total_tick_count(analog_face_, 60);
    lv_scale_set_major_tick_every(analog_face_, 5);  // 12 major ticks, one per hour
    // No numerals on the face at all - avoids the exact font-scaling/clipping
    // issue the digital clock hit, since there's no text to fit on screen.
    lv_scale_set_label_show(analog_face_, false);
    lv_obj_set_style_bg_opa(analog_face_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(analog_face_, 0, 0);
    lv_obj_set_style_line_color(analog_face_, lv_color_white(), LV_PART_INDICATOR);
    lv_obj_set_style_line_color(analog_face_, lv_color_white(), LV_PART_ITEMS);
    lv_obj_set_style_line_width(analog_face_, 2, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(analog_face_, 1, LV_PART_ITEMS);

    analog_hour_hand_ = lv_line_create(clock_container_);
    lv_obj_set_style_line_width(analog_hour_hand_, 4, 0);
    lv_obj_set_style_line_rounded(analog_hour_hand_, true, 0);
    lv_obj_set_style_line_color(analog_hour_hand_, lv_color_make(0xFF, 0x00, 0x00), 0);

    analog_minute_hand_ = lv_line_create(clock_container_);
    lv_obj_set_style_line_width(analog_minute_hand_, 3, 0);
    lv_obj_set_style_line_rounded(analog_minute_hand_, true, 0);
    lv_obj_set_style_line_color(analog_minute_hand_, lv_color_make(0xFF, 0x00, 0x00), 0);
}

void OttoEmojiDisplay::ShowIdleClock() {
    DisplayLockGuard lock(this);
    if (clock_container_ == nullptr || clock_visible_) {
        return;
    }
    if (gif_controller_) {
        gif_controller_->Stop();
    }
    lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(clock_container_, LV_OBJ_FLAG_HIDDEN);
    clock_visible_ = true;
    SetOttoServosAttached(false);
}

void OttoEmojiDisplay::HideIdleClock() {
    idle_since_ = {};
    DisplayLockGuard lock(this);
    if (!clock_visible_) {
        return;
    }
    lv_obj_add_flag(clock_container_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
    if (gif_controller_) {
        gif_controller_->Start();
    }
    clock_visible_ = false;
    SetOttoServosAttached(true);
}

void OttoEmojiDisplay::UpdateIdleClockTime() {
    time_t now = time(NULL);
    struct tm* tm_info = localtime(&now);
    // Same sanity check used for the small status-bar clock: skip until the
    // system time has actually been set (e.g. by the OTA check-in response).
    if (tm_info->tm_year < 2025 - 1900) {
        return;
    }

    char time_str[16];
    strftime(time_str, sizeof(time_str), "%H:%M", tm_info);
    char date_str[32];
    strftime(date_str, sizeof(date_str), "%a, %b %d", tm_info);

    {
        DisplayLockGuard lock(this);
        // Only push text that actually changed - lv_label_set_text() has no
        // same-text fast path in this LVGL version (see the member comment
        // on last_clock_time_str_/last_clock_date_str_ in the header): every
        // call unconditionally invalidates and recomputes the label's
        // auto-content size, even when the string is identical to what's
        // already shown. This function runs every second but HH:MM/date
        // only actually change once a minute/day, so without this guard the
        // label was being pushed through a pointless invalidate+resize cycle
        // 59 out of every 60 seconds - wasteful regardless of the display's
        // actual tearing bug (traced to SPI clock speed - see otto_robot.cc).
        if (clock_time_label_ != nullptr && strcmp(time_str, last_clock_time_str_) != 0) {
            lv_label_set_text(clock_time_label_, time_str);
            strncpy(last_clock_time_str_, time_str, sizeof(last_clock_time_str_) - 1);
        }
        if (clock_date_label_ != nullptr && strcmp(date_str, last_clock_date_str_) != 0) {
            lv_label_set_text(clock_date_label_, date_str);
            strncpy(last_clock_date_str_, date_str, sizeof(last_clock_date_str_) - 1);
        }
    }

#if OTTO_IDLE_CLOCK_STYLE == ANALOG
    // Lock released above before this call - it takes its own lock, and the
    // lock is not re-entrant. Only done when analog is actually the active
    // face: this used to run unconditionally (every tick, even in DIGITAL
    // mode, touching the hidden hand line objects), which was one of
    // several candidates investigated for a display-wide tearing/"italic"
    // bug that turned out to be an SPI clock speed issue (see
    // InitializeLcdDisplay() in otto_robot.cc), not this. Left gated anyway
    // since there's no reason to touch the analog hands while they're not
    // even shown. Restores the digital path to exactly one lock scope doing
    // exactly one thing per tick, same as before the
    // analog clock existed.
    UpdateAnalogClockHands(tm_info->tm_hour, tm_info->tm_min);
#endif
}

void OttoEmojiDisplay::UpdateAnalogClockHands(int hour, int minute) {
    if (analog_hour_hand_ == nullptr || analog_minute_hand_ == nullptr) {
        return;
    }

    constexpr float kPi = 3.14159265f;
    const float cx = width_ / 2.0f;
    const float cy = height_ / 2.0f;
    const float shortest_side = static_cast<float>(width_ < height_ ? width_ : height_);
    const float hour_len = shortest_side * 0.22f;
    const float minute_len = shortest_side * 0.34f;

    // Angle measured clockwise from 12 o'clock (0 degrees = straight up).
    float hour_angle_deg = (static_cast<float>(hour % 12) + minute / 60.0f) * 30.0f;  // 360/12
    float minute_angle_deg = static_cast<float>(minute) * 6.0f;                        // 360/60

    float hour_rad = hour_angle_deg * kPi / 180.0f;
    float minute_rad = minute_angle_deg * kPi / 180.0f;

    // lv_value_precise_t is int32_t in this build (LV_USE_FLOAT is off), so
    // every point needs an explicit round-to-int rather than an implicit
    // (narrowing, -Werror'd) float-to-int conversion.
    auto to_precise = [](float v) -> lv_value_precise_t {
        return static_cast<lv_value_precise_t>(lroundf(v));
    };

    hour_hand_points_[0] = {to_precise(cx), to_precise(cy)};
    hour_hand_points_[1] = {to_precise(cx + hour_len * sinf(hour_rad)),
                            to_precise(cy - hour_len * cosf(hour_rad))};

    minute_hand_points_[0] = {to_precise(cx), to_precise(cy)};
    minute_hand_points_[1] = {to_precise(cx + minute_len * sinf(minute_rad)),
                              to_precise(cy - minute_len * cosf(minute_rad))};

    DisplayLockGuard lock(this);
    lv_line_set_points(analog_hour_hand_, hour_hand_points_, 2);
    lv_line_set_points(analog_minute_hand_, minute_hand_points_, 2);
}

void OttoEmojiDisplay::SetupTicTacToeUI() {
    DisplayLockGuard lock(this);
    lv_obj_t* screen = lv_screen_active();

    tic_tac_toe_container_ = lv_obj_create(screen);
    lv_obj_set_size(tic_tac_toe_container_, width_, height_);
    lv_obj_align(tic_tac_toe_container_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_pad_all(tic_tac_toe_container_, 0, 0);
    lv_obj_set_style_border_width(tic_tac_toe_container_, 0, 0);
    lv_obj_set_style_radius(tic_tac_toe_container_, 0, 0);
    lv_obj_set_style_bg_color(tic_tac_toe_container_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(tic_tac_toe_container_, LV_OPA_COVER, 0);
    lv_obj_add_flag(tic_tac_toe_container_, LV_OBJ_FLAG_HIDDEN);

    // Board area: a square centered on screen, same "shortest side minus a
    // small margin" sizing the analog clock face uses.
    const int board_size = (width_ < height_ ? width_ : height_) - 8;
    const int cell_size = board_size / 3;
    const int board_left = (width_ - board_size) / 2;
    const int board_top = (height_ - board_size) / 2;

    auto to_precise = [](int v) -> lv_value_precise_t {
        return static_cast<lv_value_precise_t>(v);
    };

    // 2 vertical + 2 horizontal grid lines, set once and never touched again
    // (unlike the clock hands, this board never moves) - but lv_line_set_points()
    // only stores the pointer it's given, not a copy, so the backing array
    // still has to outlive this function. `static` (not a plain local) is
    // what makes that safe here.
    static lv_point_precise_t grid_line_points[4][2];
    for (int i = 1; i <= 2; i++) {
        int x = board_left + i * cell_size;
        lv_obj_t* v_line = lv_line_create(tic_tac_toe_container_);
        lv_obj_set_style_line_width(v_line, 2, 0);
        lv_obj_set_style_line_color(v_line, lv_color_white(), 0);
        grid_line_points[i - 1][0] = {to_precise(x), to_precise(board_top)};
        grid_line_points[i - 1][1] = {to_precise(x), to_precise(board_top + board_size)};
        lv_line_set_points(v_line, grid_line_points[i - 1], 2);
    }
    for (int i = 1; i <= 2; i++) {
        int y = board_top + i * cell_size;
        lv_obj_t* h_line = lv_line_create(tic_tac_toe_container_);
        lv_obj_set_style_line_width(h_line, 2, 0);
        lv_obj_set_style_line_color(h_line, lv_color_white(), 0);
        grid_line_points[2 + i - 1][0] = {to_precise(board_left), to_precise(y)};
        grid_line_points[2 + i - 1][1] = {to_precise(board_left + board_size), to_precise(y)};
        lv_line_set_points(h_line, grid_line_points[2 + i - 1], 2);
    }

    // 9 cells: each a small transparent, borderless box purely so its child
    // label can be centered with LV_ALIGN_CENTER regardless of whether the
    // text is "5", "X" or "O" (a differently-sized glyph would otherwise
    // shift if centered directly against the board).
    for (int row = 0; row < 3; row++) {
        for (int col = 0; col < 3; col++) {
            int index = row * 3 + col;
            lv_obj_t* box = lv_obj_create(tic_tac_toe_container_);
            lv_obj_set_size(box, cell_size, cell_size);
            lv_obj_set_pos(box, board_left + col * cell_size, board_top + row * cell_size);
            lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(box, 0, 0);
            lv_obj_set_style_pad_all(box, 0, 0);
            lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

            lv_obj_t* label = lv_label_create(box);
            lv_obj_set_style_text_color(label, lv_color_white(), 0);
            lv_obj_set_style_transform_scale(label, 512, 0);  // 2x, matches the digital clock
            lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);

            char text[2] = {static_cast<char>('1' + index), '\0'};
            lv_label_set_text(label, text);

            tic_tac_toe_cells_[index] = label;
        }
    }
}

void OttoEmojiDisplay::ShowTicTacToe() {
    // HideIdleClock() takes its own, non-reentrant lock, so - same ordering
    // as SetPreviewImage()/SetStatus()/SetEmotion() above - it has to run
    // before we take ours below. A game explicitly being started always
    // wins over the idle clock.
    if (clock_visible_) {
        HideIdleClock();
    }

    DisplayLockGuard lock(this);
    if (tic_tac_toe_container_ == nullptr || tic_tac_toe_visible_) {
        return;
    }
    if (gif_controller_) {
        gif_controller_->Stop();
    }
    lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(tic_tac_toe_container_, LV_OBJ_FLAG_HIDDEN);
    tic_tac_toe_visible_ = true;
}

void OttoEmojiDisplay::HideTicTacToe() {
    // Same reason ShowIdleClock()/HideIdleClock() reset this: otherwise the
    // idle clock could judge itself already 30s+ overdue the instant the
    // game ends, using a timestamp from before the game even started.
    idle_since_ = {};

    DisplayLockGuard lock(this);
    if (!tic_tac_toe_visible_) {
        return;
    }
    lv_obj_add_flag(tic_tac_toe_container_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
    if (gif_controller_) {
        gif_controller_->Start();
    }
    tic_tac_toe_visible_ = false;
}

void OttoEmojiDisplay::SetTicTacToeCell(int cell_number, char mark) {
    if (cell_number < 1 || cell_number > 9) {
        return;
    }
    lv_obj_t* label = tic_tac_toe_cells_[cell_number - 1];
    if (label == nullptr) {
        return;
    }

    DisplayLockGuard lock(this);
    char text[2] = {mark == ' ' ? static_cast<char>('0' + cell_number) : mark, '\0'};
    lv_label_set_text(label, text);

    lv_color_t color = lv_color_white();
    if (mark == 'X') {
        color = lv_color_make(0xFF, 0x00, 0x00);  // red, matches the clock hands
    } else if (mark == 'O') {
        color = lv_color_make(0x33, 0x99, 0xFF);  // blue, visually distinct from X
    }
    lv_obj_set_style_text_color(label, color, 0);
}

void OttoEmojiDisplay::UpdateStatusBar(bool update_all) {
    // Keep the existing small status-bar behaviour (network/battery icons,
    // the small HH:MM text) exactly as before.
    SpiLcdDisplay::UpdateStatusBar(update_all);

#if OTTO_IDLE_CLOCK_ENABLED
    auto& app = Application::GetInstance();
    bool is_idle = (app.GetDeviceState() == kDeviceStateIdle);

    if (!is_idle) {
        if (clock_visible_) {
            HideIdleClock();
        } else {
            idle_since_ = {};
        }
        return;
    }

    auto now = std::chrono::steady_clock::now();
    if (idle_since_.time_since_epoch().count() == 0) {
        idle_since_ = now;
    }

    auto idle_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - idle_since_).count();
    // Don't let the idle clock take over mid-game - a real gap between
    // moves (the human thinking) can easily cross the 30s idle threshold.
    if (!clock_visible_ && !tic_tac_toe_visible_ && idle_ms >= kIdleClockDelayMs) {
        ShowIdleClock();
    }

    if (clock_visible_) {
        UpdateIdleClockTime();
    }
#endif  // OTTO_IDLE_CLOCK_ENABLED
}
