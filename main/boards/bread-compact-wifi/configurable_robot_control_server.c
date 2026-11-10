#include "configurable_robot_control_server.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs.h"

#define ROBOT_REQUEST_BODY_MAX 4096
#define ROBOT_PROFILE_MAX 3584
#define ROBOT_PROFILE_SLOTS 5
#define ROBOT_DRIVE_WATCHDOG_US 600000ULL
#define ROBOT_NEUTRAL_PULSE_US CONFIG_BREAD_COMPACT_WIFI_MOVEMENT_STOP_PULSE_US
#define ROBOT_MIN_PULSE_US 500
#define ROBOT_MAX_PULSE_US 3000
#define BROWSER_VOICE_FRAME_SAMPLES 960
#define BROWSER_SESSION_TOKEN_LENGTH 32
#define BROWSER_VOICE_TIMEOUT_US 3000000ULL

static const char *TAG = "RobotControlWeb";
static const char *DEFAULT_PROFILE =
  "{\"version\":1,\"name\":\"KLeaderBit Robot\",\"buttons\":["
  "{\"id\":\"forward\",\"label\":\"Tiến\",\"key\":\"w\",\"motors\":["
  "{\"channel\":11,\"type\":\"continuous\",\"speed\":58,\"direction\":\"ccw\"},"
  "{\"channel\":12,\"type\":\"continuous\",\"speed\":62,\"direction\":\"cw\"}]},"
  "{\"id\":\"backward\",\"label\":\"Lùi\",\"key\":\"s\",\"motors\":["
  "{\"channel\":11,\"type\":\"continuous\",\"speed\":62,\"direction\":\"cw\"},"
  "{\"channel\":12,\"type\":\"continuous\",\"speed\":58,\"direction\":\"ccw\"}]},"
  "{\"id\":\"turn_left\",\"label\":\"Trái\",\"key\":\"a\",\"motors\":["
  "{\"channel\":11,\"type\":\"continuous\",\"speed\":0,\"direction\":\"cw\"},"
  "{\"channel\":12,\"type\":\"continuous\",\"speed\":62,\"direction\":\"cw\"}]},"
  "{\"id\":\"turn_right\",\"label\":\"Phải\",\"key\":\"d\",\"motors\":["
  "{\"channel\":11,\"type\":\"continuous\",\"speed\":100,\"direction\":\"ccw\"},"
  "{\"channel\":12,\"type\":\"continuous\",\"speed\":0,\"direction\":\"cw\"}]},"
  "{\"id\":\"arm_1_up\",\"label\":\"Nâng tay trái\",\"motors\":["
  "{\"channel\":8,\"type\":\"positional\",\"startAngle\":0,\"endAngle\":50,\"travelMs\":1000,\"motion\":\"to_end\"}]},"
  "{\"id\":\"arm_1_down\",\"label\":\"Hạ tay trái\",\"motors\":["
  "{\"channel\":8,\"type\":\"positional\",\"startAngle\":0,\"endAngle\":50,\"travelMs\":1000,\"motion\":\"to_start\"}]},"
  "{\"id\":\"arm_2_up\",\"label\":\"Nâng tay giữa\",\"motors\":["
  "{\"channel\":9,\"type\":\"positional\",\"startAngle\":15,\"endAngle\":60,\"travelMs\":1000,\"motion\":\"to_end\"}]},"
  "{\"id\":\"arm_2_down\",\"label\":\"Hạ tay giữa\",\"motors\":["
  "{\"channel\":9,\"type\":\"positional\",\"startAngle\":15,\"endAngle\":60,\"travelMs\":1000,\"motion\":\"to_start\"}]},"
  "{\"id\":\"arm_3_up\",\"label\":\"Nâng tay phải\",\"motors\":["
  "{\"channel\":10,\"type\":\"positional\",\"startAngle\":0,\"endAngle\":50,\"travelMs\":1000,\"motion\":\"to_end\"}]},"
  "{\"id\":\"arm_3_down\",\"label\":\"Hạ tay phải\",\"motors\":["
  "{\"channel\":10,\"type\":\"positional\",\"startAngle\":0,\"endAngle\":50,\"travelMs\":1000,\"motion\":\"to_start\"}]}]}";

static httpd_handle_t s_server;
static unsigned short s_server_port;
static esp_timer_handle_t s_watchdog;
static esp_timer_handle_t s_voice_watchdog;
static klh_robot_control_callbacks_t s_callbacks;
static char s_active_button[33];
static int64_t s_active_updated_us;
static float s_position_angle[16];
static bool s_position_valid[16];
static klh_robot_drive_action_t s_drive_action = KLH_ROBOT_DRIVE_STOPPED;
static char s_browser_token[BROWSER_SESSION_TOKEN_LENGTH + 1];
static int64_t s_browser_token_expires_us;
static bool s_browser_voice_active;
static int64_t s_browser_voice_updated_us;

extern const char robot_control_html_start[] asm("_binary_robot_control_html_start");
extern const char robot_control_html_end[] asm("_binary_robot_control_html_end");
extern const char robot_control_custom_html_start[] asm("_binary_robot_control_custom_html_start");
extern const char robot_control_custom_html_end[] asm("_binary_robot_control_custom_html_end");

static const char *browser_allowed_origin(httpd_req_t *request) {
  char origin[96] = {0};
  if (!request || httpd_req_get_hdr_value_str(request, "Origin", origin, sizeof(origin)) != ESP_OK) {
    return NULL;
  }
  if (strcmp(origin, "https://coding.eduhubonline.com") == 0) return "https://coding.eduhubonline.com";
  if (strcmp(origin, "http://localhost:5173") == 0) return "http://localhost:5173";
  if (strcmp(origin, "http://127.0.0.1:5173") == 0) return "http://127.0.0.1:5173";
  return NULL;
}

