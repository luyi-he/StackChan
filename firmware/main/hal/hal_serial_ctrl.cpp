/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 *
 * USB Serial Control Interface - receives JSON commands via USB CDC
 */

#include "hal.h"
#include "cJSON.h"
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/usb_serial_jtag.h>
#include <stackchan/stackchan.h>
#include <string>
#include <cstring>

static const char *TAG = "SerialCtrl";

static void send_response(const std::string &json_response)
{
    usb_serial_jtag_write_bytes(
        (const uint8_t*)(json_response + "\n").c_str(),
        json_response.length() + 1, portMAX_DELAY);
}

static void handle_command(const std::string &line)
{
    cJSON *root = cJSON_Parse(line.c_str());
    if (!root) {
        send_response("{\"ok\":false,\"error\":\"invalid JSON\"}");
        return;
    }

    cJSON *cmd_json = cJSON_GetObjectItem(root, "cmd");
    if (!cmd_json || !cJSON_IsString(cmd_json)) {
        send_response("{\"ok\":false,\"error\":\"missing cmd field\"}");
        cJSON_Delete(root);
        return;
    }

    std::string cmd = cmd_json->valuestring;
    auto &hal = GetHAL();

    if (cmd == "ping") {
        send_response("{\"ok\":true,\"pong\":true}");
    }
    else if (cmd == "set_head") {
        cJSON *yaw_json = cJSON_GetObjectItem(root, "yaw");
        cJSON *pitch_json = cJSON_GetObjectItem(root, "pitch");
        cJSON *speed_json = cJSON_GetObjectItem(root, "speed");
        int yaw = yaw_json && cJSON_IsNumber(yaw_json) ? yaw_json->valueint : -9999;
        int pitch = pitch_json && cJSON_IsNumber(pitch_json) ? pitch_json->valueint : -9999;
        int speed = speed_json && cJSON_IsNumber(speed_json) ? speed_json->valueint : 150;
        LvglLockGuard lock;
        auto &motion = GetStackChan().motion();
        if (pitch != -9999) motion.pitchServo().moveWithSpeed(pitch * 10, speed);
        if (yaw != -9999) motion.yawServo().moveWithSpeed(yaw * 10, speed);
        send_response("{\"ok\":true}");
    }
    else if (cmd == "get_head") {
        LvglLockGuard lock;
        auto &motion = GetStackChan().motion();
        int yaw = motion.yawServo().getCurrentAngle() / 10;
        int pitch = motion.pitchServo().getCurrentAngle() / 10;
        char resp[128];
        snprintf(resp, sizeof(resp), "{\"ok\":true,\"yaw\":%d,\"pitch\":%d}", yaw, pitch);
        send_response(resp);
    }
    else if (cmd == "set_all_leds") {
        cJSON *r_json = cJSON_GetObjectItem(root, "r");
        cJSON *g_json = cJSON_GetObjectItem(root, "g");
        cJSON *b_json = cJSON_GetObjectItem(root, "b");
        int r = r_json && cJSON_IsNumber(r_json) ? r_json->valueint : 0;
        int g = g_json && cJSON_IsNumber(g_json) ? g_json->valueint : 0;
        int b = b_json && cJSON_IsNumber(b_json) ? b_json->valueint : 0;
        LvglLockGuard lock;
        GetStackChan().leftNeonLight().setColor(r, g, b);
        GetStackChan().rightNeonLight().setColor(r, g, b);
        send_response("{\"ok\":true}");
    }
    else if (cmd == "clear_leds") {
        LvglLockGuard lock;
        GetStackChan().leftNeonLight().setColor(0, 0, 0);
        GetStackChan().rightNeonLight().setColor(0, 0, 0);
        send_response("{\"ok\":true}");
    }
    else if (cmd == "get_battery") {
        uint8_t level = hal.getBatteryLevel();
        bool charging = hal.isBatteryCharging();
        char resp[128];
        snprintf(resp, sizeof(resp), "{\"ok\":true,\"level\":%u,\"charging\":%s}",
                 level, charging ? "true" : "false");
        send_response(resp);
    }
    else if (cmd == "set_volume") {
        cJSON *vol = cJSON_GetObjectItem(root, "volume");
        if (vol && cJSON_IsNumber(vol)) {
            hal.setSpeakerVolume(vol->valueint);
            send_response("{\"ok\":true}");
        }
    }
    else if (cmd == "set_brightness") {
        cJSON *bri = cJSON_GetObjectItem(root, "brightness");
        if (bri && cJSON_IsNumber(bri)) {
            hal.setBackLightBrightness(bri->valueint);
            send_response("{\"ok\":true}");
        }
    }
    else {
        std::string err = "{\"ok\":false,\"error\":\"unknown command: " + cmd + "\"}";
        send_response(err);
    }

    cJSON_Delete(root);
}

// Non-blocking single-shot read - process one line if available
void serial_ctrl_process()
{
    // Skip processing - driver not ready in main loop context
    return;
}

void hal_serial_ctrl_init()
{
    ESP_LOGI(TAG, "Serial control ready - call serial_ctrl_process() from main loop");
}
