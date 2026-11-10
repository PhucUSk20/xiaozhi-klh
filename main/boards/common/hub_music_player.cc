#include "hub_music_player.h"
#include "sdkconfig.h"

#if CONFIG_IDF_TARGET_ESP32S3 && CONFIG_BREAD_COMPACT_WIFI_HUB_MUSIC_TEST
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <atomic>
#include <cstring>
#include <memory>
#include <vector>
#include "application.h"
#include "audio_codec.h"
#include "board.h"
#include "decoder/impl/esp_mp3_dec.h"
#include "esp_ae_rate_cvt.h"
#include "mcp_server.h"

namespace {
std::atomic<bool> busy{false};
std::atomic<bool> stopped{true};
std::atomic<bool> boot_test_started{false};
std::atomic<bool> boot_test_pending{false};
constexpr size_t kMaxBytes = 20 * 1024 * 1024;
constexpr const char* kPrefix = "https://coding.eduhubonline.com/api/robot-hub/music/stream/";
extern const uint8_t kMusicTestPromptBegin[] asm("_binary_hub_music_test_prompt_ogg_start");
extern const uint8_t kMusicTestPromptEnd[] asm("_binary_hub_music_test_prompt_ogg_end");

const char* UrlRejection(const std::string& url) {
    if (url.empty() || url.size() >= 2048 || url.find_first_of("\r\n") != std::string::npos)
        return "URL is empty, too long or contains line breaks";
    if (url.compare(0, strlen(kPrefix), kPrefix) != 0)
        return "URL must start with https://coding.eduhubonline.com/api/robot-hub/music/stream/. "
               "Check Robot Hub API_PUBLIC_URL and pass playbackArguments.url unchanged";
    const auto ticket = url.find("?ticket=");
    if (ticket == std::string::npos || ticket + strlen("?ticket=") >= url.size())
        return "URL is missing a playback ticket; pass playbackArguments.url unchanged";
    return nullptr;
}

std::string SafeOrigin(const std::string& url) {
    // Never print paths, userinfo, query strings or stream tickets.
    const auto scheme = url.find("://");
    if (scheme == std::string::npos || scheme > 8)
        return "unavailable";
    const auto end = url.find_first_of("/?#", scheme + 3);
    const auto origin = url.substr(0, end);
    if (origin.size() > 160 ||
        origin.find_first_not_of(
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789:.-[]/") !=
            std::string::npos)
        return "unavailable";
    return origin;
}

void Play(void* arg) {
    std::unique_ptr<std::string> url(static_cast<std::string*>(arg));
    auto& audio = Application::GetInstance().GetAudioService();
    esp_http_client_config_t config = {};
    config.url = url->c_str();
    config.timeout_ms = 5000;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.disable_auto_redirect = true;
    config.buffer_size = 4096;
    auto http = esp_http_client_init(&config);
    void* decoder = nullptr;
    esp_ae_rate_cvt_handle_t resampler = nullptr;
    size_t decoded_frames = 0;
    bool ok = false;
    if (http && esp_http_client_open(http, 0) == ESP_OK) {
        const int64_t length = esp_http_client_fetch_headers(http);
        ESP_LOGI("HubMusic", "MP3 HTTP %d, bytes=%lld", esp_http_client_get_status_code(http),
                 static_cast<long long>(length));
        if (esp_http_client_get_status_code(http) == 200 && length > 0 && length <= kMaxBytes &&
            esp_mp3_dec_open(nullptr, 0, &decoder) == ESP_AUDIO_ERR_OK) {
            std::vector<uint8_t> input(8192);
            std::vector<int16_t> pcm(2304);
            size_t buffered = 0, downloaded = 0;
            int source_rate = 0;
            bool eof = false;
            const int output_rate = Board::GetInstance().GetAudioCodec()->output_sample_rate();
            const int64_t started = esp_timer_get_time();
            ok = true;
            while (!stopped.load() && esp_timer_get_time() - started < 1200LL * 1000000) {
                if (!eof && buffered < 4096) {
                    int got =
                        esp_http_client_read(http, reinterpret_cast<char*>(input.data() + buffered),
                                             input.size() - buffered);
                    if (got < 0) {
                        ok = false;
                        break;
                    }
                    eof = got == 0;
                    downloaded += got;
                    buffered += got;
                    if (downloaded > kMaxBytes) {
                        ok = false;
                        break;
                    }
                }
                if (!buffered)
                    break;
                esp_audio_dec_in_raw_t raw = {};
                raw.buffer = input.data();
                raw.len = buffered;
                esp_audio_dec_out_frame_t frame = {};
                frame.buffer = reinterpret_cast<uint8_t*>(pcm.data());
                frame.len = pcm.size() * sizeof(int16_t);
                esp_audio_dec_info_t info = {};
                auto result = esp_mp3_dec_decode(decoder, &raw, &frame, &info);
                size_t consumed = raw.consumed;
                if (!consumed && result != ESP_AUDIO_ERR_OK) {
                    // Refill an incomplete frame; otherwise scan past ID3/sync bytes.
                    if (!eof && buffered < input.size()) {
                        int got = esp_http_client_read(
                            http, reinterpret_cast<char*>(input.data() + buffered),
                            input.size() - buffered);
                        if (got < 0) {
                            ok = false;
                            break;
                        }
                        downloaded += got;
                        buffered += got;
                        eof = got == 0;
                        if (downloaded > kMaxBytes) {
                            ok = false;
                            break;
                        }
                        if (got > 0)
                            continue;
                    }
                    consumed = 1;
                }
                if (!consumed || consumed > buffered) {
                    ok = false;
                    break;
                }
                memmove(input.data(), input.data() + consumed, buffered - consumed);
                buffered -= consumed;
                if (result != ESP_AUDIO_ERR_OK || !frame.decoded_size)
                    continue;
                if (info.bits_per_sample != 16 || (info.channel != 1 && info.channel != 2) ||
                    info.sample_rate < 8000 || info.sample_rate > 48000 ||
                    frame.decoded_size > pcm.size() * 2) {
                    ok = false;
                    break;
                }
                const size_t samples = frame.decoded_size / (2 * info.channel);
                std::vector<int16_t> mono(samples);
                for (size_t i = 0; i < samples; ++i)
                    mono[i] = info.channel == 1
                                  ? pcm[i]
                                  : (static_cast<int32_t>(pcm[i * 2]) + pcm[i * 2 + 1]) / 2;
                if (source_rate != static_cast<int>(info.sample_rate)) {
                    if (resampler) {
                        esp_ae_rate_cvt_close(resampler);
                        resampler = nullptr;
                    }
                    source_rate = info.sample_rate;
                    if (source_rate != output_rate) {
                        esp_ae_rate_cvt_cfg_t cfg = {};
                        cfg.src_rate = source_rate;
                        cfg.dest_rate = output_rate;
                        cfg.channel = 1;
                        cfg.bits_per_sample = 16;
                        cfg.complexity = 2;
                        cfg.perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED;
                        if (esp_ae_rate_cvt_open(&cfg, &resampler) != ESP_AE_ERR_OK) {
                            ok = false;
                            break;
                        }
                    }
                }
                if (resampler) {
                    uint32_t count = 0;
                    if (esp_ae_rate_cvt_get_max_out_sample_num(resampler, mono.size(), &count) !=
                        ESP_AE_ERR_OK) {
                        ok = false;
                        break;
                    }
                    std::vector<int16_t> converted(count);
                    if (esp_ae_rate_cvt_process(resampler, mono.data(), mono.size(),
                                                converted.data(), &count) != ESP_AE_ERR_OK) {
                        ok = false;
                        break;
                    }
                    converted.resize(count);
                    mono = std::move(converted);
                }
                if (stopped.load() || !audio.QueueMusicPcm(std::move(mono)))
                    break;
                decoded_frames++;
                if (decoded_frames == 1)
                    ESP_LOGI("HubMusic", "First MP3 PCM frame queued for speaker");
            }
        }
    }
    if (resampler)
        esp_ae_rate_cvt_close(resampler);
    if (decoder)
        esp_mp3_dec_close(decoder);
    if (http)
        esp_http_client_cleanup(http);
    audio.SetMusicPlaybackActive(false);
    ESP_LOGI("HubMusic", "%s frames=%u",
             stopped.load() ? "stopped" : (ok && decoded_frames ? "finished" : "failed"),
             static_cast<unsigned>(decoded_frames));
    busy = false;
    url.reset();
    vTaskDelete(nullptr);
}
}  // namespace

void StartHubMusicBootTest() {
    if (boot_test_started.exchange(true))
        return;
    boot_test_pending = true;
    if (xTaskCreate(
            [](void*) {
                // Allow activation and the startup sound to finish before opening a session.
                vTaskDelay(pdMS_TO_TICKS(3000));
                Application::GetInstance().Schedule([]() {
                    if (!boot_test_pending.exchange(false) ||
                        Application::GetInstance().GetDeviceState() != kDeviceStateIdle) {
                        ESP_LOGI("HubMusic",
                                 "Startup MCP test cancelled: device interrupted or busy");
                        return;
                    }
                    const auto size =
                        static_cast<size_t>(kMusicTestPromptEnd - kMusicTestPromptBegin);
                    ESP_LOGI("HubMusic",
                             "Startup MCP test: asking host to search and play library music");
                    Application::GetInstance().SendAudioPrompt(
                        std::string_view(reinterpret_cast<const char*>(kMusicTestPromptBegin),
                                         size),
                        "hub_music_test");
                });
                vTaskDelete(nullptr);
            },
            "hub_music_test", 3072, nullptr, 3, nullptr) != pdPASS) {
        boot_test_pending = false;
        ESP_LOGW("HubMusic", "Startup MCP test: insufficient resources");
    }
}

void StopHubMusic() {
    boot_test_pending = false;
    if (!busy.load())
        return;
    stopped = true;
    auto& audio = Application::GetInstance().GetAudioService();
    audio.SetMusicPlaybackActive(false);
    audio.ResetDecoder();
}

void RegisterHubMusicTools() {
    auto& mcp = McpServer::GetInstance();
    mcp.AddTool(
        "self.music.play_url",
        "Play Robot Hub music: first use music__search_music to find the requested song, then "
        "music__play_music to obtain its private MP3 stream URL, and pass that URL here. "
        "Queued means accepted, not "
        "confirmed audible playback. No Zing fallback.",
        PropertyList({Property("url", kPropertyTypeString)}),
        [](const PropertyList& properties) -> ReturnValue {
            auto url = properties["url"].value<std::string>();
            if (const auto* reason = UrlRejection(url)) {
                ESP_LOGW("HubMusic", "Playback URL rejected: %s; origin=%s, length=%u", reason,
                         SafeOrigin(url).c_str(), static_cast<unsigned>(url.size()));
                return std::string("Music unavailable: ") + reason;
            }
            bool expected = false;
            if (!busy.compare_exchange_strong(expected, true)) {
                ESP_LOGW("HubMusic", "Playback rejected: player busy");
                return std::string(
                    "Music unavailable: player busy. Stop current playback before retrying.");
            }
            stopped = false;
            auto& audio = Application::GetInstance().GetAudioService();
            audio.SetMusicPlaybackActive(true);
            audio.ResetDecoder();
            auto* request = new (std::nothrow) std::string(std::move(url));
            if (!request || xTaskCreate(Play, "hub_music", 12288, request, 4, nullptr) != pdPASS) {
                delete request;
                audio.SetMusicPlaybackActive(false);
                stopped = true;
                busy = false;
                return std::string("Music unavailable: insufficient resources.");
            }
            return std::string("Playback queued; audio has not yet been confirmed.");
        });
    mcp.AddTool("self.music.stop", "Stop the current Robot Hub music playback.", PropertyList(),
                [](const PropertyList&) -> ReturnValue {
                    StopHubMusic();
                    return std::string("Stop requested.");
                });
}
#else
void RegisterHubMusicTools() {}
void StopHubMusic() {}
void StartHubMusicBootTest() {}
#endif