static void set_browser_cors_headers(httpd_req_t *request) {
  const char *origin = browser_allowed_origin(request);
  if (!origin) return;
  httpd_resp_set_hdr(request, "Access-Control-Allow-Origin", origin);
  httpd_resp_set_hdr(request, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  httpd_resp_set_hdr(request, "Access-Control-Allow-Headers", "Content-Type, X-Robot-Session");
  httpd_resp_set_hdr(request, "Access-Control-Allow-Private-Network", "true");
  httpd_resp_set_hdr(request, "Access-Control-Max-Age", "600");
  httpd_resp_set_hdr(request, "Vary", "Origin");
}

static esp_err_t send_json(httpd_req_t *request, const char *json, const char *status) {
  set_browser_cors_headers(request);
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  httpd_resp_set_hdr(request, "Connection", "close");
  httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
  httpd_resp_set_hdr(request, "Referrer-Policy", "no-referrer");
  httpd_resp_set_status(request, status ? status : HTTPD_200);
  httpd_resp_set_type(request, "application/json; charset=utf-8");
  return httpd_resp_sendstr(request, json);
}

static bool receive_body(httpd_req_t *request, char *body, size_t capacity) {
  if (!body || capacity < 2 || request->content_len == 0 || request->content_len >= capacity) return false;
  size_t received = 0;
  while (received < (size_t)request->content_len) {
    int result = httpd_req_recv(request, body + received, request->content_len - received);
    if (result == HTTPD_SOCK_ERR_TIMEOUT) continue;
    if (result <= 0) return false;
    received += (size_t)result;
  }
  body[received] = '\0';
  return true;
}

static bool browser_session_valid(httpd_req_t *request) {
  if (!request || s_browser_token[0] == '\0' || esp_timer_get_time() >= s_browser_token_expires_us) {
    return false;
  }
  char token[BROWSER_SESSION_TOKEN_LENGTH + 1] = {0};
  if (httpd_req_get_hdr_value_str(request, "X-Robot-Session", token, sizeof(token)) != ESP_OK) {
    return false;
  }
  return strlen(token) == BROWSER_SESSION_TOKEN_LENGTH &&
         memcmp(token, s_browser_token, BROWSER_SESSION_TOKEN_LENGTH) == 0;
}

static esp_err_t require_browser_session(httpd_req_t *request) {
  return send_json(request, "{\"error\":\"session_required\"}", "401 Unauthorized");
}

static int clamp_int(int value, int minimum, int maximum) {
  return value < minimum ? minimum : value > maximum ? maximum : value;
}

static int continuous_pulse(int speed, bool clockwise) {
  speed = clamp_int(speed, 0, 100);
  int span = clockwise ? ROBOT_MAX_PULSE_US - ROBOT_NEUTRAL_PULSE_US
                       : ROBOT_NEUTRAL_PULSE_US - ROBOT_MIN_PULSE_US;
  return clockwise ? ROBOT_NEUTRAL_PULSE_US + (span * speed) / 100
                   : ROBOT_NEUTRAL_PULSE_US - (span * speed) / 100;
}

static int position_pulse(float angle, float maximum_angle) {
  if (!isfinite(maximum_angle) || maximum_angle < 1.0f) maximum_angle = 270.0f;
  if (!isfinite(angle) || angle < 0.0f) angle = 0.0f;
  if (angle > maximum_angle) angle = maximum_angle;
  return ROBOT_MIN_PULSE_US + (int)lroundf(
    angle * (ROBOT_MAX_PULSE_US - ROBOT_MIN_PULSE_US) / maximum_angle
  );
}

static bool set_pulse(int channel, int pulse_us) {
  return channel >= 0 && channel < 16 && s_callbacks.set_servo_pulse_us &&
    s_callbacks.set_servo_pulse_us(channel, clamp_int(pulse_us, ROBOT_MIN_PULSE_US, ROBOT_MAX_PULSE_US));
}

static void profile_key(int slot, char key[4]) {
  snprintf(key, 4, "p%d", clamp_int(slot, 0, ROBOT_PROFILE_SLOTS - 1));
}

static int active_profile_slot(void) {
  nvs_handle_t handle;
  uint8_t slot = 0;
  if (nvs_open("robot_web", NVS_READONLY, &handle) == ESP_OK) {
    (void)nvs_get_u8(handle, "active", &slot);
    nvs_close(handle);
  }
  return slot < ROBOT_PROFILE_SLOTS ? slot : 0;
}

static char *load_profile(int slot) {
  nvs_handle_t handle;
  if (nvs_open("robot_web", NVS_READONLY, &handle) != ESP_OK) {
    return slot == 0 ? strdup(DEFAULT_PROFILE) : NULL;
  }
  char key[4];
  profile_key(slot, key);
  size_t length = 0;
  if (nvs_get_str(handle, key, NULL, &length) != ESP_OK || length < 3 || length > ROBOT_PROFILE_MAX) {
    nvs_close(handle);
    return slot == 0 ? strdup(DEFAULT_PROFILE) : NULL;
  }
  char *profile = malloc(length);
  if (profile && nvs_get_str(handle, key, profile, &length) != ESP_OK) {
    free(profile);
    profile = NULL;
  }
  nvs_close(handle);
  return profile;
}

static bool save_profile(int slot, const char *json) {
  if (slot < 0 || slot >= ROBOT_PROFILE_SLOTS || !json) return false;
  size_t length = strlen(json);
  if (length < 3 || length >= ROBOT_PROFILE_MAX) return false;
  nvs_handle_t handle;
  if (nvs_open("robot_web", NVS_READWRITE, &handle) != ESP_OK) return false;
  char key[4];
  profile_key(slot, key);
  esp_err_t result = nvs_set_str(handle, key, json);
  if (result == ESP_OK) result = nvs_commit(handle);
  nvs_close(handle);
  return result == ESP_OK;
}

static cJSON *find_button(cJSON *profile, const char *button_id) {
  cJSON *buttons = profile ? cJSON_GetObjectItem(profile, "buttons") : NULL;
  if (!cJSON_IsArray(buttons)) return NULL;
  cJSON *button = NULL;
  cJSON_ArrayForEach(button, buttons) {
    cJSON *id = cJSON_GetObjectItem(button, "id");
    if (cJSON_IsString(id) && id->valuestring && strcmp(id->valuestring, button_id) == 0) return button;
  }
  return NULL;
}

static void stop_button_continuous_motors(cJSON *button) {
  cJSON *motors = button ? cJSON_GetObjectItem(button, "motors") : NULL;
  cJSON *motor = NULL;
  cJSON_ArrayForEach(motor, motors) {
    cJSON *type = cJSON_GetObjectItem(motor, "type");
    cJSON *channel = cJSON_GetObjectItem(motor, "channel");
    if (cJSON_IsString(type) && strcmp(type->valuestring, "continuous") == 0 && cJSON_IsNumber(channel)) {
      (void)set_pulse(channel->valueint, ROBOT_NEUTRAL_PULSE_US);
    }
  }
}

static bool apply_button(cJSON *button, int64_t now_us) {
  cJSON *motors = button ? cJSON_GetObjectItem(button, "motors") : NULL;
  if (!cJSON_IsArray(motors)) return false;
  float elapsed_ms = s_active_updated_us > 0 ? (float)(now_us - s_active_updated_us) / 1000.0f : 0.0f;
  if (elapsed_ms < 0.0f || elapsed_ms > 650.0f) elapsed_ms = 0.0f;
  bool success = true;
  bool used = false;
  cJSON *motor = NULL;
  cJSON_ArrayForEach(motor, motors) {
    cJSON *channel_item = cJSON_GetObjectItem(motor, "channel");
    cJSON *type = cJSON_GetObjectItem(motor, "type");
    if (!cJSON_IsNumber(channel_item) || !cJSON_IsString(type)) continue;
    int channel = channel_item->valueint;
    if (channel < 0 || channel >= 16) continue;
    if (strcmp(type->valuestring, "continuous") == 0) {
      cJSON *speed = cJSON_GetObjectItem(motor, "speed");
      cJSON *direction = cJSON_GetObjectItem(motor, "direction");
      bool clockwise = cJSON_IsString(direction) && strcmp(direction->valuestring, "cw") == 0;
      success = set_pulse(channel, continuous_pulse(cJSON_IsNumber(speed) ? speed->valueint : 0, clockwise)) && success;
      used = true;
    } else if (strcmp(type->valuestring, "positional") == 0) {
      cJSON *start_item = cJSON_GetObjectItem(motor, "startAngle");
      cJSON *end_item = cJSON_GetObjectItem(motor, "endAngle");
      cJSON *travel_item = cJSON_GetObjectItem(motor, "travelMs");
      cJSON *motion = cJSON_GetObjectItem(motor, "motion");
      cJSON *maximum_item = cJSON_GetObjectItem(motor, "maxAngle");
      float start = cJSON_IsNumber(start_item) ? (float)start_item->valuedouble : 0.0f;
      float end = cJSON_IsNumber(end_item) ? (float)end_item->valuedouble : 180.0f;
      float maximum = cJSON_IsNumber(maximum_item) ? (float)maximum_item->valuedouble : 270.0f;
      int travel_ms = cJSON_IsNumber(travel_item) ? clamp_int(travel_item->valueint, 100, 60000) : 5000;
      bool to_start = cJSON_IsString(motion) && strcmp(motion->valuestring, "to_start") == 0;
      if (start > end) { float swap = start; start = end; end = swap; }
      start = fmaxf(0.0f, fminf(start, maximum));
      end = fmaxf(start, fminf(end, maximum));
      if (!s_position_valid[channel]) {
        s_position_angle[channel] = start;
        s_position_valid[channel] = true;
      }
      float delta = (end - start) * elapsed_ms / (float)travel_ms;
      s_position_angle[channel] += to_start ? -delta : delta;
      s_position_angle[channel] = fmaxf(start, fminf(end, s_position_angle[channel]));
      success = set_pulse(channel, position_pulse(s_position_angle[channel], maximum)) && success;
      used = true;
    }
  }
  s_active_updated_us = now_us;
  return used && success;
}

static void stop_active_button(void) {
  if (s_watchdog) (void)esp_timer_stop(s_watchdog);
  if (s_active_button[0]) {
    char *json = load_profile(active_profile_slot());
    cJSON *profile = json ? cJSON_Parse(json) : NULL;
    stop_button_continuous_motors(find_button(profile, s_active_button));
    cJSON_Delete(profile);
    free(json);
  }
  s_active_button[0] = '\0';
  s_active_updated_us = 0;
  s_drive_action = KLH_ROBOT_DRIVE_STOPPED;
}

static void watchdog_callback(void *argument) {
  (void)argument;
  stop_active_button();
  if (s_callbacks.stop) s_callbacks.stop();
}

static void voice_watchdog_callback(void *argument) {
  (void)argument;
  if (!s_browser_voice_active ||
      esp_timer_get_time() - s_browser_voice_updated_us <= BROWSER_VOICE_TIMEOUT_US) {
    return;
  }
  s_browser_voice_active = false;
  s_browser_voice_updated_us = 0;
  if (s_callbacks.voice_stop) s_callbacks.voice_stop();
  ESP_LOGW(TAG, "Browser voice stopped after an inactive client");
}

static esp_err_t handle_index(httpd_req_t *request) {
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  httpd_resp_set_hdr(request, "Connection", "close");
  httpd_resp_set_hdr(request, "Content-Security-Policy", "default-src 'self'; style-src 'self' 'unsafe-inline'; script-src 'self' 'unsafe-inline'; connect-src 'self'");
  httpd_resp_set_type(request, "text/html; charset=utf-8");
  return httpd_resp_send(request, robot_control_html_start, (ssize_t)(robot_control_html_end - robot_control_html_start));
}

static esp_err_t handle_custom(httpd_req_t *request) {
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  httpd_resp_set_hdr(request, "Connection", "close");
  httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
  httpd_resp_set_hdr(request, "Content-Security-Policy", "default-src 'self'; style-src 'self' 'unsafe-inline'; script-src 'self' 'unsafe-inline'; connect-src 'self'");
  httpd_resp_set_type(request, "text/html; charset=utf-8");
  return httpd_resp_send(request, robot_control_custom_html_start,
                         (ssize_t)(robot_control_custom_html_end - robot_control_custom_html_start));
}

static esp_err_t handle_status(httpd_req_t *request) {
  if (browser_allowed_origin(request) && !browser_session_valid(request)) return require_browser_session(request);
  char ssid[33] = "";
  char ip[20] = "";
  int rssi = -100;
  wifi_ap_record_t ap = {0};
  if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
    size_t length = strnlen((const char *)ap.ssid, sizeof(ap.ssid));
    memcpy(ssid, ap.ssid, length);
    ssid[length] = '\0';
    rssi = ap.rssi;
  }
  esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  esp_netif_ip_info_t info = {0};
  if (netif && esp_netif_get_ip_info(netif, &info) == ESP_OK) snprintf(ip, sizeof(ip), IPSTR, IP2STR(&info.ip));
  cJSON *root = cJSON_CreateObject();
  if (!root) return send_json(request, "{\"error\":\"no_memory\"}", "500 Internal Server Error");
  cJSON_AddBoolToObject(root, "online", true);
  klh_robot_drive_action_t drive_action = s_callbacks.get_drive_action
    ? s_callbacks.get_drive_action() : s_drive_action;
  cJSON_AddStringToObject(root, "drive", drive_action == KLH_ROBOT_DRIVE_FORWARD ? "forward" : drive_action == KLH_ROBOT_DRIVE_BACKWARD ? "backward" : drive_action == KLH_ROBOT_DRIVE_TURN_LEFT ? "turn_left" : drive_action == KLH_ROBOT_DRIVE_TURN_RIGHT ? "turn_right" : "stopped");
  cJSON_AddStringToObject(root, "activeButton", s_active_button);
  cJSON_AddBoolToObject(root, "game_running", s_callbacks.game_running && s_callbacks.game_running());
  cJSON_AddBoolToObject(root, "hardware_ready", s_callbacks.hardware_ready && s_callbacks.hardware_ready());
  cJSON_AddBoolToObject(root, "config_mode", s_server_port != 80);
  cJSON_AddStringToObject(root, "ssid", ssid);
  cJSON_AddStringToObject(root, "ip", ip);
  cJSON_AddNumberToObject(root, "rssi", rssi);
  cJSON *arms = cJSON_AddArrayToObject(root, "arms");
  for (int channel = 8; channel <= 10; channel++) {
    int arm_index = channel - 8;
    float angle = s_callbacks.get_arm_angle ? (float)s_callbacks.get_arm_angle(arm_index)
                                            : (s_position_valid[channel] ? s_position_angle[channel] : (channel == 9 ? 15 : 0));
    cJSON_AddItemToArray(arms, cJSON_CreateNumber(angle));
  }
  cJSON *positions = cJSON_AddArrayToObject(root, "positions");
  for (int i = 0; i < 16; i++) cJSON_AddItemToArray(positions, s_position_valid[i] ? cJSON_CreateNumber(s_position_angle[i]) : cJSON_CreateNull());
  char *json = cJSON_PrintUnformatted(root);
  esp_err_t result = json ? send_json(request, json, NULL) : send_json(request, "{\"error\":\"no_memory\"}", "500 Internal Server Error");
  cJSON_free(json);
  cJSON_Delete(root);
  return result;
}

