#include "emote_display.h"

// Standard C++ headers
#include <cstring>
#include <memory>
#include <unordered_map>
#include <tuple>
#include <algorithm>
#include <cinttypes>

// Standard C headers
#include <sys/time.h>
#include <time.h>

// ESP-IDF headers
#include <esp_log.h>
#include <esp_lcd_panel_io.h>
#include <esp_timer.h>
#include <lvgl.h>

// FreeRTOS headers
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// Project headers
#include "assets/lang_config.h"
#include "assets.h"
#include "board.h"
#include "gfx.h"
#include "expression_emote.h"

// GFX widget headers for direct GIF animation control
#include "widget/gfx_anim.h"
#include "core/gfx_obj.h"


namespace emote {

// ============================================================================
// Constants and Type Definitions
// ============================================================================

static const char* TAG = "EmoteDisplay";

// ============================================================================
// Helper Functions
// ============================================================================

static bool OnFlushIoReady(const esp_lcd_panel_io_handle_t panel_io,
    esp_lcd_panel_io_event_data_t* const edata, void* user_ctx)
{
    emote_handle_t handle = static_cast<emote_handle_t>(user_ctx);
    if (handle) {
        emote_notify_flush_finished(handle);
    }
    return true;
}

// Flush callback for emote — draws to LCD panel
static void OnFlushCallback(int x_start, int y_start, int x_end, int y_end,
                            const void* data, emote_handle_t handle)
{
    esp_lcd_panel_handle_t panel = (esp_lcd_panel_handle_t)emote_get_user_data(handle);
    if (panel != nullptr) {
        esp_lcd_panel_draw_bitmap(panel, x_start, y_start, x_end, y_end, data);
    }
}

// ============================================================================
// Emote Engine Initialization
// ============================================================================

static emote_handle_t InitializeEmote(const esp_lcd_panel_handle_t panel,
                                      const int width, const int height)
{
    if (!panel) {
        ESP_LOGE(TAG, "Invalid panel");
        return nullptr;
    }

    emote_config_t emote_cfg = {
        .flags = {
            .swap = true,
            .double_buffer = true,
            .buff_dma = false,
        },
        .gfx_emote = {
            .h_res = width,
            .v_res = height,
            .fps = 30,
        },
        .buffers = {
            .buf_pixels = static_cast<size_t>(width * 16),
        },
        .task = {
            .task_priority = 5,
            .task_stack = 6 * 1024,
            .task_affinity = 0,
            .task_stack_in_ext = false,
        },
        .flush_cb = OnFlushCallback,
        .user_data = (void*)panel,
    };

    emote_handle_t emote_handle = emote_init(&emote_cfg);
    if (!emote_handle) {
        ESP_LOGE(TAG, "Failed to initialize emote");
        return nullptr;
    }

    return emote_handle;
}

// ============================================================================
// EmoteDisplay Class Implementation
// ============================================================================

EmoteDisplay::EmoteDisplay(const esp_lcd_panel_handle_t panel,
                           const esp_lcd_panel_io_handle_t panel_io,
                           const int width, const int height)
{
    emote_handle_ = InitializeEmote(panel, width, height);

    const esp_lcd_panel_io_callbacks_t cbs = {
        .on_color_trans_done = OnFlushIoReady,
    };
    esp_lcd_panel_io_register_event_callbacks(panel_io, &cbs, emote_handle_);
}

EmoteDisplay::~EmoteDisplay()
{
    if (emote_handle_) {
        emote_deinit(emote_handle_);
        emote_handle_ = nullptr;
    }
}

// ============================================================================
// 核心：混合模式 SetEmotion
// 优先使用本地加载的 GIF 数据（AddEmojiData 注册的）
// 否则回退到 expression_emote 的分区资产
// ============================================================================

void EmoteDisplay::SetEmotion(const char* const emotion)
{
    if (!emotion || !emote_handle_) {
        return;
    }

    ESP_LOGI(TAG, "SetEmotion: %s", emotion);

    // 1. 检查本地 GIF 数据
    auto it = emoji_data_map_.find(emotion);
    if (it != emoji_data_map_.end()) {
        SetEmotionGfx(it->second);
        return;
    }

    // 2. 回退到 expression_emote 分区资产
    emote_set_anim_emoji(emote_handle_, emotion);
}

// ============================================================================
// 通过 GFX API 直接设置表情动画（用于本地加载的 GIF）
// ============================================================================

void EmoteDisplay::SetEmotionGfx(const AssetData& emoji)
{
    if (!emoji.data || emoji.size == 0) {
        ESP_LOGW(TAG, "SetEmotionGfx: empty data");
        return;
    }

    emote_lock(emote_handle_);

    // 获取或创建 eye_anim 对象
    gfx_obj_t* eye = emote_get_obj_by_name(emote_handle_, EMT_DEF_ELEM_EYE_ANIM);
    if (!eye) {
        // 默认对象不可用，尝试创建一个
        eye = emote_create_obj_by_type(emote_handle_, EMOTE_OBJ_TYPE_ANIM, EMT_DEF_ELEM_EYE_ANIM);
    }
    if (!eye) {
        ESP_LOGW(TAG, "SetEmotionGfx: cannot get/create eye_anim object");
        emote_unlock(emote_handle_);
        return;
    }

    gfx_anim_set_src(eye, emoji.data, emoji.size);

    int fps = emoji.fps > 0 ? emoji.fps : 20;
    gfx_anim_set_segment(eye, 0, 0xFFFF, fps, emoji.loop);
    gfx_obj_set_visible(eye, true);
    gfx_anim_start(eye);

    emote_unlock(emote_handle_);
}

// ============================================================================
// 扩展：直接从内存添加表情数据（兼容 left/ 目录 GIF 加载）
// ============================================================================

void EmoteDisplay::AddEmojiData(const std::string& name, const void* const data,
                                const size_t size, uint8_t fps, bool loop, bool lack)
{
    emoji_data_map_[name] = AssetData(data, size, fps, loop, lack);
    ESP_LOGD(TAG, "Added emoji data: %s, size: %d, fps: %d, loop: %s",
             name.c_str(), size, fps, loop ? "true" : "false");

    // 如果第一个加载的是 happy，立即显示
    if (name == "happy") {
        SetEmotionGfx(emoji_data_map_["happy"]);
    }
}

AssetData EmoteDisplay::GetEmojiData(const std::string& name) const
{
    auto it = emoji_data_map_.find(name);
    if (it != emoji_data_map_.end()) {
        return it->second;
    }
    return AssetData();
}

// ============================================================================
// expression_emote 原生接口
// ============================================================================

void EmoteDisplay::SetChatMessage(const char* const role, const char* const content)
{
    ESP_LOGI(TAG, "SetChatMessage: %s, %s", role, content);
    if (emote_handle_ && content && strlen(content) > 0) {
        emote_set_event_msg(emote_handle_, EMOTE_MGR_EVT_SPEAK, content);
    }
}

void EmoteDisplay::SetStatus(const char* const status)
{
    if (!status || !emote_handle_) {
        return;
    }

    ESP_LOGI(TAG, "SetStatus: %s", status);

    if (strcmp(status, Lang::Strings::LISTENING) == 0) {
        emote_set_event_msg(emote_handle_, EMOTE_MGR_EVT_LISTEN, status);
    } else if (strcmp(status, Lang::Strings::STANDBY) == 0) {
        emote_set_event_msg(emote_handle_, EMOTE_MGR_EVT_IDLE, status);
    } else if (strcmp(status, Lang::Strings::SPEAKING) == 0) {
        emote_set_event_msg(emote_handle_, EMOTE_MGR_EVT_SPEAK, status);
    } else {
        emote_set_event_msg(emote_handle_, EMOTE_MGR_EVT_SYS, status);
    }
}

void EmoteDisplay::ShowNotification(const char* notification, int duration_ms)
{
    if (!notification || !emote_handle_) {
        return;
    }
    ESP_LOGI(TAG, "ShowNotification: %s", notification);
    emote_set_event_msg(emote_handle_, EMOTE_MGR_EVT_SYS, notification);
}

void EmoteDisplay::UpdateStatusBar(bool update_all)
{
    // expression_emote 内部管理状态栏
}

void EmoteDisplay::SetPowerSaveMode(bool on)
{
    ESP_LOGI(TAG, "SetPowerSaveMode: %s", on ? "ON" : "OFF");
}

void EmoteDisplay::SetPreviewImage(const void* image)
{
    // Not implemented for emote display
}

void EmoteDisplay::SetTheme(Theme* const theme)
{
    ESP_LOGI(TAG, "SetTheme: %p", theme);
}

bool EmoteDisplay::StopAnimDialog()
{
    if (emote_handle_) {
        return emote_stop_anim_dialog(emote_handle_) == ESP_OK;
    }
    return false;
}

bool EmoteDisplay::InsertAnimDialog(const char* emoji_name, uint32_t duration_ms)
{
    if (emote_handle_ && emoji_name) {
        return emote_insert_anim_dialog(emote_handle_, emoji_name, duration_ms) == ESP_OK;
    }
    return false;
}

void EmoteDisplay::RefreshAll()
{
    if (emote_handle_) {
        emote_notify_all_refresh(emote_handle_);
    }
}

// ============================================================================
// Lock / Unlock
// ============================================================================

bool EmoteDisplay::Lock(int timeout_ms)
{
    if (emote_handle_) {
        emote_lock(emote_handle_);
        return true;
    }
    return false;
}

void EmoteDisplay::Unlock()
{
    if (emote_handle_) {
        emote_unlock(emote_handle_);
    }
}

// ============================================================================
// 兼容旧 Assets 接口
// ============================================================================

void EmoteDisplay::AddIconData(const std::string& name, const void* data, size_t size)
{
    icon_data_map_[name] = AssetData(data, size);
    ESP_LOGD(TAG, "Added icon data: %s, size: %d", name.c_str(), size);
}

AssetData EmoteDisplay::GetIconData(const std::string& name) const
{
    auto it = icon_data_map_.find(name);
    if (it != icon_data_map_.end()) {
        return it->second;
    }
    return AssetData();
}

void EmoteDisplay::AddLayoutData(const std::string& name, const std::string& align_str,
                                 int x, int y, int width, int height)
{
    // expression_emote 内部管理 layout，这里做兼容空实现
    ESP_LOGD(TAG, "AddLayoutData: %s align=%s x=%d y=%d w=%d h=%d",
             name.c_str(), align_str.c_str(), x, y, width, height);
}

void EmoteDisplay::AddTextFont(std::shared_ptr<LvglFont> text_font)
{
    text_font_ = text_font;
    ESP_LOGD(TAG, "AddTextFont: font added");
}

} // namespace emote
