// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Dr.-Ing. Simon Wunderlich Systementwicklung & Beratung
// SPDX-FileContributor: Jan Fuchs <jf [at] simonwunderlich . de>

#include "beacon.h"

#include <stdbool.h>
#include <string.h>
#include <time.h>

#include "clock.h"
#include "config.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "sdkconfig.h"

#define EID_SSID             0
#define EID_SUPP_RATES       1
#define EID_DS_PARAMS        3
#define EID_TIM              5
#define EID_ERP              42
#define EID_RSN              48
#define EID_TIME_ADV         69
#define EID_TIME_ZONE        98
#define EID_EXT_CAPAB        127

#define FC_BEACON            0x80
#define FC_PROBE_REQ         0x40
#define FC_PROBE_RESP        0x50

#define HDR_LEN              24
#define DA_OFFSET            4
#define SA_OFFSET            10
#define BSSID_OFFSET         16
#define FCS_LEN              4

#define TIME_VALUE_LEN       10
#define TIME_ERROR_LEN       5

/* Timing Capabilities: Time Value is the UTC time at which TSF was 0. The
 * hardware zeroes the timestamp of raw frames whose BSSID is not the
 * interface's own MAC, so this is simply the current UTC time. */
#define TIMING_CAP_UTC_AT_TSF0 2

#define BEACON_PERIOD_US     ((uint64_t)BEACON_INTERVAL_TU * 1024)
#define TIME_ERROR_MAX_MS    ((1ULL << (TIME_ERROR_LEN * 8)) - 1)

_Static_assert(sizeof(SSID_NETWORKNAME) - 1 <= 32, "SSID_NETWORKNAME too long");
/* Keeps the frame within struct frame's buffer */
_Static_assert(sizeof(BEACON_TIMEZONE) - 1 <= 64, "BEACON_TIMEZONE too long");
_Static_assert(BEACON_INTERVAL_TU >= 15 && BEACON_INTERVAL_TU <= 65535,
               "BEACON_INTERVAL_TU out of range");

static const char *TAG = "beacon";

struct frame {
    uint8_t buf[256];
    size_t len;
    /* Offsets of the Time Value and Time Update Counter fields */
    size_t time_value_off;
    size_t time_error_off;
    size_t update_counter_off;
};

static esp_timer_handle_t s_timer;
/* Beacon is sent from the esp_timer task, probe response from the Wi-Fi
 * task, so each has its own buffer */
static struct frame s_beacon;
static struct frame s_probe_resp;
static uint8_t s_update_counter;
static bool s_synced;
static uint8_t s_mac[6];
static uint8_t s_channel;

static void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = v & 0xff;
    p[1] = v >> 8;
}

static uint8_t *put_ie(uint8_t *p, uint8_t id, const void *data, size_t len)
{
    *p++ = id;
    *p++ = len;
    memcpy(p, data, len);
    return p + len;
}

/* IEEE 802.11-2020 9.4.2.60: Year(2) Month Day Hours Minutes Seconds
 * Milliseconds(2) Reserved */
static void put_time_value(uint8_t *p, int64_t utc_us)
{
    time_t sec = utc_us / 1000000;
    struct tm tm;

    gmtime_r(&sec, &tm);
    put_le16(p, tm.tm_year + 1900);
    p[2] = tm.tm_mon + 1;
    p[3] = tm.tm_mday;
    p[4] = tm.tm_hour;
    p[5] = tm.tm_min;
    p[6] = tm.tm_sec;
    put_le16(p + 7, (utc_us / 1000) % 1000);
    p[9] = 0;
}