static int request_slot(httpd_req_t *request) {
  char query[32] = "";
  char value[8] = "0";
  if (httpd_req_get_url_query_str(request, query, sizeof(query)) == ESP_OK) (void)httpd_query_key_value(query, "slot", value, sizeof(value));
  return clamp_int(atoi(value), 0, ROBOT_PROFILE_SLOTS - 1);
}

static esp_err_t handle_profiles(httpd_req_t *request) {
  if (browser_allowed_origin(request) && !browser_session_valid(request)) return require_browser_session(request);
  cJSON *root = cJSON_CreateObject();
  cJSON_AddNumberToObject(root, "active", active_profile_slot());
  cJSON *profiles = cJSON_AddArrayToObject(root, "profiles");
  for (int slot = 0; slot < ROBOT_PROFILE_SLOTS; slot++) {
    char *json = load_profile(slot);
    cJSON *profile = json ? cJSON_Parse(json) : NULL;
    cJSON *item = cJSON_CreateObject();
    cJSON_AddNumberToObject(item, "slot", slot);
    cJSON_AddBoolToObject(item, "exists", profile != NULL);
    cJSON *name = profile ? cJSON_GetObjectItem(profile, "name") : NULL;
    cJSON_AddStringToObject(item, "name", cJSON_IsString(name) ? name->valuestring : "");
    cJSON_AddItemToArray(profiles, item);
    cJSON_Delete(profile);
    free(json);
  }
  char *json = cJSON_PrintUnformatted(root);
  esp_err_t result = json ? send_json(request, json, NULL) : send_json(request, "{\"error\":\"no_memory\"}", "500 Internal Server Error");
  cJSON_free(json);
  cJSON_Delete(root);
  return result;
}

