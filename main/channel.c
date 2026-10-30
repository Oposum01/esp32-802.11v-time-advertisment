// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Dr.-Ing. Simon Wunderlich Systementwicklung & Beratung
// SPDX-FileContributor: Jan Fuchs <jf [at] simonwunderlich . de>

#include "channel.h"

#include <stdio.h>

#include "esp_log.h"
#include "esp_wifi.h"

#define MAX_CHANNEL      13
/* Used if the scan fails before any channel was chosen */
#define FALLBACK_CHANNEL 6

static const char *TAG = "channel";

uint8_t channel_select(uint8_t current)
{
    /* Candidates, on a tie the first one wins */
    static const uint8_t candidates[] = { 1, 6, 11 };
    const wifi_scan_config_t scan = { .show_hidden = true };
    uint16_t count[MAX_CHANNEL + 1] = { 0 };
    wifi_country_t country;
    wifi_ap_record_t ap;
    uint16_t num = 0;
    uint8_t first, last, best = current;
    char buf[MAX_CHANNEL * 8];
    size_t len = 0;
    esp_err_t err;

    err = esp_wifi_get_country(&country);
    if (err == ESP_OK)
        err = esp_wifi_scan_start(&scan, true);
    if (err == ESP_OK)
        err = esp_wifi_scan_get_ap_num(&num);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan failed: %s", esp_err_to_name(err));
        esp_wifi_clear_ap_list();
        return current ? current : FALLBACK_CHANNEL;
    }

    for (uint16_t i = 0; i < num; i++) {
        if (esp_wifi_scan_get_ap_record(&ap) != ESP_OK)
            break;
        if (ap.primary <= MAX_CHANNEL)
            count[ap.primary]++;
    }
    esp_wifi_clear_ap_list();

    first = country.schan;
    last = country.schan + country.nchan - 1;
    if (last > MAX_CHANNEL)
        last = MAX_CHANNEL;

    for (size_t i = 0; i < sizeof(candidates); i++) {
        uint8_t ch = candidates[i];

        if (ch < first || ch > last)
            continue;
        if (!best || count[ch] < count[best])
            best = ch;
    }

    for (uint8_t ch = first; ch <= last; ch++)
        len += snprintf(buf + len, sizeof(buf) - len, " %d:%d", ch, count[ch]);
    ESP_LOGI(TAG, "%d BSSIDs, per channel%s -> channel %d", num, buf, best);

    return best;
}
