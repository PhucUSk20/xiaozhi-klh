#include "rock_paper_scissors_controller.h"

#include "application.h"
#include "compact_oled_emoji_display.h"
#include "compact_tft_display.h"
#include "mcp_server.h"

#include <algorithm>
#include <array>
#include <vector>

#include <driver/gpio.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_rom_sys.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace {

constexpr char kTag[] = "RockPaperScissors";
constexpr uint8_t kPca9685Address = 0x41;
constexpr int kPca9685FrequencyHz = 50;
// PWM15 drives the buzzer. Keep this channel disabled so application startup is silent.
constexpr int kPca9685BuzzerChannel = 15;
constexpr gpio_num_t kUltrasonicTrigger = GPIO_NUM_42;
constexpr gpio_num_t kUltrasonicEcho = GPIO_NUM_7;
constexpr float kMinimumHandDistanceCm = 2.0f;
constexpr float kMaximumHandDistanceCm = 10.0f;
struct GameServoPose {
    int channel;
    int rest_angle;
    int play_angle;
};

constexpr std::array<GameServoPose, 3> kGameServoPoses = {{
    {8, 0, 50},
    {9, 15, 60},
    {10, 0, 50},
}};
// All three game gestures now use servo 1 (PCA9685 channel 8).
constexpr std::array<int, 3> kGameThrowAngles = {45, 92, 140};

static_assert(kGameServoPoses[0].rest_angle == 0);
constexpr int kMovementServoStopAngle = 90;
constexpr int kMovementServoStopPulseUs = CONFIG_BREAD_COMPACT_WIFI_MOVEMENT_STOP_PULSE_US;
constexpr int kForwardServo11PulseUs = CONFIG_BREAD_COMPACT_WIFI_FORWARD_LEFT_WHEEL_PULSE_US;
constexpr int kForwardServo12PulseUs = CONFIG_BREAD_COMPACT_WIFI_FORWARD_RIGHT_WHEEL_PULSE_US;
constexpr int kBackwardServo11PulseUs = CONFIG_BREAD_COMPACT_WIFI_BACKWARD_LEFT_WHEEL_PULSE_US;
constexpr int kBackwardServo12PulseUs = CONFIG_BREAD_COMPACT_WIFI_BACKWARD_RIGHT_WHEEL_PULSE_US;
constexpr int kSpinAroundServo11PulseUs = CONFIG_BREAD_COMPACT_WIFI_SPIN_LEFT_WHEEL_PULSE_US;
constexpr int kSpinAroundServo12PulseUs = CONFIG_BREAD_COMPACT_WIFI_SPIN_RIGHT_WHEEL_PULSE_US;
constexpr int kSpinCounterServo11PulseUs = 2500;
constexpr int kSpinCounterServo12PulseUs = 2500;
constexpr int kTurnLeftServo11PulseUs = CONFIG_BREAD_COMPACT_WIFI_TURN_LEFT_LEFT_WHEEL_PULSE_US;
constexpr int kTurnLeftServo12PulseUs = CONFIG_BREAD_COMPACT_WIFI_TURN_LEFT_RIGHT_WHEEL_PULSE_US;
constexpr int kTurnRightServo11PulseUs = CONFIG_BREAD_COMPACT_WIFI_TURN_RIGHT_LEFT_WHEEL_PULSE_US;
constexpr int kTurnRightServo12PulseUs = CONFIG_BREAD_COMPACT_WIFI_TURN_RIGHT_RIGHT_WHEEL_PULSE_US;
constexpr int kDanceClockwiseServo11PulseUs =
    CONFIG_BREAD_COMPACT_WIFI_DANCE_CLOCKWISE_LEFT_WHEEL_PULSE_US;
constexpr int kDanceClockwiseServo12PulseUs =
    CONFIG_BREAD_COMPACT_WIFI_DANCE_CLOCKWISE_RIGHT_WHEEL_PULSE_US;
constexpr int kDanceCounterServo11PulseUs =
    CONFIG_BREAD_COMPACT_WIFI_DANCE_COUNTER_LEFT_WHEEL_PULSE_US;
constexpr int kDanceCounterServo12PulseUs =
    CONFIG_BREAD_COMPACT_WIFI_DANCE_COUNTER_RIGHT_WHEEL_PULSE_US;
constexpr int kDanceCommandPulseUs = -1;
constexpr int kDanceStepDurationMs = CONFIG_BREAD_COMPACT_WIFI_DANCE_STEP_DURATION_MS;
constexpr int kDanceCycles = 3;
constexpr int kDefaultMovementDurationSeconds = 1;
constexpr int kMaximumMovementDurationSeconds = 60;
constexpr uint8_t kOnePercentBrightness = 3;
constexpr int kVictoryFlashCount = 3;
constexpr uint32_t kVictoryOnTimeMs = 250;
constexpr uint32_t kVictoryOffTimeMs = 120;
constexpr std::array<StripColor, 8> kVictoryColors = {{
    {32, 0, 0},
    {32, 12, 0},
    {28, 28, 0},
    {0, 32, 0},
    {0, 24, 32},
    {0, 0, 32},
    {24, 0, 32},
    {32, 0, 18},
}};

