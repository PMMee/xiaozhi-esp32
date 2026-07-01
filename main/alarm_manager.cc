#include "alarm_manager.h"

#include <array>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "settings.h"
#include "application.h"
#include "board.h"
#include "assets/lang_config.h"

#define TAG "AlarmManager"

namespace {

constexpr const char* kSettingsNamespace = "alarm";
constexpr const char* kSettingsKey = "items";
constexpr const char* kSettingsMusicSongKey = "music_song";
constexpr const char* kSettingsMusicArtistKey = "music_artist";
constexpr int64_t kMinTimerUs = 1000 * 1000;              // 1 second
constexpr int64_t kRingDurationUs = 60LL * 1000 * 1000;    // 60 seconds
constexpr int64_t kRingIntervalUs = 1000 * 1000;           // 1 second
constexpr int64_t kRingAlertIntervalUs = 2LL * 1000 * 1000; // 2 seconds
constexpr const char* kDefaultAlarmMusicSongName = "";
constexpr const char* kDefaultAlarmMusicArtistName = "";
constexpr const char* kDeprecatedDefaultAlarmMusicSongName = "alarm.ogg";

std::string TrimCopy(const std::string& input) {
    size_t begin = 0;
    while (begin < input.size() && std::isspace(static_cast<unsigned char>(input[begin]))) {
        ++begin;
    }
    size_t end = input.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(input[end - 1]))) {
        --end;
    }
    return input.substr(begin, end - begin);
}

bool ShouldUseMusicPlayback(const std::string& song_name, const std::string& artist_name) {
    const std::string trimmed_song = TrimCopy(song_name);
    const std::string trimmed_artist = TrimCopy(artist_name);
    if (trimmed_song.empty() && trimmed_artist.empty()) {
        return false;
    }
    if (trimmed_song == kDeprecatedDefaultAlarmMusicSongName && trimmed_artist.empty()) {
        return false;
    }
    return true;
}

bool IsDefaultAlarmMusicRequest(const std::string& song_name, const std::string& artist_name) {
    const std::string trimmed_song = TrimCopy(song_name);
    const std::string trimmed_artist = TrimCopy(artist_name);
    if (trimmed_song.empty() && trimmed_artist.empty()) {
        return true;
    }
    static constexpr std::array<const char*, 7> kDefaultKeywords = {
        "默认铃音", "默认铃声", "系统铃音", "系统铃声", "本地铃音", "本地铃声", "alarm.ogg"
    };
    for (const auto& kw : kDefaultKeywords) {
        if (trimmed_song == kw) return true;
    }
    return false;
}

// FreeRTOS 音乐下载任务参数
struct MusicDownloadArg {
    std::string song_name;
    std::string artist_name;
    std::string alert_msg;
};

void MusicDownloadTask(void* arg) {
    auto* ctx = static_cast<MusicDownloadArg*>(arg);
    auto& alarm_mgr = AlarmManager::GetInstance();

    if (alarm_mgr.music_download_cancelled_) {
        // 已被 StopRinging 取消，回退到本地铃音
        Application::GetInstance().Schedule([msg = ctx->alert_msg]() {
            Application::GetInstance().Alert("Alarm", msg.c_str(), "neutral", Lang::Sounds::OGG_ALARM);
        });
        delete ctx;
        vTaskDelete(nullptr);
        return;
    }

    auto music = Board::GetInstance().GetMusic();
    if (!music) {
        Application::GetInstance().Schedule([msg = ctx->alert_msg]() {
            Application::GetInstance().PlaySound(Lang::Sounds::OGG_ALARM);
        });
        delete ctx;
        vTaskDelete(nullptr);
        return;
    }

    bool ok = music->Start(ctx->song_name, ctx->artist_name);
    if (!ok || alarm_mgr.music_download_cancelled_) {
        ESP_LOGW(TAG, "Alarm music download failed, fallback to alarm.ogg");
        Application::GetInstance().Schedule([msg = ctx->alert_msg]() {
            Application::GetInstance().Alert("Alarm", msg.c_str(), "neutral", Lang::Sounds::OGG_ALARM);
        });
    }

    delete ctx;
    vTaskDelete(nullptr);
}

} // namespace

