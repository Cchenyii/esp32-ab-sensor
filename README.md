# ESP32 嵌入式终端通信与 OTA 升级系统

由 ESP32 A 传感器板、ESP32 B 网关及 STM32F407 交互终端组成。当前运行版采用 A 的 Arduino 1.3.1、B 的原生 ESP-IDF 0.7.1 和 STM32 会话 UART/DMA 固件。OLED 接在 STM32 上。

```text
A DHT22 / HC-SR04 -- TCP 8080 --> B ESP-IDF gateway -- MQTT TLS --> ThingsBoard
                                  |                       cloud telemetry/config
                                  | UART session + ACK
                                  v
                           STM32F407 joystick + OLED
Browser -- local HTTP --> B inactive OTA slot --> health check or rollback
```

## 当前代码入口

- [A 板源码](a_esp32/) 与 [mDNS 发现及构建](docs/A_DISCOVERY.md)：2 秒采样，DHCP 和 mDNS 自动发现，Arduino-ESP32 固定 3.3.11。
- [B 原生 ESP-IDF 工程](b_esp32_idf/README.md)：固定 ESP-IDF v6.1，UART/TCP/云任务，MQTT TLS、NVS 配置、双分区 HTTP OTA。
- [STM32 配套工程](https://github.com/Cchenyii/stm32-f407-joystick)：双通道 ADC、FreeRTOS、USART2 循环 DMA 和 SSD1306 OLED。
- [当前三板状态](b_esp32_idf/docs/PROJECT_STATUS.md)、[故障验收](b_esp32_idf/docs/FAULT_INJECTION_STATUS.md) 与 [CI](b_esp32_idf/docs/CI.md)。

## 硬件连接

A：DHT22 DATA GPIO4，HC-SR04 TRIG GPIO5 / ECHO GPIO18。超声波模块若输出 5V，ECHO 必须按硬件电平要求转换至 ESP32 可接受范围。

B GPIO16 RX 接 STM32 PA2 TX；B GPIO17 TX 接 STM32 PA3 RX；GND 共地；115200 bit/s。摇杆接 STM32 PA0/PA1、按键 PA4；OLED I2C1 PB6/PB7。

## 可靠通信与升级

二进制 V1 帧带长度、32 位序号和 CRC-16/CCITT-FALSE。流式解析适配半帧、粘包与噪声；应用层 ACK、有限重试、去重和 UART 会话握手负责跨重启恢复。TCP 的可靠字节流不能代替业务完成确认。

B 通过 MQTT TLS 上传环境与摇杆最新快照，当前云端周期 60 秒。远程阈值通过 NVS 保存。本地 HTTP OTA 写未运行的应用分区，校验后切换启动分区；新程序通过健康检查后确认，失败回滚。当前未实现固件签名、Secure Boot 和离线历史补传。

## 构建与凭据

按各工程 README 安装固定依赖。A 从 secrets.h.example 创建本机 secrets.h；B 从 config/ 中的示例创建本机 Wi-Fi 和 ThingsBoard 配置。真实凭据不提交，CI 只使用占位配置。

合计 48 个不同电脑端回归用例（B 25、STM32 23），自动使用 GCC/Clang 与 ASan/UBSan；完整 ESP32/ARM 构建另列检查。CI 不等同于实板验证。

## 历史 Arduino B 版本

`b_esp32/` 及顶层早期协议/OLED/OTA 记录保留历史 Arduino B 方案，其 OLED 连接和无会话帧说明不适用于当前三板运行版本。当前 B 入口为 `b_esp32_idf/`。

## 许可证

原创代码使用 [MIT License](LICENSE)，第三方组件遵循各自许可证。
