#include "esp32_music.h"
#include "board.h"
#include "system_info.h"
#include "audio/audio_codec.h"
#include "application.h"
#include "protocols/protocol.h"
#include "display/display.h"

#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_pthread.h>
#include <esp_timer.h>
#include <mbedtls/sha256.h>
#include <cJSON.h>
#include <cstring>
#include <chrono>
#include <sstream>
#include <algorithm>
#include <cctype>  // 为isdigit函数
#include <thread>   // 为线程ID比较
#include <system_error>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "Esp32Music"

// ========== 简单的ESP32认证函数 ==========

/**
 * @brief 获取设备MAC地址
 * @return MAC地址字符串
 */
static std::string get_device_mac() {
    return SystemInfo::GetMacAddress();
}

/**
 * @brief 获取设备芯片ID
 * @return 芯片ID字符串
 */
static std::string get_device_chip_id() {
    // 使用MAC地址作为芯片ID，去除冒号分隔符
    std::string mac = SystemInfo::GetMacAddress();
    // 去除所有冒号
    mac.erase(std::remove(mac.begin(), mac.end(), ':'), mac.end());
    return mac;
}

/**
 * @brief 生成动态密钥
 * @param timestamp 时间戳
 * @return 动态密钥字符串
 */
static std::string generate_dynamic_key(int64_t timestamp) {
    // 密钥（请修改为与服务端一致）
    const std::string secret_key = "your-esp32-secret-key-2024";
    
    // 获取设备信息
    std::string mac = get_device_mac();
    std::string chip_id = get_device_chip_id();
    
    // 组合数据：MAC:芯片ID:时间戳:密钥
    std::string data = mac + ":" + chip_id + ":" + std::to_string(timestamp) + ":" + secret_key;
    
    // SHA256哈希
    unsigned char hash[32];
    mbedtls_sha256((unsigned char*)data.c_str(), data.length(), hash, 0);
    
    // 转换为十六进制字符串（前16字节）
    std::string key;
    for (int i = 0; i < 16; i++) {
        char hex[3];
        snprintf(hex, sizeof(hex), "%02X", hash[i]);
        key += hex;
    }
    
    return key;
}

/**
 * @brief 为HTTP请求添加认证头
 * @param http HTTP客户端指针
 */
static void add_auth_headers(Http* http) {
    // 获取当前时间戳
    int64_t timestamp = esp_timer_get_time() / 1000000;  // 转换为秒
    
    // 生成动态密钥
    std::string dynamic_key = generate_dynamic_key(timestamp);
    
    // 获取设备信息
    std::string mac = get_device_mac();
    std::string chip_id = get_device_chip_id();
    
    // 添加认证头
    if (http) {
        http->SetHeader("X-MAC-Address", mac);
        http->SetHeader("X-Chip-ID", chip_id);
        http->SetHeader("X-Timestamp", std::to_string(timestamp));
        http->SetHeader("X-Dynamic-Key", dynamic_key);
        
        ESP_LOGI(TAG, "Added auth headers - MAC: %s, ChipID: %s, Timestamp: %lld", 
                 mac.c_str(), chip_id.c_str(), timestamp);
    }
}

// URL编码函数
static std::string url_encode(const std::string& str) {
    std::string encoded;
    char hex[4];
    
    for (size_t i = 0; i < str.length(); i++) {
        unsigned char c = str[i];
        
        if ((c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            encoded += c;
        } else if (c == ' ') {
            encoded += '+';  // 空格编码为'+'或'%20'
        } else {
            snprintf(hex, sizeof(hex), "%%%02X", c);
            encoded += hex;
        }
    }
    return encoded;
}

// 在文件开头添加一个辅助函数，统一处理URL构建
static std::string buildUrlWithParams(const std::string& base_url, const std::string& path, const std::string& query) {
    std::string result_url = base_url + path + "?";
    size_t pos = 0;
    size_t amp_pos = 0;
    
    while ((amp_pos = query.find("&", pos)) != std::string::npos) {
        std::string param = query.substr(pos, amp_pos - pos);
        size_t eq_pos = param.find("=");
        
        if (eq_pos != std::string::npos) {
            std::string key = param.substr(0, eq_pos);
            std::string value = param.substr(eq_pos + 1);
            result_url += key + "=" + url_encode(value) + "&";
        } else {
            result_url += param + "&";
        }
        
        pos = amp_pos + 1;
    }
    
    // 处理最后一个参数
    std::string last_param = query.substr(pos);
    size_t eq_pos = last_param.find("=");
    
    if (eq_pos != std::string::npos) {
        std::string key = last_param.substr(0, eq_pos);
        std::string value = last_param.substr(eq_pos + 1);
        result_url += key + "=" + url_encode(value);
    } else {
        result_url += last_param;
    }
    
    return result_url;
}

Esp32Music::Esp32Music() : last_downloaded_data_(), current_music_url_(), current_song_name_(),
                         song_name_displayed_(false), current_lyric_url_(), lyrics_(), 
                         current_lyric_index_(-1), lyric_thread_(), is_lyric_running_(false),
                         display_mode_(DISPLAY_MODE_LYRICS), is_playing_(false), is_downloading_(false), 
                         play_thread_(), download_thread_(), audio_buffer_(), buffer_mutex_(), 
                         buffer_cv_(), buffer_size_(0), mp3_decoder_(nullptr), mp3_frame_info_(), 
                         mp3_decoder_initialized_(false) {
    ESP_LOGI(TAG, "Music player initialized with default spectrum display mode");
    InitializeMp3Decoder();
}

Esp32Music::~Esp32Music() {
    ESP_LOGI(TAG, "Destroying music player - stopping all operations");
    
    // 停止所有操作
    is_downloading_ = false;
    is_playing_ = false;
    is_lyric_running_ = false;
    
    // 通知所有等待的线程
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        buffer_cv_.notify_all();
    }
    
    // 等待下载线程结束，设置5秒超时
    if (download_thread_.joinable()) {
        ESP_LOGI(TAG, "Waiting for download thread to finish (timeout: 5s)");
        auto start_time = std::chrono::steady_clock::now();
        
        // 等待线程结束
        bool thread_finished = false;
        while (!thread_finished) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - start_time).count();
            
            if (elapsed >= 5) {
                ESP_LOGW(TAG, "Download thread join timeout after 5 seconds");
                break;
            }
            
            // 再次设置停止标志，确保线程能够检测到
            is_downloading_ = false;
            
            // 通知条件变量
            {
                std::lock_guard<std::mutex> lock(buffer_mutex_);
                buffer_cv_.notify_all();
            }
            
            // 检查线程是否已经结束
            if (!download_thread_.joinable()) {
                thread_finished = true;
            }
            
            // 定期打印等待信息
            if (elapsed > 0 && elapsed % 1 == 0) {
                ESP_LOGI(TAG, "Still waiting for download thread to finish... (%ds)", (int)elapsed);
            }
        }
        
        if (download_thread_.joinable()) {
            download_thread_.join();
        }
        ESP_LOGI(TAG, "Download thread finished");
    }
    
    // 等待播放线程结束，设置3秒超时
    if (play_thread_.joinable()) {
        ESP_LOGI(TAG, "Waiting for playback thread to finish (timeout: 3s)");
        auto start_time = std::chrono::steady_clock::now();
        
        bool thread_finished = false;
        while (!thread_finished) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - start_time).count();
            
            if (elapsed >= 3) {
                ESP_LOGW(TAG, "Playback thread join timeout after 3 seconds");
                break;
            }
            
            // 再次设置停止标志
            is_playing_ = false;
            
            // 通知条件变量
            {
                std::lock_guard<std::mutex> lock(buffer_mutex_);
                buffer_cv_.notify_all();
            }
            
            // 检查线程是否已经结束
            if (!play_thread_.joinable()) {
                thread_finished = true;
            }
        }
        
        if (play_thread_.joinable()) {
            play_thread_.join();
        }
        ESP_LOGI(TAG, "Playback thread finished");
    }
    
    // 等待歌词线程结束
    if (lyric_thread_.joinable()) {
        ESP_LOGI(TAG, "Waiting for lyric thread to finish");
        lyric_thread_.join();
        ESP_LOGI(TAG, "Lyric thread finished");
    }
    
    // 清理缓冲区和MP3解码器
    ClearAudioBuffer();
    CleanupMp3Decoder();
    
    ESP_LOGI(TAG, "Music player destroyed successfully");
}

