/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <esp_err.h>
#include <protocomm_ext.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Endpoint names matching espressif/network_provisioning (device side). */
#define NETWORK_PROVISIONER_EP_PROTO_VER   "proto-ver"
#define NETWORK_PROVISIONER_EP_SESSION     "prov-session"
#define NETWORK_PROVISIONER_EP_CONFIG      "prov-config"
#define NETWORK_PROVISIONER_EP_SCAN        "prov-scan"
#define NETWORK_PROVISIONER_EP_CTRL        "prov-ctrl"

/**
 * BLE characteristic UUID16 values used by network_provisioning / wifi_provisioning
 * (see wifi_prov manager set_config_endpoint). Register via
 * protocomm_ext_set_config_endpoint() before BLE sessions.
 */
#define NETWORK_PROVISIONER_EP_CTRL_UUID       0xFF4F
#define NETWORK_PROVISIONER_EP_SCAN_UUID       0xFF50
#define NETWORK_PROVISIONER_EP_SESSION_UUID    0xFF51
#define NETWORK_PROVISIONER_EP_CONFIG_UUID     0xFF52
#define NETWORK_PROVISIONER_EP_PROTO_VER_UUID  0xFF53

/**
 * Upper bound on Wi-Fi / Thread scan result entries accepted from a peer
 * (request `count` and reported `result_count`).
 */
#define NETWORK_PROVISIONER_MAX_SCAN_RESULTS  64

typedef struct network_provisioner network_provisioner_t;

typedef struct {
    bool wifi_prov;
    bool wifi_scan;
    bool thread_prov;
    bool thread_scan;
    bool no_sec;
    bool no_pop;
    /**
     * Security scheme from proto-ver JSON (`prov.sec_ver`, 0 / 1 / 2).
     * Required when a `prov` object is present; bare version strings fall back to 1.
     */
    int sec_ver;
} network_provisioner_capabilities_t;

typedef enum {
    NETWORK_PROVISIONER_WIFI_STATE_CONNECTED = 0,
    NETWORK_PROVISIONER_WIFI_STATE_CONNECTING,
    NETWORK_PROVISIONER_WIFI_STATE_DISCONNECTED,
    NETWORK_PROVISIONER_WIFI_STATE_CONNECTION_FAILED,
} network_provisioner_wifi_state_t;

typedef enum {
    NETWORK_PROVISIONER_WIFI_FAIL_AUTH_ERROR = 0,
    NETWORK_PROVISIONER_WIFI_FAIL_NETWORK_NOT_FOUND,
    NETWORK_PROVISIONER_WIFI_FAIL_UNKNOWN = -1,
} network_provisioner_wifi_fail_reason_t;

typedef enum {
    NETWORK_PROVISIONER_THREAD_STATE_ATTACHED = 0,
    NETWORK_PROVISIONER_THREAD_STATE_ATTACHING,
    NETWORK_PROVISIONER_THREAD_STATE_DETACHED,
    NETWORK_PROVISIONER_THREAD_STATE_ATTACHING_FAILED,
} network_provisioner_thread_state_t;

typedef enum {
    NETWORK_PROVISIONER_THREAD_FAIL_DATASET_INVALID = 0,
    NETWORK_PROVISIONER_THREAD_FAIL_NETWORK_NOT_FOUND,
    NETWORK_PROVISIONER_THREAD_FAIL_UNKNOWN = -1,
} network_provisioner_thread_fail_reason_t;

typedef struct {
    network_provisioner_wifi_state_t state;
    network_provisioner_wifi_fail_reason_t fail_reason;
    char ip4_addr[16];
    char ssid[33];
    int32_t channel;
} network_provisioner_wifi_status_t;

typedef struct {
    network_provisioner_thread_state_t state;
    network_provisioner_thread_fail_reason_t fail_reason;
    uint32_t pan_id;
    uint32_t channel;
    char name[33];
} network_provisioner_thread_status_t;

typedef struct {
    uint8_t ssid[33];
    uint8_t ssid_len;
    uint8_t bssid[6];
    uint32_t channel;
    int32_t rssi;
    int auth_mode;
} network_provisioner_wifi_ap_t;

typedef struct {
    uint32_t pan_id;
    uint32_t channel;
    int32_t rssi;
    uint32_t lqi;
    char network_name[33];
    uint8_t ext_pan_id[8];
    uint8_t ext_addr[8];
} network_provisioner_thread_network_t;

typedef struct {
    const uint8_t *ssid;
    size_t ssid_len;
    const uint8_t *passphrase;
    size_t passphrase_len;
    const uint8_t *bssid;   /**< optional, 6 bytes or NULL */
    int32_t channel;        /**< 0 = any */
    uint32_t poll_timeout_ms;
    uint32_t poll_interval_ms;
} network_provisioner_wifi_creds_t;

typedef struct {
    const uint8_t *dataset;
    size_t dataset_len;
    uint32_t poll_timeout_ms;
    uint32_t poll_interval_ms;
} network_provisioner_thread_creds_t;

