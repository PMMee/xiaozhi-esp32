#include "esp32_music.h"
#include "board.h"
#include "application.h"
#include "system_info.h"
#include "audio/audio_codec.h"
#include "display/display.h"
#include "assets/lang_config.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <cJSON.h>
#include <cstring>
#include <cctype>
#include <algorithm>
#include <sstream>

extern "C" {
#include "mp3dec.h"
}

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <mbedtls/sha256.h>

#define TAG "Esp32Music"

// ---- 音乐源配置 ----
// 可选: qq(QQ音乐) | wyy(网易云) | kg(酷狗) | kw(酷我)
// 不同源的 CDN 对 ESP32 的速度差异很大，QQ 音乐 CDN 实测 ~8KB/s
// 如遇卡顿可切换尝试
#define MUSIC_SOURCE_TYPE "kw"
// ---- 调试开关 (取消注释以启用详细网络日志) ----
// #define MUSIC_STREAM_DEBUG

#ifdef MUSIC_STREAM_DEBUG
#include <esp_wifi.h>
#endif

// ---- 认证头（与 shybot.top API 的 ESP32 动态密钥，旧版代码一致） ----

static void add_auth_headers(Http* http) {
    if (!http) return;
    int64_t timestamp = esp_timer_get_time() / 1000000;
    std::string mac = SystemInfo::GetMacAddress();
    std::string chip_id = mac;
    chip_id.erase(std::remove(chip_id.begin(), chip_id.end(), ':'), chip_id.end());
    const std::string secret_key = "your-esp32-secret-key-2024";
    std::string data = mac + ":" + chip_id + ":" + std::to_string(timestamp) + ":" + secret_key;

    unsigned char hash[32];
    mbedtls_sha256((unsigned char*)data.c_str(), data.length(), hash, 0);

    std::string key;
    for (int i = 0; i < 16; i++) {
        char hex[3];
        snprintf(hex, sizeof(hex), "%02X", hash[i]);
        key += hex;
    }

    http->SetHeader("X-MAC-Address", mac);
    http->SetHeader("X-Chip-ID", chip_id);
    http->SetHeader("X-Timestamp", std::to_string(timestamp));
    http->SetHeader("X-Dynamic-Key", key);
}

// ---- 静态工具函数 ----

static std::string url_encode(const std::string& str) {
    std::string encoded;
    char hex[4];
    for (size_t i = 0; i < str.length(); i++) {
        unsigned char c = str[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            encoded += c;
        } else if (c == ' ') {
            encoded += '+';
        } else {
            snprintf(hex, sizeof(hex), "%%%02X", c);
            encoded += hex;
        }
    }
    return encoded;
}

static std::string trim_copy(const std::string& input) {
    size_t begin = 0;
    while (begin < input.size() && std::isspace(static_cast<unsigned char>(input[begin]))) { ++begin; }
    size_t end = input.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(input[end - 1]))) { --end; }
    return input.substr(begin, end - begin);
}

// 从搜索结果中选择匹配艺术家的歌曲
static cJSON* select_song_by_artist(cJSON* response_json, const std::string& artist_name) {
    if (!cJSON_IsArray(response_json)) {
        return cJSON_IsObject(response_json) ? response_json : nullptr;
    }
    int count = cJSON_GetArraySize(response_json);
    if (count == 0) return nullptr;

    cJSON* first_item = nullptr;
    std::string trimmed_artist = trim_copy(artist_name);

    for (int i = 0; i < count; i++) {
        cJSON* item = cJSON_GetArrayItem(response_json, i);
        if (!item) continue;
        if (!first_item) first_item = item;

        if (!trimmed_artist.empty()) {
            cJSON* singer = cJSON_GetObjectItem(item, "singer");
            if (cJSON_IsString(singer)) {
                std::string singer_str = trim_copy(singer->valuestring);
                if (singer_str.find(trimmed_artist) != std::string::npos ||
                    trimmed_artist.find(singer_str) != std::string::npos) {
                    return item;
                }
            }
        }
    }
    return first_item;
}

// ---- Esp32Music 实现 ----

Esp32Music::Esp32Music() {
    mp3_read_buf_.resize(kMp3ReadBufSize);
    pcm_out_buf_.resize(kPcmOutBufSize);
}

Esp32Music::~Esp32Music() {
    Stop();
    CleanupMp3Decoder();
}