bool Esp32Music::Download(const std::string& song_name, const std::string& artist_name) {
    ESP_LOGI(TAG, "小智开源音乐固件qq交流群:826072986");
    ESP_LOGI(TAG, "Starting to get music details for: %s", song_name.c_str());
    
    // 清空之前的下载数据
    last_downloaded_data_.clear();
    
    // 保存歌名用于后续显示
    current_song_name_ = song_name;
    
    // 第一步：请求stream_pcm接口获取音频信息
    std::string base_url = "http://www.xiaozhishop.xyz:5005";
    std::string full_url = base_url + "/stream_pcm?song=" + url_encode(song_name) + "&artist=" + url_encode(artist_name);
    
    ESP_LOGI(TAG, "Request URL: %s", full_url.c_str());
    
    // 使用Board提供的HTTP客户端
    auto network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(0);
    
    // 设置基本请求头
    http->SetHeader("User-Agent", "ESP32-Music-Player/1.0");
    http->SetHeader("Accept", "application/json");
    
    // 添加ESP32认证头
    add_auth_headers(http.get());
    
    // 打开GET连接
    if (!http->Open("GET", full_url)) {
        ESP_LOGE(TAG, "Failed to connect to music API");
        return false;
    }
    
    // 检查响应状态码
    int status_code = http->GetStatusCode();
    if (status_code != 200) {
        ESP_LOGE(TAG, "HTTP GET failed with status code: %d", status_code);
        http->Close();
        return false;
    }
    
    // 读取响应数据
    last_downloaded_data_ = http->ReadAll();
    http->Close();
    
    ESP_LOGI(TAG, "HTTP GET Status = %d, content_length = %d", status_code, last_downloaded_data_.length());
    ESP_LOGD(TAG, "Complete music details response: %s", last_downloaded_data_.c_str());
    
    // 简单的认证响应检查（可选）
    if (last_downloaded_data_.find("ESP32动态密钥验证失败") != std::string::npos) {
        ESP_LOGE(TAG, "Authentication failed for song: %s", song_name.c_str());
        return false;
    }
    
    if (!last_downloaded_data_.empty()) {
        // 诊断：打印API返回的前128字节，便于确认是否返回了错误文本或HTML
        size_t preview_len = std::min(last_downloaded_data_.size(), (size_t)128);
        std::string preview = last_downloaded_data_.substr(0, preview_len);
        ESP_LOGD(TAG, "API response preview (%d bytes): %s", preview_len, preview.c_str());

        // 解析响应JSON以提取音频URL
        cJSON* response_json = cJSON_Parse(last_downloaded_data_.c_str());
        if (response_json) {
            // 提取关键信息
            cJSON* artist = cJSON_GetObjectItem(response_json, "artist");
            cJSON* title = cJSON_GetObjectItem(response_json, "title");
            cJSON* audio_url = cJSON_GetObjectItem(response_json, "audio_url");
            cJSON* lyric_url = cJSON_GetObjectItem(response_json, "lyric_url");
            
            if (cJSON_IsString(artist)) {
                ESP_LOGI(TAG, "Artist: %s", artist->valuestring);
            }
            if (cJSON_IsString(title)) {
                ESP_LOGI(TAG, "Title: %s", title->valuestring);
            }
            
            // 检查audio_url是否有效
            if (cJSON_IsString(audio_url) && audio_url->valuestring && strlen(audio_url->valuestring) > 0) {
                ESP_LOGI(TAG, "Audio URL path: %s", audio_url->valuestring);
                
                // 第二步：拼接完整的音频下载URL，确保对audio_url进行URL编码
                std::string audio_path = audio_url->valuestring;
                
                // 使用统一的URL构建功能
                if (audio_path.find("?") != std::string::npos) {
                    size_t query_pos = audio_path.find("?");
                    std::string path = audio_path.substr(0, query_pos);
                    std::string query = audio_path.substr(query_pos + 1);
                    
                    current_music_url_ = buildUrlWithParams(base_url, path, query);
                } else {
                    current_music_url_ = base_url + audio_path;
                }

                // 诊断日志：打印最终构建的音乐 URL（便于复制到浏览器验证）
                ESP_LOGI(TAG, "Built music URL: %s", current_music_url_.c_str());
                // 如果返回了 cover_url，尝试异步下载并设置为预览图
                cJSON* cover_url = cJSON_GetObjectItem(response_json, "cover_url");
                if (cJSON_IsString(cover_url) && cover_url->valuestring && strlen(cover_url->valuestring) > 0) {
                    std::string cover = cover_url->valuestring;
                    ESP_LOGI(TAG, "Found cover URL: %s", cover.c_str());

                    // 异步下载封面，避免阻塞主流程
                    std::thread([cover](){
                        auto& board = Board::GetInstance();
                        auto network = board.GetNetwork();
                        auto http = network->CreateHttp(0);
                        if (!http) {
                            ESP_LOGW(TAG, "Cover download: failed to create HTTP client");
                            return;
                        }
                        http->SetHeader("User-Agent", "ESP32-Music-Player/1.0");
                        if (!http->Open("GET", cover)) {
                            ESP_LOGW(TAG, "Cover download: failed to open %s", cover.c_str());
                            http->Close();
                            return;
                        }
                        int status = http->GetStatusCode();
                        if (status < 200 || status >= 300) {
                            ESP_LOGW(TAG, "Cover download HTTP status %d for %s", status, cover.c_str());
                            http->Close();
                            return;
                        }

                        // 读取到 SPIRAM 缓冲
                        size_t max_size = 64 * 1024; // 限制 64KB
                        uint8_t* buf = (uint8_t*)heap_caps_malloc(max_size, MALLOC_CAP_SPIRAM);
                        if (!buf) {
                            ESP_LOGE(TAG, "Cover download: failed to allocate SPIRAM buffer");
                            http->Close();
                            return;
                        }

                        size_t total = 0;
                        while (true) {
                            int r = http->Read((char*)(buf + total), (int)(max_size - total));
                            if (r > 0) {
                                total += (size_t)r;
                                if (total >= max_size) break;
                            } else if (r == 0) {
                                break;
                            } else {
                                ESP_LOGW(TAG, "Cover download: read error %d", r);
                                break;
                            }
                        }
                        http->Close();

                        if (total == 0) {
                            ESP_LOGW(TAG, "Cover download: no data received");
                            heap_caps_free(buf);
                            return;
                        }

                        // 通过 Display API 设置预览图（调用方负责释放 buf）
                        auto& board2 = Board::GetInstance();
                        auto display = board2.GetDisplay();
                        if (display) {
                            if (display->SetPreviewImageFromMemory(buf, total)) {
                                ESP_LOGI(TAG, "Cover download: preview image set, size=%d bytes", (int)total);
                                // display made an internal copy; we can free our downloaded buffer
                                heap_caps_free(buf);
                            } else {
                                ESP_LOGW(TAG, "Cover download: display rejected image buffer");
                                heap_caps_free(buf);
                            }
                        } else {
                            ESP_LOGW(TAG, "Cover download: no display available");
                            heap_caps_free(buf);
                        }
                    }).detach();
                }
                ESP_LOGI(TAG, "小智开源音乐固件qq交流群:826072986");
                ESP_LOGI(TAG, "Starting streaming playback for: %s", song_name.c_str());
                song_name_displayed_ = false;  // 重置歌名显示标志
                StartStreaming(current_music_url_);
                
                // 处理歌词URL - 只有在歌词显示模式下才启动歌词
                if (cJSON_IsString(lyric_url) && lyric_url->valuestring && strlen(lyric_url->valuestring) > 0) {
                    // 拼接完整的歌词下载URL，使用相同的URL构建逻辑
                    std::string lyric_path = lyric_url->valuestring;
                    if (lyric_path.find("?") != std::string::npos) {
                        size_t query_pos = lyric_path.find("?");
                        std::string path = lyric_path.substr(0, query_pos);
                        std::string query = lyric_path.substr(query_pos + 1);
                        
                        current_lyric_url_ = buildUrlWithParams(base_url, path, query);
                    } else {
                        current_lyric_url_ = base_url + lyric_path;
                    }

                    // 诊断日志：打印最终构建的歌词 URL
                    ESP_LOGI(TAG, "Built lyric URL: %s", current_lyric_url_.c_str());
                    
                    // 根据显示模式决定是否启动歌词
                    if (display_mode_ == DISPLAY_MODE_LYRICS) {
                        ESP_LOGI(TAG, "Loading lyrics for: %s (lyrics display mode)", song_name.c_str());
                        
                        // 启动歌词下载和显示
                        if (is_lyric_running_) {
                            is_lyric_running_ = false;
                            if (lyric_thread_.joinable()) {
                                lyric_thread_.join();
                            }
                        }
                        
                        is_lyric_running_ = true;
                        current_lyric_index_ = -1;
                        lyrics_.clear();

                        // 创建歌词线程时捕获异常（可能由于内存不足导致pthread创建失败）
                        // 在创建前记录堆使用情况，尝试一次使用较小的 pthread 栈作为回退
                        size_t free_before = esp_get_free_heap_size();
                        size_t min_free_before = esp_get_minimum_free_heap_size();
                        ESP_LOGI(TAG, "Attempting to create lyric thread - free_heap=%u, min_free_heap=%u", (unsigned)free_before, (unsigned)min_free_before);

                        bool lyric_thread_created = false;
                        try {
                            lyric_thread_ = std::thread(&Esp32Music::LyricDisplayThread, this);
                            lyric_thread_created = true;
                        } catch (const std::system_error& e) {
                            ESP_LOGW(TAG, "Initial lyric thread creation failed: %s", e.what());
                        }

                        if (!lyric_thread_created) {
                            // 尝试短延迟后用更小的 pthread 栈重试一次
                            vTaskDelay(pdMS_TO_TICKS(100));
                            // 备份并临时设置较小的 pthread 默认配置以降低创建线程时的栈需求
                            esp_pthread_cfg_t orig_cfg = esp_pthread_get_default_config();
                            esp_pthread_cfg_t safe_cfg = orig_cfg;
                            // 使用安全的最小栈（至少 8KB），避免栈溢出
                            size_t safe_stack = std::max((size_t)orig_cfg.stack_size, (size_t)8192);
                            safe_cfg.stack_size = safe_stack;
                            // 保持或降低优先级以避免抢占
                            int orig_prio = static_cast<int>(orig_cfg.prio);
                            safe_cfg.prio = (orig_prio > 1) ? (orig_prio - 1) : 1;
                            esp_pthread_set_cfg(&safe_cfg);

                            size_t free_mid = esp_get_free_heap_size();
                            ESP_LOGI(TAG, "Retrying lyric thread creation with safe stack=%u - free_heap=%u", (unsigned)safe_stack, (unsigned)free_mid);

                            try {
                                lyric_thread_ = std::thread(&Esp32Music::LyricDisplayThread, this);
                                lyric_thread_created = true;
                                ESP_LOGI(TAG, "Lyric thread created with safe stack");
                            } catch (const std::system_error& e) {
                                size_t free_after = esp_get_free_heap_size();
                                size_t min_free_after = esp_get_minimum_free_heap_size();
                                ESP_LOGE(TAG, "Failed to create lyric thread after retry: %s; free_before=%u free_after=%u min_free_before=%u min_free_after=%u",
                                        e.what(), (unsigned)free_before, (unsigned)free_after, (unsigned)min_free_before, (unsigned)min_free_after);
                                lyric_thread_created = false;
                            }

                            // 恢复原始 pthread 配置
                            esp_pthread_set_cfg(&orig_cfg);
                        }

                        if (!lyric_thread_created) {
                            ESP_LOGW(TAG, "Giving up lyric thread creation - lyrics will not be displayed for this track");
                            is_lyric_running_ = false;
                        }
                    } else {
                        ESP_LOGI(TAG, "Lyric URL found but spectrum display mode is active, skipping lyrics");
                    }
                } else {
                    ESP_LOGW(TAG, "No lyric URL found for this song");
                }
                
                    cJSON_Delete(response_json);
                return true;
            } else {
                // audio_url为空或无效
                ESP_LOGE(TAG, "Audio URL not found or empty for song: %s", song_name.c_str());
                ESP_LOGE(TAG, "Failed to find music: 没有找到歌曲 '%s'", song_name.c_str());
                    cJSON_Delete(response_json);
                return false;
            }
        } else {
            ESP_LOGE(TAG, "Failed to parse JSON response");
        }
    } else {
        ESP_LOGE(TAG, "Empty response from music API");
    }
    
    return false;
}