static esp_err_t handle_profile_get(httpd_req_t *request) {
  if (browser_allowed_origin(request) && !browser_session_valid(request)) return require_browser_session(request);
  char *json = load_profile(request_slot(request));
  if (!json) return send_json(request, "{\"error\":\"profile_not_found\"}", "404 Not Found");
  esp_err_t result = send_json(request, json, NULL);
  free(json);
  return result;
}

static bool json_number_in_range(cJSON *object, const char *name, double minimum, double maximum) {
  cJSON *item = cJSON_GetObjectItem(object, name);
  return cJSON_IsNumber(item) && isfinite(item->valuedouble) &&
    item->valuedouble >= minimum && item->valuedouble <= maximum;
}

static bool profile_is_valid(cJSON *profile) {
  cJSON *name = profile ? cJSON_GetObjectItem(profile, "name") : NULL;
  cJSON *buttons = profile ? cJSON_GetObjectItem(profile, "buttons") : NULL;
  int count = cJSON_IsArray(buttons) ? cJSON_GetArraySize(buttons) : 0;
  /* UI limits are characters; these byte limits also accept 4-byte UTF-8. */
  if (!cJSON_IsString(name) || !name->valuestring || !name->valuestring[0] ||
      strlen(name->valuestring) > 160 || count < 1 || count > 12) return false;
  cJSON *button = NULL;
  int button_index = 0;
  cJSON_ArrayForEach(button, buttons) {
    cJSON *id = cJSON_GetObjectItem(button, "id");
    cJSON *label = cJSON_GetObjectItem(button, "label");
    cJSON *motors = cJSON_GetObjectItem(button, "motors");
    int motor_count = cJSON_IsArray(motors) ? cJSON_GetArraySize(motors) : 0;
    if (!cJSON_IsString(id) || !id->valuestring || !id->valuestring[0] || strlen(id->valuestring) > 32 ||
        !cJSON_IsString(label) || !label->valuestring || !label->valuestring[0] || strlen(label->valuestring) > 192 ||
        motor_count < 1 || motor_count > 4) return false;
    for (int previous_index = 0; previous_index < button_index; previous_index++) {
      cJSON *previous = cJSON_GetArrayItem(buttons, previous_index);
      cJSON *previous_id = previous ? cJSON_GetObjectItem(previous, "id") : NULL;
      if (cJSON_IsString(previous_id) && strcmp(previous_id->valuestring, id->valuestring) == 0) return false;
    }
    cJSON *motor = NULL;
    cJSON_ArrayForEach(motor, motors) {
      cJSON *channel = cJSON_GetObjectItem(motor, "channel");
      cJSON *type = cJSON_GetObjectItem(motor, "type");
      if (!cJSON_IsNumber(channel) || channel->valueint < 0 || channel->valueint > 15 ||
          !cJSON_IsString(type) ||
          (strcmp(type->valuestring, "continuous") != 0 && strcmp(type->valuestring, "positional") != 0)) return false;
      if (strcmp(type->valuestring, "continuous") == 0) {
        cJSON *direction = cJSON_GetObjectItem(motor, "direction");
        if (!json_number_in_range(motor, "speed", 0.0, 100.0) || !cJSON_IsString(direction) ||
            (strcmp(direction->valuestring, "cw") != 0 && strcmp(direction->valuestring, "ccw") != 0)) return false;
      } else {
        cJSON *motion = cJSON_GetObjectItem(motor, "motion");
        if (!json_number_in_range(motor, "maxAngle", 1.0, 360.0) ||
            !json_number_in_range(motor, "startAngle", 0.0, 360.0) ||
            !json_number_in_range(motor, "endAngle", 0.0, 360.0) ||
            !json_number_in_range(motor, "travelMs", 100.0, 60000.0) ||
            !cJSON_IsString(motion) ||
            (strcmp(motion->valuestring, "to_end") != 0 && strcmp(motion->valuestring, "to_start") != 0)) return false;
        cJSON *maximum = cJSON_GetObjectItem(motor, "maxAngle");
        cJSON *start = cJSON_GetObjectItem(motor, "startAngle");
        cJSON *end = cJSON_GetObjectItem(motor, "endAngle");
        if (start->valuedouble > maximum->valuedouble || end->valuedouble > maximum->valuedouble) return false;
      }
    }
    button_index++;
  }
  return true;
}

