#include <driver/i2c_master.h>
#include <driver/ledc.h>
#include <driver/spi_common.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_log.h>
#include <esp_timer.h>

#include <cstdlib>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "application.h"
#include "assets/lang_config.h"
#include "board.h"
#include "button.h"
#include "codecs/no_audio_codec.h"
#include "config.h"
#include "device_state.h"
#include "display/lcd_display.h"
#include "esp32_camera.h"
#include "lamp_controller.h"
#include "led/single_led.h"
#include "mcp_server.h"
#include "mpu6050.h"
#include "otto_emoji_display.h"
#include "power_manager.h"
#include "system_reset.h"
#include "websocket_control_server.h"
#include "wifi_board.h"

#define TAG "OttoRobot"

extern void InitializeOttoController(const HardwareConfig& hw_config);
extern void TriggerOttoShake();

namespace {

// How long the "shocked" face stays up after a shake before reverting back to
// the normal idle face on its own.
constexpr int64_t kShakeRevertMs = 7000;

// Lazily created the first time a shake actually fires (so a board without an
// MPU6050 never allocates it). Restarted (not left running) on every new
// shake, so a second shake within the revert window pushes the revert out
// again instead of firing early on top of the fresh "shocked" display.
esp_timer_handle_t g_shake_revert_timer = nullptr;

void ShakeRevertTimerCallback(void*) {
    Application::GetInstance().Schedule([]() {
        // Only revert if the device is still idle - if a real conversation
        // started in the meantime (wake word, button, etc.), its own
        // emotion/state changes take priority and this must not stomp on
        // whatever it's legitimately showing.
        if (Application::GetInstance().GetDeviceState() == kDeviceStateIdle) {
            Board::GetInstance().GetDisplay()->SetEmotion("neutral");
        }
    });
}

void EnsureShakeRevertTimer() {
    if (g_shake_revert_timer != nullptr) {
        return;
    }
    esp_timer_create_args_t args = {
        .callback = ShakeRevertTimerCallback,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "shake_revert",
    };
    ESP_ERROR_CHECK(esp_timer_create(&args, &g_shake_revert_timer));
}

// Delta-threshold shake detector, same approach as the espressif/esp-vocat
// board's BMI270 implementation (main/boards/espressif/esp-vocat/esp_vocat.cc)
// just against an MPU6050 instead: poll raw accel, compare consecutive
// samples, fire on a large-enough combined delta, with a cooldown so one
// shake doesn't retrigger dozens of times in a row.
void OttoShakeDetectionTask(void* arg) {
    auto* mpu = static_cast<Mpu6050*>(arg);
    int16_t prev_x = 0, prev_y = 0, prev_z = 0;
    bool has_prev = false;
    int64_t last_shake_ms = 0;
    constexpr int kShakeDeltaThreshold = 20000;
    constexpr int64_t kShakeCooldownMs = 2000;

    while (true) {
        int16_t x, y, z;
        mpu->ReadAccel(x, y, z);

        if (has_prev) {
            int dx = abs(static_cast<int>(x) - static_cast<int>(prev_x));
            int dy = abs(static_cast<int>(y) - static_cast<int>(prev_y));
            int dz = abs(static_cast<int>(z) - static_cast<int>(prev_z));
            int shake_score = dx + dy + dz;

            int64_t now_ms = esp_timer_get_time() / 1000;
            if (shake_score > kShakeDeltaThreshold && (now_ms - last_shake_ms) > kShakeCooldownMs) {
                last_shake_ms = now_ms;
                // Runs on this dedicated task, not the main task - schedule
                // the actual response instead of touching Display/Application
                // state directly from here.
                Application::GetInstance().Schedule([]() {
                    Board::GetInstance().GetDisplay()->SetEmotion("shocked");
                    Application::GetInstance().PlaySound(Lang::Sounds::OGG_VIBRATION);
                    TriggerOttoShake();
                });

                EnsureShakeRevertTimer();
                esp_timer_stop(g_shake_revert_timer);  // no-op/harmless if not running
                ESP_ERROR_CHECK(esp_timer_start_once(g_shake_revert_timer, kShakeRevertMs * 1000));
            }
        }

        prev_x = x;
        prev_y = y;
        prev_z = z;
        has_prev = true;
        vTaskDelay(pdMS_TO_TICKS(80));
    }
}

}  // namespace