extern const uint8_t kGameStartPromptBegin[] asm("_binary_game_start_prompt_ogg_start");
extern const uint8_t kGameStartPromptEnd[] asm("_binary_game_start_prompt_ogg_end");
extern const uint8_t kGameCompletePromptBegin[] asm("_binary_game_complete_prompt_ogg_start");
extern const uint8_t kGameCompletePromptEnd[] asm("_binary_game_complete_prompt_ogg_end");

constexpr std::array<std::array<uint8_t, 25>, 6> kDigits = {{
    {0, 1, 1, 1, 0, 1, 0, 0, 0, 1, 1, 0, 0, 0, 1, 1, 0, 0, 0, 1, 0, 1, 1, 1, 0},
    {0, 0, 1, 0, 0, 0, 1, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 1, 1, 1, 0},
    {0, 1, 1, 1, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 1, 1, 1, 1, 1},
    {1, 1, 1, 1, 0, 0, 0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 0},
    {1, 0, 0, 1, 0, 1, 0, 0, 1, 0, 1, 1, 1, 1, 1, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0},
    {1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 0},
}};

bool IsHandDetected(float distance_cm) {
    return distance_cm >= kMinimumHandDistanceCm && distance_cm < kMaximumHandDistanceCm;
}

}  // namespace

RockPaperScissorsController::RockPaperScissorsController(i2c_master_bus_handle_t i2c_bus,
                                                         CompactOledEmojiDisplay* display,
                                                         CompactTftDisplay* tft_display)
    : matrix_(GPIO_NUM_46, 25), display_(display), tft_display_(tft_display) {
    i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = kPca9685Address,
        .scl_speed_hz = 400000,
        .scl_wait_us = 0,
        .flags = {},
    };
    const esp_err_t err = i2c_master_bus_add_device(i2c_bus, &device_config, &pca9685_);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "Failed to add PCA9685 at 0x41: %s", esp_err_to_name(err));
    }

    gpio_config_t trigger_config = {
        .pin_bit_mask = 1ULL << kUltrasonicTrigger,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&trigger_config);
    gpio_set_level(kUltrasonicTrigger, 0);

    gpio_config_t echo_config = {
        .pin_bit_mask = 1ULL << kUltrasonicEcho,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&echo_config);

    if (InitializePca9685()) {
        for (const auto& servo : kGameServoPoses) {
            SetServoAngle(servo.channel, servo.rest_angle);
        }
        SetMovementServoPulses(kMovementServoStopPulseUs, kMovementServoStopPulseUs);
    }

    movement_queue_ = xQueueCreate(1, sizeof(MovementCommand));
    if (movement_queue_ == nullptr ||
        xTaskCreate(MovementTask, "robot_movement", 3072, this, 3, nullptr) != pdPASS) {
        ESP_LOGE(kTag, "Failed to create robot movement task");
        if (movement_queue_ != nullptr) {
            vQueueDelete(movement_queue_);
            movement_queue_ = nullptr;
        }
    }

    McpServer::GetInstance().AddTool(
        "self.game.play_rock_paper_scissors",
        "Start one physical Rock Paper Scissors game on KLeaderBit V4. The board counts down "
        "3, 2, 1, then waits for the player to place a hand within 10 cm of the Port 4 "
        "ultrasonic sensor for each of five throws. Each throw randomly moves servo 1 to "
        "45, 92, or 140 degrees, then always returns to its 0-degree resting position. "
        "Call this once when the user asks to play; "
        "do not call it again while a game is running.",
        PropertyList(), [this](const PropertyList&) -> ReturnValue {
            if (!StartGame(false)) {
                return std::string("A Rock Paper Scissors game is already running.");
            }
            return std::string("Rock Paper Scissors started: countdown 3, 2, 1, then five throws.");
        });

    McpServer::GetInstance().AddTool(
        "self.robot.move_forward",
        "Move the physical robot forward for duration_seconds. Pass the duration stated by the "
        "user; if the user gives no duration, omit it and use the 1-second default.",
        PropertyList(
            {Property("duration_seconds", kPropertyTypeInteger, kDefaultMovementDurationSeconds, 1,
                      kMaximumMovementDurationSeconds)}),
        [this](const PropertyList& properties) -> ReturnValue {
            const int duration = properties["duration_seconds"].value<int>();
            if (!QueueMovement(kForwardServo11PulseUs, kForwardServo12PulseUs, duration * 1000,
                               DriveAction::kForward)) {
                return std::string("Failed to move the robot forward.");
            }
            return std::string("Robot moving forward for ") + std::to_string(duration) +
                   " second(s).";
        });

    McpServer::GetInstance().AddTool(
        "self.robot.move_backward",
        "Move the physical robot backward for duration_seconds. Pass the duration stated by the "
        "user; if the user gives no duration, omit it and use the 1-second default.",
        PropertyList(
            {Property("duration_seconds", kPropertyTypeInteger, kDefaultMovementDurationSeconds, 1,
                      kMaximumMovementDurationSeconds)}),
        [this](const PropertyList& properties) -> ReturnValue {
            const int duration = properties["duration_seconds"].value<int>();
            if (!QueueMovement(kBackwardServo11PulseUs, kBackwardServo12PulseUs, duration * 1000,
                               DriveAction::kBackward)) {
                return std::string("Failed to move the robot backward.");
            }
            return std::string("Robot moving backward for ") + std::to_string(duration) +
                   " second(s).";
        });

    McpServer::GetInstance().AddTool(
        "self.robot.turn_left",
        "Turn the physical robot left for duration_seconds by stopping the left wheel and "
        "driving the right wheel forward. Use the user's duration, or the 1-second default.",
        PropertyList(
            {Property("duration_seconds", kPropertyTypeInteger, kDefaultMovementDurationSeconds, 1,
                      kMaximumMovementDurationSeconds)}),
        [this](const PropertyList& properties) -> ReturnValue {
            const int duration = properties["duration_seconds"].value<int>();
            if (!QueueMovement(kTurnLeftServo11PulseUs, kTurnLeftServo12PulseUs, duration * 1000,
                               DriveAction::kTurnLeft)) {
                return std::string("Failed to turn the robot left.");
            }
            return std::string("Robot turning left for ") + std::to_string(duration) +
                   " second(s).";
        });

    McpServer::GetInstance().AddTool(
        "self.robot.turn_right",
        "Turn the physical robot right for duration_seconds by driving the left wheel forward "
        "and stopping the right wheel. Use the user's duration, or the 1-second default.",
        PropertyList(
            {Property("duration_seconds", kPropertyTypeInteger, kDefaultMovementDurationSeconds, 1,
                      kMaximumMovementDurationSeconds)}),
        [this](const PropertyList& properties) -> ReturnValue {
            const int duration = properties["duration_seconds"].value<int>();
            if (!QueueMovement(kTurnRightServo11PulseUs, kTurnRightServo12PulseUs, duration * 1000,
                               DriveAction::kTurnRight)) {
                return std::string("Failed to turn the robot right.");
            }
            return std::string("Robot turning right for ") + std::to_string(duration) +
                   " second(s).";
        });

    McpServer::GetInstance().AddTool(
        "self.robot.spin_around",
        "Spin the physical robot slowly clockwise in place. If the user gives a duration, spin "
        "for duration_seconds. If no duration is given, omit it so the robot keeps spinning "
        "until the user says the wake word again.",
        PropertyList({Property("duration_seconds", kPropertyTypeInteger, 0, 0,
                               kMaximumMovementDurationSeconds)}),
        [this](const PropertyList& properties) -> ReturnValue {
            const int duration = properties["duration_seconds"].value<int>();
            if (!QueueMovement(kSpinAroundServo11PulseUs, kSpinAroundServo12PulseUs,
                               duration * 1000, DriveAction::kTurnRight)) {
                return std::string("Failed to spin the robot around.");
            }
            if (duration <= 0) {
                return std::string(
                    "Robot spinning slowly until the user says the wake word again.");
            }
            return std::string("Robot spinning around for ") + std::to_string(duration) +
                   " second(s).";
        });

    McpServer::GetInstance().AddTool(
        "self.robot.dance",
        "Make the physical robot dance when the user says dance, do a dance, show me a dance, "
        "nhay, nhay di, nhay mua, mua di, lac lu, quay len, or asks the robot to perform, move "
        "to music, celebrate, groove, boogie, or show some moves. The "
        "robot moves forward, alternates clockwise and counterclockwise spins three times, "
        "then moves backward. All three upper arms perform a synchronized choreography "
        "throughout the complete movement sequence, then return to rest.",
        PropertyList(), [this](const PropertyList&) -> ReturnValue {
            if (!QueueDance()) {
                return std::string("Failed to start the robot dance.");
            }
            return std::string("Robot dance started.");
        });
}