static esp_err_t handle_profile_put(httpd_req_t *request) {
  if (browser_allowed_origin(request) && !browser_session_valid(request)) return require_browser_session(request);
  char *body = malloc(ROBOT_REQUEST_BODY_MAX);
  if (!body) return send_json(request, "{\"error\":\"no_memory\"}", "500 Internal Server Error");
  bool received = receive_body(request, body, ROBOT_REQUEST_BODY_MAX);
  cJSON *profile = received ? cJSON_Parse(body) : NULL;
  if (!profile_is_valid(profile)) {
    cJSON_Delete(profile);
    free(body);
    return send_json(request, "{\"error\":\"invalid_profile\"}", "400 Bad Request");
  }
  char *normalized = cJSON_PrintUnformatted(profile);
  if (request_slot(request) == active_profile_slot()) stop_active_button();
  bool saved = normalized && save_profile(request_slot(request), normalized);
  cJSON_free(normalized);
  cJSON_Delete(profile);
  free(body);
  return saved ? send_json(request, "{\"ok\":true}", NULL) : send_json(request, "{\"error\":\"profile_too_large_or_storage_full\"}", "507 Insufficient Storage");
}

static esp_err_t handle_profile_delete(httpd_req_t *request) {
  if (browser_allowed_origin(request) && !browser_session_valid(request)) return require_browser_session(request);
  int slot = request_slot(request);
  if (slot == active_profile_slot()) stop_active_button();
  nvs_handle_t handle;
  bool ok = nvs_open("robot_web", NVS_READWRITE, &handle) == ESP_OK;
  if (ok) {
    char key[4]; profile_key(slot, key);
    esp_err_t result = nvs_erase_key(handle, key);
    ok = (result == ESP_OK || result == ESP_ERR_NVS_NOT_FOUND) && nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
  }
  if (slot == active_profile_slot()) {
    nvs_handle_t active_handle;
    if (nvs_open("robot_web", NVS_READWRITE, &active_handle) == ESP_OK) {
      (void)nvs_set_u8(active_handle, "active", 0); (void)nvs_commit(active_handle); nvs_close(active_handle);
    }
  }
  stop_active_button();
  return ok ? send_json(request, "{\"ok\":true}", NULL) : send_json(request, "{\"error\":\"storage_error\"}", "500 Internal Server Error");
}

static esp_err_t handle_profile_activate(httpd_req_t *request) {
  if (browser_allowed_origin(request) && !browser_session_valid(request)) return require_browser_session(request);
  char body[96] = {0};
  if (!receive_body(request, body, sizeof(body))) return send_json(request, "{\"error\":\"invalid_body\"}", "400 Bad Request");
  cJSON *root = cJSON_Parse(body);
  cJSON *slot_item = root ? cJSON_GetObjectItem(root, "slot") : NULL;
  int slot = cJSON_IsNumber(slot_item) ? slot_item->valueint : -1;
  cJSON_Delete(root);
  char *profile = slot >= 0 && slot < ROBOT_PROFILE_SLOTS ? load_profile(slot) : NULL;
  if (!profile) return send_json(request, "{\"error\":\"profile_not_found\"}", "404 Not Found");
  free(profile);
  stop_active_button();
  nvs_handle_t handle;
  bool ok = nvs_open("robot_web", NVS_READWRITE, &handle) == ESP_OK;
  if (ok) { ok = nvs_set_u8(handle, "active", (uint8_t)slot) == ESP_OK && nvs_commit(handle) == ESP_OK; nvs_close(handle); }
  return ok ? send_json(request, "{\"ok\":true}", NULL) : send_json(request, "{\"error\":\"storage_error\"}", "500 Internal Server Error");
}

