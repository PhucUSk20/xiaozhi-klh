#ifndef ROBOT_CONTROL_SERVER_H
#define ROBOT_CONTROL_SERVER_H

#include <cstdint>
#include <string>

class RockPaperScissorsController;

class RobotControlServer {
public:
    explicit RobotControlServer(RockPaperScissorsController& controller);
    ~RobotControlServer();

    RobotControlServer(const RobotControlServer&) = delete;
    RobotControlServer& operator=(const RobotControlServer&) = delete;

    bool Start(uint16_t port = 80);
    void Stop();
    bool IsRunning() const { return running_; }
    std::string CreateAccessSession(uint32_t ttl_seconds = 600);

private:
    RockPaperScissorsController& controller_;
    bool running_ = false;
    uint16_t port_ = 0;
};

#endif  // ROBOT_CONTROL_SERVER_H
