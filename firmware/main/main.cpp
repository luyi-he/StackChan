/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include <smooth_ui_toolkit.hpp>
#include <uitk/short_namespace.hpp>
#include <mooncake_log.h>
#include <mooncake.h>
#include <apps/apps.h>
#include <hal/hal.h>
#include <hal/hal_serial_ctrl.h>
#include <nvs_flash.h>

using namespace mooncake;
using namespace smooth_ui_toolkit;

extern "C" void app_main(void)
{
    // Setup logger
    mclog::set_level(mclog::level_info);
    mclog::set_time_format(mclog::time_format_unix_milliseconds);

    // === Hardcoded WebSocket URL (bypass OTA) ===
    {
        nvs_handle_t handle;
        esp_err_t ret = nvs_open("websocket", NVS_READWRITE, &handle);
        if (ret == ESP_OK) {
            nvs_set_str(handle, "url", "ws://10.11.128.192:8765/");
            nvs_commit(handle);
            nvs_close(handle);
        }
    }

    // HAL init
    GetHAL().init();

    // Setup ui hal
    ui_hal::on_delay([](uint32_t ms) { GetHAL().delay(ms); });
    ui_hal::on_get_tick([]() { return GetHAL().millis(); });

    // Install apps
    GetMooncake().installApp(std::make_unique<AppLauncher>());
    GetMooncake().installApp(std::make_unique<AppAiAgent>());
    GetMooncake().installApp(std::make_unique<AppAvatar>());
    GetMooncake().installApp(std::make_unique<AppEspnowControl>());
    GetMooncake().installApp(std::make_unique<AppAppCenter>());
    GetMooncake().installApp(std::make_unique<AppEzdata>());
    GetMooncake().installApp(std::make_unique<AppDance>());
    GetMooncake().installApp(std::make_unique<AppSetup>());

    // Main loop
    while (1) {
        GetHAL().feedTheDog();
        GetHAL().updateHeapStatusLog();

        GetMooncake().update();

        // Process serial commands (non-blocking)
        serial_ctrl_process();

        if (GetHAL().isXiaozhiStartRequested()) {
            break;
        }
    }

    // Uninstall all apps and destroy mooncake
    GetMooncake().uninstallAllApps();
    DestroyMooncake();

    // Start xiaozhi, never returns
    GetHAL().startXiaozhi();
}
