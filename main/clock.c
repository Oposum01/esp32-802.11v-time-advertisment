// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Dr.-Ing. Simon Wunderlich Systementwicklung & Beratung
// SPDX-FileContributor: Jan Fuchs <jf [at] simonwunderlich . de>

#include "clock.h"

#include <stdbool.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

/* Error right after an NTP sync over Wi-Fi (network and sending delay) */
#define SYNC_ERROR_MS      50
/* Drift assumed for the error estimate: ESP32 crystal tolerance with some
 * margin while the drift is unknown, measurement uncertainty and temperature
 * changes once it is compensated */
#define DRIFT_UNKNOWN_PPM  20
#define DRIFT_RESIDUAL_PPM 5
/* Shorter intervals give a too noisy drift measurement */
#define MIN_MEASURE_US     (30 * 60 * 1000000LL)
/* Anything above is a bad NTP answer rather than drift */
#define MAX_DRIFT_PPB      100000

#define NVS_NAMESPACE      "clock"
#define NVS_KEY_DRIFT      "drift_ppb"

static const char *TAG = "clock";

/* Recorded by clock_ntp_cb(). esp_netif signals the sync before calling it,
 * so it may run slightly after the sync wait returned. */
static int64_t s_ntp_utc_us;
static int64_t s_ntp_mono_us;
static volatile bool s_ntp_pending;

/* Last sync */
static bool s_synced;
static int64_t s_utc_us;
static int64_t s_mono_us;

/* Rate of the true time relative to the local clock, positive if the local
 * clock is slow */
static bool s_drift_loaded;
static bool s_drift_known;
static int32_t s_drift_ppb;

static void load_drift(void)
{
    nvs_handle_t nvs;
    int32_t ppb;

    s_drift_loaded = true;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK)
        return;
    if (nvs_get_i32(nvs, NVS_KEY_DRIFT, &ppb) == ESP_OK && abs(ppb) <= MAX_DRIFT_PPB) {
        s_drift_ppb = ppb;
        s_drift_known = true;
        ESP_LOGI(TAG, "stored drift %+.3f ppm", ppb / 1000.0);
    }
    nvs_close(nvs);
}

static void store_drift(void)
{
    nvs_handle_t nvs;
    esp_err_t err;

    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_set_i32(nvs, NVS_KEY_DRIFT, s_drift_ppb);
        if (err == ESP_OK)
            err = nvs_commit(nvs);
        nvs_close(nvs);
    }
    if (err != ESP_OK)
        ESP_LOGW(TAG, "store: %s", esp_err_to_name(err));
}

static int64_t utc_at(int64_t mono_us)
{
    int64_t elapsed = mono_us - s_mono_us;

    return s_utc_us + elapsed + elapsed * s_drift_ppb / 1000000000;
}

void clock_ntp_cb(struct timeval *tv)
{
    s_ntp_mono_us = esp_timer_get_time();
    s_ntp_utc_us = (int64_t)tv->tv_sec * 1000000 + tv->tv_usec;
    s_ntp_pending = true;
}

void clock_synced(void)
{
    int64_t utc, mono;

    if (!s_drift_loaded)
        load_drift();

    if (s_ntp_pending) {
        s_ntp_pending = false;
        utc = s_ntp_utc_us;
        mono = s_ntp_mono_us;
    } else {
        /* Fall back to the system clock SNTP just set */
        struct timeval tv;

        ESP_LOGW(TAG, "no NTP time recorded, using system clock");
        gettimeofday(&tv, NULL);
        mono = esp_timer_get_time();
        utc = (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
    }

    if (s_synced) {
        int64_t elapsed = mono - s_mono_us;
        /* How far the advertised time was off right before this sync */
        int64_t offset = utc - utc_at(mono);

        ESP_LOGI(TAG, "advertised time was off by %+.1f ms after %lld s",
                 offset / 1000.0, elapsed / 1000000);

        if (elapsed >= MIN_MEASURE_US) {
            int64_t measured = s_drift_ppb + offset * 1000000000 / elapsed;

            if (llabs(measured) <= MAX_DRIFT_PPB) {
                /* Average with the previous value against NTP noise */
                s_drift_ppb = s_drift_known ? (s_drift_ppb + measured) / 2 : measured;
                s_drift_known = true;
                store_drift();
                ESP_LOGI(TAG, "drift measured %+.3f ppm, using %+.3f ppm",
                         measured / 1000.0, s_drift_ppb / 1000.0);
            } else {
                ESP_LOGW(TAG, "drift %+.3f ppm implausible, ignored", measured / 1000.0);
            }
        }
    }

    s_utc_us = utc;
    s_mono_us = mono;
    s_synced = true;
}

int64_t clock_utc_us(void)
{
    return utc_at(esp_timer_get_time());
}

uint64_t clock_error_ms(void)
{
    uint64_t elapsed = esp_timer_get_time() - s_mono_us;
    uint64_t ppm = s_drift_known ? DRIFT_RESIDUAL_PPM : DRIFT_UNKNOWN_PPM;

    return SYNC_ERROR_MS + elapsed * ppm / 1000000000;
}