static esp_err_t handle_control(httpd_req_t *request) {
  if (browser_allowed_origin(request) && !browser_session_valid(request)) return require_browser_session(request);
  char body[160] = {0};
  if (!receive_body(request, body, sizeof(body))) return send_json(request, "{\"error\":\"invalid_body\"}", "400 Bad Request");
  cJSON *root = cJSON_Parse(body);
  cJSON *id = root ? cJSON_GetObjectItem(root, "button") : NULL;
  cJSON *pressed_item = root ? cJSON_GetObjectItem(root, "pressed") : NULL;
  if (!cJSON_IsString(id) || !id->valuestring || strlen(id->valuestring) > 32 || !cJSON_IsBool(pressed_item)) {
    cJSON_Delete(root);
    return send_json(request, "{\"error\":\"invalid_control\"}", "400 Bad Request");
  }
  char button_id[33];
  strncpy(button_id, id->valuestring, sizeof(button_id) - 1);
  button_id[sizeof(button_id) - 1] = '\0';
  bool pressed = cJSON_IsTrue(pressed_item);
  cJSON_Delete(root);
  if (!pressed) {
    stop_active_button();
    return send_json(request, "{\"ok\":true}", NULL);
  }
  char *json = load_profile(active_profile_slot());
  cJSON *profile = json ? cJSON_Parse(json) : NULL;
  cJSON *button = find_button(profile, button_id);
  if (!button) {
    cJSON_Delete(profile); free(json);
    return send_json(request, "{\"error\":\"button_not_found\"}", "404 Not Found");
  }
  if (s_active_button[0] && strcmp(s_active_button, button_id) != 0) stop_active_button();
  if (!s_active_button[0]) {
    strncpy(s_active_button, button_id, sizeof(s_active_button) - 1);
    s_active_button[sizeof(s_active_button) - 1] = '\0';
    s_active_updated_us = esp_timer_get_time();
  }
  bool ok = apply_button(button, esp_timer_get_time());
  cJSON_Delete(profile); free(json);
  if (!ok) { stop_active_button(); return send_json(request, "{\"error\":\"hardware_unavailable\"}", "503 Service Unavailable"); }
  if (s_watchdog) { (void)esp_timer_stop(s_watchdog); (void)esp_timer_start_once(s_watchdog, ROBOT_DRIVE_WATCHDOG_US); }
  return send_json(request, "{\"ok\":true}", NULL);
}

static esp_err_t handle_drive(httpd_req_t *request) {
  if (browser_allowed_origin(request) && !browser_session_valid(request)) return require_browser_session(request);
  char body[128] = {0};
  if (!receive_body(request, body, sizeof(body))) return send_json(request, "{\"error\":\"invalid_body\"}", "400 Bad Request");
  cJSON *root = cJSON_Parse(body);
  cJSON *action_json = root ? cJSON_GetObjectItem(root, "action") : NULL;
  char id[33] = "";
  if (cJSON_IsString(action_json) && action_json->valuestring) {
    strncpy(id, action_json->valuestring, sizeof(id) - 1);
  }
  cJSON_Delete(root);
  if (!id[0]) return send_json(request, "{\"error\":\"invalid_action\"}", "400 Bad Request");
  if (strcmp(id, "stop") == 0) {
    stop_active_button();
    if (s_callbacks.stop) s_callbacks.stop();
    return send_json(request, "{\"ok\":true}", NULL);
  }
  klh_robot_drive_action_t action = KLH_ROBOT_DRIVE_STOPPED;
  if (strcmp(id, "forward") == 0) action = KLH_ROBOT_DRIVE_FORWARD;
  else if (strcmp(id, "backward") == 0) action = KLH_ROBOT_DRIVE_BACKWARD;
  else if (strcmp(id, "turn_left") == 0) action = KLH_ROBOT_DRIVE_TURN_LEFT;
  else if (strcmp(id, "turn_right") == 0) action = KLH_ROBOT_DRIVE_TURN_RIGHT;
  else return send_json(request, "{\"error\":\"invalid_action\"}", "400 Bad Request");
  bool ok = s_callbacks.drive && s_callbacks.drive(action);
  if (!ok) return send_json(request, "{\"error\":\"robot_busy_or_hardware_unavailable\"}", "503 Service Unavailable");
  s_drive_action = action;
  if (s_watchdog) { (void)esp_timer_stop(s_watchdog); (void)esp_timer_start_once(s_watchdog, ROBOT_DRIVE_WATCHDOG_US); }
  return send_json(request, "{\"ok\":true}", NULL);
}

static esp_err_t handle_action(httpd_req_t *request) {
  if (browser_allowed_origin(request) && !browser_session_valid(request)) return require_browser_session(request);
  char body[128] = {0};
  if (!receive_body(request, body, sizeof(body))) return send_json(request, "{\"error\":\"invalid_body\"}", "400 Bad Request");
  cJSON *root = cJSON_Parse(body);
  cJSON *action_item = root ? cJSON_GetObjectItem(root, "action") : NULL;
  const char *name = cJSON_IsString(action_item) ? action_item->valuestring : NULL;
  klh_robot_action_t action;
  bool continuous = false;
  if (name && strcmp(name, "spin_right") == 0) { action = KLH_ROBOT_ACTION_SPIN_CLOCKWISE; continuous = true; }
  else if (name && strcmp(name, "spin_left") == 0) { action = KLH_ROBOT_ACTION_SPIN_COUNTER_CLOCKWISE; continuous = true; }
  else if (name && strcmp(name, "dance") == 0) action = KLH_ROBOT_ACTION_DANCE;
  else if (name && strcmp(name, "play_game") == 0) action = KLH_ROBOT_ACTION_PLAY_GAME;
  else if (name && strcmp(name, "reset_arms") == 0) action = KLH_ROBOT_ACTION_RESET_ARMS;
  else { cJSON_Delete(root); return send_json(request, "{\"error\":\"invalid_action\"}", "400 Bad Request"); }
  cJSON_Delete(root);
  bool ok = s_callbacks.run_action && s_callbacks.run_action(action);
  if (!ok) return send_json(request, "{\"error\":\"robot_busy_or_hardware_unavailable\"}", "503 Service Unavailable");
  if (continuous && s_watchdog) {
    (void)esp_timer_stop(s_watchdog);
    (void)esp_timer_start_once(s_watchdog, ROBOT_DRIVE_WATCHDOG_US);
  }
  return send_json(request, "{\"ok\":true}", NULL);
}