bool Esp32Music::Start(const std::string& song_name, const std::string& artist_name) {
    if (playing_.load()) {
        ESP_LOGW(TAG, "Already playing, stop current first");
        Stop();
    }

    // 清除暂停状态 — 新歌总是从头播放
    paused_.store(false);
    end_reason_ = EndReason::Normal;
    resume_url_.clear();
    resume_offset_ = 0;
    resume_song_name_.clear();
    resume_artist_name_.clear();
    current_music_url_.clear();

    {
        std::lock_guard<std::mutex> lock(info_mutex_);
        current_song_name_ = trim_copy(song_name);
        current_artist_name_ = trim_copy(artist_name);
    }

    // 创建播放任务（搜索+下载+解码+播放全部在 Task 中异步执行）
    stop_requested_.store(false);
    playing_.store(true);

    auto* arg = new MusicTaskArg{this, current_song_name_, current_artist_name_};
    BaseType_t ret = xTaskCreatePinnedToCore(
        MusicTaskFunc,
        "music_task",
        10240,
        arg,
        5,
        &music_task_handle_,
        tskNO_AFFINITY
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create music task");
        delete arg;
        playing_.store(false);
        return false;
    }

    return true;
}

void Esp32Music::Pause() {
    if (!playing_.load()) return;

    ESP_LOGI(TAG, "Pausing music (resume at offset %u)", (unsigned)streamed_bytes_.load());
    stop_requested_.store(true);
    end_reason_ = EndReason::Stopped;

    // 保存续播状态
    resume_url_ = current_music_url_;
    resume_offset_ = streamed_bytes_.load();
    {
        std::lock_guard<std::mutex> lock(info_mutex_);
        resume_song_name_ = current_song_name_;
        resume_artist_name_ = current_artist_name_;
    }
    paused_.store(true);

    // 中断 HTTP 连接
    {
        std::lock_guard<std::mutex> lock(http_mutex_);
        if (active_http_) {
            active_http_->Close();
            active_http_ = nullptr;
        }
    }
}

void Esp32Music::Resume() {
    if (!paused_.load()) {
        ESP_LOGW(TAG, "Cannot resume: not paused");
        return;
    }
    if (resume_url_.empty()) {
        ESP_LOGW(TAG, "Cannot resume: no saved URL");
        paused_.store(false);
        return;
    }

    ESP_LOGI(TAG, "Resuming from offset %u: %s - %s",
             (unsigned)resume_offset_, resume_song_name_.c_str(), resume_artist_name_.c_str());

    stop_requested_.store(false);
    end_reason_ = EndReason::Normal;
    paused_.store(false);

    // 恢复上次的状态
    {
        std::lock_guard<std::mutex> lock(info_mutex_);
        current_song_name_ = resume_song_name_;
        current_artist_name_ = resume_artist_name_;
        current_music_url_ = resume_url_;
    }
    playing_.store(true);
    streamed_bytes_.store(0);

    // 重新创建播放任务
    auto* arg = new MusicTaskArg{this, resume_song_name_, resume_artist_name_};
    xTaskCreate(MusicTaskFunc, "music_play", 8192, arg, 5, &music_task_handle_);
}

void Esp32Music::Stop() {
    if (!playing_.load() && !paused_.load()) return;

    ESP_LOGI(TAG, "Stopping music playback");
    stop_requested_.store(true);
    end_reason_ = EndReason::Stopped;
    paused_.store(false);  // 清除暂停状态

    // 中断 HTTP 连接
    {
        std::lock_guard<std::mutex> lock(http_mutex_);
        if (active_http_) {
            active_http_->Close();
            active_http_ = nullptr;
        }
    }
}

bool Esp32Music::IsPlaying() const {
    return playing_.load() || paused_.load();
}

