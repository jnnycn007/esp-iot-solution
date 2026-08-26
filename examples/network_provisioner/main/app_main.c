/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Unified controller example: SoftAP/HTTP Wi-Fi, BLE Wi-Fi, and BLE Thread
 * can all be compiled in; choose one or more paths at runtime (console menu).
 * Pair with espressif/network_provisioning device examples.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_http_client.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "protocomm_ext.h"
#include "protocomm_ext_security.h"
#include "network_provisioner.h"

#if CONFIG_EXAMPLE_SUPPORT_BLE_WIFI || CONFIG_EXAMPLE_SUPPORT_BLE_THREAD
#include "protocomm_ext_nimble.h"
#include "host/ble_gap.h"
#endif

static const char *TAG = "np_example";

typedef enum {
    PROV_MODE_SOFTAP_WIFI = 1,
    PROV_MODE_BLE_WIFI = 2,
    PROV_MODE_BLE_THREAD = 3,
} prov_mode_t;

static int read_console_int(const char *prompt)
{
    /* Print prompt once; UART often returns EOF / empty when idle — do not spam. */
    printf("%s", prompt);
    fflush(stdout);

    char line[32];
    int len = 0;

    while (1) {
        int c = getchar();
        if (c == EOF) {
            clearerr(stdin);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (c == '\r' || c == '\n') {
            if (len == 0) {
                continue;
            }
            line[len] = '\0';
            return atoi(line);
        }
        if (len < (int)sizeof(line) - 1) {
            line[len++] = (char)c;
        }
    }
}

/**
 * Read a line into @p buf. If @p allow_empty, bare Enter returns empty string.
 * If @p echo is false, printable input is masked with '*'.
 * If @p default_val is non-NULL and user presses Enter with empty input,
 * copies default_val into buf (when allow_empty is false this still applies).
 */
static esp_err_t read_console_line_default(const char *prompt, char *buf, size_t buf_len,
                                           const char *default_val, bool allow_empty, bool echo)
{
    if (!buf || buf_len < 2) {
        return ESP_ERR_INVALID_ARG;
    }
    printf("%s", prompt);
    fflush(stdout);

    size_t len = 0;
    while (1) {
        int c = getchar();
        if (c == EOF) {
            clearerr(stdin);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (c == '\r' || c == '\n') {
            if (len == 0) {
                if (default_val && default_val[0]) {
                    strncpy(buf, default_val, buf_len - 1);
                    buf[buf_len - 1] = '\0';
                    printf("(default)\n");
                    return ESP_OK;
                }
                if (!allow_empty) {
                    continue;
                }
            }
            buf[len] = '\0';
            printf("\n");
            return ESP_OK;
        }
        if (c == 0x08 || c == 0x7f) { /* backspace */
            if (len > 0) {
                len--;
                printf("\b \b");
                fflush(stdout);
            }
            continue;
        }
        if (c >= 32 && c < 127 && len + 1 < buf_len) {
            buf[len++] = (char)c;
            putchar(echo ? c : '*');
            fflush(stdout);
        }
    }
}

static esp_err_t read_console_secret(const char *prompt, char *buf, size_t buf_len,
                                     const char *default_val, bool allow_empty)
{
    return read_console_line_default(prompt, buf, buf_len, default_val, allow_empty, false);
}

#if CONFIG_EXAMPLE_SUPPORT_SOFTAP_WIFI
static esp_err_t softap_disconnect(void);
static esp_err_t softap_reconnect(void);
#endif

/* Buffers must outlive protocomm_ext_set_security() (params are copied internally). */
static char s_sec1_pop[64];
static char s_sec2_user[64];
static char s_sec2_pass[64];
static protocomm_ext_security1_params_t s_sec1_params;
static protocomm_ext_security2_params_t s_sec2_params;

/**
 * Configure protocomm security from peer proto-ver capabilities.
 * SoftAP + Sec2: drop SoftAP, finish SRP keygen offline, then rejoin.
 */
static esp_err_t configure_security_from_caps(protocomm_ext_t *pc,
                                              const network_provisioner_capabilities_t *caps,
                                              bool softap_transport)
{
    int sec_ver = caps->sec_ver;
    if (sec_ver < 0 || sec_ver > 2) {
        ESP_LOGE(TAG, "Unsupported sec_ver=%d", sec_ver);
        return ESP_ERR_NOT_SUPPORTED;
    }

    printf("\nPeer security: Sec%d", sec_ver);
    if (caps->no_pop) {
        printf(" (no_pop)");
    }
    printf("\n");

    esp_err_t err = ESP_OK;
    const void *sec_data = NULL;
    protocomm_ext_security_method_t method = PROTOCOMM_EXT_SECURITY_METHOD_NONE;

    if (sec_ver == 0) {
        method = PROTOCOMM_EXT_SECURITY_METHOD_NONE;
        printf("Sec0: no credentials required.\n");
    } else if (sec_ver == 1) {
        method = PROTOCOMM_EXT_SECURITY_METHOD_SECURITY_1;
        if (caps->no_pop) {
            printf("Sec1: PoP not required (no_pop).\n");
            sec_data = NULL;
        } else {
            ESP_RETURN_ON_ERROR(read_console_secret("Enter Proof of Possession (Enter=default): ",
                                                    s_sec1_pop, sizeof(s_sec1_pop),
                                                    CONFIG_EXAMPLE_PROV_POP, false),
                                TAG, "read pop");
            s_sec1_params.data = (const uint8_t *)s_sec1_pop;
            s_sec1_params.len = strlen(s_sec1_pop);
            sec_data = &s_sec1_params;
        }
    } else {
        method = PROTOCOMM_EXT_SECURITY_METHOD_SECURITY_2;
        ESP_RETURN_ON_ERROR(read_console_line_default(
                                "Enter Sec2 username (Enter=default): ",
                                s_sec2_user, sizeof(s_sec2_user),
                                CONFIG_EXAMPLE_PROV_SEC2_USERNAME, false, true),
                            TAG, "read username");
        ESP_RETURN_ON_ERROR(read_console_secret("Enter Sec2 password (Enter=default): ",
                                                s_sec2_pass, sizeof(s_sec2_pass),
                                                CONFIG_EXAMPLE_PROV_SEC2_PASSWORD, false),
                            TAG, "read password");
        s_sec2_params.username = s_sec2_user;
        s_sec2_params.username_len = strlen(s_sec2_user);
        s_sec2_params.password = s_sec2_pass;
        s_sec2_params.password_len = strlen(s_sec2_pass);
        sec_data = &s_sec2_params;
    }

#if CONFIG_EXAMPLE_SUPPORT_SOFTAP_WIFI
    /* Sec2 SRP keygen is slow: do it offline so SoftAP idle-kick / HTTP 408 is avoided. */
    if (softap_transport && sec_ver == 2) {
        ESP_LOGI(TAG, "SoftAP+Sec2: disconnect SoftAP, security_init offline, then rejoin");
        protocomm_ext_close_session(pc);
        ESP_RETURN_ON_ERROR(softap_disconnect(), TAG, "softap disconnect");
        ESP_RETURN_ON_ERROR(protocomm_ext_set_security(pc, method, sec_data), TAG, "set_security");
        ESP_RETURN_ON_ERROR(protocomm_ext_security_init(pc), TAG, "security_init");
        ESP_RETURN_ON_ERROR(softap_reconnect(), TAG, "softap rejoin");
        return ESP_OK;
    }
#else
    (void)softap_transport;
#endif

    err = protocomm_ext_set_security(pc, method, sec_data);
    if (err != ESP_OK) {
        return err;
    }
    return ESP_OK;
}

static void clear_security_credentials(void)
{
    memset(s_sec1_pop, 0, sizeof(s_sec1_pop));
    memset(s_sec2_user, 0, sizeof(s_sec2_user));
    memset(s_sec2_pass, 0, sizeof(s_sec2_pass));
    memset(&s_sec1_params, 0, sizeof(s_sec1_params));
    memset(&s_sec2_params, 0, sizeof(s_sec2_params));
}

#if CONFIG_EXAMPLE_SUPPORT_SOFTAP_WIFI
#define WIFI_CONNECTED_BIT    BIT0
#define WIFI_DISCONNECTED_BIT BIT1
#define SOFTAP_SCAN_MAX       32

static EventGroupHandle_t s_wifi_events;
static bool s_wifi_inited;
static bool s_wifi_want_connect;
static char s_selected_softap_ssid[33];

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;
    /* Do not auto-connect on STA_START — that races with SoftAP scanning. */
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupSetBits(s_wifi_events, WIFI_DISCONNECTED_BIT);
        if (s_wifi_want_connect) {
            esp_wifi_connect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t wifi_sta_ensure_inited(void)
{
    if (!s_wifi_events) {
        s_wifi_events = xEventGroupCreate();
        ESP_RETURN_ON_FALSE(s_wifi_events, ESP_ERR_NO_MEM, TAG, "event group");
    }
    if (s_wifi_inited) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL),
                        TAG, "wifi handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL),
                        TAG, "ip handler");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set mode");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");
    s_wifi_inited = true;
    return ESP_OK;
}

