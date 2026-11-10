#include "k210_uart_camera.h"

#include "board.h"
#include "system_info.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <stdexcept>

namespace {
constexpr char kTag[] = "K210UartCamera";
constexpr uint8_t kMagic[] = {'K', 'C', 'A', 'M'};
constexpr size_t kUploadChunkSize = 4096;
}  // namespace

K210UartCamera::K210UartCamera(uart_port_t uart_port, gpio_num_t tx_pin, gpio_num_t rx_pin,
                               int baud_rate)
    : uart_port_(uart_port) {
    uart_config_t uart_config = {};
    uart_config.baud_rate = baud_rate;
    uart_config.data_bits = UART_DATA_8_BITS;
    uart_config.parity = UART_PARITY_DISABLE;
    uart_config.stop_bits = UART_STOP_BITS_1;
    uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uart_config.source_clk = UART_SCLK_DEFAULT;

    bool driver_installed = false;
    esp_err_t err = uart_driver_install(uart_port_, 16 * 1024, 0, 0, nullptr, 0);
    if (err == ESP_OK) {
        driver_installed = true;
        err = uart_param_config(uart_port_, &uart_config);
    }
    if (err == ESP_OK) {
        err = uart_set_pin(uart_port_, tx_pin, rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "Failed to initialize UART%d: %s", static_cast<int>(uart_port_),
                 esp_err_to_name(err));
        if (driver_installed) {
            uart_driver_delete(uart_port_);
        }
        return;
    }

    ready_ = true;
    ESP_LOGI(kTag, "K210 camera UART ready: port=%d TX=%d RX=%d baud=%d",
             static_cast<int>(uart_port_), static_cast<int>(tx_pin), static_cast<int>(rx_pin),
             baud_rate);
    probe_running_ = true;
    probe_thread_ = std::thread([this]() { ProbeLoop(); });
}

K210UartCamera::~K210UartCamera() {
    probe_running_ = false;
    if (probe_thread_.joinable()) {
        probe_thread_.join();
    }
    ClearFrame();
    if (ready_) {
        uart_driver_delete(uart_port_);
    }
}

void K210UartCamera::SetExplainUrl(const std::string& url, const std::string& token) {
    explain_url_ = url;
    explain_token_ = token;
}