std::string Esp32Music::Search(const std::string& song_name, const std::string& artist_name) {
    // 防止并发搜索（上一个搜索任务仍在运行）
    if (search_task_running_.load()) {
        return "{\"success\": false, \"message\": \"正在搜索中，请稍候\"}";
    }

    search_task_running_.store(true);

    auto* arg = new SearchTaskArg{};
    arg->self = this;
    arg->song_name = song_name;
    arg->artist_name = artist_name;
    arg->done = xSemaphoreCreateBinary();
    arg->consumed.store(false);

    TaskHandle_t search_task = nullptr;
    BaseType_t ret = xTaskCreatePinnedToCore(
        SearchTaskFunc,
        "music_search",
        8192,
        arg,
        5,
        &search_task,
        tskNO_AFFINITY);

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create search task");
        vSemaphoreDelete(arg->done);
        delete arg;
        search_task_running_.store(false);
        return "{\"success\": false, \"message\": \"创建搜索任务失败\"}";
    }

    // 在主事件循环中等待结果，但设置硬超时，避免 TCP 连接卡死导致主循环死锁
    // 超时后主循环立即释放，唤醒词可恢复响应
    if (xSemaphoreTake(arg->done, pdMS_TO_TICKS(kSearchTimeoutMs)) == pdTRUE) {
        // 搜索完成，复制结果
        std::string result = std::move(arg->result);
        arg->consumed.store(true);  // 通知搜索任务可以回收 arg
        // arg 由 SearchTaskFunc 负责释放
        return result;
    }

    // 超时：尝试中断 HTTP 连接，帮助搜索任务尽快退出
    ESP_LOGW(TAG, "[SEARCH] Timed out after %dms, aborting HTTP", kSearchTimeoutMs);
    {
        std::lock_guard<std::mutex> lock(http_mutex_);
        if (active_http_) {
            active_http_->Close();
            active_http_ = nullptr;
        }
    }
    // arg 由 SearchTaskFunc 超时后自行释放，此处不 delete（避免 use-after-free）
    return "{\"success\": false, \"message\": \"搜索超时，请稍后再试\"}";
}

void Esp32Music::SearchTaskFunc(void* arg) {
    auto* task_arg = static_cast<SearchTaskArg*>(arg);
    Esp32Music* self = task_arg->self;

    cJSON* response_json = self->SearchMusicInternal(task_arg->song_name, task_arg->artist_name);
    if (!response_json) {
        task_arg->result = "{\"success\": false, \"message\": \"搜索失败\"}";
    } else {
        cJSON* result = cJSON_CreateObject();
        cJSON_AddBoolToObject(result, "success", true);
        cJSON* results_array = cJSON_AddArrayToObject(result, "results");
        if (cJSON_IsArray(response_json)) {
            int count = cJSON_GetArraySize(response_json);
            int max_results = (count > 5) ? 5 : count;
            for (int i = 0; i < max_results; i++) {
                cJSON* item = cJSON_GetArrayItem(response_json, i);
                if (item) {
                    cJSON_AddItemToArray(results_array, cJSON_Duplicate(item, 1));
                }
            }
        }
        cJSON_Delete(response_json);
        char* json_str = cJSON_PrintUnformatted(result);
        task_arg->result = json_str;
        cJSON_free(json_str);
        cJSON_Delete(result);
    }

    // 通知主线程结果就绪
    xSemaphoreGive(task_arg->done);

    // 等待主线程取走结果（最多等待 500ms），然后释放 arg
    int wait_ms = 0;
    while (!task_arg->consumed.load() && wait_ms < 500) {
        vTaskDelay(pdMS_TO_TICKS(10));
        wait_ms += 10;
    }

    vSemaphoreDelete(task_arg->done);
    self->search_task_running_.store(false);
    delete task_arg;
    vTaskDelete(nullptr);
}

std::string Esp32Music::GetSongName() const {
    std::lock_guard<std::mutex> lock(info_mutex_);
    return current_song_name_;
}

std::string Esp32Music::GetArtistName() const {
    std::lock_guard<std::mutex> lock(info_mutex_);
    return current_artist_name_;
}

// ---- 搜索 ----