static esp_err_t softap_disconnect(void)
{
    if (!s_wifi_inited || !s_wifi_events) {
        return ESP_OK;
    }
    s_wifi_want_connect = false;
    xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_DISCONNECTED_BIT);
    esp_err_t err = esp_wifi_disconnect();
    if (err == ESP_ERR_WIFI_NOT_CONNECT) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_disconnect: %s", esp_err_to_name(err));
        return err;
    }
    EventBits_t bits = xEventGroupWaitBits(s_wifi_events, WIFI_DISCONNECTED_BIT, pdTRUE, pdTRUE,
                                           pdMS_TO_TICKS(5000));
    if (!(bits & WIFI_DISCONNECTED_BIT)) {
        ESP_LOGE(TAG, "SoftAP disconnect timeout");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static void softap_cleanup(void)
{
    (void)softap_disconnect();
}

static esp_err_t softap_reconnect(void)
{
    if (s_selected_softap_ssid[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(wifi_sta_ensure_inited(), TAG, "wifi init");

    xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
    wifi_config_t wifi_cfg = {0};
    strncpy((char *)wifi_cfg.sta.ssid, s_selected_softap_ssid, sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char *)wifi_cfg.sta.password, CONFIG_EXAMPLE_PROV_SOFTAP_PASSWORD,
            sizeof(wifi_cfg.sta.password) - 1);

    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg), TAG, "set config");
    s_wifi_want_connect = true;
    ESP_RETURN_ON_ERROR(esp_wifi_connect(), TAG, "wifi connect");

    EventBits_t bits = xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(30000));
    if (!(bits & WIFI_CONNECTED_BIT)) {
        s_wifi_want_connect = false;
        esp_wifi_disconnect();
        ESP_LOGE(TAG, "SoftAP rejoin timeout");
        return ESP_ERR_TIMEOUT;
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_inactive_time(WIFI_IF_STA, 60));
    ESP_LOGI(TAG, "Rejoined SoftAP '%s'", s_selected_softap_ssid);
    return ESP_OK;
}