static esp_err_t handle_arm(httpd_req_t *request) {
  if (browser_allowed_origin(request) && !browser_session_valid(request)) return require_browser_session(request);
  char body[128] = {0};
  if (!receive_body(request, body, sizeof(body))) return send_json(request, "{\"error\":\"invalid_body\"}", "400 Bad Request");
  cJSON *root = cJSON_Parse(body);
  cJSON *arm = root ? cJSON_GetObjectItem(root, "arm") : NULL;
  cJSON *angle = root ? cJSON_GetObjectItem(root, "angle") : NULL;
  int channel = cJSON_IsNumber(arm) ? arm->valueint + 7 : -1;
  int degrees = cJSON_IsNumber(angle) ? angle->valueint : -1;
  cJSON_Delete(root);
  if (channel < 8 || channel > 10 || degrees < 0 || degrees > 270) return send_json(request, "{\"error\":\"invalid_arm_command\"}", "400 Bad Request");
  bool ok = s_callbacks.set_arm_angle
    ? s_callbacks.set_arm_angle(channel - 8, degrees)
    : set_pulse(channel, position_pulse((float)degrees, 270.0f));
  if (ok) { s_position_angle[channel] = (float)degrees; s_position_valid[channel] = true; }
  return ok ? send_json(request, "{\"ok\":true}", NULL) : send_json(request, "{\"error\":\"hardware_unavailable\"}", "503 Service Unavailable");
}

static esp_err_t handle_stop(httpd_req_t *request) {
  if (browser_allowed_origin(request) && !browser_session_valid(request)) return require_browser_session(request);
  stop_active_button();
  if (s_callbacks.stop) s_callbacks.stop();
  return send_json(request, "{\"ok\":true}", NULL);
}

static esp_err_t handle_voice_start(httpd_req_t *request) {
  if (!browser_session_valid(request)) return require_browser_session(request);
  if (!s_callbacks.voice_start || !s_callbacks.voice_start()) {
    return send_json(request, "{\"error\":\"voice_unavailable\"}", "503 Service Unavailable");
  }
  s_browser_voice_active = true;
  s_browser_voice_updated_us = esp_timer_get_time();
  return send_json(request, "{\"ok\":true,\"state\":\"connecting\"}", NULL);
}

static esp_err_t handle_voice_status(httpd_req_t *request) {
  if (!browser_session_valid(request)) return require_browser_session(request);
  const bool ready = s_callbacks.voice_ready && s_callbacks.voice_ready();
  return send_json(request, ready ? "{\"ready\":true}" : "{\"ready\":false}", NULL);
}

static esp_err_t handle_voice_pcm(httpd_req_t *request) {
  if (!browser_session_valid(request)) return require_browser_session(request);
  const size_t expected_bytes = BROWSER_VOICE_FRAME_SAMPLES * sizeof(int16_t);
  if ((size_t)request->content_len != expected_bytes || !s_callbacks.voice_pcm) {
    return send_json(request, "{\"error\":\"invalid_pcm_frame\"}", "400 Bad Request");
  }
  int16_t samples[BROWSER_VOICE_FRAME_SAMPLES];
  size_t received = 0;
  while (received < expected_bytes) {
    int result = httpd_req_recv(request, ((char *)samples) + received, expected_bytes - received);
    if (result == HTTPD_SOCK_ERR_TIMEOUT) continue;
    if (result <= 0) return ESP_FAIL;
    received += (size_t)result;
  }
  if (!s_callbacks.voice_pcm(samples, BROWSER_VOICE_FRAME_SAMPLES)) {
    return send_json(request, "{\"error\":\"voice_not_ready\"}", "409 Conflict");
  }
  s_browser_voice_updated_us = esp_timer_get_time();
  return send_json(request, "{\"ok\":true}", NULL);
}

static esp_err_t handle_voice_stop(httpd_req_t *request) {
  if (!browser_session_valid(request)) return require_browser_session(request);
  s_browser_voice_active = false;
  s_browser_voice_updated_us = 0;
  if (s_callbacks.voice_stop) s_callbacks.voice_stop();
  return send_json(request, "{\"ok\":true}", NULL);
}

static esp_err_t handle_cors_options(httpd_req_t *request) {
  if (!browser_allowed_origin(request)) {
    return send_json(request, "{\"error\":\"origin_not_allowed\"}", "403 Forbidden");
  }
  set_browser_cors_headers(request);
  httpd_resp_set_status(request, "204 No Content");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  httpd_resp_set_hdr(request, "Connection", "close");
  return httpd_resp_send(request, NULL, 0);
}

static esp_err_t handle_not_found(httpd_req_t *request, httpd_err_code_t error) {
  (void)error;
  if (strcmp(request->uri, "/control") == 0 || strcmp(request->uri, "/index.html") == 0) {
    httpd_resp_set_status(request, "302 Found"); httpd_resp_set_hdr(request, "Location", "/"); httpd_resp_set_hdr(request, "Connection", "close"); return httpd_resp_send(request, NULL, 0);
  }
  return send_json(request, "{\"error\":\"not_found\"}", "404 Not Found");
}

