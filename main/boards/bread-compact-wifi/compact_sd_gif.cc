#include "compact_sd_gif.h"

#if CONFIG_BREAD_COMPACT_WIFI_TFT_SDCARD
#include "compact_sd_gif_validation.h"
#include "config.h"

#include <driver/sdmmc_host.h>
#include <esp_log.h>
#include <esp_lvgl_port.h>
#include <esp_vfs_fat.h>
#include <dirent.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sys/stat.h>
#include <cstdio>
#include <cstring>

namespace {
constexpr char kTag[] = "CompactSdGif";
constexpr char kMountPath[] = "/sdcard";
constexpr auto kDirectoryStorage =
    NormalizeCompactSdGifPath("/sdcard/" CONFIG_BREAD_COMPACT_WIFI_TFT_GIF_DIRECTORY);
constexpr const auto& kGifDirectory = kDirectoryStorage.value;
static_assert(IsCompactSdGifDirectory(CONFIG_BREAD_COMPACT_WIFI_TFT_GIF_DIRECTORY,
                                      sizeof(CONFIG_BREAD_COMPACT_WIFI_TFT_GIF_DIRECTORY) - 1),
              "TFT GIF directory must be an SD-relative path of 1..128 bytes without . or ..");
constexpr size_t kMaxGifBytes = 4 * 1024 * 1024;

class GifFileReader {
public:
    GifFileReader(FILE* file, size_t size) : file_(file), remaining_(size) {}
    bool Read(unsigned char* buffer, size_t bytes) {
        if (bytes > remaining_ || std::fread(buffer, 1, bytes, file_) != bytes) {
            return false;
        }
        remaining_ -= bytes;
        return true;
    }
    bool Skip(size_t bytes) {
        if (bytes > remaining_ || std::fseek(file_, bytes, SEEK_CUR) != 0) {
            return false;
        }
        remaining_ -= bytes;
        return true;
    }
    size_t Remaining() const { return remaining_; }

private:
    FILE* file_;
    size_t remaining_;
};

bool ValidatePath(const char* path, unsigned& width, unsigned& height) {
    struct stat st{};
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 13 ||
        static_cast<size_t>(st.st_size) > kMaxGifBytes) {
        return false;
    }
    FILE* file = std::fopen(path, "rb");
    if (file == nullptr) {
        return false;
    }
    GifFileReader reader(file, st.st_size);
    const bool valid = ValidateCompactSdGifStream(reader, width, height);
    std::fclose(file);
    return valid;
}
}  // namespace

bool CompactSdGif::Start() {
    State expected = State::Idle;
    if (!state_.compare_exchange_strong(expected, State::Mounting)) {
        return false;
    }
    if (xTaskCreate(MountTask, "sd_gif_mount", 4096, this, 2, nullptr) != pdPASS) {
        state_ = State::Failed;
        ESP_LOGE(kTag, "Could not create SD mount task");
        return false;
    }
    return true;
}

void CompactSdGif::MountTask(void* arg) {
    static_cast<CompactSdGif*>(arg)->Mount();
    vTaskDelete(nullptr);
}

void CompactSdGif::Mount() {
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.flags = SDMMC_HOST_FLAG_1BIT;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = SDCARD_CLK_PIN;
    slot.cmd = SDCARD_CMD_PIN;
    slot.d0 = SDCARD_D0_PIN;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    esp_vfs_fat_sdmmc_mount_config_t cfg = {};
    cfg.format_if_mount_failed = false;
    cfg.max_files = 4;
    cfg.allocation_unit_size = 16 * 1024;
    // A failed probe previously disabled SD GIFs for the entire boot. Retry in
    // this worker after settling, with lower transfer ceilings for marginal buses.
    // OCR still probes at the driver's identification clock: a lower ceiling
    // alone cannot repair a card that does not respond to initialization.
    constexpr int configured_khz = CONFIG_BREAD_COMPACT_WIFI_TFT_SD_MHZ * 1000;
    constexpr std::array<int, 4> frequencies_khz = {
        configured_khz, configured_khz < 20000 ? configured_khz : 20000, 10000, 10000};
    esp_err_t result = ESP_FAIL;
    vTaskDelay(pdMS_TO_TICKS(300));
    for (size_t attempt = 0; attempt < frequencies_khz.size(); ++attempt) {
        host.max_freq_khz = frequencies_khz[attempt];
        ESP_LOGI(kTag, "SD mount attempt %u/%u: 1-bit, clock ceiling %d kHz",
                 static_cast<unsigned>(attempt + 1), static_cast<unsigned>(frequencies_khz.size()),
                 host.max_freq_khz);
        result = esp_vfs_fat_sdmmc_mount(kMountPath, &host, &slot, &cfg, &card_);
        if (result == ESP_OK) {
            break;
        }
        // ESP-IDF's mount helper releases the host and card on failed probing.
        // Do not unmount/deinit a card that never mounted successfully.
        ESP_LOGW(kTag, "SD mount attempt failed: %s", esp_err_to_name(result));
        const bool transient = result == ESP_ERR_TIMEOUT || result == ESP_ERR_INVALID_RESPONSE ||
                               result == ESP_ERR_INVALID_CRC;
        if (!transient || attempt + 1 == frequencies_khz.size()) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(500 * (attempt + 1)));
    }
    if (result != ESP_OK) {
        ESP_LOGW(kTag,
                 "SD mount failed (CLK10 CMD11 D0=16): %s; card was not formatted. "
                 "Power off, reseat SD, check 3.3V/GND and CMD/D0 pull-ups, then reboot",
                 esp_err_to_name(result));
        state_ = State::Failed;
        return;
    }
    DIR* directory = opendir(kGifDirectory);
    if (directory != nullptr) {
        while (count_ < names_.size()) {
            auto* entry = readdir(directory);
            if (entry == nullptr) {
                break;
            }
            const size_t length = std::strlen(entry->d_name);
            if (!IsCompactSdGifName(entry->d_name, length)) {
                continue;
            }
            char path[sizeof(kGifDirectory) + 65];
            constexpr size_t prefix = sizeof(kGifDirectory) - 1;
            std::memcpy(path, kGifDirectory, prefix);
            path[prefix] = '/';
            std::memcpy(path + prefix + 1, entry->d_name, length + 1);
            unsigned width = 0, height = 0;
            if (ValidatePath(path, width, height)) {
                std::memcpy(names_[count_++].data(), entry->d_name, length + 1);
            }
        }
        closedir(directory);
    } else {
        ESP_LOGW(kTag, "Missing GIF directory: %s", kGifDirectory);
    }
    lvgl_port_lock(0);
    if (lv_fs_get_drv('S') != nullptr) {
        lvgl_port_unlock();
        ESP_LOGE(kTag, "LVGL drive S is already registered");
        esp_vfs_fat_sdcard_unmount(kMountPath, card_);
        card_ = nullptr;
        state_ = State::Failed;
        return;
    }
    lv_fs_drv_init(&fs_driver_);
    fs_driver_.letter = 'S';
    fs_driver_.user_data = this;
    fs_driver_.ready_cb = [](lv_fs_drv_t* drv) {
        return static_cast<CompactSdGif*>(drv->user_data)->IsReady();
    };
    fs_driver_.open_cb = Open;
    fs_driver_.close_cb = Close;
    fs_driver_.read_cb = Read;
    fs_driver_.seek_cb = Seek;
    fs_driver_.tell_cb = Tell;
    lv_fs_drv_register(&fs_driver_);
    registered_ = true;
    lvgl_port_unlock();
    state_ = State::Ready;
    ESP_LOGI(kTag, "SDMMC mounted: CLK10 CMD11 D0=16, 1-bit, clock ceiling %d kHz; %u GIFs in %s",
             host.max_freq_khz, static_cast<unsigned>(count_), kGifDirectory);
}

