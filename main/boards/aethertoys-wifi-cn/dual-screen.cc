#include "wifi_board.h"
#include "dual_network_board.h"
#include "codecs/box_audio_codec.h"
#include "display/lcd_display.h"
#include "application.h"
#include "mcp_server.h"
#include "button.h"
#include "alarm_manager.h"
#include "config.h"
#include "esp_lcd_gc9d01.h"
#include <esp_lvgl_port.h>
#include <esp_log.h>
#include <esp_random.h>
#include <driver/i2c_master.h>
#include <driver/ledc.h>
#include <driver/spi_common.h>
#include <wifi_station.h>
#include <font_emoji.h>
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include <driver/gpio.h>
#include <esp_system.h>
#include <atomic>

//电源
#include "bq27220.h"
#include <driver/temperature_sensor.h>

// 音乐播放
#include "esp32_music.h"

//RGB灯
#include "led/circular_strip.h"
#include "assets/lang_config.h"

#define TAG "AethertoysBoard"


//电量部分
static const parameter_cedv_t default_cedv = {
    .full_charge_cap = 1200,    /* 额定容量1200mAh */
    .design_cap = 1200,         /* 设计容量1200mAh */
    .reserve_cap = 60,          /* 储备容量，约5%的额定容量 */
    .near_full = 1140,          /* 接近充满阈值，95%容量 */
    .self_discharge_rate = 8,   /* 自放电率：8 × 0.0025% = 0.02%/天 */

    /* EDV参数 - 基于放电终止电压3.2V，但调整上限为4.1V */
    .EDV0 = 3200,               /* 0% SOC - 完全放空电压 */
    .EDV1 = 3300,               /* 3% SOC */
    .EDV2 = 3400,               /* 电池低电量警告电压 */

    /* 电化学参数调整 */
    .EMF = 4050,                /* 空载电压，调整为略低于4.1V充满电压 */
    .C0 = 1400,                 /* 稍小的容量相关EDV调整因子（电压范围变小） */
    .R0 = 55,                   /* 稍大的阻抗调整因子（电压窗口变小） */
    .T0 = 100,                  /* 温度阻抗变化因子 */
    .R1 = 28,                   /* 稍大的容量阻抗变化因子 */
    .TC = 125,                  /* 冷温度阻抗调整 */
    .C1 = 35,                   /* EDV0时保留容量稍大为35mAh */

    /* DOD电压曲线 - 基于3.2V-4.1V范围重新调整 */
    .DOD0 = 4100,               /* 0% DOD - 充满4.1V */
    .DOD10 = 4030,              /* 10% DOD - 4.03V */
    .DOD20 = 3970,              /* 20% DOD - 3.97V */
    .DOD30 = 3910,              /* 30% DOD - 3.91V */
    .DOD40 = 3850,              /* 40% DOD - 3.85V */
    .DOD50 = 3800,              /* 50% DOD - 3.80V */
    .DOD60 = 3750,              /* 60% DOD - 3.75V */
    .DOD70 = 3700,              /* 70% DOD - 3.70V */
    .DOD80 = 3620,              /* 80% DOD - 3.62V */
    .DOD90 = 3520,              /* 90% DOD - 3.52V */
    .DOD100 = 3200              /* 100% DOD - 放空3.2V */
};

static const gauging_config_t default_config = {
    .CCT = 1,        /* 使用充满容量百分比 */
    .CSYNC = 1,      /* 启用容量同步 */
    .EDV_CMP = 1,    /* 启用EDV补偿 */
    .SC = 1,         /* 启用短路检测 */
    .FIXED_EDV0 = 0, /* 不固定EDV0 */
    .FCC_LIM = 1,    /* 启用充满容量限制 */
    .FC_FOR_VDQ = 0, /* 不为VDQ使用充满电压 */
    .IGNORE_SD = 0,  /* 不忽略自放电 */
    .SME0 = 0        /* 禁用特定模式 */
};

temperature_sensor_handle_t temp_sensor = NULL;

