#pragma once

#include "camera.h"

#include <driver/gpio.h>
#include <driver/uart.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

class K210UartCamera : public Camera {
public:
    K210UartCamera(uart_port_t uart_port, gpio_num_t tx_pin, gpio_num_t rx_pin, int baud_rate);
    ~K210UartCamera() override;

    void SetExplainUrl(const std::string& url, const std::string& token) override;
    bool Capture() override;
    bool SetHMirror(bool enabled) override;
    bool SetVFlip(bool enabled) override;
    std::string Explain(const std::string& question) override;

private:
    static constexpr size_t kHeaderSize = 20;
    static constexpr size_t kMaxJpegSize = 512 * 1024;
    static constexpr uint8_t kProtocolVersion = 1;
    static constexpr uint8_t kCaptureRequest = 0x01;
    static constexpr uint8_t kPingRequest = 0x02;
    static constexpr uint8_t kLinkAck = 0x03;
    static constexpr uint8_t kJpegResponse = 0x81;
    static constexpr uint8_t kPongResponse = 0x82;
    static constexpr uint8_t kErrorResponse = 0xFF;

    bool ReadExact(uint8_t* data, size_t length, int timeout_ms);
    bool ReadHeader(uint8_t* header, int timeout_ms);
    bool WritePacket(uint8_t type, uint32_t request_id, const uint8_t* payload,
                     size_t payload_length);
    void ProbeLoop();
    void ClearFrame();

    static uint32_t ReadLe32(const uint8_t* data);
    static void WriteLe32(uint8_t* data, uint32_t value);
    static uint32_t Crc32(const uint8_t* data, size_t length);

    uart_port_t uart_port_;
    bool ready_ = false;
    uint32_t next_request_id_ = 1;
    uint8_t* jpeg_data_ = nullptr;
    size_t jpeg_size_ = 0;
    std::string explain_url_;
    std::string explain_token_;
    std::mutex capture_mutex_;
    std::atomic<bool> probe_running_{false};
    std::atomic<bool> linked_{false};
    std::thread probe_thread_;
};