bool K210UartCamera::Capture() {
    std::lock_guard<std::mutex> lock(capture_mutex_);
    ClearFrame();
    if (!ready_) {
        ESP_LOGE(kTag, "Cannot capture because the K210 UART is not ready");
        return false;
    }

    uart_flush_input(uart_port_);
    const uint32_t request_id = next_request_id_++;
    if (!WritePacket(kCaptureRequest, request_id, nullptr, 0)) {
        ESP_LOGE(kTag, "Failed to send capture request %lu",
                 static_cast<unsigned long>(request_id));
        return false;
    }

    uint8_t header[kHeaderSize];
    if (!ReadHeader(header, 20000)) {
        ESP_LOGE(kTag, "Timed out waiting for K210 response");
        return false;
    }

    const uint8_t version = header[4];
    const uint8_t type = header[5];
    const uint32_t response_id = ReadLe32(header + 8);
    const uint32_t payload_length = ReadLe32(header + 12);
    const uint32_t expected_crc = ReadLe32(header + 16);
    if (version != kProtocolVersion || response_id != request_id) {
        ESP_LOGE(kTag, "Invalid response: version=%u request=%lu expected=%lu", version,
                 static_cast<unsigned long>(response_id), static_cast<unsigned long>(request_id));
        return false;
    }
    if (payload_length == 0 || payload_length > kMaxJpegSize) {
        ESP_LOGE(kTag, "Invalid K210 payload length: %lu",
                 static_cast<unsigned long>(payload_length));
        return false;
    }

    uint8_t* payload = static_cast<uint8_t*>(
        heap_caps_malloc(payload_length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (payload == nullptr) {
        payload = static_cast<uint8_t*>(heap_caps_malloc(payload_length, MALLOC_CAP_8BIT));
    }
    if (payload == nullptr) {
        ESP_LOGE(kTag, "Cannot allocate %lu bytes for K210 response",
                 static_cast<unsigned long>(payload_length));
        return false;
    }

    if (!ReadExact(payload, payload_length, 20000)) {
        ESP_LOGE(kTag, "Timed out while receiving K210 payload");
        heap_caps_free(payload);
        return false;
    }
    if (Crc32(payload, payload_length) != expected_crc) {
        ESP_LOGE(kTag, "K210 payload CRC32 mismatch");
        heap_caps_free(payload);
        return false;
    }

    if (type == kErrorResponse) {
        std::string error(reinterpret_cast<char*>(payload), payload_length);
        ESP_LOGE(kTag, "K210 capture failed: %s", error.c_str());
        heap_caps_free(payload);
        return false;
    }
    if (type != kJpegResponse || payload_length < 4 || payload[0] != 0xFF || payload[1] != 0xD8 ||
        payload[payload_length - 2] != 0xFF || payload[payload_length - 1] != 0xD9) {
        ESP_LOGE(kTag, "K210 returned an invalid JPEG frame");
        heap_caps_free(payload);
        return false;
    }

    jpeg_data_ = payload;
    jpeg_size_ = payload_length;
    linked_ = true;
    ESP_LOGI(kTag, "Received K210 JPEG: %lu bytes", static_cast<unsigned long>(jpeg_size_));
    return true;
}

bool K210UartCamera::SetHMirror(bool enabled) {
    (void)enabled;
    return false;
}

bool K210UartCamera::SetVFlip(bool enabled) {
    (void)enabled;
    return false;
}

std::string K210UartCamera::Explain(const std::string& question) {
    if (explain_url_.empty()) {
        throw std::runtime_error("Image explain URL or token is not set");
    }
    if (jpeg_data_ == nullptr || jpeg_size_ == 0) {
        throw std::runtime_error("No K210 camera frame captured");
    }

    auto network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(3);
    const std::string boundary = "----K210_CAMERA_BOUNDARY";
    http->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    http->SetHeader("Client-Id", Board::GetInstance().GetUuid().c_str());
    if (!explain_token_.empty()) {
        http->SetHeader("Authorization", "Bearer " + explain_token_);
    }
    http->SetHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
    http->SetHeader("Transfer-Encoding", "chunked");
    if (!http->Open("POST", explain_url_)) {
        throw std::runtime_error("Failed to connect to image explain URL");
    }

    std::string question_field = "--" + boundary + "\r\n";
    question_field += "Content-Disposition: form-data; name=\"question\"\r\n\r\n";
    question_field += question + "\r\n";
    http->Write(question_field.c_str(), question_field.size());

    std::string file_header = "--" + boundary + "\r\n";
    file_header +=
        "Content-Disposition: form-data; name=\"file\"; filename=\"k210-camera.jpg\"\r\n";
    file_header += "Content-Type: image/jpeg\r\n\r\n";
    http->Write(file_header.c_str(), file_header.size());

    for (size_t offset = 0; offset < jpeg_size_; offset += kUploadChunkSize) {
        const size_t length = std::min(kUploadChunkSize, jpeg_size_ - offset);
        http->Write(reinterpret_cast<const char*>(jpeg_data_ + offset), length);
    }
    const std::string footer = "\r\n--" + boundary + "--\r\n";
    http->Write(footer.c_str(), footer.size());
    http->Write("", 0);

    const int status_code = http->GetStatusCode();
    if (status_code != 200) {
        http->Close();
        throw std::runtime_error("Image explain request failed with HTTP status " +
                                 std::to_string(status_code));
    }
    std::string result = http->ReadAll();
    http->Close();
    return result;
}

bool K210UartCamera::ReadExact(uint8_t* data, size_t length, int timeout_ms) {
    const int64_t deadline = esp_timer_get_time() + static_cast<int64_t>(timeout_ms) * 1000;
    size_t offset = 0;
    while (offset < length) {
        const int64_t remaining_us = deadline - esp_timer_get_time();
        if (remaining_us <= 0) {
            return false;
        }
        const TickType_t wait_ticks =
            pdMS_TO_TICKS(std::max<int64_t>(1, std::min<int64_t>(100, remaining_us / 1000)));
        const int read = uart_read_bytes(uart_port_, data + offset, length - offset, wait_ticks);
        if (read < 0) {
            return false;
        }
        offset += static_cast<size_t>(read);
    }
    return true;
}

bool K210UartCamera::ReadHeader(uint8_t* header, int timeout_ms) {
    const int64_t deadline = esp_timer_get_time() + static_cast<int64_t>(timeout_ms) * 1000;
    size_t matched = 0;
    while (esp_timer_get_time() < deadline) {
        uint8_t byte = 0;
        if (!ReadExact(&byte, 1, 100)) {
            continue;
        }
        if (byte == kMagic[matched]) {
            header[matched++] = byte;
            if (matched == sizeof(kMagic)) {
                return ReadExact(
                    header + sizeof(kMagic), kHeaderSize - sizeof(kMagic),
                    std::max(1, static_cast<int>((deadline - esp_timer_get_time()) / 1000)));
            }
        } else {
            matched = byte == kMagic[0] ? 1 : 0;
            if (matched == 1) {
                header[0] = byte;
            }
        }
    }
    return false;
}

bool K210UartCamera::WritePacket(uint8_t type, uint32_t request_id, const uint8_t* payload,
                                 size_t payload_length) {
    uint8_t header[kHeaderSize] = {};
    memcpy(header, kMagic, sizeof(kMagic));
    header[4] = kProtocolVersion;
    header[5] = type;
    WriteLe32(header + 8, request_id);
    WriteLe32(header + 12, static_cast<uint32_t>(payload_length));
    WriteLe32(header + 16, Crc32(payload, payload_length));
    if (uart_write_bytes(uart_port_, header, sizeof(header)) != sizeof(header)) {
        return false;
    }
    if (payload_length > 0 &&
        uart_write_bytes(uart_port_, payload, payload_length) != static_cast<int>(payload_length)) {
        return false;
    }
    return uart_wait_tx_done(uart_port_, pdMS_TO_TICKS(1000)) == ESP_OK;
}

void K210UartCamera::ProbeLoop() {
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    unsigned int attempt = 0;
    while (probe_running_ && !linked_) {
        ++attempt;
        {
            std::lock_guard<std::mutex> lock(capture_mutex_);
            if (!probe_running_ || linked_) {
                break;
            }

            uart_flush_input(uart_port_);
            const uint32_t request_id = next_request_id_++;
            if (WritePacket(kPingRequest, request_id, nullptr, 0)) {
                uint8_t header[kHeaderSize];
                if (ReadHeader(header, 1000)) {
                    const uint8_t version = header[4];
                    const uint8_t type = header[5];
                    const uint32_t response_id = ReadLe32(header + 8);
                    const uint32_t payload_length = ReadLe32(header + 12);
                    const uint32_t payload_crc = ReadLe32(header + 16);
                    if (version == kProtocolVersion && type == kPongResponse &&
                        response_id == request_id && payload_length == 0 && payload_crc == 0) {
                        if (WritePacket(kLinkAck, request_id, nullptr, 0)) {
                            linked_ = true;
                            ESP_LOGI(kTag, "K210 UART link confirmed on attempt %u", attempt);
                            break;
                        }
                    }
                }
            }
        }

        if (attempt == 1 || attempt % 10 == 0) {
            ESP_LOGW(kTag, "Waiting for K210 UART handshake (attempt %u)", attempt);
        }
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
}

void K210UartCamera::ClearFrame() {
    if (jpeg_data_ != nullptr) {
        heap_caps_free(jpeg_data_);
        jpeg_data_ = nullptr;
    }
    jpeg_size_ = 0;
}

uint32_t K210UartCamera::ReadLe32(const uint8_t* data) {
    return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
}

void K210UartCamera::WriteLe32(uint8_t* data, uint32_t value) {
    data[0] = static_cast<uint8_t>(value);
    data[1] = static_cast<uint8_t>(value >> 8);
    data[2] = static_cast<uint8_t>(value >> 16);
    data[3] = static_cast<uint8_t>(value >> 24);
}

uint32_t K210UartCamera::Crc32(const uint8_t* data, size_t length) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
        }
    }
    return crc ^ 0xFFFFFFFF;
}