static esp_err_t softap_pick_and_connect(void)
{
    ESP_RETURN_ON_ERROR(wifi_sta_ensure_inited(), TAG, "wifi init");

    /* Ensure STA is idle before scan (connect-in-progress rejects scan). */
    s_wifi_want_connect = false;
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(100));

    wifi_scan_config_t scan_cfg = {
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };
    ESP_LOGI(TAG, "Scanning SoftAP SSIDs (prefix hint '%s')...", CONFIG_EXAMPLE_PROV_NAME_PREFIX);
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_scan_start: %s", esp_err_to_name(err));
        return err;
    }

    uint16_t ap_num = 0;
    ESP_RETURN_ON_ERROR(esp_wifi_scan_get_ap_num(&ap_num), TAG, "get ap num");
    if (ap_num == 0) {
        ESP_LOGE(TAG, "No Wi-Fi APs found");
        return ESP_ERR_NOT_FOUND;
    }
    if (ap_num > SOFTAP_SCAN_MAX) {
        ap_num = SOFTAP_SCAN_MAX;
    }

    wifi_ap_record_t *aps = calloc(ap_num, sizeof(*aps));
    ESP_RETURN_ON_FALSE(aps, ESP_ERR_NO_MEM, TAG, "ap list");
    err = esp_wifi_scan_get_ap_records(&ap_num, aps);
    if (err != ESP_OK) {
        free(aps);
        ESP_LOGE(TAG, "esp_wifi_scan_get_ap_records: %s", esp_err_to_name(err));
        return err;
    }

    const char *prefix = CONFIG_EXAMPLE_PROV_NAME_PREFIX;
    size_t prefix_len = strlen(prefix);

    printf("\n=== SoftAP scan results (prefix '%s') ===\n", prefix);
    int listed = 0;
    int map_idx[SOFTAP_SCAN_MAX];
    for (int i = 0; i < (int)ap_num; i++) {
        if (aps[i].ssid[0] == '\0') {
            continue;
        }
        bool match = prefix_len == 0 ||
                     (strlen((const char *)aps[i].ssid) >= prefix_len &&
                      strncmp((const char *)aps[i].ssid, prefix, prefix_len) == 0);
        if (!match) {
            continue;
        }
        map_idx[listed] = i;
        printf("  %d) %s  rssi=%d\n", listed + 1, aps[i].ssid, aps[i].rssi);
        listed++;
    }

    if (listed == 0) {
        free(aps);
        ESP_LOGE(TAG, "No SoftAP SSIDs matching prefix '%s'", prefix);
        return ESP_ERR_NOT_FOUND;
    }

    int choice = 1;
    if (listed > 1) {
        choice = read_console_int("Select SoftAP (0=cancel): ");
        if (choice <= 0 || choice > listed) {
            free(aps);
            return ESP_ERR_INVALID_ARG;
        }
    } else {
        printf("Only one match, auto-selecting.\n");
    }

    const wifi_ap_record_t *sel = &aps[map_idx[choice - 1]];
    strncpy(s_selected_softap_ssid, (const char *)sel->ssid, sizeof(s_selected_softap_ssid) - 1);
    s_selected_softap_ssid[sizeof(s_selected_softap_ssid) - 1] = '\0';
    free(aps);

    xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);

    wifi_config_t wifi_cfg = {0};
    strncpy((char *)wifi_cfg.sta.ssid, s_selected_softap_ssid, sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char *)wifi_cfg.sta.password, CONFIG_EXAMPLE_PROV_SOFTAP_PASSWORD,
            sizeof(wifi_cfg.sta.password) - 1);

    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg), TAG, "set config");
    s_wifi_want_connect = true;
    ESP_RETURN_ON_ERROR(esp_wifi_connect(), TAG, "wifi connect");

    EventBits_t bits = xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(30000));
    if (!(bits & WIFI_CONNECTED_BIT)) {
        s_wifi_want_connect = false;
        esp_wifi_disconnect();
        ESP_LOGE(TAG, "SoftAP join timeout");
        return ESP_ERR_TIMEOUT;
    }

    /* SoftAP provisioning: disable STA power-save so SoftAP does not idle-kick us
     * while Sec2 SRP runs on either side. */
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_inactive_time(WIFI_IF_STA, 60));

    ESP_LOGI(TAG, "Joined device SoftAP '%s' (PS_NONE)", s_selected_softap_ssid);
    return ESP_OK;
}
#endif /* CONFIG_EXAMPLE_SUPPORT_SOFTAP_WIFI */