cJSON* Esp32Music::SearchMusicInternal(const std::string& song_name, const std::string& artist_name) {
    // 确保 WiFi 处于高性能模式，避免因 ResetProtocol 异步将 WiFi 设为 LOW_POWER 导致 TCP 连接失败
    Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);

    std::string base_url = "http://shybot.top/v2/music/api/?shykey=6df7c755ae9f9fb598572b34194a8b060fb5fb611dcb20f783f1a7a9ea6937aa&type=" MUSIC_SOURCE_TYPE;
    std::string full_url;

    if (!song_name.empty()) {
        full_url = base_url + "&name=" + url_encode(song_name);
        if (!artist_name.empty()) {
            full_url += "&singer=" + url_encode(artist_name);
        }
    } else if (!artist_name.empty()) {
        // API 强制要求 &name 参数，只传 &singer 会返回 "歌名不见了？qwq"
        // 没有歌名时直接用歌手名作为搜索关键词
        full_url = base_url + "&name=" + url_encode(artist_name);
    }

    auto network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(0);
    if (!http) {
        ESP_LOGE(TAG, "Failed to create HTTP client");
        return nullptr;
    }

    // 记录活跃 HTTP 句柄，供超时中断使用
    {
        std::lock_guard<std::mutex> lock(http_mutex_);
        active_http_ = http.get();
    }

    http->SetTimeout(8000);
    http->SetHeader("User-Agent", "ESP32-Music-Player/1.0");
    http->SetHeader("Accept", "application/json");
    add_auth_headers(http.get());

    ESP_LOGI(TAG, "[SEARCH] URL: %s (source=%s)", full_url.c_str(), MUSIC_SOURCE_TYPE);
    ESP_LOGI(TAG, "[SEARCH] Connecting to: %s", full_url.c_str());

    if (!http->Open("GET", full_url)) {
        ESP_LOGE(TAG, "[SEARCH] Failed to connect to music API");
        std::lock_guard<std::mutex> lock(http_mutex_);
        active_http_ = nullptr;
        return nullptr;
    }

    int status_code = http->GetStatusCode();
    ESP_LOGI(TAG, "[SEARCH] HTTP status: %d", status_code);
    if (status_code != 200) {
        ESP_LOGE(TAG, "[SEARCH] HTTP GET failed: %d", status_code);
        http->Close();
        std::lock_guard<std::mutex> lock(http_mutex_);
        active_http_ = nullptr;
        return nullptr;
    }

    std::string response_data = http->ReadAll();
    http->Close();
    {
        std::lock_guard<std::mutex> lock(http_mutex_);
        active_http_ = nullptr;
    }

    ESP_LOGI(TAG, "[SEARCH] Response size: %d bytes", (int)response_data.size());
    // 诊断日志：打印原始响应（方便排查歌手搜索返回空JSON等问题）
    ESP_LOGI(TAG, "[SEARCH] Raw body: [%s]", response_data.c_str());
    if (response_data.empty()) {
        ESP_LOGE(TAG, "[SEARCH] Empty response from music API");
        return nullptr;
    }

    cJSON* response_json = cJSON_Parse(response_data.c_str());
    if (!response_json) {
        ESP_LOGE(TAG, "[SEARCH] Failed to parse JSON");
        return nullptr;
    }

    int item_count = cJSON_IsArray(response_json) ? cJSON_GetArraySize(response_json) : 1;
    ESP_LOGI(TAG, "[SEARCH] Found %d results", item_count);

    // 打印搜索结果 JSON（截断前 1024 字符）
    char* raw = cJSON_PrintUnformatted(response_json);
    std::string raw_str(raw ? raw : "");
    cJSON_free(raw);
    if (raw_str.size() > 1024) raw_str = raw_str.substr(0, 1024) + "...";
    ESP_LOGI(TAG, "[SEARCH] JSON: %s", raw_str.c_str());

    return response_json;
}

// ---- 播放任务 ----