namespace {

static constexpr uint8_t kIdleBacklightBrightness = 10;
static constexpr int64_t kIdleBacklightDelayUs = 20 * 1000 * 1000LL;
static constexpr ledc_timer_t kDualBacklightTimer = LEDC_TIMER_2;
static constexpr ledc_channel_t kLeftBacklightChannel = LEDC_CHANNEL_2;
static constexpr ledc_channel_t kRightBacklightChannel = LEDC_CHANNEL_3;

// 双屏镜像：单 LVGL 实例渲染，两个面板 CS 永久拉低同时接收
static esp_lcd_panel_handle_t s_mirror_panel_primary = nullptr;

static void dual_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    // 字节交换（与原始 lvgl_port_flush_callback 行为一致）
    size_t len = lv_area_get_size(area);
    lv_draw_sw_rgb565_swap(px_map, len);

    if (s_mirror_panel_primary != nullptr) {
        esp_lcd_panel_draw_bitmap(s_mirror_panel_primary,
            area->x1, area->y1, area->x2 + 1, area->y2 + 1, px_map);
    }
}

class DualPwmBacklight : public Backlight {
public:
    DualPwmBacklight(gpio_num_t left_pin, gpio_num_t right_pin, bool output_invert, uint32_t freq_hz = 25000)
        : left_pin_(left_pin), right_pin_(right_pin), output_invert_(output_invert) {
        const ledc_timer_config_t backlight_timer = {
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .duty_resolution = LEDC_TIMER_10_BIT,
            .timer_num = kDualBacklightTimer,
            .freq_hz = freq_hz,
            .clk_cfg = LEDC_AUTO_CLK,
            .deconfigure = false
        };
        ESP_ERROR_CHECK(ledc_timer_config(&backlight_timer));

        ConfigureChannel(left_pin_, kLeftBacklightChannel);
        ConfigureChannel(right_pin_, kRightBacklightChannel);
    }

    ~DualPwmBacklight() {
        if (left_pin_ != GPIO_NUM_NC) {
            ledc_stop(LEDC_LOW_SPEED_MODE, kLeftBacklightChannel, 0);
        }
        if (right_pin_ != GPIO_NUM_NC) {
            ledc_stop(LEDC_LOW_SPEED_MODE, kRightBacklightChannel, 0);
        }
    }

    void SetBrightnessImpl(uint8_t brightness) override {
        uint32_t duty_cycle = (1023 * brightness) / 100;
        // 硬件 output_invert 会反相输出，需要在软件层补偿，
        // 否则 brightness 越大实际屏幕越暗
        if (output_invert_) {
            duty_cycle = 1023 - duty_cycle;
        }
        UpdateChannel(kLeftBacklightChannel, left_pin_, duty_cycle);
        UpdateChannel(kRightBacklightChannel, right_pin_, duty_cycle);
    }

private:
    void ConfigureChannel(gpio_num_t pin, ledc_channel_t channel) {
        if (pin == GPIO_NUM_NC) {
            return;
        }

        const ledc_channel_config_t backlight_channel = {
            .gpio_num = pin,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = channel,
            .intr_type = LEDC_INTR_DISABLE,
            .timer_sel = kDualBacklightTimer,
            .duty = 0,
            .hpoint = 0,
            .sleep_mode = LEDC_SLEEP_MODE_NO_ALIVE_NO_PD,
            .flags = {
                .output_invert = output_invert_,
            }
        };
        ESP_ERROR_CHECK(ledc_channel_config(&backlight_channel));
    }

    void UpdateChannel(ledc_channel_t channel, gpio_num_t pin, uint32_t duty_cycle) {
        if (pin == GPIO_NUM_NC) {
            return;
        }

        ledc_set_duty(LEDC_LOW_SPEED_MODE, channel, duty_cycle);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, channel);
    }

    gpio_num_t left_pin_;
    gpio_num_t right_pin_;
    bool output_invert_;
};

} // namespace


class CyberAiDualScreen : public DualNetworkBoard {
private:
    static constexpr uint16_t kBootButtonLongPressMs = 3000;

    i2c_master_bus_handle_t codec_i2c_bus_;
    Button boot_button_;
    Display* display_;
    Display* dual_display_[2];//双屏异显
    adc_oneshot_unit_handle_t adc1_handle;
    adc_cali_handle_t adc1_cali_handle;
    bool do_calibration = false;

    // 是否已对关机 GPIO 进行初始化（懒初始化）
    std::atomic_bool shutdown_gpio_initialized_{false};

