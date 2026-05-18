/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 *
 * USB Serial Control Interface - receives JSON commands via USB CDC
 * and forwards them to the hardware control layer.
 *
 * Commands (JSON, one per line):
 *   {"cmd":"set_head","yaw":45,"pitch":30,"speed":150}
 *   {"cmd":"get_head"}
 *   {"cmd":"set_led","index":0,"r":255,"g":0,"b":0}
 *   {"cmd":"set_all_leds","r":255,"g":0,"b":0}
 *   {"cmd":"clear_leds"}
 *   {"cmd":"set_servo_power","on":true}
 *   {"cmd":"get_battery"}
 *   {"cmd":"set_avatar","face":"happy"}
 *   {"cmd":"ping"}
 *   {"cmd":"set_volume","volume":80}
 *   {"cmd":"set_brightness","brightness":80}
 *
 * Response: {"ok":true} or {"ok":false,"error":"..."}
 * For get_* commands, response includes data: {"ok":true,"yaw":0,"pitch":0}
 */

#include "hal.h"
#include "cJSON.h"
#include <driver/uart.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stackchan/stackchan.h>
#include <string>
#include <cstring>

static const char *TAG = "SerialCtrl";

#define SERIAL_CTRL_BUF_SIZE 512

// Forward declaration
void serial_ctrl_task(void *pvParameters);

static void send_response(const std::string &json_response)
{
    // Send response via USB CDC (UART0 = console)
    uart_write_bytes(UART_NUM_0, (json_response + "\n").c_str(), json_response.length() + 1);
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

    } else if (cmd == "set_head") {
        cJSON *yaw_json = cJSON_GetObjectItem(root, "yaw");
        cJSON *pitch_json = cJSON_GetObjectItem(root, "pitch");
        cJSON *speed_json = cJSON_GetObjectItem(root, "speed");

        int yaw = yaw_json && cJSON_IsNumber(yaw_json) ? yaw_json->valueint : -9999;
        int pitch = pitch_json && cJSON_IsNumber(pitch_json) ? pitch_json->valueint : -9999;
        int speed = speed_json && cJSON_IsNumber(speed_json) ? speed_json->valueint : 150;

        try {
            auto &motion = GetStackChan().motion();
            LvglLockGuard lock;
            if (pitch != -9999) {
                motion.pitchServo().moveWithSpeed(pitch * 10, speed);
            }
            if (yaw != -9999) {
                motion.yawServo().moveWithSpeed(yaw * 10, speed);
            }
            send_response("{\"ok\":true}");
        } catch (const std::exception &e) {
            std::string err = "{\"ok\":false,\"error\":\"";
            err += e.what();
            err += "\"}";
            send_response(err);
        }

    } else if (cmd == "get_head") {
        try {
            auto &motion = GetStackChan().motion();
            LvglLockGuard lock;
            int yaw = motion.yawServo().getCurrentAngle() / 10;
            int pitch = motion.pitchServo().getCurrentAngle() / 10;
            char response[128];
            snprintf(response, sizeof(response), "{\"ok\":true,\"yaw\":%d,\"pitch\":%d}", yaw, pitch);
            send_response(response);
        } catch (const std::exception &e) {
            send_response("{\"ok\":false,\"error\":\"failed to read head angles\"}");
        }

    } else if (cmd == "set_led" || cmd == "set_all_leds") {
        cJSON *r_json = cJSON_GetObjectItem(root, "r");
        cJSON *g_json = cJSON_GetObjectItem(root, "g");
        cJSON *b_json = cJSON_GetObjectItem(root, "b");
        int r = r_json && cJSON_IsNumber(r_json) ? r_json->valueint : 0;
        int g = g_json && cJSON_IsNumber(g_json) ? g_json->valueint : 0;
        int b = b_json && cJSON_IsNumber(b_json) ? b_json->valueint : 0;

        if (cmd == "set_all_leds") {
            LvglLockGuard lock;
            GetStackChan().leftNeonLight().setColor(r, g, b);
            GetStackChan().rightNeonLight().setColor(r, g, b);
        } else {
            cJSON *idx_json = cJSON_GetObjectItem(root, "index");
            int idx = idx_json && cJSON_IsNumber(idx_json) ? idx_json->valueint : 0;
            LvglLockGuard lock;
            hal.setRgbColor(idx, r, g, b);
            hal.refreshRgb();
        }
        send_response("{\"ok\":true}");

    } else if (cmd == "clear_leds") {
        LvglLockGuard lock;
        GetStackChan().leftNeonLight().setColor(0, 0, 0);
        GetStackChan().rightNeonLight().setColor(0, 0, 0);
        send_response("{\"ok\":true}");

    } else if (cmd == "set_servo_power") {
        cJSON *on_json = cJSON_GetObjectItem(root, "on");
        bool on = on_json && cJSON_IsBool(on_json) ? cJSON_IsTrue(on_json) : false;
        hal.setServoPowerEnabled(on);
        send_response("{\"ok\":true}");

    } else if (cmd == "get_battery") {
        uint8_t level = hal.getBatteryLevel();
        bool charging = hal.isBatteryCharging();
        char response[128];
        snprintf(response, sizeof(response), "{\"ok\":true,\"level\":%u,\"charging\":%s}",
                 level, charging ? "true" : "false");
        send_response(response);

    } else if (cmd == "set_volume") {
        cJSON *vol_json = cJSON_GetObjectItem(root, "volume");
        if (vol_json && cJSON_IsNumber(vol_json)) {
            hal.setSpeakerVolume(vol_json->valueint);
            send_response("{\"ok\":true}");
        } else {
            send_response("{\"ok\":false,\"error\":\"missing volume\"}");
        }

    } else if (cmd == "set_brightness") {
        cJSON *bri_json = cJSON_GetObjectItem(root, "brightness");
        if (bri_json && cJSON_IsNumber(bri_json)) {
            hal.setBackLightBrightness(bri_json->valueint);
            send_response("{\"ok\":true}");
        } else {
            send_response("{\"ok\":false,\"error\":\"missing brightness\"}");
        }

    } else {
        std::string err = "{\"ok\":false,\"error\":\"unknown command: ";
        err += cmd;
        err += "\"}";
        send_response(err);
    }

    cJSON_Delete(root);
}