void Esp32Music::MusicTaskFunc(void* arg) {
    auto* task_arg = static_cast<MusicTaskArg*>(arg);
    Esp32Music* self = task_arg->self;
    std::string song = std::move(task_arg->song_name);
    std::string artist = std::move(task_arg->artist_name);
    delete task_arg;

    ESP_LOGI(TAG, "[TASK] Starting music task: song='%s' artist='%s'", song.c_str(), artist.c_str());

    // 如果是续播（URL 已在 Resume() 中设置好），跳过搜索直接播放
    if (!self->current_music_url_.empty()) {
        ESP_LOGI(TAG, "[TASK] Resume mode, skipping search");
        self->MusicTaskLoop();
    } else {
        // 搜索歌曲（在 Task 中异步执行，不阻塞主循环）
        cJSON* response_json = self->SearchMusicInternal(song, artist);
        if (!response_json) {
            ESP_LOGE(TAG, "[TASK] Search failed for: %s", song.c_str());
        } else {
            cJSON* selected = select_song_by_artist(response_json, artist);
            if (!selected) {
                ESP_LOGE(TAG, "[TASK] No song found in results");
                cJSON_Delete(response_json);
            } else {
                cJSON* url_json = cJSON_GetObjectItem(selected, "url");
                const char* url_str = (cJSON_IsString(url_json) && url_json->valuestring) ? url_json->valuestring : "";

                ESP_LOGI(TAG, "[TASK] Selected: '%s' - '%s', url=%s",
                    cJSON_IsString(cJSON_GetObjectItem(selected, "name")) ? cJSON_GetObjectItem(selected, "name")->valuestring : "?",
                    cJSON_IsString(cJSON_GetObjectItem(selected, "singer")) ? cJSON_GetObjectItem(selected, "singer")->valuestring : "?",
                    url_str);

                if (url_str[0] != '\0') {
                    if (cJSON_IsString(cJSON_GetObjectItem(selected, "name"))) {
                        std::lock_guard<std::mutex> lock(self->info_mutex_);
                        self->current_song_name_ = cJSON_GetObjectItem(selected, "name")->valuestring;
                    }
                    if (cJSON_IsString(cJSON_GetObjectItem(selected, "singer"))) {
                        std::lock_guard<std::mutex> lock(self->info_mutex_);
                        self->current_artist_name_ = cJSON_GetObjectItem(selected, "singer")->valuestring;
                    }

                    std::string music_url = url_str;
                    if (music_url.find('?') == std::string::npos) {
                        music_url += "?";
                    } else {
                        music_url += "&";
                    }
                    music_url += "shykey=6df7c755ae9f9fb598572b34194a8b060fb5fb611dcb20f783f1a7a9ea6937aa";

                    ESP_LOGI(TAG, "[TASK] Final stream URL: %s", music_url.c_str());
                    {
                        std::lock_guard<std::mutex> lock(self->info_mutex_);
                        self->current_music_url_ = std::move(music_url);
                    }
                    ESP_LOGI(TAG, "[TASK] Starting stream: %s - %s",
                        self->current_song_name_.c_str(), self->current_artist_name_.c_str());
                    self->MusicTaskLoop();
                } else {
                    ESP_LOGE(TAG, "[TASK] No audio URL in search result");
                }
                cJSON_Delete(response_json);
            }
        }
    }

    // 通知 Application 重新连接
    Application::GetInstance().Schedule([self]() {
        self->playing_.store(false);
        self->CleanupMp3Decoder();

        // 根据结束原因选择行为
        auto& app = Application::GetInstance();
        if (self->end_reason_ == EndReason::Stopped) {
            // 被用户主动停止（按键/唤醒词），不需要触发 AI 对话，
            // 唤醒词处理函数已经接管了音频通道
            ESP_LOGI(TAG, "Music stopped by user, skip resume");
        } else {
            const char* wake_word = nullptr;
            const char* notification = nullptr;
            switch (self->end_reason_) {
                case EndReason::Normal:
                    wake_word = "音乐播放完毕";
                    notification = "播放完毕";
                    break;
                case EndReason::Error:
                    wake_word = "音乐播放中断，网络不太好";
                    notification = "播放中断";
                    break;
                default:
                    break;
            }
            auto display = Board::GetInstance().GetDisplay();
            if (display) {
                display->ShowNotification(notification);
            }
            app.ResumeAudioChannel(wake_word);
        }
    });

    self->music_task_handle_ = nullptr;
    vTaskDelete(nullptr);
}