// ==================== 单例 ====================

AlarmManager& AlarmManager::GetInstance() {
    static AlarmManager instance;
    return instance;
}

void AlarmManager::Start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (started_) {
        return;
    }

    esp_timer_create_args_t args = {
        .callback = &AlarmManager::TimerCallback,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "alarm_timer",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&args, &timer_));

    esp_timer_create_args_t ring_args = {
        .callback = &AlarmManager::RingTimerCallback,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "alarm_ring_timer",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&ring_args, &ring_timer_));

    LoadFromSettings();
    LoadAlarmMusicLocked();
    NormalizeLoadedAlarmsLocked();
    last_check_time_ = time(nullptr);
    RearmTimerLocked();
    started_ = true;
    ESP_LOGI(TAG, "Alarm manager started with %d alarms", (int)alarms_.size());
}

// ==================== 响铃控制 ====================

bool AlarmManager::StopRinging() {
    bool was_ringing = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        was_ringing = ringing_;
        ringing_ = false;
        ring_end_us_ = 0;
        ring_last_alert_us_ = 0;
        ringing_message_.clear();
        ringing_with_music_ = false;
        music_download_cancelled_ = true;  // 取消正在进行的音乐下载
    }

    if (!was_ringing) {
        return false;
    }

    if (ring_timer_) {
        esp_timer_stop(ring_timer_);
    }

    Application::GetInstance().Schedule([]() {
        auto& app = Application::GetInstance();
        auto music = Board::GetInstance().GetMusic();
        if (music) {
            music->Stop();
        }
        app.DismissAlert();
    });
    ESP_LOGI(TAG, "Alarm ringing stopped");
    return true;
}

bool AlarmManager::IsRinging() {
    std::lock_guard<std::mutex> lock(mutex_);
    return ringing_;
}

// ==================== 增删改查 ====================

bool AlarmManager::AddAlarm(const std::string& hhmm, bool daily, const std::string& label,
                            std::string& out_id, std::string& out_error) {
    int hour = 0, minute = 0;
    if (!ParseHHMM(hhmm, hour, minute)) {
        out_error = "Invalid time format, expected HH:MM";
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    AlarmItem item;
    item.id = GenerateAlarmId();
    item.enabled = true;
    item.daily = daily;
    item.hour = hour;
    item.minute = minute;
    item.label = label;
    item.next_trigger = ComputeNextTrigger(hour, minute, daily, time(nullptr));
    if (!daily && item.next_trigger <= 0) {
        out_error = "One-shot alarm time has already passed";
        return false;
    }

    alarms_.push_back(item);
    SaveToSettings();
    RearmTimerLocked();
    out_id = item.id;
    ESP_LOGI(TAG, "Alarm added: id=%s, %02d:%02d, daily=%d, label='%s'",
        out_id.c_str(), hour, minute, daily, label.c_str());
    return true;
}

bool AlarmManager::CancelAlarm(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto old_size = alarms_.size();
    alarms_.erase(std::remove_if(alarms_.begin(), alarms_.end(), [&id](const AlarmItem& item) {
        return item.id == id;
    }), alarms_.end());

    bool removed = old_size != alarms_.size();
    if (removed) {
        SaveToSettings();
        RearmTimerLocked();
        ESP_LOGI(TAG, "Alarm cancelled: id=%s", id.c_str());
    }
    return removed;
}

bool AlarmManager::UpdateAlarmTime(const std::string& id, const std::string& hhmm, std::string& out_error) {
    int hour = 0, minute = 0;
    if (!ParseHHMM(hhmm, hour, minute)) {
        out_error = "Invalid time format, expected HH:MM";
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& item : alarms_) {
        if (item.id == id) {
            item.hour = hour;
            item.minute = minute;
            if (item.enabled) {
                item.next_trigger = ComputeNextTrigger(hour, minute, item.daily, time(nullptr));
                if (!item.daily && item.next_trigger <= 0) {
                    item.enabled = false;
                    out_error = "One-shot alarm time has already passed";
                }
            }
            SaveToSettings();
            RearmTimerLocked();
            ESP_LOGI(TAG, "Alarm time updated: id=%s, %02d:%02d", id.c_str(), hour, minute);
            return true;
        }
    }
    out_error = "alarm not found";
    return false;
}

bool AlarmManager::SetEnabled(const std::string& id, bool enabled, std::string* out_error) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& item : alarms_) {
        if (item.id == id) {
            item.enabled = enabled;
            if (enabled) {
                item.next_trigger = ComputeNextTrigger(item.hour, item.minute, item.daily, time(nullptr));
                if (!item.daily && item.next_trigger <= 0) {
                    item.enabled = false;
                    item.next_trigger = 0;
                    if (out_error) *out_error = "One-shot alarm time has already passed";
                    SaveToSettings();
                    RearmTimerLocked();
                    return false;
                }
            }
            SaveToSettings();
            RearmTimerLocked();
            ESP_LOGI(TAG, "Alarm %s: id=%s", enabled ? "enabled" : "disabled", id.c_str());
            return true;
        }
    }
    return false;
}

