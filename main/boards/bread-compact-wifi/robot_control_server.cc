#include "robot_control_server.h"

#include "application.h"
#include "rock_paper_scissors_controller.h"

extern "C" {
#include "configurable_robot_control_server.h"
}

#include <esp_log.h>
#include <atomic>

namespace {

constexpr char kTag[] = "RobotControlWeb";
RockPaperScissorsController* g_controller = nullptr;
std::atomic<bool> g_game_start_pending{false};

bool SetServoPulse(int channel, int pulse_us) {
    return g_controller != nullptr && g_controller->SetServoPulseUs(channel, pulse_us);
}

bool DriveRobot(klh_robot_drive_action_t action) {
    if (g_controller == nullptr) {
        return false;
    }
    switch (action) {
        case KLH_ROBOT_DRIVE_FORWARD:
            return g_controller->Drive(RockPaperScissorsController::DriveAction::kForward);
        case KLH_ROBOT_DRIVE_BACKWARD:
            return g_controller->Drive(RockPaperScissorsController::DriveAction::kBackward);
        case KLH_ROBOT_DRIVE_TURN_LEFT:
            return g_controller->Drive(RockPaperScissorsController::DriveAction::kTurnLeft);
        case KLH_ROBOT_DRIVE_TURN_RIGHT:
            return g_controller->Drive(RockPaperScissorsController::DriveAction::kTurnRight);
        case KLH_ROBOT_DRIVE_STOPPED:
            g_controller->StopMovement();
            return true;
    }
    return false;
}

bool SetArmAngle(int arm_index, int angle) {
    return g_controller != nullptr && arm_index >= 0 &&
           g_controller->SetArmAngle(static_cast<size_t>(arm_index), angle);
}

bool RunRobotAction(klh_robot_action_t action) {
    if (g_controller == nullptr) {
        return false;
    }
    switch (action) {
        case KLH_ROBOT_ACTION_SPIN_CLOCKWISE:
            return g_controller->SpinClockwise();
        case KLH_ROBOT_ACTION_SPIN_COUNTER_CLOCKWISE:
            return g_controller->SpinCounterClockwise();
        case KLH_ROBOT_ACTION_DANCE:
            return g_controller->Dance();
        case KLH_ROBOT_ACTION_PLAY_GAME: {
            if (g_controller->IsRunning() || g_game_start_pending.exchange(true)) {
                return false;
            }
            auto* controller = g_controller;
            Application::GetInstance().Schedule([controller]() {
                controller->StopMovement();
                if (!controller->StartGame(true)) {
                    ESP_LOGW(kTag,
                             "Queued browser game start rejected: robot busy or input suspended");
                }
                g_game_start_pending = false;
            });
            return true;  // Accepted; game status reports whether startup succeeded.
        }
        case KLH_ROBOT_ACTION_RESET_ARMS:
            if (g_controller->IsRunning()) {
                return false;
            }
            g_controller->ResetArms();
            return true;
    }
    return false;
}

void StopRobot() {
    if (g_controller != nullptr) {
        g_controller->StopGame();
        g_controller->StopMovement();
    }
}

klh_robot_drive_action_t GetDriveAction() {
    if (g_controller == nullptr) {
        return KLH_ROBOT_DRIVE_STOPPED;
    }
    switch (g_controller->GetRobotStatus().drive_action) {
        case RockPaperScissorsController::DriveAction::kForward:
            return KLH_ROBOT_DRIVE_FORWARD;
        case RockPaperScissorsController::DriveAction::kBackward:
            return KLH_ROBOT_DRIVE_BACKWARD;
        case RockPaperScissorsController::DriveAction::kTurnLeft:
            return KLH_ROBOT_DRIVE_TURN_LEFT;
        case RockPaperScissorsController::DriveAction::kTurnRight:
            return KLH_ROBOT_DRIVE_TURN_RIGHT;
        case RockPaperScissorsController::DriveAction::kStopped:
            return KLH_ROBOT_DRIVE_STOPPED;
    }
    return KLH_ROBOT_DRIVE_STOPPED;
}

bool GameRunning() { return g_controller != nullptr && g_controller->IsRunning(); }

int GetArmAngle(int arm_index) {
    if (g_controller == nullptr || arm_index < 0 || arm_index >= 3) {
        return 0;
    }
    return g_controller->GetRobotStatus().arm_angles[static_cast<size_t>(arm_index)];
}

bool HardwareReady() {
    return g_controller != nullptr && g_controller->GetRobotStatus().hardware_ready;
}

bool StartBrowserVoice() { return Application::GetInstance().BeginBrowserVoiceInput(); }

bool BrowserVoiceReady() { return Application::GetInstance().IsBrowserVoiceReady(); }

bool PushBrowserVoice(const int16_t* samples, size_t sample_count) {
    return Application::GetInstance().PushBrowserVoicePcm(samples, sample_count);
}

void StopBrowserVoice() { Application::GetInstance().EndBrowserVoiceInput(); }

}  // namespace

RobotControlServer::RobotControlServer(RockPaperScissorsController& controller)
    : controller_(controller) {}

RobotControlServer::~RobotControlServer() { Stop(); }

bool RobotControlServer::Start(uint16_t port) {
    if (running_ && port_ == port) {
        return true;
    }
    if (running_) {
        Stop();
    }

    g_controller = &controller_;
    const klh_robot_control_callbacks_t callbacks = {
        .set_servo_pulse_us = SetServoPulse,
        .drive = DriveRobot,
        .set_arm_angle = SetArmAngle,
        .run_action = RunRobotAction,
        .stop = StopRobot,
        .hardware_ready = HardwareReady,
        .get_drive_action = GetDriveAction,
        .game_running = GameRunning,
        .get_arm_angle = GetArmAngle,
        .voice_start = StartBrowserVoice,
        .voice_ready = BrowserVoiceReady,
        .voice_pcm = PushBrowserVoice,
        .voice_stop = StopBrowserVoice,
    };
    running_ = klh_configurable_robot_control_server_start(&callbacks, port);
    port_ = running_ ? port : 0;
    if (!running_) {
        g_controller = nullptr;
        ESP_LOGE(kTag, "Failed to start configurable robot server on port %u",
                 static_cast<unsigned int>(port));
    }
    return running_;
}

std::string RobotControlServer::CreateAccessSession(uint32_t ttl_seconds) {
    if (!running_) {
        return {};
    }
    char url[128] = {0};
    if (!klh_configurable_robot_control_server_create_access_url(url, sizeof(url), ttl_seconds)) {
        return {};
    }
    return url;
}

void RobotControlServer::Stop() {
    if (!running_) {
        return;
    }
    klh_configurable_robot_control_server_stop();
    running_ = false;
    port_ = 0;
    g_controller = nullptr;
}