bool RockPaperScissorsController::StartGame(bool notify_server) {
    if (Application::GetInstance().GetAudioService().IsInputSuspended()) {
        ESP_LOGW(kTag, "Game cannot start while another activity owns microphone input");
        return false;
    }
    if (running_.exchange(true)) {
        return false;
    }
    stop_requested_ = false;
    SuspendMicrophone();
    if (xTaskCreate(GameTask, "rps_game", 4096, this, 3, nullptr) != pdPASS) {
        ResumeMicrophone();
        running_ = false;
        return false;
    }
    if (notify_server) {
        NotifyGameStarted();
    }
    return true;
}

void RockPaperScissorsController::StopGame() {
    if (running_) {
        stop_requested_ = true;
        ESP_LOGI(kTag, "Game stop requested from Button A");
    }
}

void RockPaperScissorsController::InterruptForWakeWord() {
    if (!running_) {
        return;
    }
    stop_requested_ = true;
    // Release the uplink gate before the application starts the wake acknowledgement/listening.
    // The game task will finish servo/display cleanup asynchronously.
    ResumeMicrophone();
    ESP_LOGI(kTag, "Game interrupted by wake word");
}

void RockPaperScissorsController::StopMovement() {
    if (movement_queue_ == nullptr) {
        return;
    }
    movement_stop_requested_ = true;
    const MovementCommand command = {
        .servo11_pulse_us = kMovementServoStopPulseUs,
        .servo12_pulse_us = kMovementServoStopPulseUs,
        .duration_ms = 0,
        .action = DriveAction::kStopped,
    };
    drive_action_ = DriveAction::kStopped;
    xQueueOverwrite(movement_queue_, &command);
    ESP_LOGI(kTag, "Queued an immediate wheel stop");
}

