#pragma once
#include "sdkconfig.h"

#if CONFIG_BREAD_COMPACT_WIFI_TFT_SDCARD
#include "compact_sd_gif_validation.h"
#ifndef CONFIG_BREAD_COMPACT_WIFI_TFT_GIF_DIRECTORY
#define CONFIG_BREAD_COMPACT_WIFI_TFT_GIF_DIRECTORY "Kira-Emotes/Kira-Emotes/gif"
#endif
#ifndef CONFIG_BREAD_COMPACT_WIFI_TFT_SD_MHZ
#define CONFIG_BREAD_COMPACT_WIFI_TFT_SD_MHZ 40
#endif
#include <lvgl.h>
#include <sdmmc_cmd.h>
#include <array>
#include <atomic>
#include <string>

class CompactSdGif {
private:
    static constexpr auto kPrefixStorage =
        NormalizeCompactSdGifPath(CONFIG_BREAD_COMPACT_WIFI_TFT_GIF_DIRECTORY "/");

public:
    static constexpr const char* kRelativePrefix = kPrefixStorage.value;
    static constexpr size_t kRelativePrefixLength = sizeof(kPrefixStorage.value) - 1;
    ~CompactSdGif();
    bool Start();
    bool IsReady() const { return state_.load() == State::Ready; }
    bool IsInitializing() const { return state_.load() == State::Mounting; }
    size_t Count() const { return IsReady() ? count_ : 0; }
    const char* Name(size_t index) const { return index < Count() ? names_[index].data() : ""; }
    bool Contains(const std::string& name) const;
    bool ValidateFile(const std::string& name, unsigned& width, unsigned& height) const;

private:
    enum class State { Idle, Mounting, Ready, Failed };
    static void MountTask(void* arg);
    void Mount();
    static void* Open(lv_fs_drv_t* drv, const char* path, lv_fs_mode_t mode);
    static lv_fs_res_t Close(lv_fs_drv_t*, void* file);
    static lv_fs_res_t Read(lv_fs_drv_t*, void* file, void* buffer, uint32_t bytes, uint32_t* read);
    static lv_fs_res_t Seek(lv_fs_drv_t*, void* file, uint32_t pos, lv_fs_whence_t whence);
    static lv_fs_res_t Tell(lv_fs_drv_t*, void* file, uint32_t* pos);
    std::atomic<State> state_{State::Idle};
    sdmmc_card_t* card_ = nullptr;
    lv_fs_drv_t fs_driver_{};
    bool registered_ = false;
    std::array<std::array<char, 65>, 64> names_{};
    size_t count_ = 0;
};
#endif
