**Network Provisioner 配网控制器**
==================================

:link_to_translation:`en:[English]`

``network_provisioner`` 是面向
`espressif/network_provisioning <https://components.espressif.com/components/espressif/network_provisioning>`_
的 **控制器侧** 配网客户端。

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - 组件
     - 角色
   * - **network_provisioning**
     - 设备端 **Server** ：接收 Wi-Fi / Thread 凭证
   * - **protocomm_ext**
     - 控制器 **管道** （传输 + Security 0 / 1 / 2）
   * - **network_provisioner**
     - 控制器 **业务** （``prov-config`` / ``prov-scan`` / ``prov-ctrl``）

本组件负责编解码配网 protobuf。会话传输与安全握手仍由 ``protocomm_ext`` 完成。

功能
----

- SoftAP + HTTP：Wi-Fi 配网
- BLE（NimBLE Central）：Wi-Fi 与 Thread 配网
- UART Console：调试通路
- 从 ``proto-ver`` 解析能力（``prov.cap`` / ``prov.sec_ver``）
- 高层接口 ``network_provisioner_provision_wifi()`` / ``provision_thread()``

**支持目标：** ESP32 / ESP32-S3 / ESP32-C2 / ESP32-C3 / ESP32-C5 / ESP32-C6 / ESP32-C61 / ESP32-H2

典型流程
--------

.. code:: c

    #include "network_provisioner.h"
    #include "protocomm_ext.h"
    #include "protocomm_ext_security.h"

    protocomm_ext_t *pc = protocomm_ext_new(&pe_cfg); /* 传输 + Sec0 占位 */
    network_provisioner_t *np = network_provisioner_create(pc);

    network_provisioner_fetch_capabilities(np, /* BLE: &ble_addr，否则 NULL */ NULL);
    network_provisioner_capabilities_t caps = {0};
    network_provisioner_get_capabilities(np, &caps);
    /* 按 caps.sec_ver 调用 protocomm_ext_set_security() 配置 PoP / Sec2 */
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

若安全方案已知，仍可用 ``network_provisioner_start_session()`` 一步完成
open → proto-ver → handshake。

**线程安全：** 同一实例的 API 非线程安全，请在单任务中串行调用。

添加依赖
--------

.. code:: yaml

    dependencies:
      espressif/network_provisioner: "^1.0.0"

示例
----

见 ``examples/network_provisioner``。设备侧请使用 ``network_provisioning``
官方示例 ``wifi_prov`` / ``thread_prov``。

API 参考
--------

.. include-build-file:: inc/network_provisioner.inc