class OttoRobot : public WifiBoard {
private:
    LcdDisplay* display_;
    PowerManager* power_manager_;
    Button boot_button_;
    WebSocketControlServer* ws_control_server_;
    HardwareConfig hw_config_;
    AudioCodec* audio_codec_;
    i2c_master_bus_handle_t i2c_bus_;
    Camera* camera_;
    bool has_camera_;
    OttoCameraType camera_type_;

    // Separate I2C bus/port from i2c_bus_ above (that one is only for the
    // transient camera-detection probe and gets torn down once camera init
    // decides one way or the other). This one is long-lived, for the MPU6050.
    i2c_master_bus_handle_t mpu_i2c_bus_;
    Mpu6050* mpu6050_;

    bool DetectHardwareVersion() {
        ledc_timer_config_t ledc_timer = {
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .duty_resolution = LEDC_TIMER_2_BIT,
            .timer_num = LEDC_TIMER,
            .freq_hz = CAMERA_XCLK_FREQ,
            .clk_cfg = LEDC_AUTO_CLK,
        };
        esp_err_t ret = ledc_timer_config(&ledc_timer);
        if (ret != ESP_OK) {
            return false;
        }

        ledc_channel_config_t ledc_channel = {
            .gpio_num = CAMERA_XCLK,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = LEDC_CHANNEL,
            .intr_type = LEDC_INTR_DISABLE,
            .timer_sel = LEDC_TIMER,
            .duty = 2,
            .hpoint = 0,
        };
        ret = ledc_channel_config(&ledc_channel);
        if (ret != ESP_OK) {
            return false;
        }

        vTaskDelay(pdMS_TO_TICKS(100));
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = CAMERA_VERSION_CONFIG.i2c_sda_pin,
            .scl_io_num = CAMERA_VERSION_CONFIG.i2c_scl_pin,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags =
                {
                    .enable_internal_pullup = 1,
                },
        };

