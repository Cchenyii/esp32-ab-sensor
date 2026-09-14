# 实机测试记录

记录对象：本仓库默认分支最终固件（B 板 `FW_VERSION` **1.2.0**）。  
测试方式：Arduino IDE USB 烧录 / HTTP OTA，串口监视器 **115200**，OLED 目视。  
环境：两块 ESP32 同 WiFi（含手机热点），可选 STM32F407 经 USART2 接入 B 板 Serial2。

本文件只写板上实际出现过的现象，不写未统计的成功率或长时间压测数字。

## 1. 上电与协议自测

| 步骤 | 期望 | 结果 |
|------|------|------|
| 先烧录并上电 B，再上电 A | B 先提供 TCP 8080 / HTTP 80 | 通过 |
| A 串口 | `Protocol self-test: OK` | 通过 |
| B 串口 | `Firmware 1.2.0`、`Protocol self-test: OK` | 通过 |
| B 串口 | `UART2 joystick link: RX=16 TX=17` | 通过 |
| B 串口 | `IP: x.x.x.x`、`mDNS: esp32-b.local`、`HTTP OTA: http://<ip>/` | 通过 |

`selfTest()` 覆盖：CRC-16/CCITT 向量、SENSOR_DATA 拆包、SENSOR_DATA+ACK 粘包、CRC 损坏拒绝。启动打印 `OK` 即上述用例通过。

## 2. A → B 传感器链路

| 步骤 | 期望 | 结果 |
|------|------|------|
| A 发现 B | 串口 `Connecting to ...` 后 `Connected` | 通过 |
| B 接受连接 | `Client connected` | 通过 |
| OLED（接在 B 上时） | `Sensor Data` 页刷新温度、湿度、距离 | 通过 |
| 周期性统计 | A：`ACK created=... acknowledged=...`；B：`Protocol frames=... crc=0` | 通过 |
| 任务调度 | OLED 持续刷新，不再长时间卡死 | 通过 |

调度说明：早期 DisplayTask 优先级低于 ServerTask，OLED 会卡住。改为同优先级、加大 `displayQueue`、`xQueueSend` 带超时后，屏幕恢复约 2 s 一拍的数据刷新。

Waiting 误报：传感器周期约 2 s，曾在 `idle_ms≈2087` 时闪 `Waiting...`。现仅在连续约 6 s 无新数据时显示 Waiting。

## 3. 断线与半开连接

| 步骤 | 期望 | 结果 |
|------|------|------|
| 拔掉 A 或关热点 | B 约数秒后断开空连接，OLED 可出现 `No client` / `Waiting...` | 通过 |
| 恢复 WiFi / 重新上电 A | A 能再次 `Connected`，OLED 恢复传感器页 | 通过 |
| 手机热点偶发断连 | 数秒内靠 mDNS / 缓存 IP / 重连恢复，允许偶发重连计数增加 | 通过 |

半开连接：对端已掉、本端 socket 仍显示 connected 时，靠接收超时断开并 `client.stop()`，避免一直占着 accept。

## 4. HTTP OTA（仅 B 板）

| 步骤 | 期望 | 结果 |
|------|------|------|
| 分区方案选带 OTA 的表，USB 全量烧录一次 | 串口打印当前版本与 IP | 通过 |
| 1.1.0 → 1.1.1 网页升级 | 浏览器完成上传，OLED 有进度，重启后版本号更新 | 通过 |
| 升到 1.2.0 | 串口 `Firmware 1.2.0`，传感器链路可恢复 | 通过 |
| 启动确认 | 新分区标记有效，未误回滚 | 通过 |

操作步骤见 [`OTA.md`](OTA.md)。

## 5. STM32 摇杆 UART（B ≥ 1.2.0）

接线：STM32 PA2/PA3 ↔ B GPIO16/GPIO17，共地，115200。

| 步骤 | 期望 | 结果 |
|------|------|------|
| STM32 调试口 USART1 | `boot`、`oled ok`、`TX seq=... ACK` | 通过 |
| B USB 串口 | `JOY seq=... X=... Y=... CENTER/LEFT/... SW=0/1` | 通过 |
| 推摇杆 | 方向随死区外运动变化；按下 SW 时 `SW=1` | 通过 |
| OLED 在 B 上时 | 可切到 `Joystick UART` 页 | 通过 |

STM32 侧记录见配套仓库 [`docs/TEST_RECORD.md`](https://github.com/Cchenyii/stm32-f407-joystick/blob/main/docs/TEST_RECORD.md)。

## 6. 未纳入本记录的内容

- 未做 24 小时不间断计数统计。
- 未统计 OTA 成功率百分比。
- A 板无 HTTP OTA，仍用 USB 烧录。
