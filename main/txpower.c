// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Dr.-Ing. Simon Wunderlich Systementwicklung & Beratung
// SPDX-FileContributor: Jan Fuchs <jf [at] simonwunderlich . de>

#include "txpower.h"

#include <stdbool.h>

#include "config.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#define TX_POWER_MIN_DBM 2
#define TX_POWER_MAX_DBM 20

/* BOOT button, low when pressed */
#define BUTTON_GPIO      GPIO_NUM_0
/* Also debounces the button */
#define BUTTON_POLL_MS   50
/* Holding the button this long resets to TX_POWER_DBM */
#define BUTTON_LONG_MS   3000

#define NVS_NAMESPACE    "txpower"
#define NVS_KEY_DBM      "dbm"
/* TX_POWER_DBM the stored value was based on */
#define NVS_KEY_DEFAULT  "default"

_Static_assert(TX_POWER_DBM >= TX_POWER_MIN_DBM && TX_POWER_DBM <= TX_POWER_MAX_DBM,
               "TX_POWER_DBM out of range");

static const char *TAG = "txpower";

static int s_dbm = TX_POWER_DBM;

static void apply(void)
{
    int8_t actual;
    esp_err_t err;

    /* Unit 0.25 dBm. The hardware supports only a few levels, the next lower
     * one is used. */
    err = esp_wifi_set_max_tx_power(s_dbm * 4);
    if (err == ESP_OK)
        err = esp_wifi_get_max_tx_power(&actual);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set %d dBm: %s", s_dbm, esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "max TX power %d dBm (driver reports %d.%02d dBm)",
             s_dbm, actual / 4, actual % 4 * 25);
}

/* The value set via the button survives reboots, unless TX_POWER_DBM was
 * changed since */
static void load(void)
{
    nvs_handle_t nvs;
    int8_t dbm, def;

    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK)
        return;
    if (nvs_get_i8(nvs, NVS_KEY_DBM, &dbm) == ESP_OK &&
        nvs_get_i8(nvs, NVS_KEY_DEFAULT, &def) == ESP_OK &&
        def == TX_POWER_DBM && dbm >= TX_POWER_MIN_DBM && dbm <= TX_POWER_MAX_DBM)
        s_dbm = dbm;
    nvs_close(nvs);
}

static void store(void)
{
    nvs_handle_t nvs;
    esp_err_t err;

    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_set_i8(nvs, NVS_KEY_DBM, s_dbm);
        if (err == ESP_OK)
            err = nvs_set_i8(nvs, NVS_KEY_DEFAULT, TX_POWER_DBM);
        if (err == ESP_OK)
            err = nvs_commit(nvs);
        nvs_close(nvs);
    }
    if (err != ESP_OK)
        ESP_LOGW(TAG, "store: %s", esp_err_to_name(err));
}

static void set(int dbm)
{
    s_dbm = dbm;
    apply();
    store();
}

/* Short press (on release): 1 dB down, long press: back to TX_POWER_DBM */
static void button_task(void *arg)
{
    unsigned int held_ms = 0;

    for (;;) {
        bool pressed = !gpio_get_level(BUTTON_GPIO);

        if (pressed) {
            held_ms += BUTTON_POLL_MS;
            if (held_ms == BUTTON_LONG_MS) {
                ESP_LOGI(TAG, "long press, reset");
                set(TX_POWER_DBM);
            }
        } else {
            if (held_ms && held_ms < BUTTON_LONG_MS)
                set(s_dbm > TX_POWER_MIN_DBM ? s_dbm - 1 : TX_POWER_MAX_DBM);
            held_ms = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
    }
}

esp_err_t txpower_init(void)
{
    const gpio_config_t io = {
        .pin_bit_mask = BIT64(BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };

    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "gpio");
    load();
    apply();

    if (xTaskCreate(button_task, "button", 3072, NULL, 5, NULL) != pdPASS)
        return ESP_ERR_NO_MEM;
    return ESP_OK;
}
