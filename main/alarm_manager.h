#ifndef ALARM_MANAGER_H
#define ALARM_MANAGER_H

#include <string>
#include <vector>
#include <mutex>
#include <ctime>

#include <esp_timer.h>
#include <cJSON.h>

struct AlarmItem {
    std::string id;
    bool enabled = true;
    bool daily = true;
    int hour = 0;
    int minute = 0;
    std::string label;
    time_t next_trigger = 0;
};

class AlarmManager {
public:
    static AlarmManager& GetInstance();

    void Start();

    // 响铃控制
    bool StopRinging();
    bool IsRinging();

    // 闹钟增删改查
    bool AddAlarm(const std::string& hhmm, bool daily, const std::string& label,
                  std::string& out_id, std::string& out_error);
    bool CancelAlarm(const std::string& id);
    bool UpdateAlarmTime(const std::string& id, const std::string& hhmm, std::string& out_error);
    bool SetEnabled(const std::string& id, bool enabled, std::string* out_error = nullptr);
    bool SetAlarmMusic(const std::string& song_name, const std::string& artist_name, std::string& out_error);

    // 查询
    cJSON* GetAlarmsJson();
    cJSON* GetAlarmMusicJson();
    time_t GetNextAlarm() const;

    // 音乐下载取消标志（供 MusicDownloadTask 访问）
    bool music_download_cancelled_ = false;

private:
    AlarmManager() = default;
    ~AlarmManager() = default;
    AlarmManager(const AlarmManager&) = delete;
    AlarmManager& operator=(const AlarmManager&) = delete;

    static void TimerCallback(void* arg);
    static void RingTimerCallback(void* arg);
    void HandleTimer();
    void HandleRingTimer();
    void StartRinging(const AlarmItem& item);

    bool ParseHHMM(const std::string& hhmm, int& hour, int& minute);
    time_t ComputeNextTrigger(int hour, int minute, bool daily, time_t now);
    std::string GenerateAlarmId();

    void NormalizeLoadedAlarmsLocked();
    void LoadAlarmMusicLocked();
    void SaveAlarmMusicLocked();

    void LoadFromSettings();
    void SaveToSettings();
    void RearmTimerLocked();

    bool started_ = false;
    esp_timer_handle_t timer_ = nullptr;
    esp_timer_handle_t ring_timer_ = nullptr;

    std::vector<AlarmItem> alarms_;
    std::string alarm_music_song_;
    std::string alarm_music_artist_;
    bool ringing_ = false;
    int64_t ring_end_us_ = 0;
    int64_t ring_last_alert_us_ = 0;
    time_t last_check_time_ = 0;
    std::string ringing_message_;
    bool ringing_with_music_ = false;
    mutable std::mutex mutex_;
};

#endif
