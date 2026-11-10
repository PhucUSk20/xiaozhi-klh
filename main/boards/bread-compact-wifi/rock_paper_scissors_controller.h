#ifndef ROCK_PAPER_SCISSORS_CONTROLLER_H
#define ROCK_PAPER_SCISSORS_CONTROLLER_H

#include "led/circular_strip.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <driver/i2c_master.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

class CompactOledEmojiDisplay;
class CompactTftDisplay;

class RockPaperScissorsController {
public:
    enum class DriveAction : uint8_t {
        kStopped,
        kForward,
        kBackward,
        kTurnLeft,
        kTurnRight,
    };

    struct RobotStatus {
        DriveAction drive_action;
        std::array<int, 3> arm_angles;
        bool game_running;
        bool hardware_ready;
    };

    RockPaperScissorsController(i2c_master_bus_handle_t i2c_bus, CompactOledEmojiDisplay* display,
                                CompactTftDisplay* tft_display = nullptr);
    bool StartGame(bool notify_server = false);
    void StopGame();
    // Called by the board on the application task before handling a detected wake word.
    void InterruptForWakeWord();
    void StopMovement();
    bool Drive(DriveAction action, uint32_t watchdog_ms = 600);
    bool SpinClockwise(uint32_t watchdog_ms = 600);
    bool SpinCounterClockwise(uint32_t watchdog_ms = 600);
    bool Dance();
    bool SetArmAngle(size_t arm_index, int angle);
    bool SetServoPulseUs(int channel, int pulse_us);
    void ResetArms();
    RobotStatus GetRobotStatus() const;
    static const char* DriveActionName(DriveAction action);
    static int GetArmMinimumAngle(size_t arm_index);
    static int GetArmMaximumAngle(size_t arm_index);
    bool IsRunning() const { return running_.load(); }

private:
    static constexpr int kAttemptCount = 5;

    struct MovementCommand {
        int servo11_pulse_us;
        int servo12_pulse_us;
        uint32_t duration_ms;
        DriveAction action;
    };

    i2c_master_dev_handle_t pca9685_ = nullptr;
    std::atomic<bool> pca9685_initialized_{false};
    CircularStrip matrix_;
    CompactOledEmojiDisplay* display_ = nullptr;
    CompactTftDisplay* tft_display_ = nullptr;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<DriveAction> drive_action_{DriveAction::kStopped};
    std::atomic<bool> movement_stop_requested_{false};
    std::atomic<int> arm_1_angle_{0};
    std::atomic<int> arm_2_angle_{15};
    std::atomic<int> arm_3_angle_{0};
    QueueHandle_t movement_queue_ = nullptr;
    bool game_audio_paused_ = false;  // Owned by the application task.

    static void GameTask(void* arg);
    static void MovementTask(void* arg);
    void RunGame();
    void ShowGameCountdown(int digit);
    void ShowGameRound(int round);
    bool InitializePca9685();
    bool SetServoAngle(int channel, int angle);
    bool SetMovementServoPulses(int servo11_pulse_us, int servo12_pulse_us);
    bool QueueMovement(int servo11_pulse_us, int servo12_pulse_us, uint32_t duration_ms,
                       DriveAction action);
    bool QueueDance();
    float ReadDistanceCm();
    void ShowDigit(int digit);
    void ShowReady();
    void ShowFinished();
    bool ShowVictoryCelebration();
    void ClearMatrix();
    void SetOrientedMatrixColors(const std::vector<StripColor>& logical_colors);
    bool WaitOrStop(uint32_t duration_ms);
    void RestoreEmojiMode();
    void SuspendMicrophone();
    void ResumeMicrophone();
    void NotifyGameStarted();
    void PlayGameCompletedSound();
};

#endif  // ROCK_PAPER_SCISSORS_CONTROLLER_H