#if CONFIG_EXAMPLE_SUPPORT_BLE_WIFI || CONFIG_EXAMPLE_SUPPORT_BLE_THREAD
#define BLE_SCAN_SELECT_MAX 32

typedef struct {
    ble_addr_t addr;
    char name[32];
} ble_peer_choice_t;

static esp_err_t ble_pick_device(protocomm_ext_t *pc, ble_addr_t *out_addr)
{
    protocomm_ext_transport_handle_t th = protocomm_ext_get_transport_handle(pc);
    ESP_RETURN_ON_FALSE(th, ESP_ERR_INVALID_STATE, TAG, "no transport handle");

    protocomm_ext_nimble_scan_config_t scan_cfg = {
        .scan_timeout_ms = 10000,
    };
    ESP_LOGI(TAG, "Scanning BLE (prefix '%s')...", CONFIG_EXAMPLE_PROV_NAME_PREFIX);
    ESP_RETURN_ON_ERROR(esp_protocomm_ext_nimble_start_scan(th, &scan_cfg), TAG, "scan start");
    esp_protocomm_ext_nimble_stop_scan(th);

    int count = esp_protocomm_ext_nimble_get_scanned_device_count(th);
    const char *prefix = CONFIG_EXAMPLE_PROV_NAME_PREFIX;
    size_t prefix_len = strlen(prefix);

    ble_peer_choice_t choices[BLE_SCAN_SELECT_MAX];
    int listed = 0;

    printf("\n=== BLE scan results (prefix '%s') ===\n", prefix);
    for (int i = 0; i < count && listed < BLE_SCAN_SELECT_MAX; i++) {
        protocomm_ext_nimble_scanned_device_info_t info = {0};
        if (esp_protocomm_ext_nimble_get_scanned_device_info(th, i, &info) != ESP_OK) {
            continue;
        }
        if (!info.name || info.name_len == 0) {
            free(info.name);
            free(info.mfg_data);
            continue;
        }

        bool match = prefix_len == 0 ||
                     (info.name_len >= prefix_len && memcmp(info.name, prefix, prefix_len) == 0);
        if (!match) {
            free(info.name);
            free(info.mfg_data);
            continue;
        }

        size_t copy_len = info.name_len < sizeof(choices[listed].name) - 1 ?
                          info.name_len : sizeof(choices[listed].name) - 1;
        memcpy(choices[listed].name, info.name, copy_len);
        choices[listed].name[copy_len] = '\0';
        choices[listed].addr = info.addr;

        printf("  %d) %s  %02x:%02x:%02x:%02x:%02x:%02x\n",
               listed + 1, choices[listed].name,
               info.addr.val[5], info.addr.val[4], info.addr.val[3],
               info.addr.val[2], info.addr.val[1], info.addr.val[0]);
        listed++;

        free(info.name);
        free(info.mfg_data);
    }

    if (listed == 0) {
        ESP_LOGE(TAG, "No BLE devices matching prefix '%s'", prefix);
        return ESP_ERR_NOT_FOUND;
    }

    int choice = 1;
    if (listed > 1) {
        choice = read_console_int("Select BLE device (0=cancel): ");
        if (choice <= 0 || choice > listed) {
            return ESP_ERR_INVALID_ARG;
        }
    } else {
        printf("Only one match, auto-selecting '%s'.\n", choices[0].name);
    }

    *out_addr = choices[choice - 1].addr;
    ESP_LOGI(TAG, "Selected BLE peer '%s'", choices[choice - 1].name);
    return ESP_OK;
}
#endif