static void build_frame(struct frame *f, uint8_t fc)
{
    /* 11g only (OFDM): 6(B) 9 12(B) 18 24(B) 36 48 54 */
    static const uint8_t rates[] = { 0x8c, 0x12, 0x98, 0x24, 0xb0, 0x48, 0x60, 0x6c };
    /* No non-ERP STAs present, no protection needed */
    static const uint8_t erp = 0;
    /* WPA2-PSK, CCMP. Advertised only: nobody answers authentication, so no
     * key exists. */
    static const uint8_t rsn[] = {
        0x01, 0x00,                 /* Version 1 */
        0x00, 0x0f, 0xac, 0x04,     /* Group cipher: CCMP */
        0x01, 0x00,                 /* Pairwise cipher count */
        0x00, 0x0f, 0xac, 0x04,     /* CCMP */
        0x01, 0x00,                 /* AKM count */
        0x00, 0x0f, 0xac, 0x02,     /* PSK */
        0x00, 0x00,                 /* RSN capabilities */
    };
    /* DTIM count 0, DTIM period 1, bitmap control 0, partial virtual bitmap 0 */
    static const uint8_t tim[] = { 0, 1, 0, 0 };
    /* Bit 27: UTC TSF Offset */
    static const uint8_t ext_capab[] = { 0x00, 0x00, 0x00, 0x08 };
    const char *ssid = SSID_NETWORKNAME;
    const char *tz = BEACON_TIMEZONE;
    uint8_t *p = f->buf;

    /* MAC header */
    *p++ = fc;                  /* Frame Control: management, beacon/probe resp */
    *p++ = 0x00;
    *p++ = 0x00;                /* Duration */
    *p++ = 0x00;
    memset(p, 0xff, 6);         /* DA: broadcast, probe resp: set per request */
    p += 6;
    memcpy(p, s_mac, 6);        /* SA */
    p += 6;
    memcpy(p, s_mac, 6);        /* BSSID */
    p += 6;
    *p++ = 0x00;                /* Sequence control, filled by driver */
    *p++ = 0x00;

    /* Fixed fields */
    p += 8;                     /* Timestamp, zeroed by hardware */
    put_le16(p, BEACON_INTERVAL_TU);
    p += 2;
    put_le16(p, 0x0411);        /* Capabilities: ESS, privacy, short slot time */
    p += 2;

    /* Elements */
    p = put_ie(p, EID_SSID, ssid, strlen(ssid));
    p = put_ie(p, EID_SUPP_RATES, rates, sizeof(rates));
    p = put_ie(p, EID_DS_PARAMS, &s_channel, 1);
    if (fc == FC_BEACON)
        p = put_ie(p, EID_TIM, tim, sizeof(tim));
    p = put_ie(p, EID_ERP, &erp, 1);
    p = put_ie(p, EID_RSN, rsn, sizeof(rsn));
    p = put_ie(p, EID_EXT_CAPAB, ext_capab, sizeof(ext_capab));

    *p++ = EID_TIME_ADV;
    *p++ = 1 + TIME_VALUE_LEN + TIME_ERROR_LEN + 1;
    *p++ = TIMING_CAP_UTC_AT_TSF0;
    f->time_value_off = p - f->buf;
    p += TIME_VALUE_LEN;
    f->time_error_off = p - f->buf;
    p += TIME_ERROR_LEN;
    f->update_counter_off = p - f->buf;
    *p++ = s_update_counter;

    p = put_ie(p, EID_TIME_ZONE, tz, strlen(tz));

    f->len = p - f->buf;
}

/* IEEE 802.11-2024 9.4.2.59: with Timing Capabilities 2 the standard
 * deviation of the error in the Time Value, unsigned, in milliseconds */
static void put_time_error(uint8_t *p)
{
    uint64_t ms = clock_error_ms();

    if (ms > TIME_ERROR_MAX_MS)
        ms = TIME_ERROR_MAX_MS;
    for (int i = 0; i < TIME_ERROR_LEN; i++)
        p[i] = ms >> (8 * i);
}

static void send_frame(struct frame *f)
{
    esp_err_t err;

    put_time_value(f->buf + f->time_value_off, clock_utc_us());
    put_time_error(f->buf + f->time_error_off);

    err = esp_wifi_80211_tx(WIFI_IF_STA, f->buf, f->len, true);
    if (err != ESP_OK)
        ESP_LOGW(TAG, "esp_wifi_80211_tx: %s", esp_err_to_name(err));
}

