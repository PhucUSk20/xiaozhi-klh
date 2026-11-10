#ifndef KLH_CONFIGURABLE_ROBOT_CONTROL_SERVER_H
#define KLH_CONFIGURABLE_ROBOT_CONTROL_SERVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
  KLH_ROBOT_DRIVE_STOPPED = 0,
  KLH_ROBOT_DRIVE_FORWARD,
  KLH_ROBOT_DRIVE_BACKWARD,
  KLH_ROBOT_DRIVE_TURN_LEFT,
  KLH_ROBOT_DRIVE_TURN_RIGHT,
} klh_robot_drive_action_t;

typedef enum {
  KLH_ROBOT_ACTION_SPIN_CLOCKWISE = 0,
  KLH_ROBOT_ACTION_SPIN_COUNTER_CLOCKWISE,
  KLH_ROBOT_ACTION_DANCE,
  KLH_ROBOT_ACTION_PLAY_GAME,
  KLH_ROBOT_ACTION_RESET_ARMS,
} klh_robot_action_t;

typedef struct {
  /** Write a calibrated servo pulse. The UI never exposes pulse widths. */
  bool (*set_servo_pulse_us)(int channel, int pulse_us);
  bool (*drive)(klh_robot_drive_action_t action);
  bool (*set_arm_angle)(int arm_index, int angle);
  bool (*run_action)(klh_robot_action_t action);
  void (*stop)(void);
  bool (*hardware_ready)(void);
  klh_robot_drive_action_t (*get_drive_action)(void);
  bool (*game_running)(void);
  int (*get_arm_angle)(int arm_index);
  bool (*voice_start)(void);
  bool (*voice_ready)(void);
  bool (*voice_pcm)(const int16_t *samples, size_t sample_count);
  void (*voice_stop)(void);
} klh_robot_control_callbacks_t;

/** Start the KLeaderBit LAN robot-control page and REST API on port 80. */
bool klh_configurable_robot_control_server_start(const klh_robot_control_callbacks_t *callbacks, unsigned short server_port);

/** Stop both the physical robot and the HTTP server. Safe to call repeatedly. */
void klh_configurable_robot_control_server_stop(void);

/** Rotate the short-lived browser session and return its LAN URL. */
bool klh_configurable_robot_control_server_create_access_url(char *url, size_t capacity,
                                                              unsigned int ttl_seconds);

#endif