std::string Esp32Music::GetDownloadResult() {
    return last_downloaded_data_;
}

// 开始流式播放
bool Esp32Music::StartStreaming(const std::string& music_url) {
    if (music_url.empty()) {
        ESP_LOGE(TAG, "Music URL is empty");
        return false;
    }
    
    ESP_LOGD(TAG, "Starting streaming for URL: %s", music_url.c_str());
    
    // 停止之前的播放和下载
    is_downloading_ = false;
    is_playing_ = false;
    
    // 等待之前的线程完全结束
    if (download_thread_.joinable()) {
        {
            std::lock_guard<std::mutex> lock(buffer_mutex_);
            buffer_cv_.notify_all();  // 通知线程退出
        }
        download_thread_.join();
    }
    if (play_thread_.joinable()) {
        {
            std::lock_guard<std::mutex> lock(buffer_mutex_);
            buffer_cv_.notify_all();  // 通知线程退出
        }
        play_thread_.join();
    }
    
    // 清空缓冲区
    ClearAudioBuffer();
    // 在开始播放前暂停显示层动画（例如 GIF），以避免渲染时阻塞导致音频卡顿
    {
        auto& board = Board::GetInstance();
        auto display = board.GetDisplay();
        if (display) {
            display->PauseAnimations();
            ESP_LOGI(TAG, "Paused display animations before starting streaming");
        }
    }
    
    // 在创建线程前记录堆快照，帮助诊断 pthread 创建失败问题
    size_t free_before = esp_get_free_heap_size();
    size_t min_free_before = esp_get_minimum_free_heap_size();
    ESP_LOGI(TAG, "Creating streaming threads - free_heap=%u, min_free_heap=%u", (unsigned)free_before, (unsigned)min_free_before);

    // 备份原始 pthread 配置，并使用安全的栈大小（至少 8KB 或原始值）以避免栈溢出
    esp_pthread_cfg_t orig_cfg = esp_pthread_get_default_config();
    esp_pthread_cfg_t cfg = orig_cfg;
    size_t safe_stack = std::max((size_t)orig_cfg.stack_size, (size_t)8192);
    cfg.stack_size = safe_stack;  // 使用安全栈大小
    cfg.prio = 5;                // 中等优先级
    cfg.thread_name = "audio_stream";
    esp_pthread_set_cfg(&cfg);

    bool download_created = false;
    bool play_created = false;

    // 创建下载线程（带异常保护）
    try {
        is_downloading_ = true;
        download_thread_ = std::thread(&Esp32Music::DownloadAudioStream, this, music_url);
        download_created = true;
    } catch (const std::system_error& e) {
        ESP_LOGW(TAG, "Failed to create download thread: %s", e.what());
        is_downloading_ = false;
        download_created = false;
    }

    // 创建播放线程（带异常保护）
    try {
        is_playing_ = true;
        play_thread_ = std::thread(&Esp32Music::PlayAudioStream, this);
        play_created = true;
    } catch (const std::system_error& e) {
        ESP_LOGW(TAG, "Failed to create play thread: %s", e.what());
        is_playing_ = false;
        play_created = false;
    }

    // 无论创建是否成功，都应立即恢复原始 pthread 配置，避免将全局默认置为过小的栈导致其他任务崩溃
    esp_pthread_set_cfg(&orig_cfg);

    // 如果任一线程创建失败，记录并放弃（不要回退到更小的栈，回退会导致栈溢出/崩溃）
    if (!download_created || !play_created) {
        ESP_LOGW(TAG, "Streaming thread creation failed - download=%d play=%d; free_before=%u min_free_before=%u", (int)download_created, (int)play_created, (unsigned)free_before, (unsigned)min_free_before);
        // 恢复原始 pthread 配置以保持系统稳定
        esp_pthread_set_cfg(&orig_cfg);

        // 清理可能部分创建的线程
        if (download_created && download_thread_.joinable()) {
            download_thread_.join();
        }
        if (play_created && play_thread_.joinable()) {
            play_thread_.join();
        }
        // 恢复显示动画（因为开始播放前可能已暂停）
        {
            auto& board = Board::GetInstance();
            auto display = board.GetDisplay();
            if (display) {
                display->ResumeAnimations();
                ESP_LOGI(TAG, "Resumed display animations after failed StartStreaming");
            }
        }
        return false;
    }

    // 恢复原始 pthread 配置，确保系统其他线程仍使用原始设置
    esp_pthread_set_cfg(&orig_cfg);
    
    ESP_LOGI(TAG, "Streaming threads started successfully");
    
    return true;
}