        ret = i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_);
        if (ret != ESP_OK) {
            ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL, 0);
            return false;
        }
        const uint8_t camera_addresses[] = {0x30, 0x3C, 0x21, 0x60};
        bool camera_found = false;
        uint16_t detected_pid = 0;

        for (size_t i = 0; i < sizeof(camera_addresses); i++) {
            uint8_t addr = camera_addresses[i];
            i2c_device_config_t dev_cfg = {
                .dev_addr_length = I2C_ADDR_BIT_LEN_7,
                .device_address = addr,
                .scl_speed_hz = 100000,
            };

            i2c_master_dev_handle_t dev_handle;
            ret = i2c_master_bus_add_device(i2c_bus_, &dev_cfg, &dev_handle);
            if (ret == ESP_OK) {
                uint8_t data[2] = {0, 0};

                uint8_t reg_addr_8bit = 0x0A;
                ret = i2c_master_transmit_receive(dev_handle, &reg_addr_8bit, 1, data, 2, 200);
                if (ret == ESP_OK && (data[0] != 0 || data[1] != 0)) {
                    detected_pid = (data[0] << 8) | data[1];
                    ESP_LOGI(TAG, "检测到摄像头 (OV2640方式) PID=0x%04X (地址=0x%02X)",
                             detected_pid, addr);
                    camera_found = true;
                    i2c_master_bus_rm_device(dev_handle);
                    break;
                }

                uint8_t reg_addr_high[2] = {0x30, 0x0A};
                uint8_t reg_addr_low[2] = {0x30, 0x0B};
                uint8_t pid_high = 0, pid_low = 0;

                ret = i2c_master_transmit_receive(dev_handle, reg_addr_high, 2, &pid_high, 1, 200);
                if (ret == ESP_OK) {
                    ret =
                        i2c_master_transmit_receive(dev_handle, reg_addr_low, 2, &pid_low, 1, 200);
                    if (ret == ESP_OK) {
                        detected_pid = (pid_high << 8) | pid_low;
                        if (detected_pid != 0) {
                            ESP_LOGI(TAG, "检测到摄像头 (OV3660方式) PID=0x%04X (地址=0x%02X)",
                                     detected_pid, addr);
                            camera_found = true;
                            i2c_master_bus_rm_device(dev_handle);
                            break;
                        }
                    }
                }

                i2c_master_bus_rm_device(dev_handle);
            }
        }

        if (!camera_found) {
            i2c_del_master_bus(i2c_bus_);
            i2c_bus_ = nullptr;
            ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL, 0);
            camera_type_ = OTTO_CAMERA_NONE;
        } else {
            // 根据 PID 判断摄像头类型
            if (detected_pid == OTTO_OV2640_PID_1 || detected_pid == OTTO_OV2640_PID_2) {
                camera_type_ = OTTO_CAMERA_OV2640;
                ESP_LOGI(TAG, "摄像头类型: OV2640 (PID=0x%04X)", detected_pid);
            } else if (detected_pid == OTTO_OV3660_PID) {
                camera_type_ = OTTO_CAMERA_OV3660;
                ESP_LOGI(TAG, "摄像头类型: OV3660 (PID=0x%04X)", detected_pid);
            } else {
                camera_type_ = OTTO_CAMERA_UNKNOWN;
                ESP_LOGW(TAG, "未知摄像头类型，PID=0x%04X", detected_pid);
            }
        }
        return camera_found;
    }

    void InitializePowerManager() {
        power_manager_ = new PowerManager(hw_config_.power_charge_detect_pin,
                                          hw_config_.power_adc_unit, hw_config_.power_adc_channel);
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = hw_config_.display_mosi_pin;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = hw_config_.display_clk_pin;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeLcdDisplay() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = hw_config_.display_cs_pin;
        io_config.dc_gpio_num = hw_config_.display_dc_pin;
        io_config.spi_mode = DISPLAY_SPI_MODE;
        // Dropped from 40MHz - confirmed fix for a display-wide tearing bug
        // (looked like "italic"/sheared text on the idle clock and
        // tic-tac-toe, but even static untouched grid lines teared too).
        // Root cause: SPI signal integrity, not software. On the custom
        // veroboard's hand-soldered wiring (this pin set was remapped for
        // the prototype - see the pin-planning notes in clock.md/imu.md),
        // 40MHz didn't leave enough margin for the ST7789's address-window
        // command bytes to arrive intact on every transaction; a corrupted
        // bit there shifts where the panel starts writing incoming pixel
        // data, producing exactly the shear that was seen. Only shows up
        // when a real SPI transaction actually fires (i.e. when content
        // visibly changes), which is why it looked content-dependent rather
        // than constant. 10MHz gives each bit ~4x longer to settle and has
        // tested clean; raise it again only after re-testing on real
        // hardware if display responsiveness ever needs to be faster.
        io_config.pclk_hz = 10 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = hw_config_.display_rst_pin;
        panel_config.rgb_ele_order = DISPLAY_RGB_ORDER;
        panel_config.bits_per_pixel = 16;

        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));

        esp_lcd_panel_reset(panel);

        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, DISPLAY_INVERT_COLOR);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);

        display_ = new OttoEmojiDisplay(panel_io, panel, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                        DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X,
                                        DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
    }

    void InitializeOttoController() { ::InitializeOttoController(hw_config_); }

    // Optional: shake detection via an MPU6050. Guarded like every other
    // optional capability on this board (camera, battery) - no MPU6050 wired
    // up is a normal, supported configuration, not an error.
    void InitializeMpu6050() {
        if (hw_config_.i2c_sda_pin == GPIO_NUM_NC || hw_config_.i2c_scl_pin == GPIO_NUM_NC) {
            ESP_LOGI(TAG, "MPU6050 I2C pins not configured, skipping shake detection");
            return;
        }

        i2c_master_bus_config_t bus_cfg = {
            .i2c_port = I2C_NUM_1,  // distinct from i2c_bus_'s I2C_NUM_0 above
            .sda_io_num = hw_config_.i2c_sda_pin,
            .scl_io_num = hw_config_.i2c_scl_pin,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags =
                {
                    .enable_internal_pullup = 1,
                },
        };
        if (i2c_new_master_bus(&bus_cfg, &mpu_i2c_bus_) != ESP_OK) {
            ESP_LOGW(TAG, "Failed to create MPU6050 I2C bus");
            mpu_i2c_bus_ = nullptr;
            return;
        }

        // Probe manually first - I2cDevice's WriteReg()/ReadRegs() use
        // ESP_ERROR_CHECK, which would panic the whole firmware if nothing
        // answers. No MPU6050 present is expected/normal, not fatal.
        i2c_device_config_t probe_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = Mpu6050::kDefaultAddress,
            .scl_speed_hz = 100000,
        };
        i2c_master_dev_handle_t probe_handle;
        if (i2c_master_bus_add_device(mpu_i2c_bus_, &probe_cfg, &probe_handle) != ESP_OK) {
            ESP_LOGW(TAG, "MPU6050 probe device add failed");
            i2c_del_master_bus(mpu_i2c_bus_);
            mpu_i2c_bus_ = nullptr;
            return;
        }

        uint8_t who_am_i_reg = Mpu6050::kWhoAmIReg;
        uint8_t who_am_i = 0;
        esp_err_t probe_ret =
            i2c_master_transmit_receive(probe_handle, &who_am_i_reg, 1, &who_am_i, 1, 100);
        i2c_master_bus_rm_device(probe_handle);

        if (probe_ret != ESP_OK || who_am_i != Mpu6050::kWhoAmIValue) {
            ESP_LOGW(TAG, "No MPU6050 detected (ret=%d, who_am_i=0x%02X), shake detection disabled",
                     probe_ret, who_am_i);
            i2c_del_master_bus(mpu_i2c_bus_);
            mpu_i2c_bus_ = nullptr;
            return;
        }

        mpu6050_ = new Mpu6050(mpu_i2c_bus_);
        ESP_LOGI(TAG, "MPU6050 detected, starting shake detection");
        xTaskCreate(OttoShakeDetectionTask, "shake_detect", 3072, mpu6050_, 3, nullptr);
    }

