// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Dr.-Ing. Simon Wunderlich Systementwicklung & Beratung
// SPDX-FileContributor: Jan Fuchs <jf [at] simonwunderlich . de>

#include <stdbool.h>
#include <time.h>

#include "beacon.h"
#include "channel.h"
#include "clock.h"
#include "config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "txpower.h"
#include "uplink.h"

static const char *TAG = "main";

_Static_assert(RADIO_CHANNEL >= 0 && RADIO_CHANNEL <= 13, "RADIO_CHANNEL out of range");

static void log_time(void)
{
    char buf[64];
    time_t now = time(NULL);
    struct tm tm;

    gmtime_r(&now, &tm);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S UTC", &tm);
    ESP_LOGI(TAG, "time synced: %s", buf);
}

void app_main(void)
{
    bool synced_once = false;
    uint8_t channel = RADIO_CHANNEL;
    esp_err_t err;

    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(uplink_init());
    ESP_ERROR_CHECK(txpower_init());

    for (;;) {
        uint32_t wait_s;

        beacon_stop();
        err = uplink_sync_time();
        if (err == ESP_OK) {
            synced_once = true;
            clock_synced();
            beacon_time_synced();
            log_time();
        }

        /* Without any sync there is no valid time to advertise. After a
         * failed resync keep advertising the free-running clock. */
        if (synced_once) {
            if (!RADIO_CHANNEL)
                channel = channel_select(channel);
            ESP_ERROR_CHECK(beacon_start(channel));
        }

        wait_s = err == ESP_OK ? NTP_SYNC_INTERVAL_MIN * 60 : CONFIG_RETRY_INTERVAL_S;
        vTaskDelay(wait_s * configTICK_RATE_HZ);
    }
}
