# Network Provisioner

面向 [espressif/network_provisioning](https://components.espressif.com/components/espressif/network_provisioning) 的**控制器侧**配网客户端。

| 组件 | 角色 |
|------|------|
| **network_provisioning** | 设备端 **Server** |
| **protocomm_ext** | 控制器 **管道**（传输 + Sec0/1/2） |
| **network_provisioner**（本组件） | 控制器 **业务**（`prov-config` / `prov-scan` / `prov-ctrl`） |

## 能力矩阵

| 链路 \\ 凭证 | Wi-Fi | Thread |
|--------------|:-----:|:------:|
| SoftAP + HTTP | ✓ | — |
| BLE | ✓ | ✓ |
| UART Console | 调试 | 调试 |

## 典型流程

推荐按对端 `proto-ver` 运行时识别安全等级：

```c
network_provisioner_t *np = network_provisioner_create(pc);
network_provisioner_fetch_capabilities(np, NULL);
/* protocomm_ext_set_security() 按 caps.sec_ver 配置 PoP / Sec2 */
network_provisioner_establish_security(np);
network_provisioner_provision_wifi(np, &creds, NULL);
network_provisioner_stop_session(np);
network_provisioner_delete(np);
```

若安全方案已知，仍可用 `network_provisioner_start_session()` 一步完成。

**线程安全：** 同一实例的 API 非线程安全，请在单任务中串行调用。

## 依赖

```yaml
dependencies:
  espressif/network_provisioner: "^1.0.0"
```

## 测试 / 示例

- 测试：`components/network_provisioner/test_apps`
- 示例：`examples/network_provisioner`（同一工程可选 SoftAP / BLE / Thread）

**支持目标：** ESP32 / ESP32-S3 / ESP32-C2 / ESP32-C3 / ESP32-C5 / ESP32-C6 / ESP32-C61 / ESP32-H2

## 许可证

Apache-2.0，见 `license.txt`。
