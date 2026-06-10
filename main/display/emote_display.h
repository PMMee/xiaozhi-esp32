#pragma once

#include "display.h"
#include "lvgl_font.h"
#include "expression_emote.h"

#include <memory>
#include <functional>
#include <map>
#include <string>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>

namespace emote {

// Simple data structure for storing raw GIF asset data
struct AssetData {
    const void* data;
    size_t size;
    union {
        uint8_t flags;  // 1 byte for all animation flags
        struct {
            uint8_t fps : 6;    // FPS (0-63) - 6 bits
            uint8_t loop : 1;   // Loop animation - 1 bit
            uint8_t lack : 1;   // Lack animation - 1 bit
        };
    };

    AssetData() : data(nullptr), size(0), flags(0) {}
    AssetData(const void* d, size_t s) : data(d), size(s), flags(0) {}
    AssetData(const void* d, size_t s, uint8_t f, bool l, bool k)
        : data(d), size(s)
    {
        fps = f > 63 ? 63 : f;
        loop = l;
        lack = k;
    }
};

class EmoteDisplay : public Display {
public:
    EmoteDisplay(esp_lcd_panel_handle_t panel, esp_lcd_panel_io_handle_t panel_io, int width, int height);
    virtual ~EmoteDisplay();

    virtual void SetEmotion(const char* emotion) override;
    virtual void SetStatus(const char* status) override;
    virtual void SetChatMessage(const char* role, const char* content) override;
    virtual void SetTheme(Theme* theme) override;
    virtual void ShowNotification(const char* notification, int duration_ms = 3000) override;
    virtual void UpdateStatusBar(bool update_all = false) override;
    virtual void SetPowerSaveMode(bool on) override;
    virtual void SetPreviewImage(const void* image);

    // expression_emote 原生接口
    bool StopAnimDialog();
    bool InsertAnimDialog(const char* emoji_name, uint32_t duration_ms);
    void RefreshAll();

    // 扩展：直接从内存添加表情数据（兼容 left/ 目录 GIF 加载）
    void AddEmojiData(const std::string &name, const void* data, size_t size, uint8_t fps = 0, bool loop = false, bool lack = false);
    AssetData GetEmojiData(const std::string &name) const;

    // 兼容旧 Assets 接口
    void AddIconData(const std::string &name, const void* data, size_t size);
    AssetData GetIconData(const std::string &name) const;
    void AddLayoutData(const std::string &name, const std::string &align_str, int x, int y, int width = 0, int height = 0);
    void AddTextFont(std::shared_ptr<LvglFont> text_font);

    // emote handle 访问器
    emote_handle_t GetEmoteHandle() const { return emote_handle_; }

private:
    virtual bool Lock(int timeout_ms = 0) override;
    virtual void Unlock() override;

    // 通过 GFX API 直接设置表情（用于本地加载的 GIF）
    void SetEmotionGfx(const AssetData& emoji);

    emote_handle_t emote_handle_ = nullptr;

    // 本地加载的数据
    std::map<std::string, AssetData> emoji_data_map_;
    std::map<std::string, AssetData> icon_data_map_;
    std::shared_ptr<LvglFont> text_font_ = nullptr;
};

} // namespace emote

