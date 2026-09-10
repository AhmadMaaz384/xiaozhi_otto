#pragma once

#include "display/lcd_display.h"

#include <chrono>

// Set to 0 to disable the full-screen idle clock entirely (both the digital
// and analog faces) - the emotion GIF then just stays up indefinitely while
// idle, the same as before this feature existed. Nothing else on the board
// depends on this; it only gates SetupIdleClockUI() and the idle-timer logic
// in UpdateStatusBar() (otto_emoji_display.cc).
#define OTTO_IDLE_CLOCK_ENABLED 1

// Sentinel values for OTTO_IDLE_CLOCK_STYLE below - not otto/clock-specific,
// so keep an eye out if some other header ever wants these same names.
#define ANALOG 1
#define DIGITAL 0

// Which face shows when the idle clock is enabled above: ANALOG (face +
// rotating hands) or DIGITAL (HH:MM + date text). Both are always built
// either way (see SetupIdleClockUI()/SetupAnalogClockUI() in
// otto_emoji_display.cc) and kept updated every tick regardless of which is
// hidden, so flipping this needs nothing else changed.
#define OTTO_IDLE_CLOCK_STYLE DIGITAL

/**
 * @brief Otto机器人GIF表情显示类
 * 继承SpiLcdDisplay，通过EmojiCollection添加GIF表情支持
 */
class OttoEmojiDisplay : public SpiLcdDisplay {
   public:
    /**
     * @brief 构造函数，参数与SpiLcdDisplay相同
     */
    OttoEmojiDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width, int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y, bool swap_xy);

    virtual ~OttoEmojiDisplay() = default;
    virtual void SetStatus(const char* status) override;
    virtual void SetEmotion(const char* emotion) override;
    virtual void SetPreviewImage(std::unique_ptr<LvglImage> image) override;
    virtual void SetupUI() override;
    virtual void UpdateStatusBar(bool update_all = false) override;

    // Full-screen tic-tac-toe board, driven entirely by the self.otto.tictactoe_*
    // MCP tools (otto_controller.cc) - not tied to idle state at all, unlike
    // the idle clock below: it has to stay up through normal conversation
    // turns (each move *is* a conversation turn), so SetStatus()/SetEmotion()
    // deliberately don't touch it. Cell numbers are 1-9, phone-keypad style;
    // `mark` is 'X', 'O', or ' ' to restore the plain cell number (used on
    // Reset()/a new game).
    void ShowTicTacToe();
    void HideTicTacToe();
    void SetTicTacToeCell(int cell_number, char mark);
    bool IsTicTacToeVisible() const { return tic_tac_toe_visible_; }

   private:
    void InitializeOttoEmojis();
    void SetupPreviewImage();

    // Full-screen idle clock: after the device has been continuously idle for
    // kIdleClockDelayMs, it takes over from the emotion GIF, the same way
    // SetPreviewImage() takes over for a camera preview. Any status/emotion
    // change (i.e. any activity) hides it again immediately.
    void SetupIdleClockUI();
    void ShowIdleClock();
    void HideIdleClock();
    void UpdateIdleClockTime();

    // Analog face + hands, currently the active idle-clock look. The digital
    // labels (clock_time_label_/clock_date_label_) are still created and kept
    // updated, just hidden - see SetupIdleClockUI(). Swap which is hidden to
    // go back to the digital version.
    void SetupAnalogClockUI();
    void UpdateAnalogClockHands(int hour, int minute);

    void SetupTicTacToeUI();

    lv_obj_t* tic_tac_toe_container_ = nullptr;
    lv_obj_t* tic_tac_toe_cells_[9] = {};  // index 0..8 = cell number 1..9
    bool tic_tac_toe_visible_ = false;

    lv_obj_t* clock_container_ = nullptr;
    lv_obj_t* clock_time_label_ = nullptr;
    lv_obj_t* clock_date_label_ = nullptr;
    lv_obj_t* analog_face_ = nullptr;
    lv_obj_t* analog_hour_hand_ = nullptr;
    lv_obj_t* analog_minute_hand_ = nullptr;
    lv_point_precise_t hour_hand_points_[2] = {};
    lv_point_precise_t minute_hand_points_[2] = {};
    bool clock_visible_ = false;
    std::chrono::steady_clock::time_point idle_since_{};

    static constexpr int64_t kIdleClockDelayMs = 30000;  // 30s continuously idle
};