#if CONFIG_EXAMPLE_SUPPORT_BLE_THREAD
static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static esp_err_t parse_hex(const char *hex, uint8_t **out, size_t *out_len)
{
    size_t n = strlen(hex);
    if (n == 0 || (n % 2) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t len = n / 2;
    uint8_t *buf = calloc(1, len);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    for (size_t i = 0; i < len; i++) {
        int hi = hex_nibble(hex[2 * i]);
        int lo = hex_nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            free(buf);
            return ESP_ERR_INVALID_ARG;
        }
        buf[i] = (uint8_t)((hi << 4) | lo);
    }
    *out = buf;
    *out_len = len;
    return ESP_OK;
}
#endif

static esp_err_t run_provision_wifi(network_provisioner_t *np, bool softap_transport)
{
#if CONFIG_EXAMPLE_SUPPORT_SOFTAP_WIFI || CONFIG_EXAMPLE_SUPPORT_BLE_WIFI
#define WIFI_SCAN_LIST_MAX 32
#define WIFI_SCAN_BATCH    8

    network_provisioner_capabilities_t caps = {0};
    if (network_provisioner_get_capabilities(np, &caps) != ESP_OK || !caps.wifi_scan) {
        ESP_LOGE(TAG, "Peer does not advertise wifi_scan");
        return ESP_ERR_NOT_SUPPORTED;
    }

    /*
     * SoftAP: scan in channel groups so the SoftAP can still send beacons
     * (esp_prov uses group_channels=5). BLE can scan all channels at once.
     */
    uint8_t group_channels = softap_transport ? 5 : 0;
    ESP_LOGI(TAG, "Starting Wi-Fi scan on the peer device (group_channels=%u)...",
             (unsigned)group_channels);
    esp_err_t err = network_provisioner_wifi_scan_start(np, true, false, group_channels, 120);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi_scan_start: %s", esp_err_to_name(err));
        return err;
    }

    bool finished = false;
    uint32_t result_count = 0;
    for (int i = 0; i < 60; i++) {
        err = network_provisioner_wifi_scan_status(np, &finished, &result_count);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "wifi_scan_status: %s", esp_err_to_name(err));
            return err;
        }
        if (finished) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (!finished) {
        ESP_LOGE(TAG, "Peer Wi-Fi scan timeout");
        return ESP_ERR_TIMEOUT;
    }
    if (result_count == 0) {
        ESP_LOGE(TAG, "Peer found no Wi-Fi APs");
        return ESP_ERR_NOT_FOUND;
    }

    if (result_count > WIFI_SCAN_LIST_MAX) {
        result_count = WIFI_SCAN_LIST_MAX;
    }

    network_provisioner_wifi_ap_t *aps = calloc(result_count, sizeof(*aps));
    ESP_RETURN_ON_FALSE(aps, ESP_ERR_NO_MEM, TAG, "ap list");

    uint32_t got = 0;
    while (got < result_count) {
        uint32_t batch = result_count - got;
        if (batch > WIFI_SCAN_BATCH) {
            batch = WIFI_SCAN_BATCH;
        }
        uint32_t n = 0;
        err = network_provisioner_wifi_scan_result(np, got, batch, &aps[got], &n);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "wifi_scan_result: %s", esp_err_to_name(err));
            free(aps);
            return err;
        }
        if (n == 0) {
            break;
        }
        got += n;
    }

    printf("\n=== Wi-Fi APs seen by peer (%lu) ===\n", (unsigned long)got);
    int listed = 0;
    for (uint32_t i = 0; i < got; i++) {
        if (aps[i].ssid_len == 0) {
            continue;
        }
        char ssid_z[33] = {0};
        memcpy(ssid_z, aps[i].ssid, aps[i].ssid_len);
        printf("  %d) %-32s  rssi=%d  ch=%lu  auth=%d\n",
               listed + 1, ssid_z, (int)aps[i].rssi,
               (unsigned long)aps[i].channel, aps[i].auth_mode);
        /* Compact list index maps through packed order of non-empty SSIDs. */
        if ((uint32_t)listed != i) {
            aps[listed] = aps[i];
        }
        listed++;
    }

    if (listed == 0) {
        free(aps);
        ESP_LOGE(TAG, "No APs with an SSID");
        return ESP_ERR_NOT_FOUND;
    }

    int choice = read_console_int("Select Wi-Fi AP (0=cancel): ");
    if (choice <= 0 || choice > listed) {
        free(aps);
        return ESP_ERR_INVALID_ARG;
    }

    network_provisioner_wifi_ap_t sel = aps[choice - 1];
    free(aps);

    char ssid_z[33] = {0};
    memcpy(ssid_z, sel.ssid, sel.ssid_len);
    char password[65] = {0};
    printf("Selected SSID: %s\n", ssid_z);
    ESP_RETURN_ON_ERROR(read_console_secret("Enter Wi-Fi password (Enter=open): ",
                                            password, sizeof(password), NULL, true),
                        TAG, "password");

    static const uint8_t zero_bssid[6] = {0};
    const uint8_t *bssid_ptr = (memcmp(sel.bssid, zero_bssid, sizeof(zero_bssid)) == 0)
                               ? NULL : sel.bssid;

    network_provisioner_wifi_creds_t creds = {
        .ssid = sel.ssid,
        .ssid_len = sel.ssid_len,
        .passphrase = (const uint8_t *)password,
        .passphrase_len = strlen(password),
        .bssid = bssid_ptr,
        .channel = (int32_t)sel.channel,
        .poll_timeout_ms = 60000,
        .poll_interval_ms = 1000,
    };
    network_provisioner_wifi_status_t st = {0};
    err = network_provisioner_provision_wifi(np, &creds, &st);
    memset(password, 0, sizeof(password));
    ESP_LOGI(TAG, "provision_wifi -> %s state=%d ip=%s ssid=%s",
             esp_err_to_name(err), st.state, st.ip4_addr, st.ssid);
    return err;