void serial_ctrl_task(void *pvParameters)
{
    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    };

    // Use UART_NUM_0 (USB CDC / console port)
    ESP_ERROR_CHECK(uart_param_config(UART_NUM_0, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM_0, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    std::string buffer;
    char tmp[64];

    ESP_LOGI(TAG, "Serial control task started");

    while (1) {
        int len = uart_read_bytes(UART_NUM_0, tmp, sizeof(tmp) - 1, pdMS_TO_TICKS(100));
        if (len > 0) {
            tmp[len] = '\0';
            buffer += tmp;

            // Process complete lines
            size_t pos;
            while ((pos = buffer.find('\n')) != std::string::npos ||
                   (pos = buffer.find('\r')) != std::string::npos) {
                std::string line = buffer.substr(0, pos);
                buffer.erase(0, pos + 1);

                // Trim whitespace
                line.erase(0, line.find_first_not_of(" \t\r\n"));
                line.erase(line.find_last_not_of(" \t\r\n") + 1);

                if (!line.empty() && line[0] == '{') {
                    handle_command(line);
                }
            }

            // Prevent buffer from growing indefinitely
            if (buffer.length() > SERIAL_CTRL_BUF_SIZE) {
                buffer.clear();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void hal_serial_ctrl_init()
{
    ESP_LOGI(TAG, "Initializing serial control interface");
    xTaskCreate(serial_ctrl_task, "serial_ctrl", 4096, NULL, 5, NULL);
}