public:
    const HardwareConfig& GetHardwareConfig() const { return hw_config_; }

    OttoCameraType GetCameraType() const { return camera_type_; }

private:
    void InitializeWebSocketControlServer() {
        ws_control_server_ = new WebSocketControlServer();
        if (!ws_control_server_->Start(8080)) {
            delete ws_control_server_;
            ws_control_server_ = nullptr;
            return;
        }
        // 将 MCP 响应同时广播回连接到 8080 端口的 WebSocket 客户端
        Application::GetInstance().RegisterMcpBroadcastCallback([this](const std::string& payload) {
            if (ws_control_server_) {
                ws_control_server_->BroadcastMessage(payload);
            }
        });
    }

    void StartNetwork() override {
        WifiBoard::StartNetwork();
        vTaskDelay(pdMS_TO_TICKS(1000));

        InitializeWebSocketControlServer();
    }

    bool InitializeCamera() {
        if (!has_camera_ || i2c_bus_ == nullptr) {
            return false;
        }

        try {
            // 释放检测阶段占用的 I2C 资源，避免与 esp_camera 初始化冲突。
            i2c_del_master_bus(i2c_bus_);
            i2c_bus_ = nullptr;
            // 停止检测阶段输出的 XCLK，交由 esp_camera 自行接管。
            ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL, 0);

            camera_config_t config = {};
            config.ledc_channel = LEDC_CHANNEL_0;
            config.ledc_timer = LEDC_TIMER_0;
            config.pin_d0 = CAMERA_D0;
            config.pin_d1 = CAMERA_D1;
            config.pin_d2 = CAMERA_D2;
            config.pin_d3 = CAMERA_D3;
            config.pin_d4 = CAMERA_D4;
            config.pin_d5 = CAMERA_D5;
            config.pin_d6 = CAMERA_D6;
            config.pin_d7 = CAMERA_D7;
            config.pin_xclk = CAMERA_XCLK;
            config.pin_pclk = CAMERA_PCLK;
            config.pin_vsync = CAMERA_VSYNC;
            config.pin_href = CAMERA_HSYNC;
            config.pin_sccb_sda = CAMERA_VERSION_CONFIG.i2c_sda_pin;
            config.pin_sccb_scl = CAMERA_VERSION_CONFIG.i2c_scl_pin;
            config.sccb_i2c_port = 0;
            config.pin_pwdn = CAMERA_PWDN;
            config.pin_reset = CAMERA_RESET;
            config.xclk_freq_hz = CAMERA_XCLK_FREQ;
            config.pixel_format = PIXFORMAT_RGB565;
            config.frame_size = FRAMESIZE_240X240;
            config.jpeg_quality = 12;
            config.fb_count = 1;
            config.fb_location = CAMERA_FB_IN_PSRAM;
            config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;

            camera_ = new Esp32Camera(config);
            return true;
        } catch (...) {
            camera_ = nullptr;
            return false;
        }
    }

    void InitializeAudioCodec() {
        if (hw_config_.audio_use_simplex) {
            audio_codec_ = new NoAudioCodecSimplex(
                hw_config_.audio_input_sample_rate, hw_config_.audio_output_sample_rate,
                hw_config_.audio_i2s_spk_gpio_bclk, hw_config_.audio_i2s_spk_gpio_lrck,
                hw_config_.audio_i2s_spk_gpio_dout, hw_config_.audio_i2s_mic_gpio_sck,
                hw_config_.audio_i2s_mic_gpio_ws, hw_config_.audio_i2s_mic_gpio_din);
        } else {
            audio_codec_ = new NoAudioCodecDuplex(
                hw_config_.audio_input_sample_rate, hw_config_.audio_output_sample_rate,
                hw_config_.audio_i2s_gpio_bclk, hw_config_.audio_i2s_gpio_ws,
                hw_config_.audio_i2s_gpio_dout, hw_config_.audio_i2s_gpio_din);
        }
    }

