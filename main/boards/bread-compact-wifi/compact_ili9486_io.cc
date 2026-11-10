#include "compact_ili9486_io.h"

#if CONFIG_BREAD_COMPACT_WIFI_TFT_ILI9486_480X320
#include "compact_rgb565_to_rgb666.h"

#include <driver/gpio.h>
#include <esp_heap_caps.h>
#include <esp_lcd_panel_io_interface.h>
#include <esp_rom_sys.h>
#include <freertos/FreeRTOS.h>

#include <algorithm>
#include <cstdint>
#include <new>

namespace {
constexpr size_t kChunkBytes = 32768;
constexpr size_t kColorChunkPixels = 512;

struct Ili9486Io {
    esp_lcd_panel_io_t base{};
    spi_device_handle_t device = nullptr;
    gpio_num_t dc = GPIO_NUM_NC;
    esp_lcd_panel_io_color_trans_done_cb_t color_done = nullptr;
    void* user_context = nullptr;
    uint8_t* color_buffer = nullptr;
    size_t row_pixels = 480;
};

Ili9486Io* Context(esp_lcd_panel_io_t* io) { return reinterpret_cast<Ili9486Io*>(io); }

esp_err_t WriteBytes(Ili9486Io* io, const void* data, size_t size) {
    auto* bytes = static_cast<const uint8_t*>(data);
    while (size > 0) {
        const size_t chunk = std::min(size, kChunkBytes);
        spi_transaction_t transaction{};
        transaction.length = chunk * 8;
        transaction.tx_buffer = bytes;
        // Deliberately release CS after every transaction, as in Robot.
        const esp_err_t error = spi_device_polling_transmit(io->device, &transaction);
        if (error != ESP_OK) {
            return error;
        }
        bytes += chunk;
        size -= chunk;
    }
    return ESP_OK;
}

esp_err_t WritePixels(Ili9486Io* io, const void* data, size_t size) {
    auto* source = static_cast<const uint8_t*>(data);
    while (size > 0) {
        // The working diagnostic releases CS at each complete image row.
        // Follow CASET rather than splitting every 512 pixels across rows.
        const size_t pixels = std::min(size / 2, io->row_pixels);
        for (size_t i = 0; i < pixels; ++i) {
            CompactRgb565ToRgb666(io->color_buffer + i * 3, source[i * 2], source[i * 2 + 1]);
        }
        const esp_err_t error = WriteBytes(io, io->color_buffer, pixels * 3);
        if (error != ESP_OK) {
            return error;
        }
        source += pixels * 2;
        size -= pixels * 2;
    }
    return ESP_OK;
}

esp_err_t Transfer(esp_lcd_panel_io_t* base, int command, const void* data, size_t size,
                   bool color) {
    if (command < -1 || command > 255 || (size > 0 && data == nullptr) || (color && size % 2)) {
        return ESP_ERR_INVALID_ARG;
    }
    auto* io = Context(base);
    esp_err_t error = spi_device_acquire_bus(io->device, portMAX_DELAY);
    if (error != ESP_OK) {
        return error;
    }
    if (command >= 0) {
        error = gpio_set_level(io->dc, 0);
        if (error == ESP_OK) {
            const uint8_t value = static_cast<uint8_t>(command);
            error = WriteBytes(io, &value, sizeof(value));
            esp_rom_delay_us(10);
        }
    }
    if (error == ESP_OK && size > 0) {
        error = gpio_set_level(io->dc, 1);
        if (error == ESP_OK) {
            error = color ? WritePixels(io, data, size) : WriteBytes(io, data, size);
        }
    }
    spi_device_release_bus(io->device);
    return error;
}

esp_err_t Write(esp_lcd_panel_io_t* base, int command, const void* data, size_t size) {
    size_t row_pixels = 0;
    if (command == 0x2A && data != nullptr && size == 4) {
        const auto* bytes = static_cast<const uint8_t*>(data);
        const unsigned first = (unsigned(bytes[0]) << 8) | bytes[1];
        const unsigned last = (unsigned(bytes[2]) << 8) | bytes[3];
        if (last < first || last - first + 1 > kColorChunkPixels) {
            return ESP_ERR_INVALID_ARG;
        }
        row_pixels = last - first + 1;
    }
    const esp_err_t error = Transfer(base, command, data, size, false);
    if (error == ESP_OK && row_pixels > 0) {
        Context(base)->row_pixels = row_pixels;
    }
    return error;
}

esp_err_t WriteColor(esp_lcd_panel_io_t* base, int command, const void* data, size_t size) {
    const esp_err_t error = Transfer(base, command, data, size, true);
    auto* io = Context(base);
    if (error == ESP_OK && io->color_done != nullptr) {
        // Polling has finished: LVGL may now reuse its draw buffer. This runs
        // in the calling task, not an ISR, so the callback's yield flag is unused.
        io->color_done(base, nullptr, io->user_context);
    }
    return error;
}

esp_err_t RegisterCallbacks(esp_lcd_panel_io_t* base, const esp_lcd_panel_io_callbacks_t* callbacks,
                            void* context) {
    if (callbacks == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    auto* io = Context(base);
    io->color_done = callbacks->on_color_trans_done;
    io->user_context = context;
    return ESP_OK;
}

esp_err_t Read(esp_lcd_panel_io_t*, int, void*, size_t) { return ESP_ERR_NOT_SUPPORTED; }

esp_err_t Delete(esp_lcd_panel_io_t* base) {
    auto* io = Context(base);
    const esp_err_t error = spi_bus_remove_device(io->device);
    if (error != ESP_OK) {
        return error;
    }
    gpio_reset_pin(io->dc);
    heap_caps_free(io->color_buffer);
    delete io;
    return ESP_OK;
}
}  // namespace

esp_err_t NewCompactIli9486Io(spi_host_device_t bus, const esp_lcd_panel_io_spi_config_t& config,
                              esp_lcd_panel_io_handle_t* result) {
    if (result == nullptr || !GPIO_IS_VALID_OUTPUT_GPIO(config.dc_gpio_num)) {
        return ESP_ERR_INVALID_ARG;
    }
    *result = nullptr;
    auto* io = new (std::nothrow) Ili9486Io;
    if (io == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    // Allocate once; no per-frame or per-chunk heap allocations in the LVGL task.
    io->color_buffer = static_cast<uint8_t*>(
        heap_caps_malloc(kColorChunkPixels * 3, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    if (io->color_buffer == nullptr) {
        delete io;
        return ESP_ERR_NO_MEM;
    }
    io->dc = static_cast<gpio_num_t>(config.dc_gpio_num);
    gpio_config_t gpio{};
    gpio.pin_bit_mask = 1ULL << config.dc_gpio_num;
    gpio.mode = GPIO_MODE_OUTPUT;
    esp_err_t error = gpio_config(&gpio);
    if (error == ESP_OK) {
        error = gpio_set_level(io->dc, 0);
    }
    if (error == ESP_OK) {
        spi_device_interface_config_t device{};
        device.clock_speed_hz = config.pclk_hz;
        device.mode = config.spi_mode;
        device.spics_io_num = config.cs_gpio_num;
        device.queue_size = 1;
        device.flags = SPI_DEVICE_HALFDUPLEX;
        error = spi_bus_add_device(bus, &device, &io->device);
    }
    if (error != ESP_OK) {
        gpio_reset_pin(io->dc);
        heap_caps_free(io->color_buffer);
        delete io;
        return error;
    }
    io->base.rx_param = Read;
    io->base.tx_param = Write;
    io->base.tx_color = WriteColor;
    io->base.del = Delete;
    io->base.register_event_callbacks = RegisterCallbacks;
    *result = &io->base;
    return ESP_OK;
}
#endif