void Esp32Music::MusicTaskLoop() {
    if (!InitMp3Decoder()) {
        ESP_LOGE(TAG, "Failed to init MP3 decoder");
        return;
    }

    auto network = Board::GetInstance().GetNetwork();
    std::string url = current_music_url_;
    bool stream_success = false;

    // 外层重试（最多 5 次，应对 WiFi 瞬断 / CDN 限流）
    for (int retry = 0; retry < 5 && !stop_requested_.load(); retry++) {
        if (retry > 0) {
            int delay_ms = 1000 * retry;
            ESP_LOGI(TAG, "Retry %d/%d after %dms...", retry + 1, 5, delay_ms);
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
        }

        // 内层重定向（最多 3 跳）
        for (int redirect = 0; redirect < 3; redirect++) {
            // 每次 HTTP 连接前确保 WiFi 不休眠（可能被 ResetProtocol 异步覆盖）
            Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);

            auto http = network->CreateHttp(0);
            if (!http) {
                ESP_LOGE(TAG, "Failed to create HTTP client");
                break;
            }

            http->SetTimeout(8000);  // 8s 超时（原 15s），更快检测 TCP 断流
            http->SetKeepAlive(true);
            http->SetHeader("User-Agent", "ESP32-Music-Player/1.0");
            http->SetHeader("Accept", "*/*");
            http->SetHeader("Referer", "http://shybot.top/");
            // 续播时从上次中断位置继续（循环内保存，全部重定向后才清零）
            if (resume_offset_ > 0) {
                char range_hdr[64];
                snprintf(range_hdr, sizeof(range_hdr), "bytes=%u-", (unsigned)resume_offset_);
                http->SetHeader("Range", range_hdr);
                ESP_LOGI(TAG, "[STREAM] Resuming from offset %u", (unsigned)resume_offset_);
            } else {
                http->SetHeader("Range", "bytes=0-");
            }
            add_auth_headers(http.get());

            {
                std::lock_guard<std::mutex> lock(http_mutex_);
                active_http_ = http.get();
            }

            ESP_LOGI(TAG, "[STREAM] GET %s", url.c_str());

            if (!http->Open("GET", url)) {
                ESP_LOGE(TAG, "Failed to open HTTP stream");
                {
                    std::lock_guard<std::mutex> lock(http_mutex_);
                    active_http_ = nullptr;
                }
                break;
            }

            int status = http->GetStatusCode();

            if (status == 301 || status == 302 || status == 307 || status == 308) {
                std::string location = http->GetResponseHeader("Location");
                http->Close();
                {
                    std::lock_guard<std::mutex> lock(http_mutex_);
                    active_http_ = nullptr;
                }

                if (location.empty()) {
                    ESP_LOGE(TAG, "Redirect without Location header");
                    break;
                }

                if (location[0] == '/') {
                    size_t scheme_end = url.find("://");
                    size_t host_start = (scheme_end != std::string::npos) ? scheme_end + 3 : 0;
                    size_t host_end = url.find('/', host_start);
                    if (host_end != std::string::npos) {
                        location = url.substr(0, host_end) + location;
                    }
                }

                ESP_LOGI(TAG, "Following redirect(%d) to: %s", status, location.c_str());
                url = std::move(location);
                continue;
            }

            if (status != 200 && status != 206) {
                ESP_LOGE(TAG, "HTTP stream failed: %d", status);
                http->Close();
                {
                    std::lock_guard<std::mutex> lock(http_mutex_);
                    active_http_ = nullptr;
                }
                break;
            }

            ESP_LOGI(TAG, "[STREAM] connected HTTP %d, body_len=%u",
                     status, (unsigned)http->GetBodyLength());

#ifdef MUSIC_STREAM_DEBUG
            // ---- 网络调试: WiFi 信号 + 省电模式 ----
            {
                wifi_ap_record_t ap_info;
                if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
                    wifi_ps_type_t ps_type;
                    esp_wifi_get_ps(&ps_type);
                    ESP_LOGI(TAG, "[NET] WiFi RSSI=%d dBm ch=%d ps_type=%d (0=NONE 2=MAX_MODEM)",
                             ap_info.rssi, ap_info.primary, (int)ps_type);
                }
            }
#endif

            // 续播偏移已消费（所有重定向完成，正式开始流式读取）
            resume_offset_ = 0;

            // ---- 流式读取 + 解码 + 播放 ----
            size_t buf_pos = 0;
            bool first_frame = true;
            int total_body_bytes = 0;

#ifdef MUSIC_STREAM_DEBUG
            int64_t stream_start_us = esp_timer_get_time();
            int64_t last_speed_log_us = stream_start_us;
            int bytes_since_last_log = 0;
            static constexpr int kSpeedLogIntervalS = 5;
#endif

            bool need_more = false;  // 解码器 underflow 后强制读
            while (!stop_requested_.load()) {
                // 先尽量从HTTP读取数据填满缓冲区
                if (buf_pos < kMp3ReadBufSize / 4 || need_more) {
                    need_more = false;
                    int bytes_read = http->Read(
                        reinterpret_cast<char*>(mp3_read_buf_.data() + buf_pos),
                        kMp3ReadBufSize - buf_pos);

                    if (bytes_read < 0) {
#ifdef MUSIC_STREAM_DEBUG
                        int elapsed_s = (int)((esp_timer_get_time() - stream_start_us) / 1000000);
                        int avg_kbps = (elapsed_s > 0) ? (int)((total_body_bytes * 8LL) / (elapsed_s * 1000)) : 0;
                        ESP_LOGW(TAG, "[STREAM] Read ERR after %d s, decoded=%d total=%d buf=%d, avg=%d Kbps",
                                 elapsed_s, (int)!first_frame, total_body_bytes, (int)buf_pos, avg_kbps);
#else
                        ESP_LOGW(TAG, "[STREAM] Read ERR, decoded=%d total=%d buf=%d",
                                 (int)!first_frame, total_body_bytes, (int)buf_pos);
#endif
                        break;
                    }

                    if (bytes_read == 0) {
#ifdef MUSIC_STREAM_DEBUG
                        int elapsed_s = (int)((esp_timer_get_time() - stream_start_us) / 1000000);
                        int avg_kbps = (elapsed_s > 0) ? (int)((total_body_bytes * 8LL) / (elapsed_s * 1000)) : 0;
                        ESP_LOGI(TAG, "[STREAM] EOF after %d s, decoded=%d total=%d avg=%d Kbps",
                                 elapsed_s, (int)!first_frame, total_body_bytes, avg_kbps);
#else
                        ESP_LOGI(TAG, "[STREAM] EOF, decoded=%d total=%d buf=%d",
                                 (int)!first_frame, total_body_bytes, (int)buf_pos);
#endif
                        if (!first_frame) stream_success = true;
                        break;
                    }

                    total_body_bytes += bytes_read;
                    buf_pos += bytes_read;
                    streamed_bytes_.store((size_t)total_body_bytes, std::memory_order_relaxed);

#ifdef MUSIC_STREAM_DEBUG
                    bytes_since_last_log += bytes_read;
                    int64_t now_us = esp_timer_get_time();
                    int elapsed_s = (int)((now_us - last_speed_log_us) / 1000000);
                    if (elapsed_s >= kSpeedLogIntervalS && bytes_since_last_log > 0) {
                        int kbps = (int)((bytes_since_last_log * 8LL) / (elapsed_s * 1000));
                        ESP_LOGI(TAG, "[NET] Speed: %d Kbps (%d bytes in %d s), total=%d buf=%d%%",
                                 kbps, bytes_since_last_log, elapsed_s,
                                 total_body_bytes, (int)(buf_pos * 100 / kMp3ReadBufSize));
                        last_speed_log_us = now_us;
                        bytes_since_last_log = 0;
                    }
#endif
                }

                // 跳过 ID3v2 标签
                if (first_frame && buf_pos >= 10) {
                    if (memcmp(mp3_read_buf_.data(), "ID3", 3) == 0) {
                        uint32_t id3_size = ((mp3_read_buf_[6] & 0x7F) << 21) |
                                           ((mp3_read_buf_[7] & 0x7F) << 14) |
                                           ((mp3_read_buf_[8] & 0x7F) << 7) |
                                           (mp3_read_buf_[9] & 0x7F);
                        id3_size += 10;
                        ESP_LOGI(TAG, "[STREAM] Found ID3v2 tag, size=%u bytes", id3_size);
                        if (id3_size < buf_pos) {
                            memmove(mp3_read_buf_.data(), mp3_read_buf_.data() + id3_size,
                                    buf_pos - id3_size);
                            buf_pos -= id3_size;
                        }
                    }
                }

                // 尝试解码一帧
                unsigned char* inbuf = mp3_read_buf_.data();
                int bytes_left = static_cast<int>(buf_pos);
                int decode_result = MP3Decode(mp3_decoder_, &inbuf, &bytes_left,
                                               pcm_out_buf_.data(), 0);

                if (decode_result == ERR_MP3_NONE) {
                    MP3GetLastFrameInfo(mp3_decoder_, &mp3_frame_info_);
                    int samples = mp3_frame_info_.outputSamps;

                    if (first_frame) {
                        ESP_LOGI(TAG, "[STREAM] MP3 first frame: %dHz %dch bitrate=%d samples=%d",
                            mp3_frame_info_.samprate, mp3_frame_info_.nChans,
                            mp3_frame_info_.bitrate, samples);
                        first_frame = false;
                    }

                    WriteAudioFrames(pcm_out_buf_.data(), samples);

                    int consumed = static_cast<int>(buf_pos) - bytes_left;
                    if (consumed > 0 && consumed < static_cast<int>(buf_pos)) {
                        memmove(mp3_read_buf_.data(), mp3_read_buf_.data() + consumed, bytes_left);
                        buf_pos = bytes_left;
                    } else if (consumed >= static_cast<int>(buf_pos)) {
                        buf_pos = 0;
                    }
                    // 继续循环，尽量多解码几帧再读HTTP
                } else if (decode_result == ERR_MP3_INDATA_UNDERFLOW) {
                    if (buf_pos >= kMp3ReadBufSize - 1024) {
                        // 缓冲区已满但没有完整帧，跳过一些数据找同步字
                        int sync = MP3FindSyncWord(mp3_read_buf_.data(), static_cast<int>(buf_pos));
                        if (sync > 0) {
                            memmove(mp3_read_buf_.data(), mp3_read_buf_.data() + sync, buf_pos - sync);
                            buf_pos -= sync;
                        } else {
                            buf_pos = 0;
                        }
                    } else {
                        // 数据不足但缓冲区未满 → 强制下一轮读 HTTP
                        need_more = true;
                    }
                } else {
                    static int decode_err_cnt = 0;
                    int sync_offset = -1;
                    if (buf_pos > 4) {
                        sync_offset = MP3FindSyncWord(mp3_read_buf_.data(), static_cast<int>(buf_pos));
                    }
                    if (++decode_err_cnt <= 5) {
                        ESP_LOGW(TAG, "[STREAM] MP3Decode err=%d buf=%d sync=%d",
                                 decode_result, (int)buf_pos, sync_offset);
                    }
                    if (buf_pos > 4) {
                        if (sync_offset >= 0) {
                            memmove(mp3_read_buf_.data(), mp3_read_buf_.data() + sync_offset,
                                    buf_pos - sync_offset);
                            buf_pos -= sync_offset;
                        } else {
                            buf_pos = 0;
                        }
                    }
                }
            }  // while !stop_requested

            http->Close();
            {
                std::lock_guard<std::mutex> lock(http_mutex_);
                active_http_ = nullptr;
            }

            if (stream_success || stop_requested_.load()) {
                break;  // 正常结束或被停止
            }
            break;  // 流失败，跳到外层重试
        }  // for redirect

        if (stream_success || stop_requested_.load()) {
            break;
        }
        ESP_LOGI(TAG, "Retry loop: stream_success=%d stop=%d, will retry",
                 (int)stream_success, (int)stop_requested_.load());
    }  // for retry

    // 标记结束原因：重试耗尽且非用户停止 = 网络错误
    if (!stream_success && !stop_requested_.load()) {
        end_reason_ = EndReason::Error;
        ESP_LOGW(TAG, "All retries exhausted, mark as Error");
    }

    // 禁用音频输出
    auto codec = Board::GetInstance().GetAudioCodec();
    if (codec) {
        codec->EnableOutput(false);
    }
}