    DualPwmBacklight* backlight_ = nullptr;
    esp_timer_handle_t idle_backlight_timer_ = nullptr;
    bool idle_backlight_dimmed_ = false;
    int idle_backlight_listener_id_ = -1;

    bq27220_handle_t bq27220 = NULL;
    Music* music_ = nullptr;

    void StopIdleBacklightTimer() {
        if (idle_backlight_timer_ == nullptr) {
            return;
        }

        esp_err_t err = esp_timer_stop(idle_backlight_timer_);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_ERROR_CHECK(err);
        }
    }

    void DimBacklightForIdle() {
        if (backlight_ == nullptr) {
            return;
        }

        if (Application::GetInstance().GetDeviceState() != kDeviceStateIdle) {
            return;
        }

        idle_backlight_dimmed_ = true;
        backlight_->SetBrightness(kIdleBacklightBrightness);
        ESP_LOGI(TAG, "Dim backlight after idle timeout");
    }

    void ScheduleIdleBacklightDim() {
        if (backlight_ == nullptr || idle_backlight_timer_ == nullptr) {
            return;
        }

        StopIdleBacklightTimer();
        idle_backlight_dimmed_ = false;
        ESP_ERROR_CHECK(esp_timer_start_once(idle_backlight_timer_, kIdleBacklightDelayUs));
    }

    void RestoreBacklightFromSettings() {
        if (backlight_ == nullptr) {
            return;
        }

        StopIdleBacklightTimer();
        idle_backlight_dimmed_ = false;
        backlight_->RestoreBrightness();
    }

    void InitializeBacklight() {
        if (DISPLAY_LEFT_BACKLIGHT_PIN == GPIO_NUM_NC && DISPLAY_RIGHT_BACKLIGHT_PIN == GPIO_NUM_NC) {
            return;
        }

        backlight_ = new DualPwmBacklight(
            DISPLAY_LEFT_BACKLIGHT_PIN,
            DISPLAY_RIGHT_BACKLIGHT_PIN,
            DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        backlight_->RestoreBrightness();

        const esp_timer_create_args_t timer_args = {
            .callback = [](void* arg) {
                auto self = static_cast<CyberAiDualScreen*>(arg);
                self->DimBacklightForIdle();
            },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "idle_backlight",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &idle_backlight_timer_));

        // 注册状态变化监听：进入 idle 时启动空闲调暗定时器，离开 idle 时恢复背光
        idle_backlight_listener_id_ = Application::GetInstance().AddStateChangeListener(
            [this](DeviceState old_state, DeviceState new_state) {
                if (new_state == kDeviceStateIdle) {
                    // 回到空闲状态，重新调度调暗定时器
                    ScheduleIdleBacklightDim();
                } else if (old_state == kDeviceStateIdle) {
                    // 离开空闲状态（唤醒、开始对话等），恢复背光亮度
                    if (idle_backlight_dimmed_) {
                        RestoreBacklightFromSettings();
                    } else {
                        // 定时器还没触发，只是取消定时器即可
                        StopIdleBacklightTimer();
                    }
                }
            });

        // 如果当前已经处于空闲状态，立即启动定时器
        if (Application::GetInstance().GetDeviceState() == kDeviceStateIdle) {
            ScheduleIdleBacklightDim();
        }
    }

    bool CanSwitchNetworkInCurrentState() const {
        auto state = Application::GetInstance().GetDeviceState();
         return state != kDeviceStateUnknown &&
             state != kDeviceStateUpgrading &&
             state != kDeviceStateActivating &&
             state != kDeviceStateAudioTesting &&
             state != kDeviceStateFatalError;
    }

    const char* GetNetworkTypeName(NetworkType type) const {
        return type == NetworkType::ML307 ? "4g" : "wifi";
    }

    bool RequestNetworkSwitch(NetworkType target_type, const char* source) {
        auto& app = Application::GetInstance();
        auto state = app.GetDeviceState();
        if (!CanSwitchNetworkInCurrentState()) {
            ESP_LOGW(TAG, "Ignore network switch from %s, busy state=%d", source, state);
            if (display_ != nullptr) {
                display_->ShowNotification(Lang::Strings::PLEASE_WAIT);
            }
            return false;
        }

        if (GetNetworkType() == target_type) {
            ESP_LOGI(TAG, "Network already in %s, source=%s", GetNetworkTypeName(target_type), source);
            return true;
        }

        ESP_LOGI(TAG, "Switch network from %s to %s, source=%s",
            GetNetworkTypeName(GetNetworkType()), GetNetworkTypeName(target_type), source);
        SwitchNetworkType();
        return true;
    }

    void RegisterLocalMcpTools() {
        auto& mcp_server = McpServer::GetInstance();
        auto network_switch_callback = [this](const PropertyList& properties) -> ReturnValue {
            std::string target = properties["target"].value<std::string>();
            NetworkType desired_type = GetNetworkType();

            if (target == "toggle") {
                desired_type = GetNetworkType() == NetworkType::WIFI ? NetworkType::ML307 : NetworkType::WIFI;
            } else if (target == "wifi") {
                desired_type = NetworkType::WIFI;
            } else if (target == "4g" || target == "ml307") {
                desired_type = NetworkType::ML307;
            } else {
                return std::string("{\"ok\":false,\"message\":\"invalid target, use wifi, 4g, ml307 or toggle\"}");
            }

            if (!CanSwitchNetworkInCurrentState()) {
                return std::string("{\"ok\":false,\"message\":\"device cannot switch network in the current state\"}");
            }

            if (desired_type == GetNetworkType()) {
                return std::string("{\"ok\":true,\"message\":\"already on target network\",\"network\":\"") +
                    GetNetworkTypeName(desired_type) + "\"}";
            }

            RequestNetworkSwitch(desired_type, "mcp");
            return std::string("{\"ok\":true,\"message\":\"switching network\",\"target\":\"") +
                GetNetworkTypeName(desired_type) + "\"}";
        };

        mcp_server.AddTool("self.network.switch_net_mode",
            "Switch active network on Aethertoys-wifi-CN. Use target=wifi, 4g, ml307 or toggle. The device will reboot after switching.",
            PropertyList({
                Property("target", kPropertyTypeString, std::string("toggle"))
            }),
            network_switch_callback);

        mcp_server.AddUserOnlyTool("self.network.switch",
            "Switch active network on Aethertoys-wifi-CN. Use target=wifi, 4g, ml307 or toggle. The device will reboot after switching.",
            PropertyList({
                Property("target", kPropertyTypeString, std::string("toggle"))
            }),
            network_switch_callback);

        // 添加关机 MCP 工具
        mcp_server.AddTool("self.power.shutdown", "关机",
            PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                ESP_LOGI(TAG, "MCP tool called: self.power.shutdown (关机)");
                if (!shutdown_gpio_initialized_.load(std::memory_order_acquire)) {
                    gpio_config_t cfg = {};
                    cfg.pin_bit_mask = (1ULL << (uint64_t)GPIO_NUM_21);
                    cfg.mode = GPIO_MODE_OUTPUT;
                    cfg.pull_up_en = GPIO_PULLUP_DISABLE;
                    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
                    cfg.intr_type = GPIO_INTR_DISABLE;
                    esp_err_t err = gpio_config(&cfg);
                    if (err != ESP_OK) {
                        ESP_LOGE(TAG, "Failed to config shutdown GPIO: %s", esp_err_to_name(err));
                        return std::string("{\"ok\":false,\"message\":\"gpio_config failed\"}");
                    }
                    shutdown_gpio_initialized_.store(true, std::memory_order_release);
                }
                ESP_ERROR_CHECK(gpio_set_level(GPIO_NUM_21, 0));
                return true;
            });
    }

    void RegisterMcpTools() {
        auto& mcp_server = McpServer::GetInstance();

        // 背光亮度控制
        mcp_server.AddTool("self.brightness.set",
            "Set backlight brightness (0-100)",
            PropertyList({
                Property("value", kPropertyTypeInteger, 0)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                int brightness = properties["value"].value<int>();
                if (brightness < 0) brightness = 0;
                if (brightness > 100) brightness = 100;
                if (backlight_ != nullptr) {
                    backlight_->SetBrightness(brightness, true);
                    ESP_LOGI(TAG, "Backlight set to %d%%", brightness);
                }
                return true;
            });

        // 表情控制
        mcp_server.AddTool("self.emotion.set",
            "设置屏幕表情。支持: neutral, happy, laughing, sad, angry, crying, loving, surprised, shocked, thinking, winking, confused, sleepy, delicious, kissy, cool, silly",
            PropertyList({
                Property("emotion", kPropertyTypeString, std::string("neutral"))
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                std::string emotion = properties["emotion"].value<std::string>();
                if (display_ != nullptr) {
                    display_->SetEmotion(emotion.c_str());
                    ESP_LOGI(TAG, "Emotion set to: %s", emotion.c_str());
                }
                return std::string("{\"ok\":true,\"emotion\":\"") + emotion + "\"}";
            });

        mcp_server.AddTool("self.emotion.random",
            "随机切换屏幕表情",
            PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                static const char* emotions[] = {
                    "neutral", "happy", "laughing", "sad", "angry", "crying",
                    "loving", "surprised", "shocked", "thinking", "winking",
                    "confused", "sleepy", "delicious", "kissy", "cool", "silly"
                };
                static const int count = sizeof(emotions) / sizeof(emotions[0]);
                const char* emotion = emotions[esp_random() % count];
                if (display_ != nullptr) {
                    display_->SetEmotion(emotion);
                    ESP_LOGI(TAG, "Random emotion: %s", emotion);
                }
                return std::string("{\"ok\":true,\"emotion\":\"") + emotion + "\"}";
            });
    }

    void RegisterMcpMusicTools() {
        auto& mcp_server = McpServer::GetInstance();

        mcp_server.AddTool("self.music.play",
            "播放音乐。先断开语音服务连接，播放完毕后自动重连",
            PropertyList({
                Property("song_name", kPropertyTypeString, std::string("")),
                Property("artist_name", kPropertyTypeString, std::string("")),
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                std::string song = properties["song_name"].value<std::string>();
                std::string artist = properties["artist_name"].value<std::string>();
                if (song.empty()) {
                    return std::string("{\"ok\":false,\"message\":\"请提供歌曲名\"}");
                }

                Application::GetInstance().Schedule([this, song, artist]() {
                    auto& app = Application::GetInstance();
                    app.SuspendAudioChannel();  // 只关音频通道，保留 MQTT 连接
                    vTaskDelay(pdMS_TO_TICKS(200));
                    if (!music_->Start(song, artist)) {
                        app.Alert("Error", "播放失败", "neutral", Lang::Sounds::OGG_EXCLAMATION);
                        app.ResumeAudioChannel("音乐播放失败");
                    }
                });
                return std::string("{\"ok\":true,\"message\":\"正在播放 " + song + "\"}");
            });

        mcp_server.AddTool("self.music.stop",
            "停止播放音乐",
            PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                if (music_ && music_->IsPlaying()) {
                    music_->Stop();
                    return std::string("{\"ok\":true,\"message\":\"已停止播放\"}");
                }
                return std::string("{\"ok\":false,\"message\":\"当前未在播放\"}");
            });

        mcp_server.AddTool("self.music.search",
            "搜索歌曲（不播放），返回搜索结果JSON",
            PropertyList({
                Property("song_name", kPropertyTypeString, std::string("")),
                Property("artist_name", kPropertyTypeString, std::string("")),
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                std::string song = properties["song_name"].value<std::string>();
                std::string artist = properties["artist_name"].value<std::string>();
                if (song.empty() && artist.empty()) {
                    return std::string("{\"ok\":false,\"message\":\"请提供歌曲名或艺术家名\"}");
                }
                std::string result = music_->Search(song, artist);
                return result;
            });
    }

    void InitializeI2c() {
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &codec_i2c_bus_));

        vTaskDelay(pdMS_TO_TICKS(50));

        temperature_sensor_config_t temp_sensor_config = TEMPERATURE_SENSOR_CONFIG_DEFAULT(10, 50);
        ESP_ERROR_CHECK(temperature_sensor_install(&temp_sensor_config, &temp_sensor));
        ESP_ERROR_CHECK(temperature_sensor_enable(temp_sensor));
        ESP_LOGI(TAG, "I2C initialized");
    }

    void InitializeADC() {
        adc_oneshot_unit_init_cfg_t init_config1 = {
            .unit_id = ADC_UNIT_1
        };
        ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config1, &adc1_handle));

        adc_oneshot_chan_cfg_t chan_config = {
            .atten = ADC_ATTEN,
            .bitwidth = ADC_WIDTH,
        };
        ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, VBAT_ADC_CHANNEL, &chan_config));

        adc_cali_handle_t handle = NULL;
        esp_err_t ret = ESP_FAIL;

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
        adc_cali_curve_fitting_config_t cali_config = {
            .unit_id = ADC_UNIT_1,
            .atten = ADC_ATTEN,
            .bitwidth = ADC_WIDTH,
        };
        ret = adc_cali_create_scheme_curve_fitting(&cali_config, &handle);
        if (ret == ESP_OK) {
            do_calibration = true;
            adc1_cali_handle = handle;
            ESP_LOGI(TAG, "ADC Curve Fitting calibration succeeded");
        }