/**
 * @brief Wrap an existing protocomm_ext instance (session may not be open yet)
 *
 * Ownership of @p pc remains with the caller; call network_provisioner_delete()
 * before protocomm_ext_delete().
 *
 * Registers BLE endpoint UUID mappings (`NETWORK_PROVISIONER_EP_*_UUID`) via
 * `protocomm_ext_set_config_endpoint()` (no-op for HTTP / Console).
 *
 * @note APIs on a given instance are not thread-safe. Use one task, or
 *       serialize externally. Do not call network_provisioner_delete() or
 *       network_provisioner_stop_session() while another call is in progress.
 */
network_provisioner_t *network_provisioner_create(protocomm_ext_t *pc);

void network_provisioner_delete(network_provisioner_t *np);

/**
 * @brief open_session → get_version(proto-ver) → establish_security(prov-session)
 *
 * Requires the protocomm_ext instance to already use the peer's security scheme
 * (Sec0/1/2) and credentials.
 *
 * For runtime discovery from proto-ver (recommended):
 *   1. network_provisioner_fetch_capabilities()
 *   2. protocomm_ext_set_security() with peer sec_ver / PoP or Sec2 creds
 *   3. network_provisioner_establish_security()
 *
 * @param np Network provisioner instance
 * @param transport_connect_cfg HTTP: NULL; BLE: ble_addr_t *; Console: NULL
 */
esp_err_t network_provisioner_start_session(network_provisioner_t *np,
                                            const void *transport_connect_cfg);

/**
 * @brief open_session + read proto-ver (plaintext); does not run the security handshake
 *
 * Stores capabilities for network_provisioner_get_capabilities().
 */
esp_err_t network_provisioner_fetch_capabilities(network_provisioner_t *np,
                                                 const void *transport_connect_cfg);

/**
 * @brief security_init + establish_security after credentials are configured
 */
esp_err_t network_provisioner_establish_security(network_provisioner_t *np);

esp_err_t network_provisioner_stop_session(network_provisioner_t *np);

esp_err_t network_provisioner_get_capabilities(network_provisioner_t *np,
                                               network_provisioner_capabilities_t *caps);

/* ---- low-level config ---- */
esp_err_t network_provisioner_wifi_set_config(network_provisioner_t *np,
                                              const uint8_t *ssid, size_t ssid_len,
                                              const uint8_t *passphrase, size_t passphrase_len,
                                              const uint8_t *bssid, int32_t channel);
esp_err_t network_provisioner_wifi_apply_config(network_provisioner_t *np);
esp_err_t network_provisioner_wifi_get_status(network_provisioner_t *np,
                                              network_provisioner_wifi_status_t *status);

esp_err_t network_provisioner_thread_set_config(network_provisioner_t *np,
                                                const uint8_t *dataset, size_t dataset_len);
esp_err_t network_provisioner_thread_apply_config(network_provisioner_t *np);
esp_err_t network_provisioner_thread_get_status(network_provisioner_t *np,
                                                network_provisioner_thread_status_t *status);

/* ---- scan ---- */
esp_err_t network_provisioner_wifi_scan_start(network_provisioner_t *np, bool blocking,
                                              bool passive, uint32_t group_channels,
                                              uint32_t period_ms);
esp_err_t network_provisioner_wifi_scan_status(network_provisioner_t *np,
                                               bool *finished, uint32_t *result_count);
esp_err_t network_provisioner_wifi_scan_result(network_provisioner_t *np,
                                               uint32_t start_index, uint32_t count,
                                               network_provisioner_wifi_ap_t *out,
                                               uint32_t *out_count);

esp_err_t network_provisioner_thread_scan_start(network_provisioner_t *np, bool blocking,
                                                uint32_t channel_mask);
esp_err_t network_provisioner_thread_scan_status(network_provisioner_t *np,
                                                 bool *finished, uint32_t *result_count);
esp_err_t network_provisioner_thread_scan_result(network_provisioner_t *np,
                                                 uint32_t start_index, uint32_t count,
                                                 network_provisioner_thread_network_t *out,
                                                 uint32_t *out_count);

/* ---- ctrl ---- */
esp_err_t network_provisioner_wifi_reset(network_provisioner_t *np);
esp_err_t network_provisioner_wifi_reprov(network_provisioner_t *np);
esp_err_t network_provisioner_thread_reset(network_provisioner_t *np);
esp_err_t network_provisioner_thread_reprov(network_provisioner_t *np);

/* ---- high-level ---- */
/**
 * @brief Set + Apply + poll until Connected or fail/timeout
 */
esp_err_t network_provisioner_provision_wifi(network_provisioner_t *np,
                                             const network_provisioner_wifi_creds_t *creds,
                                             network_provisioner_wifi_status_t *out_status);

esp_err_t network_provisioner_provision_thread(network_provisioner_t *np,
                                               const network_provisioner_thread_creds_t *creds,
                                               network_provisioner_thread_status_t *out_status);

protocomm_ext_t *network_provisioner_get_protocomm(network_provisioner_t *np);

#ifdef __cplusplus
}
#endif