#else
    (void)np;
    (void)softap_transport;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

static esp_err_t run_session_and_provision(protocomm_ext_config_data_t *pe_cfg,
                                           protocomm_ext_t *pc_in,
                                           const void *session_cfg,
                                           bool do_wifi, bool do_thread)
{
    protocomm_ext_t *pc = pc_in;
    bool pc_owned = false;
    if (!pc) {
        pc = protocomm_ext_new(pe_cfg);
        if (!pc) {
            ESP_LOGE(TAG, "protocomm_ext_new failed");
            return ESP_FAIL;
        }
        pc_owned = true;
    }

#if CONFIG_EXAMPLE_SUPPORT_BLE_WIFI || CONFIG_EXAMPLE_SUPPORT_BLE_THREAD
    static ble_addr_t peer_addr;
    if (pe_cfg->transport_method == PROTOCOMM_EXT_TRANSPORT_METHOD_BLE) {
        if (ble_pick_device(pc, &peer_addr) != ESP_OK) {
            ESP_LOGE(TAG, "BLE device selection failed");
            if (pc_owned) {
                protocomm_ext_delete(pc);
            }
            return ESP_ERR_NOT_FOUND;
        }
        session_cfg = &peer_addr;
    }
#else
    (void)session_cfg;
#endif

    network_provisioner_t *np = network_provisioner_create(pc);
    bool softap = (pe_cfg->transport_method == PROTOCOMM_EXT_TRANSPORT_METHOD_HTTP);
    if (!np) {
        ESP_LOGE(TAG, "network_provisioner_create failed");
        if (pc_owned) {
            protocomm_ext_delete(pc);
        }
#if CONFIG_EXAMPLE_SUPPORT_SOFTAP_WIFI
        if (softap) {
            softap_cleanup();
        }
#endif
        return ESP_FAIL;
    }

    esp_err_t err = ESP_OK;

    /* 1) Open transport + read proto-ver (plaintext) → learn peer sec_ver. */
    err = network_provisioner_fetch_capabilities(np, session_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "fetch_capabilities: %s", esp_err_to_name(err));
        goto cleanup;
    }

    network_provisioner_capabilities_t caps = {0};
    network_provisioner_get_capabilities(np, &caps);
    ESP_LOGI(TAG, "caps sec_ver=%d wifi_prov=%d thread_prov=%d",
             caps.sec_ver, caps.wifi_prov, caps.thread_prov);

    /* 2) Prompt for PoP / username+password and switch security scheme. */
    err = configure_security_from_caps(pc, &caps, softap);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "configure_security: %s", esp_err_to_name(err));
        goto cleanup;
    }

    /* SoftAP+Sec2 path closed the HTTP session for offline SRP; reopen. */
    if (softap && caps.sec_ver == 2) {
        err = protocomm_ext_open_session(pc, NULL);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "re-open_session: %s", esp_err_to_name(err));
            goto cleanup;
        }
    }

    /* 3) Security handshake. */
    err = network_provisioner_establish_security(np);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "establish_security: %s", esp_err_to_name(err));
        goto cleanup;
    }

    if (do_wifi) {
        if (!caps.wifi_prov) {
            ESP_LOGE(TAG, "Peer does not advertise wifi_prov");
            err = ESP_ERR_NOT_SUPPORTED;
        } else {
            err = run_provision_wifi(np, softap);
        }
    }