bool RockPaperScissorsController::Drive(DriveAction action, uint32_t watchdog_ms) {
    if (running_) {
        ESP_LOGW(kTag, "Remote drive rejected while the game is running");
        return false;
    }
    watchdog_ms = std::clamp<uint32_t>(watchdog_ms, 200, 2000);
    switch (action) {
        case DriveAction::kForward:
            return QueueMovement(kForwardServo11PulseUs, kForwardServo12PulseUs, watchdog_ms,
                                 action);
        case DriveAction::kBackward:
            return QueueMovement(kBackwardServo11PulseUs, kBackwardServo12PulseUs, watchdog_ms,
                                 action);
        case DriveAction::kTurnLeft:
            return QueueMovement(kTurnLeftServo11PulseUs, kTurnLeftServo12PulseUs, watchdog_ms,
                                 action);
        case DriveAction::kTurnRight:
            return QueueMovement(kTurnRightServo11PulseUs, kTurnRightServo12PulseUs, watchdog_ms,
                                 action);
        case DriveAction::kStopped:
            StopMovement();
            return true;
    }
    return false;
}

bool RockPaperScissorsController::SpinClockwise(uint32_t watchdog_ms) {
    if (running_) {
        ESP_LOGW(kTag, "Remote clockwise spin rejected while the game is running");
        return false;
    }
    watchdog_ms = std::clamp<uint32_t>(watchdog_ms, 200, 2000);
    return QueueMovement(kSpinAroundServo11PulseUs, kSpinAroundServo12PulseUs, watchdog_ms,
                         DriveAction::kTurnRight);
}

bool RockPaperScissorsController::SpinCounterClockwise(uint32_t watchdog_ms) {
    if (running_) {
        ESP_LOGW(kTag, "Remote counter-clockwise spin rejected while the game is running");
        return false;
    }
    watchdog_ms = std::clamp<uint32_t>(watchdog_ms, 200, 2000);
    return QueueMovement(kSpinCounterServo11PulseUs, kSpinCounterServo12PulseUs, watchdog_ms,
                         DriveAction::kTurnLeft);
}

bool RockPaperScissorsController::Dance() {
    if (running_) {
        ESP_LOGW(kTag, "Remote dance rejected while the game is running");
        return false;
    }
    return QueueDance();
}

bool RockPaperScissorsController::SetArmAngle(size_t arm_index, int angle) {
    if (running_ || arm_index >= kGameServoPoses.size()) {
        return false;
    }
    angle = std::clamp(angle, GetArmMinimumAngle(arm_index), GetArmMaximumAngle(arm_index));
    if (!SetServoAngle(kGameServoPoses[arm_index].channel, angle)) {
        return false;
    }
    switch (arm_index) {
        case 0:
            arm_1_angle_ = angle;
            break;
        case 1:
            arm_2_angle_ = angle;
            break;
        case 2:
            arm_3_angle_ = angle;
            break;
    }
    ESP_LOGI(kTag, "Remote arm %u moved to %d degrees", static_cast<unsigned>(arm_index + 1),
             angle);
    return true;
}

bool RockPaperScissorsController::SetServoPulseUs(int channel, int pulse_us) {
    if (running_ || channel < 0 || channel >= 16 || channel == kPca9685BuzzerChannel ||
        pulse_us < 500 || pulse_us > 3000 || !InitializePca9685()) {
        return false;
    }
    const int ticks = std::clamp(
        static_cast<int>(static_cast<float>(pulse_us) * kPca9685FrequencyHz * 4096.0f / 1000000.0f +
                         0.5f),
        1, 4095);
    const uint8_t first_register = static_cast<uint8_t>(0x06 + 4 * channel);
    const uint8_t payload[] = {first_register, 0x00, 0x00, static_cast<uint8_t>(ticks & 0xFF),
                               static_cast<uint8_t>((ticks >> 8) & 0x0F)};
    const esp_err_t err = i2c_master_transmit(pca9685_, payload, sizeof(payload), 100);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "PCA9685 raw pulse failed channel=%d pulse=%d: %s", channel, pulse_us,
                 esp_err_to_name(err));
        return false;
    }
    return true;
}

void RockPaperScissorsController::ResetArms() {
    for (size_t index = 0; index < kGameServoPoses.size(); ++index) {
        SetArmAngle(index, kGameServoPoses[index].rest_angle);
    }
}

RockPaperScissorsController::RobotStatus RockPaperScissorsController::GetRobotStatus() const {
    return {
        .drive_action = drive_action_.load(),
        .arm_angles = {arm_1_angle_.load(), arm_2_angle_.load(), arm_3_angle_.load()},
        .game_running = running_.load(),
        .hardware_ready = pca9685_initialized_.load(),
    };
}

const char* RockPaperScissorsController::DriveActionName(DriveAction action) {
    switch (action) {
        case DriveAction::kForward:
            return "forward";
        case DriveAction::kBackward:
            return "backward";
        case DriveAction::kTurnLeft:
            return "turn_left";
        case DriveAction::kTurnRight:
            return "turn_right";
        case DriveAction::kStopped:
        default:
            return "stopped";
    }
}