#endif
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_SPI_MOSI_PIN;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = DISPLAY_SPI_SCLK_PIN;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            // 闹钟响铃中 — 单击停止（最高优先级）
            if (AlarmManager::GetInstance().IsRinging()) {
                AlarmManager::GetInstance().StopRinging();
                return;
            }

            auto& app = Application::GetInstance();
            ESP_LOGI(TAG, "Boot button single click, state=%d", app.GetDeviceState());

            // 正在播放音乐时，单击停止
            if (music_ && music_->IsPlaying()) {
                music_->Stop();
                return;
            }

            if (app.GetDeviceState() == kDeviceStateStarting) {
                    auto& wifi_board = static_cast<WifiBoard&>(GetCurrentBoard());
                    wifi_board.EnterWifiConfigMode();
            }
            // 恢复背光
            RestoreBacklightFromSettings();
            app.WakeWordInvoke(Lang::Strings::HELLO_MY_FRIEND);
        });
        boot_button_.OnMultipleClick([this]() {
            ESP_LOGI(TAG, "Boot button 4-click, request network switch");
            auto target_type = GetNetworkType() == NetworkType::WIFI ? NetworkType::ML307 : NetworkType::WIFI;
            RequestNetworkSwitch(target_type, "button_4click");
        }, 4);
        boot_button_.OnDoubleClick([this]() {
            auto& app = Application::GetInstance();
            ESP_LOGI(TAG, "Boot button double click, state=%d", app.GetDeviceState());

            // 配网模式下双击重启设备
            if (app.GetDeviceState() == kDeviceStateWifiConfiguring) {
                ESP_LOGI(TAG, "Double click in wifi config mode, rebooting...");
                esp_restart();
                return;
            }

            if (GetNetworkType() == NetworkType::WIFI) {
                if (app.GetDeviceState() == kDeviceStateIdle || app.GetDeviceState() == kDeviceStateStarting || app.GetDeviceState() == kDeviceStateActivating) {
                    auto& wifi_board = static_cast<WifiBoard&>(GetCurrentBoard());
                    wifi_board.EnterWifiConfigMode();
                }
            }
        });
    }

    void InitializeDisplay() {
        gpio_config_t charge_detect_config = {
            .pin_bit_mask = (1ULL << DISPLAY_RIGHT_SPI_CS_PIN),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE
        };
        ESP_ERROR_CHECK(gpio_config(&charge_detect_config));
        gpio_set_level(DISPLAY_RIGHT_SPI_CS_PIN , 0);

        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        ESP_LOGI(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_LEFT_SPI_CS_PIN;
        io_config.dc_gpio_num = DISPLAY_SPI_DC_PIN;
        io_config.spi_mode = 0;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_config, &panel_io));

        ESP_LOGI(TAG, "Install GC9D01 panel driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_LEFT_SPI_RESET_PIN;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_gc9d01(panel_io, &panel_config, &panel));

        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, false));
        ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y));
        ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY));
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));
#if CONFIG_USE_EMOTE_MESSAGE_STYLE
        display_ = new emote::EmoteDisplay(panel, panel_io, DISPLAY_WIDTH, DISPLAY_HEIGHT);