bool AlarmManager::SetAlarmMusic(const std::string& song_name, const std::string& artist_name, std::string& out_error) {
    std::string trimmed_song = TrimCopy(song_name);
    std::string trimmed_artist = TrimCopy(artist_name);
    out_error.clear();

    if (IsDefaultAlarmMusicRequest(trimmed_song, trimmed_artist)) {
        trimmed_song.clear();
        trimmed_artist.clear();
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        alarm_music_song_ = trimmed_song;
        alarm_music_artist_ = trimmed_artist;
        SaveAlarmMusicLocked();
    }

    ESP_LOGI(TAG, "Alarm music updated: song='%s', artist='%s'",
        trimmed_song.c_str(), trimmed_artist.c_str());
    return true;
}

cJSON* AlarmManager::GetAlarmsJson() {
    std::lock_guard<std::mutex> lock(mutex_);
    cJSON* root = cJSON_CreateObject();
    cJSON* list = cJSON_CreateArray();
    for (const auto& item : alarms_) {
        cJSON* one = cJSON_CreateObject();
        cJSON_AddStringToObject(one, "id", item.id.c_str());
        cJSON_AddBoolToObject(one, "enabled", item.enabled);
        cJSON_AddBoolToObject(one, "daily", item.daily);
        cJSON_AddNumberToObject(one, "hour", item.hour);
        cJSON_AddNumberToObject(one, "minute", item.minute);
        cJSON_AddStringToObject(one, "label", item.label.c_str());
        cJSON_AddNumberToObject(one, "next_trigger", (double)item.next_trigger);
        cJSON_AddItemToArray(list, one);
    }
    cJSON_AddItemToObject(root, "alarms", list);
    return root;
}

cJSON* AlarmManager::GetAlarmMusicJson() {
    std::lock_guard<std::mutex> lock(mutex_);
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "song_name", alarm_music_song_.c_str());
    cJSON_AddStringToObject(root, "artist_name", alarm_music_artist_.c_str());
    return root;
}

time_t AlarmManager::GetNextAlarm() const {
    std::lock_guard<std::mutex> lock(mutex_);
    time_t next = 0;
    for (const auto& item : alarms_) {
        if (!item.enabled || item.next_trigger <= 0) continue;
        if (next == 0 || item.next_trigger < next) {
            next = item.next_trigger;
        }
    }
    return next;
}

// ==================== 定时器回调 ====================

void AlarmManager::TimerCallback(void* arg) {
    auto* self = static_cast<AlarmManager*>(arg);
    self->HandleTimer();
}