int RockPaperScissorsController::GetArmMinimumAngle(size_t arm_index) {
    return arm_index < kGameServoPoses.size() ? std::min(kGameServoPoses[arm_index].rest_angle,
                                                         kGameServoPoses[arm_index].play_angle)
                                              : 0;
}

int RockPaperScissorsController::GetArmMaximumAngle(size_t arm_index) {
    if (arm_index == 0) {
        return 180;
    }
    return arm_index < kGameServoPoses.size() ? std::max(kGameServoPoses[arm_index].rest_angle,
                                                         kGameServoPoses[arm_index].play_angle)
                                              : 0;
}

void RockPaperScissorsController::GameTask(void* arg) {
    auto* controller = static_cast<RockPaperScissorsController*>(arg);
    controller->RunGame();
    Application::GetInstance().Schedule([controller]() {
        controller->RestoreEmojiMode();
        controller->ResumeMicrophone();
        controller->running_ = false;
    });
    vTaskDelete(nullptr);
}

void RockPaperScissorsController::MovementTask(void* arg) {
    auto* controller = static_cast<RockPaperScissorsController*>(arg);
    MovementCommand command;
    while (true) {
        if (xQueueReceive(controller->movement_queue_, &command, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (command.servo11_pulse_us == kDanceCommandPulseUs &&
            command.servo12_pulse_us == kDanceCommandPulseUs) {
            auto dance_step = [controller](int servo11_pulse_us, int servo12_pulse_us,
                                           int arm8_angle, int arm9_angle,
                                           int arm10_angle) -> bool {
                if (controller->movement_stop_requested_) {
                    return false;
                }
                controller->SetMovementServoPulses(servo11_pulse_us, servo12_pulse_us);
                controller->SetServoAngle(8, arm8_angle);
                controller->SetServoAngle(9, arm9_angle);
                controller->SetServoAngle(10, arm10_angle);
                constexpr int kDancePollMs = 20;
                for (int elapsed = 0; elapsed < kDanceStepDurationMs; elapsed += kDancePollMs) {
                    if (controller->movement_stop_requested_) {
                        return false;
                    }
                    vTaskDelay(pdMS_TO_TICKS(kDancePollMs));
                }
                return !controller->movement_stop_requested_;
            };

            // Enter the dance while moving forward. The arms alternate rather
            // than all mirroring each other, giving the upper body a wave.
            bool completed =
                dance_step(kForwardServo11PulseUs, kForwardServo12PulseUs, 50, 15, 0) &&
                dance_step(kForwardServo11PulseUs, kForwardServo12PulseUs, 0, 60, 50);

            // Continue the arm choreography through every left/right spin.
            for (int cycle = 0; completed && cycle < kDanceCycles; ++cycle) {
                if ((cycle & 1) == 0) {
                    completed = dance_step(kDanceClockwiseServo11PulseUs,
                                           kDanceClockwiseServo12PulseUs, 50, 60, 0) &&
                                dance_step(kDanceCounterServo11PulseUs, kDanceCounterServo12PulseUs,
                                           0, 15, 50);
                } else {
                    completed = dance_step(kDanceClockwiseServo11PulseUs,
                                           kDanceClockwiseServo12PulseUs, 0, 60, 50) &&
                                dance_step(kDanceCounterServo11PulseUs, kDanceCounterServo12PulseUs,
                                           50, 15, 0);
                }
            }

            // Finish by backing into the starting area while the arms perform
            // one final inverted wave, then restore every actuator to rest.
            if (completed) {
                completed =
                    dance_step(kBackwardServo11PulseUs, kBackwardServo12PulseUs, 50, 15, 50) &&
                    dance_step(kBackwardServo11PulseUs, kBackwardServo12PulseUs, 0, 60, 0);
            }
            controller->SetMovementServoPulses(kMovementServoStopPulseUs,
                                               kMovementServoStopPulseUs);
            for (const auto& servo : kGameServoPoses) {
                controller->SetServoAngle(servo.channel, servo.rest_angle);
            }
            controller->drive_action_ = DriveAction::kStopped;
            ESP_LOGI(kTag, "Robot dance %s; wheels stopped and all arms restored",
                     completed ? "completed" : "cancelled");
            continue;
        }

        while (true) {
            controller->drive_action_ = command.action;
            controller->SetMovementServoPulses(command.servo11_pulse_us, command.servo12_pulse_us);
            const TickType_t wait_ticks =
                command.duration_ms == 0 ? portMAX_DELAY : pdMS_TO_TICKS(command.duration_ms);
            MovementCommand next_command;
            if (xQueueReceive(controller->movement_queue_, &next_command, wait_ticks) != pdTRUE) {
                break;
            }
            if (next_command.servo11_pulse_us == kDanceCommandPulseUs &&
                next_command.servo12_pulse_us == kDanceCommandPulseUs) {
                xQueueOverwrite(controller->movement_queue_, &next_command);
                break;
            }
            command = next_command;
        }

        controller->SetMovementServoPulses(kMovementServoStopPulseUs, kMovementServoStopPulseUs);
        controller->drive_action_ = DriveAction::kStopped;
        ESP_LOGI(kTag, "Robot movement completed and both continuous servos stopped");
    }
}

void RockPaperScissorsController::ShowGameCountdown(int digit) {
    if (display_ != nullptr) {
        display_->ShowGameCountdown(digit);
    }
#if CONFIG_BREAD_COMPACT_WIFI_TFT_128X160
    if (tft_display_ != nullptr) {
        tft_display_->ShowGameCountdown(digit);
    }
#endif
}

void RockPaperScissorsController::ShowGameRound(int round) {
    if (display_ != nullptr) {
        display_->ShowGameRound(round, kAttemptCount);
    }
#if CONFIG_BREAD_COMPACT_WIFI_TFT_128X160
    if (tft_display_ != nullptr) {
        tft_display_->ShowGameRound(round, kAttemptCount);
    }
#endif
}

void RockPaperScissorsController::RunGame() {
    ShowGameCountdown(3);
    if (!InitializePca9685()) {
        ESP_LOGE(kTag, "Game aborted because PCA9685 initialization failed");
        ShowFinished();
        return;
    }

    for (const auto& servo : kGameServoPoses) {
        SetServoAngle(servo.channel, servo.rest_angle);
        if (WaitOrStop(100)) {
            return;
        }
    }

    for (int digit = 3; digit >= 1; --digit) {
        ShowDigit(digit);
        ShowGameCountdown(digit);
        if (WaitOrStop(1000)) {
            return;
        }
    }
    ShowReady();

    for (int attempt = 0; attempt < kAttemptCount; ++attempt) {
        ShowGameRound(attempt + 1);
        // Keep the physical LED matrix and OLED on the same round number
        // while waiting for the player's hand and while actuating the servo.
        ShowDigit(attempt + 1);
        ESP_LOGI(kTag, "Round %d/%d waiting for hand below 10 cm on Port 4", attempt + 1,
                 kAttemptCount);
        while (true) {
            if (stop_requested_) {
                return;
            }
            const float distance_cm = ReadDistanceCm();
            if (IsHandDetected(distance_cm)) {
                ESP_LOGI(kTag, "Round %d hand detected at %.1f cm", attempt + 1,
                         static_cast<double>(distance_cm));
                break;
            }
            if (WaitOrStop(50)) {
                return;
            }
        }

        const size_t choice = esp_random() % kGameThrowAngles.size();
        const auto& servo = kGameServoPoses[0];
        const int play_angle = kGameThrowAngles[choice];
        const int rest_angle = servo.rest_angle;
        ESP_LOGI(kTag, "Throw %d/%d channel=%d angle=%d rest=%d", attempt + 1, kAttemptCount,
                 servo.channel, play_angle, rest_angle);
        if (!SetServoAngle(servo.channel, play_angle)) {
            ESP_LOGE(kTag, "Game aborted because servo 1 could not move");
            ShowFinished();
            return;
        }
        if (WaitOrStop(1000)) {
            SetServoAngle(servo.channel, rest_angle);
            return;
        }
        if (!SetServoAngle(servo.channel, rest_angle)) {
            ESP_LOGE(kTag, "Game aborted because servo 1 could not return to rest");
            ShowFinished();
            return;
        }
        if (WaitOrStop(100)) {
            return;
        }
        float distance_cm = ReadDistanceCm();
        while (IsHandDetected(distance_cm)) {
            if (WaitOrStop(50)) {
                return;
            }
            distance_cm = ReadDistanceCm();
        }
    }

    ShowFinished();
    if (WaitOrStop(500)) {
        return;
    }
    // Queue the local completion sound immediately before the three flashes,
    // so the speaker and the 25-pixel celebration start together.
    PlayGameCompletedSound();
    if (ShowVictoryCelebration()) {
        return;
    }
    ClearMatrix();
    ESP_LOGI(kTag, "Five-throw game completed");
}

void RockPaperScissorsController::SuspendMicrophone() {
    auto& audio = Application::GetInstance().GetAudioService();
    audio.SetWakeWordOnlyMode(true);
    game_audio_paused_ = true;
    ESP_LOGI(kTag, "Game microphone: wake word available; voice uplink paused");
}

void RockPaperScissorsController::ResumeMicrophone() {
    if (game_audio_paused_) {
        game_audio_paused_ = false;
        Application::GetInstance().GetAudioService().SetWakeWordOnlyMode(false);
        ESP_LOGI(kTag, "Normal microphone control restored after game");
    }
}

void RockPaperScissorsController::NotifyGameStarted() {
    const auto size = static_cast<size_t>(kGameStartPromptEnd - kGameStartPromptBegin);
    Application::GetInstance().SendAudioPrompt(
        std::string_view(reinterpret_cast<const char*>(kGameStartPromptBegin), size),
        "game_started");
}

void RockPaperScissorsController::PlayGameCompletedSound() {
    const auto size = static_cast<size_t>(kGameCompletePromptEnd - kGameCompletePromptBegin);
    Application::GetInstance().PlaySound(
        std::string_view(reinterpret_cast<const char*>(kGameCompletePromptBegin), size));
    ESP_LOGI(kTag, "Local game completion sound queued (%u bytes)", static_cast<unsigned>(size));
}

bool RockPaperScissorsController::WaitOrStop(uint32_t duration_ms) {
    constexpr uint32_t kSliceMs = 20;
    uint32_t elapsed_ms = 0;
    while (elapsed_ms < duration_ms) {
        if (stop_requested_) {
            return true;
        }
        const uint32_t slice_ms = std::min(kSliceMs, duration_ms - elapsed_ms);
        vTaskDelay(pdMS_TO_TICKS(slice_ms));
        elapsed_ms += slice_ms;
    }
    return stop_requested_.load();
}

void RockPaperScissorsController::RestoreEmojiMode() {
    for (const auto& servo : kGameServoPoses) {
        SetServoAngle(servo.channel, servo.rest_angle);
    }
    ClearMatrix();
    if (display_ != nullptr) {
        // Release the display only after the game task has finished cleaning
        // up. Until this point ShowEmojiMode intentionally ignores status
        // updates so the ROUND panel cannot be interrupted.
        display_->EndGameMode();
        display_->ShowEmojiMode();
    }
#if CONFIG_BREAD_COMPACT_WIFI_TFT_128X160
    if (tft_display_ != nullptr) {
        tft_display_->EndGameMode();
    }
#endif
    ESP_LOGI(kTag, "%s",
             stop_requested_.load() ? "Game stopped" : "Game display returned to emoji mode");
}

bool RockPaperScissorsController::InitializePca9685() {
    if (pca9685_initialized_) {
        return true;
    }
    if (pca9685_ == nullptr) {
        return false;
    }
    constexpr uint8_t kMode1 = 0x00;
    constexpr uint8_t kPrescale = 0xFE;
    const int prescale =
        static_cast<int>(25000000.0f / (4096.0f * kPca9685FrequencyHz) - 1.0f + 0.5f);
    const uint8_t sleep[] = {kMode1, 0x10};
    const uint8_t frequency[] = {kPrescale, static_cast<uint8_t>(prescale)};
    const uint8_t wake[] = {kMode1, 0x00};
    const uint8_t restart[] = {kMode1, 0xA1};
    constexpr uint8_t kLed0OnLow = 0x06;
    const uint8_t buzzer_full_off[] = {
        static_cast<uint8_t>(kLed0OnLow + 4 * kPca9685BuzzerChannel),
        0x00,
        0x00,
        0x00,
        0x10,  // LEDn_OFF_H bit 4: force PWM output low (full off)
    };
    if (i2c_master_transmit(pca9685_, sleep, sizeof(sleep), 100) != ESP_OK ||
        i2c_master_transmit(pca9685_, frequency, sizeof(frequency), 100) != ESP_OK ||
        i2c_master_transmit(pca9685_, wake, sizeof(wake), 100) != ESP_OK) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(5));
    const esp_err_t restart_err = i2c_master_transmit(pca9685_, restart, sizeof(restart), 100);
    const esp_err_t buzzer_err =
        restart_err == ESP_OK
            ? i2c_master_transmit(pca9685_, buzzer_full_off, sizeof(buzzer_full_off), 100)
            : restart_err;
    if (buzzer_err != ESP_OK) {
        ESP_LOGE(kTag, "PCA9685 initialization failed: %s", esp_err_to_name(buzzer_err));
        return false;
    }
    pca9685_initialized_ = true;
    ESP_LOGI(kTag, "PCA9685 ready address=0x%02X frequency=%dHz", kPca9685Address,
             kPca9685FrequencyHz);
    return true;
}

bool RockPaperScissorsController::SetMovementServoPulses(int servo11_pulse_us,
                                                         int servo12_pulse_us) {
    if (!InitializePca9685()) {
        ESP_LOGE(kTag, "Cannot move robot because PCA9685 initialization failed");
        return false;
    }
    const bool servo11_ok = SetServoPulseUs(11, servo11_pulse_us);
    const bool servo12_ok = SetServoPulseUs(12, servo12_pulse_us);
    if (servo11_ok && servo12_ok) {
        ESP_LOGI(kTag, "Robot movement pulse servo11=%dus servo12=%dus", servo11_pulse_us,
                 servo12_pulse_us);
        return true;
    }
    return false;
}

bool RockPaperScissorsController::QueueMovement(int servo11_pulse_us, int servo12_pulse_us,
                                                uint32_t duration_ms, DriveAction action) {
    if (movement_queue_ == nullptr) {
        return false;
    }
    movement_stop_requested_ = false;
    const MovementCommand command = {
        .servo11_pulse_us = servo11_pulse_us,
        .servo12_pulse_us = servo12_pulse_us,
        .duration_ms = duration_ms,
        .action = action,
    };
    if (xQueueOverwrite(movement_queue_, &command) != pdTRUE) {
        return false;
    }
    drive_action_ = action;
    ESP_LOGI(kTag, "Queued robot movement action=%s servo11=%dus servo12=%dus duration=%ums",
             DriveActionName(action), servo11_pulse_us, servo12_pulse_us,
             static_cast<unsigned>(duration_ms));
    return true;
}

bool RockPaperScissorsController::QueueDance() {
    return QueueMovement(kDanceCommandPulseUs, kDanceCommandPulseUs, 0, DriveAction::kStopped);
}

bool RockPaperScissorsController::SetServoAngle(int channel, int angle) {
    if (channel == kPca9685BuzzerChannel) {
        return false;
    }
    angle = std::clamp(angle, 0, 180);
    const bool is_movement_servo = channel == 11 || channel == 12;
    const float pulse_us = is_movement_servo && angle == kMovementServoStopAngle
                               ? kMovementServoStopPulseUs
                               : 500.0f + static_cast<float>(angle) * (2000.0f / 180.0f);
    const int ticks = std::clamp(
        static_cast<int>(pulse_us * kPca9685FrequencyHz * 4096.0f / 1000000.0f + 0.5f), 1, 4095);
    const uint8_t first_register = static_cast<uint8_t>(0x06 + 4 * channel);
    const uint8_t payload[] = {first_register, 0x00, 0x00, static_cast<uint8_t>(ticks & 0xFF),
                               static_cast<uint8_t>((ticks >> 8) & 0x0F)};
    if (pca9685_ == nullptr) {
        return false;
    }
    const esp_err_t err = i2c_master_transmit(pca9685_, payload, sizeof(payload), 100);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "PCA9685 servo write failed channel=%d angle=%d: %s", channel, angle,
                 esp_err_to_name(err));
        return false;
    }
    if (channel == kGameServoPoses[0].channel) {
        arm_1_angle_ = angle;
    } else if (channel == kGameServoPoses[1].channel) {
        arm_2_angle_ = angle;
    } else if (channel == kGameServoPoses[2].channel) {
        arm_3_angle_ = angle;
    }
    return true;
}

