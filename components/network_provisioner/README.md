# Network Provisioner

Controller-side client for [espressif/network_provisioning](https://components.espressif.com/components/espressif/network_provisioning).

| Component | Role |
|-----------|------|
| **network_provisioning** | Device **server** (receive credentials) |
| **protocomm_ext** | Controller **pipe** (transport + Sec0/1/2) |
| **network_provisioner** (this) | Controller **business** (`prov-config` / `prov-scan` / `prov-ctrl`) |

## Capability matrix

| Link \\ Credential | Wi-Fi | Thread |
|--------------------|:----:|:------:|
| SoftAP + HTTP | ✓ | — |
| BLE | ✓ | ✓ |
| UART Console | debug | debug |

## Typical flow

Runtime security discovery (recommended):

```c
#include "network_provisioner.h"
#include "protocomm_ext.h"
#include "protocomm_ext_security.h"

protocomm_ext_t *pc = protocomm_ext_new(&pe_cfg); /* transport + Sec0 placeholder */
network_provisioner_t *np = network_provisioner_create(pc);

network_provisioner_fetch_capabilities(np, /* BLE: &ble_addr, else NULL */ NULL);
network_provisioner_capabilities_t caps = {0};
network_provisioner_get_capabilities(np, &caps);
/* protocomm_ext_set_security(pc, ...) from caps.sec_ver + PoP / Sec2 creds */
network_provisioner_establish_security(np);

network_provisioner_wifi_creds_t creds = {
    .ssid = (const uint8_t *)"myssid",
    .ssid_len = 6,
    .passphrase = (const uint8_t *)"mypass",
    .passphrase_len = 6,
};
network_provisioner_provision_wifi(np, &creds, NULL);

network_provisioner_stop_session(np);
network_provisioner_delete(np);
protocomm_ext_delete(pc);
```

If the peer scheme is already known, `network_provisioner_start_session()` still
covers open → proto-ver → handshake in one call.

**Thread safety:** APIs on one instance are not thread-safe; serialize from a single task.

## Dependency

```yaml
dependencies:
  espressif/network_provisioner: "^1.0.0"
```

Or develop in-tree with `protocomm_ext` via `override_path`.

## Tests

```bash
cd components/network_provisioner/test_apps
idf.py set-target esp32 build
```

## Example

See [`examples/network_provisioner`](../../examples/network_provisioner). Pair with the official device examples `wifi_prov` / `thread_prov` from `network_provisioning`.

**Supported targets:** ESP32 / ESP32-S3 / ESP32-C2 / ESP32-C3 / ESP32-C5 / ESP32-C6 / ESP32-C61 / ESP32-H2

## License

Apache-2.0. See `license.txt`.