void AlarmManager::RingTimerCallback(void* arg) {
    auto* self = static_cast<AlarmManager*>(arg);
    self->HandleRingTimer();
}

void AlarmManager::HandleTimer() {
    std::vector<AlarmItem> triggered;
    bool changed = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        time_t now = time(nullptr);

        // 时间跳变检测（>120s）
        if (last_check_time_ > 0 && std::llabs((long long)(now - last_check_time_)) > 120) {
            ESP_LOGW(TAG, "System time changed significantly, normalizing alarms");
            NormalizeLoadedAlarmsLocked();
            changed = true;
        }
        last_check_time_ = now;

        for (auto& item : alarms_) {
            if (!item.enabled || item.next_trigger <= 0) continue;
            if (item.next_trigger <= now + 1) {
                triggered.push_back(item);
                changed = true;
                if (item.daily) {
                    item.next_trigger = ComputeNextTrigger(item.hour, item.minute, true, now + 60);
                } else {
                    item.enabled = false;
                }
            }
        }

        // 清理已过期的单次闹钟
        auto before = alarms_.size();
        alarms_.erase(std::remove_if(alarms_.begin(), alarms_.end(), [](const AlarmItem& item) {
            return !item.daily && !item.enabled;
        }), alarms_.end());
        if (alarms_.size() != before) changed = true;

        if (changed) {
            SaveToSettings();
            RearmTimerLocked();
        }
    }

    for (const auto& item : triggered) {
        StartRinging(item);
    }
}

void AlarmManager::StartRinging(const AlarmItem& item) {
    bool need_start_timer = false;
    std::string msg = item.label.empty() ? "闹钟响了" : ("闹钟: " + item.label);
    std::string song_name, artist_name;
    bool use_music = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        ring_end_us_ = esp_timer_get_time() + kRingDurationUs;
        ring_last_alert_us_ = esp_timer_get_time();
        ringing_message_ = msg;
        song_name = alarm_music_song_;
        artist_name = alarm_music_artist_;
        use_music = ShouldUseMusicPlayback(song_name, artist_name);
        ringing_with_music_ = use_music;
        music_download_cancelled_ = false;
        if (!ringing_) {
            ringing_ = true;
            need_start_timer = true;
        }
    }

    Application::GetInstance().Schedule([msg, song_name, artist_name, use_music]() {
        auto& app = Application::GetInstance();
        // 闹钟最高优先级：强制回到空闲态，抢占音频资源
        if (app.GetDeviceState() != kDeviceStateIdle) {
            app.SetDeviceState(kDeviceStateIdle);
        }

        auto music = Board::GetInstance().GetMusic();
        if (music) {
            music->Stop();  // 清空残留音频缓冲区
        }

        if (!use_music) {
            // 本地铃音模式
            app.Alert("Alarm", msg.c_str(), "neutral", Lang::Sounds::OGG_ALARM);
            return;
        }

        // 在线音乐模式：先展示 Alert 文字，异步下载音乐
        app.Alert("Alarm", msg.c_str(), "neutral", "");

        if (music) {
            auto* ctx = new MusicDownloadArg{song_name, artist_name, msg};
            xTaskCreate(MusicDownloadTask, "alarm_music", 8192, ctx, 5, nullptr);
        } else {
            app.PlaySound(Lang::Sounds::OGG_ALARM);
        }
    });

    if (need_start_timer && ring_timer_) {
        esp_timer_stop(ring_timer_);
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_timer_start_periodic(ring_timer_, kRingIntervalUs));
    }
    ESP_LOGI(TAG, "Alarm ringing for %d seconds", (int)(kRingDurationUs / 1000000));
}