float RockPaperScissorsController::ReadDistanceCm() {
    gpio_set_level(kUltrasonicTrigger, 0);
    esp_rom_delay_us(2);
    gpio_set_level(kUltrasonicTrigger, 1);
    esp_rom_delay_us(10);
    gpio_set_level(kUltrasonicTrigger, 0);

    int64_t deadline = esp_timer_get_time() + 30000;
    while (gpio_get_level(kUltrasonicEcho) == 0) {
        if (esp_timer_get_time() >= deadline) {
            return -1.0f;
        }
    }
    const int64_t started = esp_timer_get_time();
    deadline = started + 30000;
    while (gpio_get_level(kUltrasonicEcho) != 0) {
        if (esp_timer_get_time() >= deadline) {
            return -1.0f;
        }
    }
    return static_cast<float>(esp_timer_get_time() - started) * 0.01716f;
}

void RockPaperScissorsController::ShowDigit(int digit) {
    digit = std::clamp(digit, 0, 5);
    std::vector<StripColor> colors(25, {0, 0, 0});
    for (size_t i = 0; i < colors.size(); ++i) {
        if (kDigits[digit][i] != 0) {
            colors[i] = {0, 0, kOnePercentBrightness};
        }
    }
    SetOrientedMatrixColors(colors);
}