bool klh_configurable_robot_control_server_start(const klh_robot_control_callbacks_t *callbacks, unsigned short server_port) {
  if (s_server) return true;
  if (!callbacks || !callbacks->set_servo_pulse_us || !callbacks->stop) return false;
  s_callbacks = *callbacks;
  s_server_port = server_port;
  if (!s_watchdog) {
    const esp_timer_create_args_t timer_args = {.callback = watchdog_callback, .arg = NULL, .dispatch_method = ESP_TIMER_TASK, .name = "robot_stop", .skip_unhandled_events = true};
    if (esp_timer_create(&timer_args, &s_watchdog) != ESP_OK) return false;
  }
  if (!s_voice_watchdog) {
    const esp_timer_create_args_t timer_args = {.callback = voice_watchdog_callback, .arg = NULL, .dispatch_method = ESP_TIMER_TASK, .name = "browser_voice", .skip_unhandled_events = true};
    if (esp_timer_create(&timer_args, &s_voice_watchdog) != ESP_OK) return false;
  }
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = server_port; config.ctrl_port = 32769; config.max_uri_handlers = 29;
  // Port 81 shares the AP with the Wi-Fi configuration HTTP server. Limit it
  // to two clients there, while allowing a few more clients in station mode.
  config.max_open_sockets = server_port == 81 ? 2 : 5;
  config.stack_size = 7168; config.lru_purge_enable = true;
  config.recv_wait_timeout = 5; config.send_wait_timeout = 5;
  if (httpd_start(&s_server, &config) != ESP_OK) { s_server = NULL; return false; }
  if (esp_timer_start_periodic(s_voice_watchdog, 1000000ULL) != ESP_OK) {
    httpd_stop(s_server); s_server = NULL; return false;
  }
  const httpd_uri_t handlers[] = {
    {.uri = "/", .method = HTTP_GET, .handler = handle_index},
    {.uri = "/custom", .method = HTTP_GET, .handler = handle_custom},
    {.uri = "/api/robot/status", .method = HTTP_GET, .handler = handle_status},
    {.uri = "/api/robot/drive", .method = HTTP_POST, .handler = handle_drive},
    {.uri = "/api/robot/action", .method = HTTP_POST, .handler = handle_action},
    {.uri = "/api/robot/arm", .method = HTTP_POST, .handler = handle_arm},
    {.uri = "/api/robot/stop", .method = HTTP_POST, .handler = handle_stop},
    {.uri = "/api/robot/control", .method = HTTP_POST, .handler = handle_control},
    {.uri = "/api/robot/profiles", .method = HTTP_GET, .handler = handle_profiles},
    {.uri = "/api/robot/profile", .method = HTTP_GET, .handler = handle_profile_get},
    {.uri = "/api/robot/profile", .method = HTTP_PUT, .handler = handle_profile_put},
    {.uri = "/api/robot/profile", .method = HTTP_DELETE, .handler = handle_profile_delete},
    {.uri = "/api/robot/profile/activate", .method = HTTP_POST, .handler = handle_profile_activate},
    {.uri = "/api/voice/start", .method = HTTP_POST, .handler = handle_voice_start},
    {.uri = "/api/voice/status", .method = HTTP_GET, .handler = handle_voice_status},
    {.uri = "/api/voice/pcm", .method = HTTP_POST, .handler = handle_voice_pcm},
    {.uri = "/api/voice/stop", .method = HTTP_POST, .handler = handle_voice_stop},
    {.uri = "/api/voice/start", .method = HTTP_OPTIONS, .handler = handle_cors_options},
    {.uri = "/api/voice/status", .method = HTTP_OPTIONS, .handler = handle_cors_options},
    {.uri = "/api/voice/pcm", .method = HTTP_OPTIONS, .handler = handle_cors_options},
    {.uri = "/api/voice/stop", .method = HTTP_OPTIONS, .handler = handle_cors_options},
    {.uri = "/api/robot/status", .method = HTTP_OPTIONS, .handler = handle_cors_options},
    {.uri = "/api/robot/drive", .method = HTTP_OPTIONS, .handler = handle_cors_options},
    {.uri = "/api/robot/action", .method = HTTP_OPTIONS, .handler = handle_cors_options},
    {.uri = "/api/robot/arm", .method = HTTP_OPTIONS, .handler = handle_cors_options},
    {.uri = "/api/robot/stop", .method = HTTP_OPTIONS, .handler = handle_cors_options},
    {.uri = "/api/robot/control", .method = HTTP_OPTIONS, .handler = handle_cors_options},
    {.uri = "/api/robot/profiles", .method = HTTP_OPTIONS, .handler = handle_cors_options},
    {.uri = "/api/robot/profile", .method = HTTP_OPTIONS, .handler = handle_cors_options},
  };
  for (size_t i = 0; i < sizeof(handlers) / sizeof(handlers[0]); i++) {
    if (httpd_register_uri_handler(s_server, &handlers[i]) != ESP_OK) { klh_configurable_robot_control_server_stop(); return false; }
  }
  httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, handle_not_found);
  ESP_LOGI(TAG, "Configurable KLeaderBit robot-control server started on port %u", server_port);
  return true;
}

void klh_configurable_robot_control_server_stop(void) {
  s_browser_voice_active = false;
  s_browser_voice_updated_us = 0;
  if (s_voice_watchdog) (void)esp_timer_stop(s_voice_watchdog);
  if (s_callbacks.voice_stop) s_callbacks.voice_stop();
  stop_active_button();
  if (s_callbacks.stop) s_callbacks.stop();
  if (!s_server) return;
  httpd_stop(s_server); s_server = NULL;
  ESP_LOGI(TAG, "Robot control server stopped");
}

bool klh_configurable_robot_control_server_create_access_url(char *url, size_t capacity,
                                                              unsigned int ttl_seconds) {
  if (!s_server || !url || capacity < 48 || ttl_seconds < 30) return false;
  for (size_t offset = 0; offset < BROWSER_SESSION_TOKEN_LENGTH; offset += 8) {
    snprintf(s_browser_token + offset, sizeof(s_browser_token) - offset, "%08lx",
             (unsigned long)esp_random());
  }
  s_browser_token[BROWSER_SESSION_TOKEN_LENGTH] = '\0';
  s_browser_token_expires_us = esp_timer_get_time() + (int64_t)ttl_seconds * 1000000LL;

  esp_netif_t *netif = esp_netif_get_handle_from_ifkey(
      s_server_port == 81 ? "WIFI_AP_DEF" : "WIFI_STA_DEF");
  esp_netif_ip_info_t info = {0};
  if (!netif || esp_netif_get_ip_info(netif, &info) != ESP_OK || info.ip.addr == 0) return false;
  const int written = snprintf(url, capacity, "http://" IPSTR "%s/?t=%s",
                               IP2STR(&info.ip), s_server_port == 80 ? "" : ":81",
                               s_browser_token);
  return written > 0 && (size_t)written < capacity;
}