void AlarmManager::HandleRingTimer() {
    bool should_stop = false, should_alert = false, should_repeat_local = false;
    std::string msg;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!ringing_) return;

        int64_t now_us = esp_timer_get_time();
        if (now_us >= ring_end_us_) {
            // 60s 超时，自动停止
            ringing_ = false;
            ring_end_us_ = 0;
            ring_last_alert_us_ = 0;
            ringing_message_.clear();
            ringing_with_music_ = false;
            music_download_cancelled_ = true;
            should_stop = true;
        } else {
            msg = ringing_message_.empty() ? "闹钟响了" : ringing_message_;
            if ((now_us - ring_last_alert_us_) >= kRingAlertIntervalUs) {
                ring_last_alert_us_ = now_us;
                should_alert = true;
                should_repeat_local = !ringing_with_music_;
            }
        }
    }

    if (should_stop) {
        if (ring_timer_) esp_timer_stop(ring_timer_);
        Application::GetInstance().Schedule([]() {
            auto& app = Application::GetInstance();
            auto music = Board::GetInstance().GetMusic();
            if (music) music->Stop();
            app.DismissAlert();
        });
        ESP_LOGI(TAG, "Alarm ringing timeout, auto stopped");
        return;
    }

    if (should_alert) {
        const std::string_view sound = should_repeat_local ? Lang::Sounds::OGG_ALARM : std::string_view{};
        Application::GetInstance().Schedule([msg, sound]() {
            Application::GetInstance().Alert("Alarm", msg.c_str(), "neutral", sound);
        });
    }
}

// ==================== 时间计算（纯 UTC） ====================

bool AlarmManager::ParseHHMM(const std::string& hhmm, int& hour, int& minute) {
    if (hhmm.size() != 5 || hhmm[2] != ':') return false;
    if (!std::isdigit(hhmm[0]) || !std::isdigit(hhmm[1]) ||
        !std::isdigit(hhmm[3]) || !std::isdigit(hhmm[4])) return false;
    hour = (hhmm[0] - '0') * 10 + (hhmm[1] - '0');
    minute = (hhmm[3] - '0') * 10 + (hhmm[4] - '0');
    return (hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59);
}

time_t AlarmManager::ComputeNextTrigger(int hour, int minute, bool daily, time_t now) {
    // 纯 UTC 计算，避免 localtime_r/mktime 的 DST 问题
    time_t today_start = (now / 86400) * 86400;
    time_t candidate = today_start + hour * 3600 + minute * 60;

    if (daily) {
        if (candidate <= now) candidate += 86400;
        return candidate;
    }
    return (candidate > now) ? candidate : 0;
}

std::string AlarmManager::GenerateAlarmId() {
    std::ostringstream oss;
    oss << "alarm_" << esp_timer_get_time();
    return oss.str();
}

// ==================== NVS 持久化 ====================

void AlarmManager::LoadFromSettings() {
    Settings settings(kSettingsNamespace, false);
    auto json = settings.GetString(kSettingsKey, "");
    if (json.empty()) { alarms_.clear(); return; }

    cJSON* root = cJSON_Parse(json.c_str());
    if (!root) { alarms_.clear(); return; }

    auto* list = cJSON_GetObjectItem(root, "alarms");
    if (!cJSON_IsArray(list)) { cJSON_Delete(root); alarms_.clear(); return; }

    alarms_.clear();
    cJSON* one = nullptr;
    cJSON_ArrayForEach(one, list) {
        if (!cJSON_IsObject(one)) continue;
        auto* id = cJSON_GetObjectItem(one, "id");
        auto* hour = cJSON_GetObjectItem(one, "hour");
        auto* minute = cJSON_GetObjectItem(one, "minute");
        if (!cJSON_IsString(id) || !cJSON_IsNumber(hour) || !cJSON_IsNumber(minute)) continue;

        AlarmItem item;
        item.id = id->valuestring;
        item.enabled = cJSON_IsTrue(cJSON_GetObjectItem(one, "enabled"));
        item.daily = cJSON_IsTrue(cJSON_GetObjectItem(one, "daily"));
        item.hour = hour->valueint;
        item.minute = minute->valueint;
        auto* label = cJSON_GetObjectItem(one, "label");
        item.label = cJSON_IsString(label) ? label->valuestring : "";
        auto* next = cJSON_GetObjectItem(one, "next_trigger");
        item.next_trigger = cJSON_IsNumber(next)
            ? (time_t)next->valuedouble
            : ComputeNextTrigger(item.hour, item.minute, item.daily, time(nullptr));
        alarms_.push_back(std::move(item));
    }
    cJSON_Delete(root);
}