void RockPaperScissorsController::ShowReady() {
    std::vector<StripColor> colors(25, {0, 0, 0});
    for (int i = 0; i < 25; ++i) {
        if (i % 6 == 0 || i % 4 == 0) {
            colors[i] = {0, kOnePercentBrightness, 0};
        }
    }
    SetOrientedMatrixColors(colors);
}

void RockPaperScissorsController::ShowFinished() {
    std::vector<StripColor> colors(25, {0, 0, 0});
    constexpr uint8_t kCheckMark[] = {
        0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 1, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0,
    };
    for (size_t i = 0; i < colors.size(); ++i) {
        if (kCheckMark[i] != 0) {
            colors[i] = {0, kOnePercentBrightness, 0};
        }
    }
    SetOrientedMatrixColors(colors);
}

bool RockPaperScissorsController::ShowVictoryCelebration() {
    for (int flash = 0; flash < kVictoryFlashCount; ++flash) {
        std::vector<StripColor> colors(25);
        for (auto& color : colors) {
            color = kVictoryColors[esp_random() % kVictoryColors.size()];
        }
        SetOrientedMatrixColors(colors);
        if (WaitOrStop(kVictoryOnTimeMs)) {
            ClearMatrix();
            return true;
        }
        ClearMatrix();
        if (flash + 1 < kVictoryFlashCount && WaitOrStop(kVictoryOffTimeMs)) {
            return true;
        }
    }
    return false;
}

void RockPaperScissorsController::ClearMatrix() { matrix_.SetAllColor({0, 0, 0}); }

void RockPaperScissorsController::SetOrientedMatrixColors(
    const std::vector<StripColor>& logical_colors) {
    if (logical_colors.size() != 25) {
        return;
    }
    std::vector<StripColor> corrected(25, {0, 0, 0});
    // The physical matrix currently renders a row-major image rotated 90
    // degrees clockwise. Pre-rotate it counter-clockwise to restore the
    // original upright orientation on the board.
    for (int row = 0; row < 5; ++row) {
        for (int column = 0; column < 5; ++column) {
            corrected[row * 5 + column] = logical_colors[column * 5 + (4 - row)];
        }
    }
    matrix_.SetMultiColors(corrected);
}