// 停止流式播放
bool Esp32Music::StopStreaming() {
    ESP_LOGI(TAG, "Stopping music streaming - current state: downloading=%d, playing=%d", 
            is_downloading_.load(), is_playing_.load());

    // 重置采样率到原始值
    ResetSampleRate();
    
    // 检查是否有流式播放正在进行
    if (!is_playing_ && !is_downloading_) {
        ESP_LOGW(TAG, "No streaming in progress");
        // 如果之前暂停了动画，恢复它
        {
            auto& board = Board::GetInstance();
            auto display = board.GetDisplay();
            if (display) {
                display->ResumeAnimations();
                ESP_LOGI(TAG, "Resumed display animations (no streaming in progress)");
            }
        }
        return true;
    }
    
    // 停止下载和播放标志
    is_downloading_ = false;
    is_playing_ = false;
    
    // 清空歌名显示
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    if (display) {
        display->SetMusicInfo("");  // 清空歌名显示
        ESP_LOGI(TAG, "Cleared song name display");
    }
    
    // 通知所有等待的线程
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        buffer_cv_.notify_all();
    }
    
    // 等待线程结束（避免重复代码，让StopStreaming也能等待线程完全停止）
    if (download_thread_.joinable()) {
        download_thread_.join();
        ESP_LOGI(TAG, "Download thread joined in StopStreaming");
    }
    
    // 等待播放线程结束，使用更安全的方式
    if (play_thread_.joinable()) {
        // 先设置停止标志
        is_playing_ = false;
        
        // 通知条件变量，确保线程能够退出
        {
            std::lock_guard<std::mutex> lock(buffer_mutex_);
            buffer_cv_.notify_all();
        }
        
        // 使用超时机制等待线程结束，避免死锁
        bool thread_finished = false;
        int wait_count = 0;
        const int max_wait = 100; // 最多等待1秒
        
        while (!thread_finished && wait_count < max_wait) {
            vTaskDelay(pdMS_TO_TICKS(10));
            wait_count++;
            
            // 检查线程是否仍然可join
            if (!play_thread_.joinable()) {
                thread_finished = true;
                break;
            }
        }
        
        if (play_thread_.joinable()) {
            if (wait_count >= max_wait) {
                ESP_LOGW(TAG, "Play thread join timeout, detaching thread");
                play_thread_.detach();
            } else {
                play_thread_.join();
                ESP_LOGI(TAG, "Play thread joined in StopStreaming");
            }
        }
    }
    
    // 在线程完全结束后，只在频谱模式下停止FFT显示
    if (display && display_mode_ == DISPLAY_MODE_SPECTRUM) {
        display->stopFft();
        ESP_LOGI(TAG, "Stopped FFT display in StopStreaming (spectrum mode)");
    } else if (display) {
        ESP_LOGI(TAG, "Not in spectrum mode, skipping FFT stop in StopStreaming");
    }
    // 恢复显示动画（如果之前被暂停）
    if (display) {
        display->ResumeAnimations();
        ESP_LOGI(TAG, "Resumed display animations after stopping streaming");
    }
    
    ESP_LOGI(TAG, "Music streaming stop signal sent");
    return true;
}

