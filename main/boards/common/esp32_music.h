#ifndef ESP32_MUSIC_H
#define ESP32_MUSIC_H

#include "music.h"

#include <string>
#include <atomic>
#include <mutex>
#include <vector>
#include <queue>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>

// Forward declarations
class Http;
struct cJSON;

extern "C" {
#include "mp3dec.h"
}

class Esp32Music : public Music {
public:
    Esp32Music();
    virtual ~Esp32Music();

    // Music interface
    virtual bool Start(const std::string& song_name, const std::string& artist_name = "") override;
    virtual void Stop() override;
    virtual bool IsPlaying() const override;
    virtual std::string Search(const std::string& song_name, const std::string& artist_name = "") override;
    virtual std::string GetSongName() const override;
    virtual std::string GetArtistName() const override;

private:
    // ---- 搜索阶段 ----
    cJSON* SearchMusicInternal(const std::string& song_name, const std::string& artist_name);

    // ---- 播放任务 ----
    struct MusicTaskArg {
        Esp32Music* self;
        std::string song_name;
        std::string artist_name;
    };
    static void MusicTaskFunc(void* arg);
    void MusicTaskLoop();

    bool DownloadAndPlay(const std::string& music_url);

    // ---- MP3 解码 ----
    bool InitMp3Decoder();
    void CleanupMp3Decoder();

    // ---- 音频输出 ----
    void WriteAudioFrames(const int16_t* pcm_data, size_t frame_count);

    // ---- 数据 ----
    std::string current_song_name_;
    std::string current_artist_name_;
    std::string current_music_url_;
    std::atomic<bool> playing_{false};
    std::atomic<bool> stop_requested_{false};

    TaskHandle_t music_task_handle_{nullptr};

    // MP3 解码器
    HMP3Decoder mp3_decoder_{nullptr};
    MP3FrameInfo mp3_frame_info_{};

    // 读写缓冲区
    std::vector<uint8_t> mp3_read_buf_;
    std::vector<int16_t> pcm_out_buf_;

    // 线程安全
    mutable std::mutex info_mutex_;

    // 正在使用的 HTTP 连接（用于外部 abort）
    std::mutex http_mutex_;
    Http* active_http_{nullptr};

    static constexpr size_t kMp3ReadBufSize = 1024 * 8;
    static constexpr size_t kPcmOutBufSize  = 4608;  // 1152 * 2 * 2

    // Event bits
    static constexpr EventBits_t EVENT_STOP = 1 << 0;
};

#endif // ESP32_MUSIC_H
