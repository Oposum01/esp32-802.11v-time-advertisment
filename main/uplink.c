// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Dr.-Ing. Simon Wunderlich Systementwicklung & Beratung
// SPDX-FileContributor: Jan Fuchs <jf [at] simonwunderlich . de>

#include "uplink.h"

#include <string.h>

#include "clock.h"
#include "config.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/dhcp.h"
#include "sdkconfig.h"

#define GOT_IP_BIT       BIT0
#define DISCONNECTED_BIT BIT1

/* Time for the DHCPRELEASE to leave before disassociating */
#define DHCP_RELEASE_WAIT_MS 200

static const char *TAG = "uplink";

_Static_assert(sizeof(DHCP_HOSTNAME) <= 33, "DHCP_HOSTNAME too long");
_Static_assert(sizeof(WIFI_COUNTRY) == 3, "WIFI_COUNTRY must be a two-letter code");

struct uplink {
    const char *ssid;
    /* Empty for an open network */
    const char *password;
};

static const struct uplink s_uplinks[] = { UPLINK_NETWORKS };

static EventGroupHandle_t s_events;
static esp_netif_t *s_sta;

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *ev = data;

        ESP_LOGI(TAG, "disconnected (reason %d)", ev->reason);
        xEventGroupClearBits(s_events, GOT_IP_BIT);
        xEventGroupSetBits(s_events, DISCONNECTED_BIT);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ev = data;

        ESP_LOGI(TAG, "got IP " IPSTR " as \"%s\"", IP2STR(&ev->ip_info.ip), DHCP_HOSTNAME);
        xEventGroupSetBits(s_events, GOT_IP_BIT);
    }
}

esp_err_t uplink_init(void)
{
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

    s_events = xEventGroupCreate();

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");
    s_sta = esp_netif_create_default_wifi_sta();
    ESP_RETURN_ON_ERROR(esp_netif_set_hostname(s_sta, DHCP_HOSTNAME), TAG, "hostname");

    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                                   event_handler, NULL), TAG, "handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                   event_handler, NULL), TAG, "handler");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "storage");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_country_code(WIFI_COUNTRY, false), TAG, "country");
    /* Raw beacons are 11g only, must be configured before esp_wifi_start() */
    ESP_RETURN_ON_ERROR(esp_wifi_config_80211_tx_rate(WIFI_IF_STA, WIFI_PHY_RATE_6M), TAG, "tx rate");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start");
    /* Beacons must go out on time */
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), TAG, "ps");

    return ESP_OK;
}

static esp_err_t connect(const struct uplink *up)
{
    wifi_config_t cfg = {
        .sta = {
            .threshold.authmode = up->password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN,
        },
    };
    EventBits_t bits;

    if (strlen(up->ssid) > sizeof(cfg.sta.ssid) ||
        strlen(up->password) >= sizeof(cfg.sta.password)) {
        ESP_LOGE(TAG, "SSID or password of \"%s\" too long", up->ssid);
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(cfg.sta.ssid, up->ssid, strlen(up->ssid));
    strcpy((char *)cfg.sta.password, up->password);

    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &cfg), TAG, "config");

    xEventGroupClearBits(s_events, GOT_IP_BIT | DISCONNECTED_BIT);
    ESP_LOGI(TAG, "connecting to \"%s\"", up->ssid);
    ESP_RETURN_ON_ERROR(esp_wifi_connect(), TAG, "connect");

    bits = xEventGroupWaitBits(s_events, GOT_IP_BIT | DISCONNECTED_BIT, pdFALSE, pdFALSE,
                               pdMS_TO_TICKS(CONFIG_UPLINK_CONNECT_TIMEOUT_S * 1000));
    if (bits & GOT_IP_BIT)
        return ESP_OK;

    ESP_LOGW(TAG, "connect failed");
    return ESP_FAIL;
}

/* Runs in the TCP/IP task */
static esp_err_t dhcp_release_cb(void *ctx)
{
    dhcp_release_and_stop(esp_netif_get_netif_impl(s_sta));
    return ESP_OK;
}

/* ESP-IDF only stops the DHCP client on disconnect, it does not release the
 * lease */
static void release_lease(void)
{
    if (!(xEventGroupGetBits(s_events) & GOT_IP_BIT))
        return;

    ESP_LOGI(TAG, "releasing DHCP lease");
    esp_netif_tcpip_exec(dhcp_release_cb, NULL);
    vTaskDelay(pdMS_TO_TICKS(DHCP_RELEASE_WAIT_MS));
}

static void disconnect(void)
{
    release_lease();
    xEventGroupClearBits(s_events, DISCONNECTED_BIT);
    if (esp_wifi_disconnect() != ESP_OK)
        return;
    xEventGroupWaitBits(s_events, DISCONNECTED_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(3000));
}

/* Before connecting, so that the DHCP client takes the NTP server from
 * option 42 into slot 0 */
static esp_err_t ntp_init(void)
{
    esp_sntp_config_t cfg = {
        .server_from_dhcp = NTP_SERVER_FROM_DHCP,
        .wait_for_sync = true,
        .sync_cb = clock_ntp_cb,
    };

    /* Drop the servers of the previous sync */
    for (int i = 0; i < CONFIG_LWIP_SNTP_MAX_SERVERS; i++)
        esp_sntp_setserver(i, NULL);

    return esp_netif_sntp_init(&cfg);
}

static void log_ntp_servers(void)
{
    char buf[128] = "";
    size_t len = 0;

    for (int i = 0; i < CONFIG_LWIP_SNTP_MAX_SERVERS; i++) {
        const char *name = esp_sntp_getservername(i);
        const ip_addr_t *addr = esp_sntp_getserver(i);

        if (name)
            len += snprintf(buf + len, sizeof(buf) - len, " %s", name);
        else if (!ip_addr_isany(addr))
            len += snprintf(buf + len, sizeof(buf) - len, " %s (DHCP)", ipaddr_ntoa(addr));
        if (len >= sizeof(buf))
            break;
    }
    ESP_LOGI(TAG, "NTP servers:%s", buf);
}

static esp_err_t sync_ntp(void)
{
    /* Slot 0 holds the server from DHCP, if any */
    int idx = !ip_addr_isany(esp_sntp_getserver(0));
    esp_err_t err;

    esp_sntp_setservername(idx++, NTP_SERVER_1);
    if (NTP_SERVER_2[0])
        esp_sntp_setservername(idx, NTP_SERVER_2);
    log_ntp_servers();

    ESP_RETURN_ON_ERROR(esp_netif_sntp_start(), TAG, "sntp start");
    err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(CONFIG_NTP_TIMEOUT_S * 1000));
    if (err != ESP_OK)
        ESP_LOGW(TAG, "NTP sync failed: %s", esp_err_to_name(err));
    return err;
}

static esp_err_t sync_via(const struct uplink *up)
{
    esp_err_t err;

    ESP_RETURN_ON_ERROR(ntp_init(), TAG, "sntp init");
    err = connect(up);
    if (err == ESP_OK)
        err = sync_ntp();
    esp_netif_sntp_deinit();
    disconnect();

    return err;
}

esp_err_t uplink_sync_time(void)
{
    esp_err_t err = ESP_FAIL;

    /* In the configured order until one sync succeeds */
    for (size_t i = 0; i < sizeof(s_uplinks) / sizeof(s_uplinks[0]); i++) {
        err = sync_via(&s_uplinks[i]);
        if (err == ESP_OK)
            break;
    }

    return err;
}