// 流式下载音频数据
void Esp32Music::DownloadAudioStream(const std::string& music_url) {
    ESP_LOGD(TAG, "Starting audio stream download from: %s", music_url.c_str());
    
    // 验证URL有效性
    if (music_url.empty() || music_url.find("http") != 0) {
        ESP_LOGE(TAG, "Invalid URL format: %s", music_url.c_str());
        is_downloading_ = false;
        return;
    }
    
    auto network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(0);
    
    // 设置基本请求头
    http->SetHeader("User-Agent", "ESP32-Music-Player/1.0");
    http->SetHeader("Accept", "*/*");
    http->SetHeader("Range", "bytes=0-");  // 支持断点续传
    
    // 添加ESP32认证头
    add_auth_headers(http.get());
    
    if (!http->Open("GET", music_url)) {
        ESP_LOGE(TAG, "Failed to connect to music stream URL: %s", music_url.c_str());
        is_downloading_ = false;
        return;
    }
    
    int status_code = http->GetStatusCode();
    if (status_code != 200 && status_code != 206) {  // 206 for partial content
        ESP_LOGE(TAG, "HTTP GET failed with status code: %d for URL: %s", status_code, music_url.c_str());
        http->Close();
        is_downloading_ = false;
        return;
    }
    
    ESP_LOGI(TAG, "Started downloading audio stream, status: %d", status_code);
    
    // 分块读取音频数据
    const size_t chunk_size = 4096;  // 4KB每块
    char buffer[chunk_size];
    size_t total_downloaded = 0;
    
    while (is_downloading_ && is_playing_) {
        int bytes_read = http->Read(buffer, chunk_size);
        if (bytes_read < 0) {
            ESP_LOGE(TAG, "Failed to read audio data: error code %d", bytes_read);
            break;
        }
        if (bytes_read == 0) {
            ESP_LOGI(TAG, "Audio stream download completed, total: %d bytes", total_downloaded);
            break;
        }
        
        // 打印数据块信息
        // ESP_LOGI(TAG, "Downloaded chunk: %d bytes at offset %d", bytes_read, total_downloaded);
        
        // 安全地打印数据块的十六进制内容（前16字节）
        if (bytes_read >= 16) {
            // 诊断：当出现问题时打印首 32 字节（DEBUG 级），便于确认文件头
            int show = std::min((int)bytes_read, 32);
            std::string head;
            char tmp[8];
            for (int i = 0; i < show; ++i) {
                snprintf(tmp, sizeof(tmp), "%02X", (unsigned char)buffer[i]);
                head += tmp;
                if (i != show - 1) head += ' ';
            }
            ESP_LOGD(TAG, "Download chunk head (%d bytes): %s", show, head.c_str());
        } else {
            ESP_LOGI(TAG, "Data chunk too small: %d bytes", bytes_read);
        }
        
        // 尝试检测文件格式（检查文件头）
        if (total_downloaded == 0 && bytes_read >= 4) {
            if (memcmp(buffer, "ID3", 3) == 0) {
                ESP_LOGI(TAG, "Detected MP3 file with ID3 tag");
            } else if (buffer[0] == 0xFF && (buffer[1] & 0xE0) == 0xE0) {
                ESP_LOGI(TAG, "Detected MP3 file header");
            } else if (memcmp(buffer, "RIFF", 4) == 0) {
                ESP_LOGI(TAG, "Detected WAV file");
            } else if (memcmp(buffer, "fLaC", 4) == 0) {
                ESP_LOGI(TAG, "Detected FLAC file");
            } else if (memcmp(buffer, "OggS", 4) == 0) {
                ESP_LOGI(TAG, "Detected OGG file");
            } else {
                ESP_LOGI(TAG, "Unknown audio format, first 4 bytes: %02X %02X %02X %02X", 
                        (unsigned char)buffer[0], (unsigned char)buffer[1], 
                        (unsigned char)buffer[2], (unsigned char)buffer[3]);
                // 额外记录首 32 字节的文本/十六进制，帮助确认是不是错误页面或 JSON
                int show = std::min((int)bytes_read, 32);
                std::string head;
                char tmp[8];
                for (int i = 0; i < show; ++i) {
                    snprintf(tmp, sizeof(tmp), "%02X", (unsigned char)buffer[i]);
                    head += tmp;
                    if (i != show - 1) head += ' ';
                }
                ESP_LOGW(TAG, "Unknown format head (%d bytes): %s", show, head.c_str());
            }
        }
        
        // 创建音频数据块
        uint8_t* chunk_data = (uint8_t*)heap_caps_malloc(bytes_read, MALLOC_CAP_SPIRAM);
        if (!chunk_data) {
            ESP_LOGE(TAG, "Failed to allocate memory for audio chunk");
            break;
        }
        memcpy(chunk_data, buffer, bytes_read);
        
        // 等待缓冲区有空间
        {
            std::unique_lock<std::mutex> lock(buffer_mutex_);
            buffer_cv_.wait(lock, [this] { return buffer_size_ < MAX_BUFFER_SIZE || !is_downloading_; });
            
            if (is_downloading_) {
                audio_buffer_.push(AudioChunk(chunk_data, bytes_read));
                buffer_size_ += bytes_read;
                total_downloaded += bytes_read;
                
                // 通知播放线程有新数据
                buffer_cv_.notify_one();
                
                if (total_downloaded % (256 * 1024) == 0) {  // 每256KB打印一次进度
                    ESP_LOGI(TAG, "Downloaded %d bytes, buffer size: %d", total_downloaded, buffer_size_);
                }
            } else {
                heap_caps_free(chunk_data);
                break;
            }
        }
    }
    
    http->Close();
    is_downloading_ = false;
    
    // 通知播放线程下载完成
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        buffer_cv_.notify_all();
    }
    
    ESP_LOGI(TAG, "Audio stream download thread finished");
}

