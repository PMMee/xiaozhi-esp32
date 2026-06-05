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
#include <mbedtls/sha256.h>

#define TAG "Esp32Music"

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

void Esp32Music::Stop() {
    if (!playing_.load()) return;

    ESP_LOGI(TAG, "Stopping music playback");
    stop_requested_.store(true);

    // 中断 HTTP 连接
    {
        std::lock_guard<std::mutex> lock(http_mutex_);
        if (active_http_) {
            active_http_->Close();
            active_http_ = nullptr;
        }
    }

    // 等待任务退出（最多 2 秒）
    if (music_task_handle_) {
        TaskHandle_t task = music_task_handle_;
        music_task_handle_ = nullptr;

        int wait_ms = 0;
        while (eTaskGetState(task) != eDeleted && wait_ms < 2000) {
            vTaskDelay(pdMS_TO_TICKS(50));
            wait_ms += 50;
        }
        if (eTaskGetState(task) != eDeleted) {
            vTaskDelete(task);
        }
    }

    CleanupMp3Decoder();
    playing_.store(false);
    stop_requested_.store(false);
}

bool Esp32Music::IsPlaying() const {
    return playing_.load();
}

std::string Esp32Music::Search(const std::string& song_name, const std::string& artist_name) {
    cJSON* response_json = SearchMusicInternal(song_name, artist_name);
    if (!response_json) {
        return "{\"success\": false, \"message\": \"搜索失败\"}";
    }

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
    std::string ret(json_str);
    cJSON_free(json_str);
    cJSON_Delete(result);
    return ret;
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
    std::string base_url = "http://shybot.top/v2/music/api/?shykey=6df7c755ae9f9fb598572b34194a8b060fb5fb611dcb20f783f1a7a9ea6937aa&type=qq";
    std::string full_url;

    if (!song_name.empty()) {
        full_url = base_url + "&name=" + url_encode(song_name);
    } else {
        full_url = base_url + "&singer=" + url_encode(artist_name);
    }

    auto network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(0);
    if (!http) {
        ESP_LOGE(TAG, "Failed to create HTTP client");
        return nullptr;
    }

    http->SetTimeout(10000);
    http->SetHeader("User-Agent", "ESP32-Music-Player/1.0");
    http->SetHeader("Accept", "application/json");
    add_auth_headers(http.get());

    ESP_LOGI(TAG, "[SEARCH] URL: %s", full_url.c_str());
    ESP_LOGI(TAG, "[SEARCH] Connecting to: %s", full_url.c_str());

    if (!http->Open("GET", full_url)) {
        ESP_LOGE(TAG, "[SEARCH] Failed to connect to music API");
        return nullptr;
    }

    int status_code = http->GetStatusCode();
    ESP_LOGI(TAG, "[SEARCH] HTTP status: %d", status_code);
    if (status_code != 200) {
        ESP_LOGE(TAG, "[SEARCH] HTTP GET failed: %d", status_code);
        http->Close();
        return nullptr;
    }

    std::string response_data = http->ReadAll();
    http->Close();

    ESP_LOGI(TAG, "[SEARCH] Response size: %d bytes", (int)response_data.size());
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

    // 搜索歌曲（在 Task 中异步执行，不阻塞主循环）
    cJSON* response_json = self->SearchMusicInternal(song, artist);
    if (!response_json) {
        ESP_LOGE(TAG, "[TASK] Search failed for: %s", song.c_str());
        goto exit_task;
    }

    {
        cJSON* selected = select_song_by_artist(response_json, artist);
        if (!selected) {
            ESP_LOGE(TAG, "[TASK] No song found in results");
            cJSON_Delete(response_json);
            goto exit_task;
        }

        cJSON* title_json = cJSON_GetObjectItem(selected, "name");
        cJSON* artist_json = cJSON_GetObjectItem(selected, "singer");
        cJSON* url_json = cJSON_GetObjectItem(selected, "url");

        const char* title_str = cJSON_IsString(title_json) ? title_json->valuestring : "?";
        const char* artist_str = cJSON_IsString(artist_json) ? artist_json->valuestring : "?";
        const char* url_str = (cJSON_IsString(url_json) && url_json->valuestring) ? url_json->valuestring : "";

        ESP_LOGI(TAG, "[TASK] Selected: '%s' - '%s', url=%s", title_str, artist_str, url_str);

        // 打印选中歌曲的完整 JSON
        char* item_json = cJSON_PrintUnformatted(selected);
        ESP_LOGI(TAG, "[TASK] Item JSON: %s", item_json ? item_json : "null");
        cJSON_free(item_json);

        if (cJSON_IsString(title_json)) {
            std::lock_guard<std::mutex> lock(self->info_mutex_);
            self->current_song_name_ = title_json->valuestring;
        }
        if (cJSON_IsString(artist_json)) {
            std::lock_guard<std::mutex> lock(self->info_mutex_);
            self->current_artist_name_ = artist_json->valuestring;
        }

        if (url_str[0] == '\0') {
            ESP_LOGE(TAG, "[TASK] No audio URL in search result");
            cJSON_Delete(response_json);
            goto exit_task;
        }

        std::string music_url = url_str;
        if (music_url.find('?') == std::string::npos) {
            music_url += "?";
        } else {
            music_url += "&";
        }
        music_url += "shykey=6df7c755ae9f9fb598572b34194a8b060fb5fb611dcb20f783f1a7a9ea6937aa";

        ESP_LOGI(TAG, "[TASK] Final stream URL: %s", music_url.c_str());

        cJSON_Delete(response_json);

        {
            std::lock_guard<std::mutex> lock(self->info_mutex_);
            self->current_music_url_ = std::move(music_url);
        }

        ESP_LOGI(TAG, "[TASK] Starting stream: %s - %s",
            self->current_song_name_.c_str(), self->current_artist_name_.c_str());
    }

    self->MusicTaskLoop();

exit_task:
    ESP_LOGI(TAG, "Music task exiting");

    // 通知 Application 重新连接
    Application::GetInstance().Schedule([self]() {
        self->playing_.store(false);
        self->CleanupMp3Decoder();
        ESP_LOGI(TAG, "Music playback finished, triggering reconnect...");

        // 通知显示
        auto display = Board::GetInstance().GetDisplay();
        if (display) {
            display->ShowNotification("播放完毕");
        }

        // 重建协议连接（Reconnect 内部会触发 wake word 进入对话）
        auto& app = Application::GetInstance();
        app.Reconnect();
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

            http->SetTimeout(15000);
            http->SetKeepAlive(true);
            http->SetHeader("User-Agent", "ESP32-Music-Player/1.0");
            http->SetHeader("Accept", "*/*");
            http->SetHeader("Referer", "http://shybot.top/");
            http->SetHeader("Range", "bytes=0-");
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

            // 流式读取 + 解码 + 播放
            // 策略：先尽量填满缓冲区，然后批量解码多帧写入I2S，
            // 只在缓冲区不足时再读HTTP，避免串行读-解码-写造成的音频间隙
            size_t buf_pos = 0;
            bool first_frame = true;
            int total_body_bytes = 0;

            while (!stop_requested_.load()) {
                // 先尽量从HTTP读取数据填满缓冲区
                if (buf_pos < kMp3ReadBufSize / 4) {
                    int bytes_read = http->Read(
                        reinterpret_cast<char*>(mp3_read_buf_.data() + buf_pos),
                        kMp3ReadBufSize - buf_pos);

                    if (bytes_read < 0) {
                        ESP_LOGW(TAG, "[STREAM] Read ERR, decoded=%d total=%d buf=%d",
                                 (int)!first_frame, total_body_bytes, (int)buf_pos);
                        break;
                    }

                    if (bytes_read == 0) {
                        ESP_LOGI(TAG, "[STREAM] EOF, decoded=%d total=%d buf=%d",
                                 (int)!first_frame, total_body_bytes, (int)buf_pos);
                        if (!first_frame) stream_success = true;
                        break;
                    }

                    total_body_bytes += bytes_read;
                    buf_pos += bytes_read;
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
                    // 数据不足，不延迟，立即回循环顶部读HTTP
                    if (buf_pos >= kMp3ReadBufSize - 1024) {
                        // 缓冲区已满但没有完整帧，跳过一些数据找同步字
                        int sync = MP3FindSyncWord(mp3_read_buf_.data(), static_cast<int>(buf_pos));
                        if (sync > 0) {
                            memmove(mp3_read_buf_.data(), mp3_read_buf_.data() + sync, buf_pos - sync);
                            buf_pos -= sync;
                        } else {
                            buf_pos = 0;
                        }
                    }
                    // buf_pos < threshold，继续读HTTP
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