static void send_beacon(void *arg)
{
    send_frame(&s_beacon);
}

#if PROBE_RESPONSES
static bool is_bcast_or_own(const uint8_t *addr)
{
    static const uint8_t bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

    return !memcmp(addr, bcast, 6) || !memcmp(addr, s_mac, 6);
}

/* Wildcard or our SSID */
static bool ssid_matches(const uint8_t *ies, size_t len)
{
    const char *ssid = SSID_NETWORKNAME;

    while (len >= 2 && ies[1] + 2u <= len) {
        if (ies[0] == EID_SSID)
            return ies[1] == 0 ||
                   (ies[1] == strlen(ssid) && !memcmp(ies + 2, ssid, ies[1]));
        len -= ies[1] + 2;
        ies += ies[1] + 2;
    }
    return false;
}

static void rx_mgmt(void *buf, wifi_promiscuous_pkt_type_t type)
{
    const wifi_promiscuous_pkt_t *pkt = buf;
    const uint8_t *hdr = pkt->payload;
    int len = pkt->rx_ctrl.sig_len - FCS_LEN;

    if (type != WIFI_PKT_MGMT || len < HDR_LEN || hdr[0] != FC_PROBE_REQ)
        return;
    if (!is_bcast_or_own(hdr + DA_OFFSET) || !is_bcast_or_own(hdr + BSSID_OFFSET))
        return;
    if (!ssid_matches(hdr + HDR_LEN, len - HDR_LEN))
        return;

    memcpy(s_probe_resp.buf + DA_OFFSET, hdr + SA_OFFSET, 6);
    send_frame(&s_probe_resp);
}

static esp_err_t probe_resp_start(void)
{
    const wifi_promiscuous_filter_t filter = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT,
    };

    ESP_RETURN_ON_ERROR(esp_wifi_set_promiscuous_filter(&filter), TAG, "promisc filter");
    ESP_RETURN_ON_ERROR(esp_wifi_set_promiscuous_rx_cb(rx_mgmt), TAG, "promisc cb");
    return esp_wifi_set_promiscuous(true);
}
#endif

void beacon_time_synced(void)
{
    /* Counts the clock updates, starting at 0 with the first sync */
    if (s_synced)
        s_update_counter++;
    s_synced = true;
}

esp_err_t beacon_start(uint8_t channel)
{
    esp_err_t err;

    if (!s_timer) {
        const esp_timer_create_args_t args = {
            .callback = send_beacon,
            .name = "beacon",
        };
        uint8_t sta_mac[6];

        /* Own locally administered BSSID, so frames addressed to the time
         * beacon do not match the STA interface and stay unanswered */
        esp_read_mac(sta_mac, ESP_MAC_WIFI_STA);
        esp_derive_local_mac(s_mac, sta_mac);
        ESP_RETURN_ON_ERROR(esp_timer_create(&args, &s_timer), TAG, "timer");
    }

    /* The channel may change with every start */
    s_channel = channel;
    build_frame(&s_beacon, FC_BEACON);
    build_frame(&s_probe_resp, FC_PROBE_RESP);

    err = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set channel: %s", esp_err_to_name(err));
        return err;
    }

#if PROBE_RESPONSES
    ESP_RETURN_ON_ERROR(probe_resp_start(), TAG, "probe responses");
#endif

    send_beacon(NULL);
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(s_timer, BEACON_PERIOD_US), TAG, "timer start");
    ESP_LOGI(TAG, "sending \"%s\" on channel %d every %d TU, probe responses %s",
             SSID_NETWORKNAME, channel, BEACON_INTERVAL_TU,
             PROBE_RESPONSES ? "on" : "off");
    return ESP_OK;
}

void beacon_stop(void)
{
#if PROBE_RESPONSES
    esp_wifi_set_promiscuous(false);
#endif
    if (s_timer && esp_timer_is_active(s_timer))
        esp_timer_stop(s_timer);
}