// 流式播放音频数据
void Esp32Music::PlayAudioStream() {
    ESP_LOGI(TAG, "Starting audio stream playback");
    
    // 初始化时间跟踪变量
    current_play_time_ms_ = 0;
    last_frame_time_ms_ = 0;
    total_frames_decoded_ = 0;
    
    auto codec = Board::GetInstance().GetAudioCodec();
    if (!codec || !codec->output_enabled()) {
        ESP_LOGE(TAG, "Audio codec not available or not enabled");
        is_playing_ = false;
        // Ensure animations are resumed on early exit
        {
            auto& board = Board::GetInstance();
            auto display = board.GetDisplay();
            if (display) {
                display->ResumeAnimations();
                ESP_LOGI(TAG, "Resumed display animations (early exit: codec unavailable)");
            }
        }
        return;
    }
    
    if (!mp3_decoder_initialized_) {
        ESP_LOGE(TAG, "MP3 decoder not initialized");
        is_playing_ = false;
        // Ensure animations are resumed on early exit
        {
            auto& board = Board::GetInstance();
            auto display = board.GetDisplay();
            if (display) {
                display->ResumeAnimations();
                ESP_LOGI(TAG, "Resumed display animations (early exit: decoder not initialized)");
            }
        }
        return;
    }
    
    
    // 等待缓冲区有足够数据开始播放
    {
        std::unique_lock<std::mutex> lock(buffer_mutex_);
        buffer_cv_.wait(lock, [this] { 
            return buffer_size_ >= MIN_BUFFER_SIZE || (!is_downloading_ && !audio_buffer_.empty()); 
        });
    }
    
    ESP_LOGI(TAG, "小智开源音乐固件qq交流群:826072986");
    ESP_LOGI(TAG, "Starting playback with buffer size: %d", buffer_size_);
    
    size_t total_played = 0;
    uint8_t* mp3_input_buffer = nullptr;
    int bytes_left = 0;
    uint8_t* read_ptr = nullptr;
    
    // 分配MP3输入缓冲区
    mp3_input_buffer = (uint8_t*)heap_caps_malloc(8192, MALLOC_CAP_SPIRAM);
    if (!mp3_input_buffer) {
        ESP_LOGE(TAG, "Failed to allocate MP3 input buffer");
        is_playing_ = false;
        // Ensure animations are resumed on early exit
        {
            auto& board = Board::GetInstance();
            auto display = board.GetDisplay();
            if (display) {
                display->ResumeAnimations();
                ESP_LOGI(TAG, "Resumed display animations (early exit: MP3 input alloc failed)");
            }
        }
        return;
    }
    
    // 标记是否已经处理过ID3标签
    bool id3_processed = false;
    
    while (is_playing_) {
        // 检查设备状态，只有在空闲状态才播放音乐
        auto& app = Application::GetInstance();
        DeviceState current_state = app.GetDeviceState();

        // // 等小智把话说完了，变成聆听状态之后，马上转成待机状态，进入音乐播放
        // if (current_state == kDeviceStateListening)
        // {
        //     ESP_LOGI(TAG, "Device is in listening state, switching to idle state for music playback");

            // 状态转换：说话中-》聆听中-》待机状态-》播放音乐
            if (current_state == kDeviceStateListening || current_state == kDeviceStateSpeaking) {
                if (current_state == kDeviceStateSpeaking) {
                    ESP_LOGI(TAG, "Device is in speaking state, switching to listening state for music playback");
                }
                if (current_state == kDeviceStateListening) {
                    ESP_LOGI(TAG, "Device is in listening state, switching to idle state for music playback");
                }
            // 切换状态
            app.ToggleChatState(); // 变成待机状态
            vTaskDelay(pdMS_TO_TICKS(300));
            continue;
        }
        else if (current_state != kDeviceStateIdle)
        { // 不是待机状态，就一直卡在这里，不让播放音乐
            ESP_LOGD(TAG, "Device state is %d, pausing music playback", current_state);
            // 如果不是空闲状态，暂停播放
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        // 设备状态检查通过，显示当前播放的歌名
        if (!song_name_displayed_ && !current_song_name_.empty()) {
            auto& board = Board::GetInstance();
            auto display = board.GetDisplay();
            if (display) {
                // 格式化歌名显示为《歌名》播放中...
                std::string formatted_song_name = "《" + current_song_name_ + "》播放中...";
                display->SetMusicInfo(formatted_song_name.c_str());
                ESP_LOGI(TAG, "Displaying song name: %s", formatted_song_name.c_str());
                song_name_displayed_ = true;
            }

            // 根据显示模式启动相应的显示功能
            if (display) {
                if (display_mode_ == DISPLAY_MODE_SPECTRUM) {
                    display->start();
                    ESP_LOGI(TAG, "Display start() called for spectrum visualization");
                } else {
                    ESP_LOGI(TAG, "Lyrics display mode active, FFT visualization disabled");
                }
            }
        }
        
        // 如果需要更多MP3数据，从缓冲区读取
        if (bytes_left < 4096) {  // 保持至少4KB数据用于解码
            AudioChunk chunk;
            
            // 从缓冲区获取音频数据
            {
                std::unique_lock<std::mutex> lock(buffer_mutex_);
                if (audio_buffer_.empty()) {
                    if (!is_downloading_) {
                        // 下载完成且缓冲区为空，播放结束
                        ESP_LOGI(TAG, "Playback finished, total played: %d bytes", total_played);
                        break;
                    }
                    // 等待新数据
                    buffer_cv_.wait(lock, [this] { return !audio_buffer_.empty() || !is_downloading_; });
                    if (audio_buffer_.empty()) {
                        continue;
                    }
                }
                
                chunk = audio_buffer_.front();
                audio_buffer_.pop();
                buffer_size_ -= chunk.size;
                
                // 通知下载线程缓冲区有空间
                buffer_cv_.notify_one();
            }
            
            // 将新数据添加到MP3输入缓冲区
            if (chunk.data && chunk.size > 0) {
                // 移动剩余数据到缓冲区开头
                if (bytes_left > 0 && read_ptr != mp3_input_buffer) {
                    memmove(mp3_input_buffer, read_ptr, bytes_left);
                }
                
                // 检查缓冲区空间
                size_t space_available = 8192 - bytes_left;
                size_t copy_size = std::min(chunk.size, space_available);
                
                // 复制新数据
                memcpy(mp3_input_buffer + bytes_left, chunk.data, copy_size);
                bytes_left += copy_size;
                read_ptr = mp3_input_buffer;
                
                // 检查并跳过ID3标签（仅在开始时处理一次）
                if (!id3_processed && bytes_left >= 10) {
                    size_t id3_skip = SkipId3Tag(read_ptr, bytes_left);
                    if (id3_skip > 0) {
                        read_ptr += id3_skip;
                        bytes_left -= id3_skip;
                        ESP_LOGI(TAG, "Skipped ID3 tag: %u bytes", (unsigned int)id3_skip);
                    }
                    id3_processed = true;
                }
                
                // 释放chunk内存
                heap_caps_free(chunk.data);
            }
        }
        
        // 尝试找到MP3帧同步
        int sync_offset = MP3FindSyncWord(read_ptr, bytes_left);
        if (sync_offset < 0) {
            ESP_LOGW(TAG, "No MP3 sync word found, skipping %d bytes", bytes_left);
            // 打印缓冲区前 32 字节以便诊断（DEBUG 级）
            if (bytes_left > 0 && read_ptr) {
                int show = std::min(bytes_left, 32);
                std::string head;
                char tmp[8];
                for (int i = 0; i < show; ++i) {
                    snprintf(tmp, sizeof(tmp), "%02X", (unsigned char)read_ptr[i]);
                    head += tmp;
                    if (i != show - 1) head += ' ';
                }
                ESP_LOGD(TAG, "Buffer head when no sync (%d bytes available): %s", bytes_left, head.c_str());
            }
            bytes_left = 0;
            continue;
        }
        
        // 跳过到同步位置
        if (sync_offset > 0) {
            read_ptr += sync_offset;
            bytes_left -= sync_offset;
        }
        
        // 解码MP3帧
        int16_t pcm_buffer[2304];
        int decode_result = MP3Decode(mp3_decoder_, &read_ptr, &bytes_left, pcm_buffer, 0);
        
    if (decode_result == 0) {
            // 解码成功，获取帧信息
            MP3GetLastFrameInfo(mp3_decoder_, &mp3_frame_info_);
            total_frames_decoded_++;
            
            // 基本的帧信息有效性检查，防止除零错误
            if (mp3_frame_info_.samprate == 0 || mp3_frame_info_.nChans == 0) {
                ESP_LOGW(TAG, "Invalid frame info: rate=%d, channels=%d, skipping", 
                        mp3_frame_info_.samprate, mp3_frame_info_.nChans);
                continue;
            }
            
            // 计算当前帧的持续时间(毫秒)
            int frame_duration_ms = (mp3_frame_info_.outputSamps * 1000) / 
                                  (mp3_frame_info_.samprate * mp3_frame_info_.nChans);
            
            // 更新当前播放时间
            current_play_time_ms_ += frame_duration_ms;
            
            ESP_LOGD(TAG, "Frame %d: time=%lldms, duration=%dms, rate=%d, ch=%d", 
                    total_frames_decoded_, current_play_time_ms_, frame_duration_ms,
                    mp3_frame_info_.samprate, mp3_frame_info_.nChans);
            
            // 更新歌词显示
            int buffer_latency_ms = 600; // 实测调整值
            UpdateLyricDisplay(current_play_time_ms_ + buffer_latency_ms);
            
            // 将PCM数据发送到Application的音频解码队列
            if (mp3_frame_info_.outputSamps > 0) {
                int16_t* final_pcm_data = pcm_buffer;
                int final_sample_count = mp3_frame_info_.outputSamps;
                std::vector<int16_t> mono_buffer;
                
                // 如果是双通道，转换为单通道混合
                if (mp3_frame_info_.nChans == 2) {
                    // 双通道转单通道：将左右声道混合
                    int stereo_samples = mp3_frame_info_.outputSamps;  // 包含左右声道的总样本数
                    int mono_samples = stereo_samples / 2;  // 实际的单声道样本数
                    
                    mono_buffer.resize(mono_samples);
                    
                    for (int i = 0; i < mono_samples; ++i) {
                        // 混合左右声道 (L + R) / 2
                        int left = pcm_buffer[i * 2];      // 左声道
                        int right = pcm_buffer[i * 2 + 1]; // 右声道
                        mono_buffer[i] = (int16_t)((left + right) / 2);
                    }
                    
                    final_pcm_data = mono_buffer.data();
                    final_sample_count = mono_samples;

                    ESP_LOGD(TAG, "Converted stereo to mono: %d -> %d samples", 
                            stereo_samples, mono_samples);
                } else if (mp3_frame_info_.nChans == 1) {
                    // 已经是单声道，无需转换
                    ESP_LOGD(TAG, "Already mono audio: %d samples", final_sample_count);
                } else {
                    ESP_LOGW(TAG, "Unsupported channel count: %d, treating as mono", 
                            mp3_frame_info_.nChans);
                }
                
                // 创建AudioStreamPacket
                AudioStreamPacket packet;
                packet.sample_rate = mp3_frame_info_.samprate;
                packet.frame_duration = 60;  // 使用Application默认的帧时长
                packet.timestamp = 0;
                
                // 将int16_t PCM数据转换为uint8_t字节数组
                size_t pcm_size_bytes = final_sample_count * sizeof(int16_t);
                packet.payload.resize(pcm_size_bytes);
                memcpy(packet.payload.data(), final_pcm_data, pcm_size_bytes);

                if (final_pcm_data_fft == nullptr) {
                    final_pcm_data_fft = (int16_t*)heap_caps_malloc(
                        final_sample_count * sizeof(int16_t),
                        MALLOC_CAP_SPIRAM
                    );
                }
                
                memcpy(
                    final_pcm_data_fft,
                    final_pcm_data,
                    final_sample_count * sizeof(int16_t)
                );
                
                ESP_LOGD(TAG, "Sending %d PCM samples (%d bytes, rate=%d, channels=%d->1) to Application", 
                        final_sample_count, pcm_size_bytes, mp3_frame_info_.samprate, mp3_frame_info_.nChans);
                
                // 发送到Application的音频解码队列
                // 在发送前进行校验，防止无效大小导致底层驱动错误
                if (packet.payload.size() == 0 || (packet.payload.size() % sizeof(int16_t)) != 0) {
                    ESP_LOGW(TAG, "Invalid PCM payload size: %d, skipping frame", (int)packet.payload.size());
                } else {
                    // 检查并确保AudioCodec的输出采样率与帧采样率一致
                    auto& board = Board::GetInstance();
                    auto codec = board.GetAudioCodec();
                    if (codec) {
                        if (codec->output_sample_rate() != mp3_frame_info_.samprate) {
                            ESP_LOGI(TAG, "Attempting to set codec output sample rate to %d Hz", mp3_frame_info_.samprate);
                            if (!codec->SetOutputSampleRate(mp3_frame_info_.samprate)) {
                                ESP_LOGE(TAG, "Failed to set codec sample rate to %d Hz, stopping playback to avoid driver errors", mp3_frame_info_.samprate);
                                is_playing_ = false;
                                // Ensure animations are resumed before breaking out
                                {
                                    auto& board = Board::GetInstance();
                                    auto display = board.GetDisplay();
                                    if (display) {
                                        display->ResumeAnimations();
                                        ESP_LOGI(TAG, "Resumed display animations (codec sample rate set failed)");
                                    }
                                }
                                // chunk.data 已在上游复制到 mp3_input_buffer 后被释放，
                                // 此处不能再次访问或释放它（会导致编译错误/双重释放）。
                                break;
                            }
                        }
                    }

                    app.AddAudioData(std::move(packet));
                }
                total_played += pcm_size_bytes;
                
                // 打印播放进度
                if (total_played % (128 * 1024) == 0) {
                    ESP_LOGI(TAG, "Played %d bytes, buffer size: %d", total_played, buffer_size_);
                }
            }
            
        } else {
            // 解码失败
            ESP_LOGW(TAG, "MP3 decode failed with error: %d, bytes_left=%d", decode_result, bytes_left);

            // 打印解码失败时缓冲区前 64 字节（DEBUG 级）
            if (bytes_left > 0 && read_ptr) {
                int show = std::min(bytes_left, 64);
                std::string head;
                char tmp[8];
                for (int i = 0; i < show; ++i) {
                    snprintf(tmp, sizeof(tmp), "%02X", (unsigned char)read_ptr[i]);
                    head += tmp;
                    if (i != show - 1) head += ' ';
                }
                ESP_LOGD(TAG, "Buffer head at decode failure (%d bytes): %s", show, head.c_str());
            }

            // 跳过一些字节继续尝试（记录跳过次数）
            static int skip_count = 0;
            if (bytes_left > 1) {
                read_ptr++;
                bytes_left--;
                skip_count++;
                ESP_LOGD(TAG, "Incremental skip for resync, total skips=%d", skip_count);
            } else {
                bytes_left = 0;
            }
        }
    }
    
    // 清理
    if (mp3_input_buffer) {
        heap_caps_free(mp3_input_buffer);
    }
    
    // 播放结束时进行基本清理，但不调用StopStreaming避免线程自我等待
    ESP_LOGI(TAG, "Audio stream playback finished, total played: %d bytes", total_played);
    ESP_LOGI(TAG, "Performing basic cleanup from play thread");
    
    // 停止播放标志
    is_playing_ = false;
    
    // 只在频谱显示模式下才停止FFT显示
    if (display_mode_ == DISPLAY_MODE_SPECTRUM) {
        auto& board = Board::GetInstance();
        auto display = board.GetDisplay();
        if (display) {
            display->stopFft();
            ESP_LOGI(TAG, "Stopped FFT display from play thread (spectrum mode)");
        }
    } else {
        ESP_LOGI(TAG, "Not in spectrum mode, skipping FFT stop");
    }

    // 确保在播放结束后恢复动画显示（覆盖之前 PauseAnimations 的调用）
    {
        auto& board = Board::GetInstance();
        auto display = board.GetDisplay();
        if (display) {
            display->ResumeAnimations();
            ESP_LOGI(TAG, "Resumed display animations (playback finished cleanup)");
        }
    }
    // 清理封面预览（如果有的话）
    {
        auto& board = Board::GetInstance();
        auto display = board.GetDisplay();
        if (display) {
            display->ClearPreviewImage();
            ESP_LOGI(TAG, "Cleared preview image from play cleanup");
        }
    }
}

// 清空音频缓冲区
void Esp32Music::ClearAudioBuffer() {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    
    while (!audio_buffer_.empty()) {
        AudioChunk chunk = audio_buffer_.front();
        audio_buffer_.pop();
        if (chunk.data) {
            heap_caps_free(chunk.data);
        }
    }
    
    buffer_size_ = 0;
    ESP_LOGI(TAG, "Audio buffer cleared");
}

// 初始化MP3解码器
bool Esp32Music::InitializeMp3Decoder() {
    mp3_decoder_ = MP3InitDecoder();
    if (mp3_decoder_ == nullptr) {
        ESP_LOGE(TAG, "Failed to initialize MP3 decoder");
        mp3_decoder_initialized_ = false;
        return false;
    }
    
    mp3_decoder_initialized_ = true;
    ESP_LOGI(TAG, "MP3 decoder initialized successfully");
    return true;
}

// 清理MP3解码器
void Esp32Music::CleanupMp3Decoder() {
    if (mp3_decoder_ != nullptr) {
        MP3FreeDecoder(mp3_decoder_);
        mp3_decoder_ = nullptr;
    }
    mp3_decoder_initialized_ = false;
    ESP_LOGI(TAG, "MP3 decoder cleaned up");
}

// 重置采样率到原始值
void Esp32Music::ResetSampleRate() {
    auto& board = Board::GetInstance();
    auto codec = board.GetAudioCodec();
    if (codec && codec->original_output_sample_rate() > 0 && 
        codec->output_sample_rate() != codec->original_output_sample_rate()) {
        ESP_LOGI(TAG, "重置采样率：从 %d Hz 重置到原始值 %d Hz", 
                codec->output_sample_rate(), codec->original_output_sample_rate());
        if (codec->SetOutputSampleRate(-1)) {  // -1 表示重置到原始值
            ESP_LOGI(TAG, "成功重置采样率到原始值: %d Hz", codec->output_sample_rate());
        } else {
            ESP_LOGW(TAG, "无法重置采样率到原始值");
        }
    }
}

// 跳过MP3文件开头的ID3标签
size_t Esp32Music::SkipId3Tag(uint8_t* data, size_t size) {
    if (!data || size < 10) {
        return 0;
    }
    
    // 检查ID3v2标签头 "ID3"
    if (memcmp(data, "ID3", 3) != 0) {
        return 0;
    }
    
    // 计算标签大小（synchsafe integer格式）
    uint32_t tag_size = ((uint32_t)(data[6] & 0x7F) << 21) |
                        ((uint32_t)(data[7] & 0x7F) << 14) |
                        ((uint32_t)(data[8] & 0x7F) << 7)  |
                        ((uint32_t)(data[9] & 0x7F));
    
    // ID3v2头部(10字节) + 标签内容
    size_t total_skip = 10 + tag_size;
    
    // 确保不超过可用数据大小
    if (total_skip > size) {
        total_skip = size;
    }
    
    ESP_LOGI(TAG, "Found ID3v2 tag, skipping %u bytes", (unsigned int)total_skip);
    return total_skip;
}

// 下载歌词
bool Esp32Music::DownloadLyrics(const std::string& lyric_url) {
    ESP_LOGI(TAG, "Downloading lyrics from: %s", lyric_url.c_str());

    if (lyric_url.empty()) {
        ESP_LOGE(TAG, "Lyric URL is empty!");
        return false;
    }

    // 简化的重试逻辑（最多3次）
    const int max_retries = 3;
    std::string lyric_content;
    bool success = false;

    for (int attempt = 0; attempt < max_retries && !success; ++attempt) {
        if (attempt > 0) {
            ESP_LOGI(TAG, "Retrying lyric download, attempt %d", attempt + 1);
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }

        auto network = Board::GetInstance().GetNetwork();
        auto http = network->CreateHttp(0);
        if (!http) {
            ESP_LOGE(TAG, "Failed to create HTTP client for lyric download");
            continue;
        }

        http->SetHeader("User-Agent", "ESP32-Music-Player/1.0");
        http->SetHeader("Accept", "text/plain");
        add_auth_headers(http.get());

        if (!http->Open("GET", lyric_url)) {
            ESP_LOGW(TAG, "Failed to open lyric URL: %s", lyric_url.c_str());
            http->Close();
            continue;
        }

        int status = http->GetStatusCode();
        if (status < 200 || status >= 300) {
            ESP_LOGW(TAG, "Lyric HTTP status %d for %s", status, lyric_url.c_str());
            http->Close();
            continue;
        }

        // 读取内容到堆缓冲
        const size_t chunk_sz = 1024;
        char* buf = (char*)heap_caps_malloc(chunk_sz, MALLOC_CAP_SPIRAM);
        if (!buf) {
            ESP_LOGE(TAG, "Failed to allocate SPIRAM buffer for lyrics");
            http->Close();
            break;
        }

        lyric_content.clear();
        while (true) {
            int r = http->Read(buf, (int)chunk_sz - 1);
            if (r > 0) {
                buf[r] = '\0';
                lyric_content.append(buf, (size_t)r);
            } else if (r == 0) {
                success = true;
                break;
            } else {
                ESP_LOGW(TAG, "Error reading lyrics: %d", r);
                break;
            }
        }

        heap_caps_free(buf);
        http->Close();
    }

    if (!success) {
        ESP_LOGW(TAG, "Failed to download lyrics after %d attempts", max_retries);
        return false;
    }

    // 逐行解析 LRC 时间标签 [mm:ss.xx]text
    lyrics_.clear();
    size_t pos = 0;
    while (pos < lyric_content.size()) {
        size_t nl = lyric_content.find('\n', pos);
        std::string line = (nl == std::string::npos) ? lyric_content.substr(pos) : lyric_content.substr(pos, nl - pos);
        pos = (nl == std::string::npos) ? lyric_content.size() : nl + 1;

        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;

        if (line.size() > 4 && line[0] == '[') {
            size_t rb = line.find(']');
            if (rb == std::string::npos) continue;
            std::string tag = line.substr(1, rb - 1);
            std::string text = (rb + 1 < line.size()) ? line.substr(rb + 1) : std::string();
            size_t colon = tag.find(':');
            if (colon == std::string::npos) continue;
            bool digits = true;
            for (size_t i = 0; i < colon; ++i) if (!isdigit((unsigned char)tag[i])) { digits = false; break; }
            if (!digits) continue;
            try {
                int mm = std::stoi(tag.substr(0, colon));
                float ss = std::stof(tag.substr(colon + 1));
                int ts = mm * 60000 + (int)(ss * 1000);
                lyrics_.push_back(std::make_pair(ts, text));
            } catch (...) {
                continue;
            }
        }
    }

    std::sort(lyrics_.begin(), lyrics_.end());
    ESP_LOGI(TAG, "Parsed %d lyric lines", lyrics_.size());
    return !lyrics_.empty();
}

// 歌词显示线程
void Esp32Music::LyricDisplayThread() {
    ESP_LOGI(TAG, "Lyric display thread started");
    
    if (!DownloadLyrics(current_lyric_url_)) {
        ESP_LOGE(TAG, "Failed to download or parse lyrics");
        is_lyric_running_ = false;
        return;
    }
    // 下载成功后，立即尝试一次更新显示，避免出现播放第一首歌时歌词未及时显示的竞态
    {
        int buffer_latency_ms = 600; // 与播放线程保持一致的延迟补偿
        int64_t snap_time = current_play_time_ms_ + buffer_latency_ms;
        ESP_LOGI(TAG, "Lyrics downloaded, forcing initial lyric update at %lldms", snap_time);
        UpdateLyricDisplay(snap_time);
        // 小等待后再尝试一次，覆盖极端竞态情况
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        UpdateLyricDisplay(snap_time);
    }
    
    // 定期检查是否需要更新显示(频率可以降低)
    while (is_lyric_running_ && is_playing_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    
    ESP_LOGI(TAG, "Lyric display thread finished");
}

void Esp32Music::UpdateLyricDisplay(int64_t current_time_ms) {
    std::lock_guard<std::mutex> lock(lyrics_mutex_);
    
    if (lyrics_.empty()) {
        return;
    }
    
    // 查找当前应该显示的歌词
    int new_lyric_index = -1;
    
    // 从当前歌词索引开始查找，提高效率
    int start_index = (current_lyric_index_.load() >= 0) ? current_lyric_index_.load() : 0;
    
    // 正向查找：找到最后一个时间戳小于等于当前时间的歌词
    for (int i = start_index; i < (int)lyrics_.size(); i++) {
        if (lyrics_[i].first <= current_time_ms) {
            new_lyric_index = i;
        } else {
            break;  // 时间戳已超过当前时间
        }
    }
    
    // 如果没有找到(可能当前时间比第一句歌词还早)，显示空
    if (new_lyric_index == -1) {
        new_lyric_index = -1;
    }
    
    // 如果歌词索引发生变化，更新显示
    if (new_lyric_index != current_lyric_index_) {
        current_lyric_index_ = new_lyric_index;
        
        auto& board = Board::GetInstance();
        auto display = board.GetDisplay();
        if (display) {
            std::string lyric_text;
            
            if (current_lyric_index_ >= 0 && current_lyric_index_ < (int)lyrics_.size()) {
                lyric_text = lyrics_[current_lyric_index_].second;
            }
            
            // 显示歌词
            display->SetChatMessage("lyric", lyric_text.c_str());
            
            ESP_LOGD(TAG, "Lyric update at %lldms: %s", 
                    current_time_ms, 
                    lyric_text.empty() ? "(no lyric)" : lyric_text.c_str());
        }
    }
}

// 删除复杂的认证初始化方法，使用简单的静态函数

// 删除复杂的类方法，使用简单的静态函数

/**
 * @brief 添加认证头到HTTP请求
 * @param http_client HTTP客户端指针
 * 
 * 添加的认证头包括：
 * - X-MAC-Address: 设备MAC地址
 * - X-Chip-ID: 设备芯片ID
 * - X-Timestamp: 当前时间戳
 * - X-Dynamic-Key: 动态生成的密钥
 */
// 删除复杂的AddAuthHeaders方法，使用简单的静态函数

// 删除复杂的认证验证和配置方法，使用简单的静态函数

// 显示模式控制方法实现
void Esp32Music::SetDisplayMode(DisplayMode mode) {
    DisplayMode old_mode = display_mode_.load();
    display_mode_ = mode;
    
    ESP_LOGI(TAG, "Display mode changed from %s to %s", 
            (old_mode == DISPLAY_MODE_SPECTRUM) ? "SPECTRUM" : "LYRICS",
            (mode == DISPLAY_MODE_SPECTRUM) ? "SPECTRUM" : "LYRICS");
}