void AlarmManager::LoadAlarmMusicLocked() {
    Settings settings(kSettingsNamespace, false);
    alarm_music_song_ = TrimCopy(settings.GetString(kSettingsMusicSongKey, kDefaultAlarmMusicSongName));
    alarm_music_artist_ = TrimCopy(settings.GetString(kSettingsMusicArtistKey, kDefaultAlarmMusicArtistName));

    // 兼容旧版遗留配置：自动迁移到默认铃音
    if ((alarm_music_song_ == kDeprecatedDefaultAlarmMusicSongName && alarm_music_artist_.empty()) ||
        (alarm_music_song_ == "月亮代表我的心" && alarm_music_artist_ == "邓丽君")) {
        alarm_music_song_.clear();
        alarm_music_artist_.clear();
        SaveAlarmMusicLocked();
    }
}

void AlarmManager::SaveAlarmMusicLocked() {
    Settings settings(kSettingsNamespace, true);
    settings.SetString(kSettingsMusicSongKey, alarm_music_song_);
    settings.SetString(kSettingsMusicArtistKey, alarm_music_artist_);
}

void AlarmManager::NormalizeLoadedAlarmsLocked() {
    time_t now = time(nullptr);
    bool changed = false;
    for (auto& item : alarms_) {
        if (!item.enabled) continue;
        time_t next = ComputeNextTrigger(item.hour, item.minute, item.daily, now);
        if (!item.daily && next <= 0) {
            item.enabled = false;
            item.next_trigger = 0;
            changed = true;
            continue;
        }
        if (next > 0 && next != item.next_trigger) {
            item.next_trigger = next;
            changed = true;
        }
    }
    auto old = alarms_.size();
    alarms_.erase(std::remove_if(alarms_.begin(), alarms_.end(), [](const AlarmItem& item) {
        return !item.daily && (!item.enabled || item.next_trigger <= 0);
    }), alarms_.end());
    if (changed || alarms_.size() != old) SaveToSettings();
}

void AlarmManager::SaveToSettings() {
    cJSON* root = cJSON_CreateObject();
    cJSON* list = cJSON_CreateArray();
    for (const auto& item : alarms_) {
        cJSON* one = cJSON_CreateObject();
        cJSON_AddStringToObject(one, "id", item.id.c_str());
        cJSON_AddBoolToObject(one, "enabled", item.enabled);
        cJSON_AddBoolToObject(one, "daily", item.daily);
        cJSON_AddNumberToObject(one, "hour", item.hour);
        cJSON_AddNumberToObject(one, "minute", item.minute);
        cJSON_AddStringToObject(one, "label", item.label.c_str());
        cJSON_AddNumberToObject(one, "next_trigger", (double)item.next_trigger);
        cJSON_AddItemToArray(list, one);
    }
    cJSON_AddItemToObject(root, "alarms", list);
    char* json = cJSON_PrintUnformatted(root);
    std::string out = json ? json : "{\"alarms\":[]}";
    if (json) cJSON_free(json);
    cJSON_Delete(root);

    Settings settings(kSettingsNamespace, true);
    settings.SetString(kSettingsKey, out);
}

void AlarmManager::RearmTimerLocked() {
    if (!timer_) return;
    bool has = false;
    for (const auto& item : alarms_) {
        if (item.enabled && item.next_trigger > 0) { has = true; break; }
    }
    if (!has) {
        if (esp_timer_is_active(timer_)) esp_timer_stop(timer_);
        return;
    }
    if (!esp_timer_is_active(timer_)) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_timer_start_periodic(timer_, kMinTimerUs));
    }
}