#else
        display_ = new SpiLcdDisplay(panel_io, panel,
                        DISPLAY_WIDTH, DISPLAY_HEIGHT,
                        DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
                        DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
#endif
    }

    void IntializeDualScreen()
    {
            // Panel left screen
        esp_lcd_panel_io_handle_t io_handle1 = NULL;
        esp_lcd_panel_io_spi_config_t io_config1 = GC9D01_PANEL_IO_SPI_CONFIG(DISPLAY_LEFT_SPI_CS_PIN, DISPLAY_SPI_DC_PIN, NULL, NULL);
        io_config1.spi_mode = 0;
        io_config1.pclk_hz = LCD_SPI_PCLK_HZ;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_config1, &io_handle1));

        esp_lcd_panel_handle_t panel_handle1 = NULL;
        esp_lcd_panel_dev_config_t panel_config1 = {};
        panel_config1.reset_gpio_num = DISPLAY_LEFT_SPI_RESET_PIN; // 左屏独立 RST
        panel_config1.rgb_endian = LCD_RGB_ENDIAN_RGB;
        panel_config1.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config1.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_gc9d01(io_handle1, &panel_config1, &panel_handle1));

        // Panel right screen
        esp_lcd_panel_io_handle_t io_handle2 = NULL;
        esp_lcd_panel_io_spi_config_t io_config2 = GC9D01_PANEL_IO_SPI_CONFIG(DISPLAY_RIGHT_SPI_CS_PIN, DISPLAY_SPI_DC_PIN, NULL, NULL);
        io_config2.spi_mode = 0;
        io_config2.pclk_hz = LCD_SPI_PCLK_HZ;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_config2, &io_handle2));

        esp_lcd_panel_handle_t panel_handle2 = NULL;
        esp_lcd_panel_dev_config_t panel_config2 = {};
        panel_config2.reset_gpio_num = DISPLAY_RIGHT_SPI_RESET_PIN; // 右屏独立 RST
        panel_config2.rgb_endian = LCD_RGB_ENDIAN_RGB;
        panel_config2.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config2.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_gc9d01(io_handle2, &panel_config2, &panel_handle2));

        // Init Panel 1 (左屏: 左转90° CCW)
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle1)); // 左屏独立 RST 复位
        vTaskDelay(pdMS_TO_TICKS(20));
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle1));
        vTaskDelay(pdMS_TO_TICKS(20));
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle1, false));
        ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel_handle1, DISPLAY_LEFT_SWAP_XY));
        ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle1, DISPLAY_LEFT_MIRROR_X, DISPLAY_LEFT_MIRROR_Y));
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle1, true));

        // Init Panel 2 (右屏: 右转180°)
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle2)); // 右屏独立 RST，单独复位
        vTaskDelay(pdMS_TO_TICKS(20));
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle2));
        vTaskDelay(pdMS_TO_TICKS(20));
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle2, false));
        ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel_handle2, DISPLAY_RIGHT_SWAP_XY));
        ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle2, DISPLAY_RIGHT_MIRROR_X, DISPLAY_RIGHT_MIRROR_Y));
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle2, true));

        // LVGL 以无旋转模式渲染（逻辑正向），同一帧缓冲发给两个面板
        // 各面板硬件独立旋转（左90°CCW / 右180°），补偿物理安装角度
        dual_display_[0] = new SpiLcdDisplay(io_handle1, panel_handle1,
                        DISPLAY_WIDTH, DISPLAY_HEIGHT,
                        DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
                        false, false, false);  // LVGL 无旋转
        dual_display_[1] = dual_display_[0];

        // 重新应用硬件旋转（lvgl_port_add_disp 会用 LVGL 旋转覆盖硬件设置）
        // 注意：这是最后一次使用 io_handle2，之后右屏 CS 将永久拉低
        esp_lcd_panel_swap_xy(panel_handle1, DISPLAY_LEFT_SWAP_XY);
        esp_lcd_panel_mirror(panel_handle1, DISPLAY_LEFT_MIRROR_X, DISPLAY_LEFT_MIRROR_Y);
        esp_lcd_panel_swap_xy(panel_handle2, DISPLAY_RIGHT_SWAP_XY);
        esp_lcd_panel_mirror(panel_handle2, DISPLAY_RIGHT_MIRROR_X, DISPLAY_RIGHT_MIRROR_Y);

        // 存储面板句柄
        s_mirror_panel_primary = panel_handle1;

        // 替换 flush 回调
        lv_display_t *lv_disp = lv_display_get_next(NULL);
        if (lv_disp != nullptr) {
            lv_display_set_flush_cb(lv_disp, dual_flush_cb);
        }

        // 初始化完成后，永久拉低右屏 CS (DISPLAY_RIGHT_SPI_CS_PIN)
        // GC9D01 无 MISO 回读，两个 CS 同时低电平 → 所有 SPI 数据两个面板同时接收
        gpio_config_t right_cs_cfg = {
            .pin_bit_mask = (1ULL << DISPLAY_RIGHT_SPI_CS_PIN),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&right_cs_cfg);
        gpio_set_level(DISPLAY_RIGHT_SPI_CS_PIN, 0);  // 永久拉低

    }

    void Initialize4G(){
        gpio_config_t charge_detect_config = {
            .pin_bit_mask = (1ULL << EN_4G),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE
        };
        ESP_ERROR_CHECK(gpio_config(&charge_detect_config));
        gpio_set_level(EN_4G , 0);//4G高电平开机
        ESP_LOGI(TAG,"使能4G 14引脚电平1: %d", gpio_get_level(GPIO_NUM_14));
        vTaskDelay(pdMS_TO_TICKS(100));
        gpio_set_level(EN_4G , 1);
        ESP_LOGI(TAG,"使能4G 14引脚电平2: %d", gpio_get_level(GPIO_NUM_14));
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    //初始化bq272220芯片(电量监控芯片)
    void Initializebq27220()
    {
        if (codec_i2c_bus_ == NULL) {
            ESP_LOGE("BQ27220", "I2C bus is NULL, initialization failed");
            return;
        }

        bq27220_config_t bq27220_cfg = {
            .i2c_bus = codec_i2c_bus_,
            .cfg = &default_config,
            .cedv = &default_cedv,
        };

        bq27220 = bq27220_create(&bq27220_cfg);

        if (bq27220) {
            ESP_LOGI("BQ27220", "BQ27220 initialized successfully");
        } else {
            ESP_LOGE("BQ27220", "BQ27220 initialization failed");
        }
    }

    void InitializeRGBLight()
    {
        static CircularStrip ambient_light(RGB_GPIO, 4);  // 4个WS2812B LED
    }

public:
    CyberAiDualScreen() : DualNetworkBoard(ML307_TX_PIN, ML307_RX_PIN, GPIO_NUM_NC, (int32_t)NetworkType::WIFI),
    boot_button_(BOOT_BUTTON_GPIO, false, kBootButtonLongPressMs){
        InitializeI2c();
        Initialize4G();

        InitializeSpi();
        IntializeDualScreen();
        display_ = dual_display_[0];
        InitializeBacklight();
        InitializeButtons();
        Initializebq27220();

        // 创建音乐播放器
        music_ = CreateMusic();

        RegisterMcpTools();
        RegisterMcpMusicTools();
        RegisterLocalMcpTools();

        // 去掉顶部半透明白底
        Application::GetInstance().Schedule([this]() {
            auto* lcd = dynamic_cast<LcdDisplay*>(display_);
            if (lcd != nullptr) {
                lcd->SetTopBarTransparent();
            }
        });
    }

    virtual AudioCodec* GetAudioCodec() override
    {
        static BoxAudioCodec audio_codec(
            codec_i2c_bus_,
            AUDIO_INPUT_SAMPLE_RATE,
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK,
            AUDIO_I2S_GPIO_BCLK,
            AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT,
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN,
            AUDIO_CODEC_ES8311_ADDR,
            AUDIO_CODEC_ES7210_ADDR,
            AUDIO_INPUT_REFERENCE);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual Music* GetMusic() override {
        return music_;
    }

    virtual bool GetBatteryLevel(int &level, bool& charging, bool& discharging) override
    {
        if (bq27220 == NULL) {
            return false;
        }
        level = bq27220_get_state_of_charge(bq27220);
        battery_status_t status = {};
        bq27220_get_battery_status(bq27220, &status);

        int16_t current = bq27220_get_current(bq27220);

        // 充电状态判断
        if (status.FC) {
            charging = false;
            discharging = false;
        } else if (current > 20) {
            charging = true;
            discharging = false;
        } else if (current < -20) {
            charging = false;
            discharging = true;
        } else {
            charging = false;
            discharging = true;
        }
        return true;
    }

    virtual Backlight* GetBacklight() override {
        return backlight_;
    }
};

DECLARE_BOARD(CyberAiDualScreen);