#if CONFIG_EXAMPLE_SUPPORT_BLE_THREAD
    if (do_thread && err == ESP_OK) {
        if (!caps.thread_prov) {
            ESP_LOGE(TAG, "Peer does not advertise thread_prov");
            err = ESP_ERR_NOT_SUPPORTED;
        } else {
            char dataset_hex[513] = {0};
            const char *def_hex = CONFIG_EXAMPLE_THREAD_DATASET_HEX;
            err = read_console_secret(
                      def_hex[0] ? "Enter Thread dataset hex (Enter=default): "
                      : "Enter Thread dataset hex (required): ",
                      dataset_hex, sizeof(dataset_hex),
                      def_hex[0] ? def_hex : NULL, false);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "read dataset hex failed");
            } else {
                uint8_t *dataset = NULL;
                size_t dataset_len = 0;
                err = parse_hex(dataset_hex, &dataset, &dataset_len);
                memset(dataset_hex, 0, sizeof(dataset_hex));
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "invalid Thread dataset hex");
                } else {
                    network_provisioner_thread_creds_t tcreds = {
                        .dataset = dataset,
                        .dataset_len = dataset_len,
                        .poll_timeout_ms = 60000,
                        .poll_interval_ms = 1000,
                    };
                    network_provisioner_thread_status_t tst = {0};
                    err = network_provisioner_provision_thread(np, &tcreds, &tst);
                    memset(dataset, 0, dataset_len);
                    free(dataset);
                    ESP_LOGI(TAG, "provision_thread -> %s state=%d name=%s",
                             esp_err_to_name(err), tst.state, tst.name);
                }
            }
        }
    }