void Esp32Music::WriteAudioFrames(const int16_t* pcm_data, size_t frame_count) {
    auto codec = Board::GetInstance().GetAudioCodec();
    if (!codec) return;

    static bool output_enabled = false;
    if (!output_enabled) {
        codec->EnableOutput(true);
        output_enabled = true;
    }

    // 立体声 → 单声道混音 + 采样率转换（44100→24000）
    size_t in_samples = frame_count;
    if (mp3_frame_info_.nChans == 2) {
        // stereo → mono first
        size_t mono_count = frame_count / 2;
        for (size_t i = 0; i < mono_count; i++) {
            int32_t mixed = (int32_t)pcm_data[i * 2] + (int32_t)pcm_data[i * 2 + 1];
            pcm_out_buf_[i] = (int16_t)(mixed / 2);
        }
        in_samples = mono_count;
        pcm_data = pcm_out_buf_.data();
    }

    // 44100 → 24000 降采样（固定点线性插值）
    const int in_rate = mp3_frame_info_.samprate;
    constexpr int out_rate = 24000;
    if (in_rate != out_rate) {
        size_t out_samples = (size_t)((uint64_t)in_samples * out_rate / in_rate);
        for (size_t i = 0; i < out_samples; i++) {
            uint64_t src_idx = (uint64_t)i * in_rate / out_rate;
            size_t src_i = (size_t)src_idx;
            if (src_i + 1 < in_samples) {
                // linear interpolation
                int32_t a = pcm_data[src_i];
                int32_t b = pcm_data[src_i + 1];
                uint64_t frac = ((uint64_t)i * in_rate) % out_rate;
                int32_t val = (int32_t)(a + ((b - a) * (int64_t)frac / out_rate));
                pcm_out_buf_[i] = (int16_t)val;
            } else {
                pcm_out_buf_[i] = pcm_data[src_i];
            }
        }
        codec->WriteRaw(pcm_out_buf_.data(), out_samples);
    } else {
        codec->WriteRaw(pcm_data, in_samples);
    }
}

// ---- MP3 解码器 ----

bool Esp32Music::InitMp3Decoder() {
    if (mp3_decoder_) return true;

    mp3_decoder_ = MP3InitDecoder();
    if (!mp3_decoder_) {
        ESP_LOGE(TAG, "MP3InitDecoder failed");
        return false;
    }
    return true;
}

void Esp32Music::CleanupMp3Decoder() {
    if (mp3_decoder_) {
        MP3FreeDecoder(mp3_decoder_);
        mp3_decoder_ = nullptr;
    }
}

// ---- 工厂函数 ----

Music* CreateMusic() {
    return new Esp32Music();
}