public:
    OttoRobot()
        : boot_button_(BOOT_BUTTON_GPIO),
          audio_codec_(nullptr),
          i2c_bus_(nullptr),
          camera_(nullptr),
          has_camera_(false),
          camera_type_(OTTO_CAMERA_NONE),
          mpu_i2c_bus_(nullptr),
          mpu6050_(nullptr) {
#if OTTO_HARDWARE_VERSION == OTTO_VERSION_AUTO
        // 自动检测硬件版本（同时检测摄像头类型）
        has_camera_ = DetectHardwareVersion();
        ESP_LOGI(TAG, "自动检测硬件版本: %s", has_camera_ ? "摄像头版" : "无摄像头版");
#elif OTTO_HARDWARE_VERSION == OTTO_VERSION_CAMERA
        // 强制使用摄像头版本，但仍检测具体摄像头类型
        has_camera_ = DetectHardwareVersion();
        if (!has_camera_) {
            // 检测失败时仍使用摄像头配置，但不知道具体类型
            has_camera_ = true;
            camera_type_ = OTTO_CAMERA_UNKNOWN;
            ESP_LOGW(TAG, "强制使用摄像头版本配置，但未能检测到摄像头类型");
            // 初始化 I2C 总线用于摄像头
            i2c_master_bus_config_t i2c_bus_cfg = {
                .i2c_port = I2C_NUM_0,
                .sda_io_num = CAMERA_VERSION_CONFIG.i2c_sda_pin,
                .scl_io_num = CAMERA_VERSION_CONFIG.i2c_scl_pin,
                .clk_source = I2C_CLK_SRC_DEFAULT,
                .glitch_ignore_cnt = 7,
                .intr_priority = 0,
                .trans_queue_depth = 0,
                .flags =
                    {
                        .enable_internal_pullup = 1,
                    },
            };
            i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_);
        } else {
            ESP_LOGI(TAG, "强制使用摄像头版本配置");
        }
#elif OTTO_HARDWARE_VERSION == OTTO_VERSION_NO_CAMERA
        // 强制使用无摄像头版本
        has_camera_ = false;
        camera_type_ = OTTO_CAMERA_NONE;
        ESP_LOGI(TAG, "强制使用无摄像头版本配置");
#else
#error \
    "OTTO_HARDWARE_VERSION 设置无效，请使用 OTTO_VERSION_AUTO, OTTO_VERSION_CAMERA 或 OTTO_VERSION_NO_CAMERA"
#endif

        if (has_camera_)
            hw_config_ = CAMERA_VERSION_CONFIG;
        else
            hw_config_ = NON_CAMERA_VERSION_CONFIG;

        InitializeSpi();
        InitializeLcdDisplay();
        InitializeButtons();
        InitializePowerManager();
        InitializeAudioCodec();

        if (has_camera_) {
            if (!InitializeCamera()) {
                has_camera_ = false;
            }
        }

        InitializeOttoController();
        InitializeMpu6050();
        ws_control_server_ = nullptr;
        GetBacklight()->RestoreBrightness();
    }

    virtual AudioCodec* GetAudioCodec() override { return audio_codec_; }

    virtual Display* GetDisplay() override { return display_; }

    virtual Backlight* GetBacklight() override {
        static PwmBacklight* backlight = nullptr;
        if (backlight == nullptr) {
            backlight =
                new PwmBacklight(hw_config_.display_backlight_pin, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        }
        return backlight;
    }

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

    virtual Camera* GetCamera() override { return has_camera_ ? camera_ : nullptr; }
};

DECLARE_BOARD(OttoRobot);