#else
    (void)do_thread;
#endif

cleanup:
    network_provisioner_stop_session(np);
    network_provisioner_delete(np);
    /* Always delete pc: network_provisioner does not own it. */
    protocomm_ext_delete(pc);
    clear_security_credentials();
#if CONFIG_EXAMPLE_SUPPORT_SOFTAP_WIFI
    if (softap) {
        softap_cleanup();
    }
#else
    (void)softap;
#endif
    (void)pc_owned;
    return err;
}

static esp_err_t run_mode(prov_mode_t mode)
{
    /* Start with Sec0 placeholder; real scheme comes from peer proto-ver. */
    protocomm_ext_config_data_t pe_cfg = {
        .security_method = PROTOCOMM_EXT_SECURITY_METHOD_NONE,
        .security_data = NULL,
    };

    switch (mode) {
#if CONFIG_EXAMPLE_SUPPORT_SOFTAP_WIFI
    case PROV_MODE_SOFTAP_WIFI: {
        /* Sec2 SRP + SoftAP grouped Wi-Fi scan can exceed 10s; match esp_prov. */
        static esp_http_client_config_t http_cfg = {
            .url = CONFIG_EXAMPLE_PROV_HTTP_URL,
            .timeout_ms = 60000,
        };
        pe_cfg.transport_method = PROTOCOMM_EXT_TRANSPORT_METHOD_HTTP;
        pe_cfg.transport_data = &http_cfg;

        protocomm_ext_t *pc = protocomm_ext_new(&pe_cfg);
        if (!pc) {
            return ESP_FAIL;
        }

        esp_err_t err = softap_pick_and_connect();
        if (err != ESP_OK) {
            protocomm_ext_delete(pc);
            return err;
        }
        return run_session_and_provision(&pe_cfg, pc, NULL, true, false);
    }
#endif
#if CONFIG_EXAMPLE_SUPPORT_BLE_WIFI
    case PROV_MODE_BLE_WIFI:
        pe_cfg.transport_method = PROTOCOMM_EXT_TRANSPORT_METHOD_BLE;
        pe_cfg.transport_data = NULL;
        return run_session_and_provision(&pe_cfg, NULL, NULL, true, false);
#endif
#if CONFIG_EXAMPLE_SUPPORT_BLE_THREAD
    case PROV_MODE_BLE_THREAD:
        pe_cfg.transport_method = PROTOCOMM_EXT_TRANSPORT_METHOD_BLE;
        pe_cfg.transport_data = NULL;
        return run_session_and_provision(&pe_cfg, NULL, NULL, false, true);
#endif
    default:
        ESP_LOGE(TAG, "Mode %d not compiled in", (int)mode);
        return ESP_ERR_NOT_SUPPORTED;
    }
}

static int read_menu_choice(void)
{
    printf("\n=== network_provisioner example ===\n");
    printf("Device name prefix: %s\n", CONFIG_EXAMPLE_PROV_NAME_PREFIX);
#if CONFIG_EXAMPLE_SUPPORT_SOFTAP_WIFI
    printf("  1) SoftAP (HTTP) + Wi-Fi\n");
#endif
#if CONFIG_EXAMPLE_SUPPORT_BLE_WIFI
    printf("  2) BLE + Wi-Fi\n");
#endif
#if CONFIG_EXAMPLE_SUPPORT_BLE_THREAD
    printf("  3) BLE + Thread\n");
#endif
    printf("  0) Exit\n");
    return read_console_int("Select: ");
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

#if !CONFIG_EXAMPLE_SUPPORT_SOFTAP_WIFI && !CONFIG_EXAMPLE_SUPPORT_BLE_WIFI && !CONFIG_EXAMPLE_SUPPORT_BLE_THREAD
#error "Enable at least one of EXAMPLE_SUPPORT_SOFTAP_WIFI / BLE_WIFI / BLE_THREAD"
#endif

    while (1) {
        int choice = read_menu_choice();
        if (choice == 0) {
            ESP_LOGI(TAG, "exit");
            break;
        }
        if (choice < 0) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        esp_err_t err = run_mode((prov_mode_t)choice);
        ESP_LOGI(TAG, "mode %d finished: %s", choice, esp_err_to_name(err));
    }
}