bool CompactSdGif::Contains(const std::string& name) const {
    if (!IsReady() || !IsCompactSdGifName(name.c_str(), name.size())) {
        return false;
    }
    for (size_t i = 0; i < count_; ++i) {
        if (name == names_[i].data()) {
            return true;
        }
    }
    return false;
}

bool CompactSdGif::ValidateFile(const std::string& name, unsigned& width, unsigned& height) const {
    if (!Contains(name)) {
        return false;
    }
    char path[sizeof(kGifDirectory) + 65];
    std::snprintf(path, sizeof(path), "%s/%s", kGifDirectory, name.c_str());
    return ValidatePath(path, width, height);
}

void* CompactSdGif::Open(lv_fs_drv_t* drv, const char* path, lv_fs_mode_t mode) {
    if (mode != LV_FS_MODE_RD || !static_cast<CompactSdGif*>(drv->user_data)->IsReady()) {
        return nullptr;
    }
    if (*path == '/') {
        ++path;
    }
    // The normalized prefix is exposed as a pointer. Its storage, not the
    // pointer's sizeof, determines where the GIF filename starts.
    constexpr size_t prefix_length = kRelativePrefixLength;
    if (std::strncmp(path, kRelativePrefix, prefix_length) != 0 ||
        !IsCompactSdGifName(path + prefix_length, std::strlen(path + prefix_length))) {
        return nullptr;
    }
    char full_path[sizeof(kGifDirectory) + 65];
    std::snprintf(full_path, sizeof(full_path), "%s/%s", kMountPath, path);
    return std::fopen(full_path, "rb");
}

lv_fs_res_t CompactSdGif::Close(lv_fs_drv_t*, void* file) {
    return std::fclose(static_cast<FILE*>(file)) == 0 ? LV_FS_RES_OK : LV_FS_RES_FS_ERR;
}

lv_fs_res_t CompactSdGif::Read(lv_fs_drv_t*, void* file, void* buffer, uint32_t bytes,
                               uint32_t* read) {
    *read = std::fread(buffer, 1, bytes, static_cast<FILE*>(file));
    return std::ferror(static_cast<FILE*>(file)) ? LV_FS_RES_FS_ERR : LV_FS_RES_OK;
}

lv_fs_res_t CompactSdGif::Seek(lv_fs_drv_t*, void* file, uint32_t pos, lv_fs_whence_t whence) {
    const int origin = whence == LV_FS_SEEK_SET   ? SEEK_SET
                       : whence == LV_FS_SEEK_CUR ? SEEK_CUR
                                                  : SEEK_END;
    return std::fseek(static_cast<FILE*>(file), pos, origin) == 0 ? LV_FS_RES_OK : LV_FS_RES_FS_ERR;
}

lv_fs_res_t CompactSdGif::Tell(lv_fs_drv_t*, void* file, uint32_t* pos) {
    const long value = std::ftell(static_cast<FILE*>(file));
    if (value < 0) {
        return LV_FS_RES_FS_ERR;
    }
    *pos = value;
    return LV_FS_RES_OK;
}

CompactSdGif::~CompactSdGif() {
    if (registered_) {
        lvgl_port_lock(0);
        lv_fs_remove_drive('S');
        lvgl_port_unlock();
    }
    if (card_ != nullptr) {
        esp_vfs_fat_sdcard_unmount(kMountPath, card_);
    }
}
#endif
