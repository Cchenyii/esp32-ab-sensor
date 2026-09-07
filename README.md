# ESP32 A/B 双板传感器链路

两块 ESP32 组成的嵌入式演示项目：**A 板采集温湿度与超声波距离，经 WiFi TCP 发到 B 板；B 板解析协议并在 OLED 上显示。**

适合秋招作品集展示：FreeRTOS 双任务、二进制应用层协议、CRC、非阻塞 ACK/重传、mDNS 发现、任务看门狗、**B 板 HTTP OTA + OLED 进度**。

## 架构

```text
[A: DHT22 + HC-SR04]                  [B: SSD1306 OLED]
 SensorTask ──queue──> TCPTask          ServerTask ──queue──> DisplayTask
        |                                      |
        +---- TCP :8080 / V1 binary frame -----+
        |         mDNS: esp32-b.local          |
        +---------- ACK / retransmit ----------+
```

| 角色 | 职责 | 主要技术点 |
|------|------|------------|
| **A** | 传感器采集 + TCP 客户端 | FreeRTOS、定点编码、有限重传、TCP keepalive |
| **B** | TCP 服务端 + OLED + **STM32 UART 摇杆** | mDNS、环形缓冲、序号去重、TWDT、Serial2 V1 |

协议细节见 [`docs/ESP32_AB_PROTOCOL.md`](docs/ESP32_AB_PROTOCOL.md)。STM32 联调见下方「STM32 UART」。

## 硬件与引脚

### A 板

| 外设 | 引脚 |
|------|------|
| DHT22 DATA | GPIO 4 |
| 超声波 TRIG | GPIO 5 |
| 超声波 ECHO | GPIO 18 |

### B 板

| 外设 | 引脚 |
|------|------|
| OLED I2C SCL | GPIO 22 |
| OLED I2C SDA | GPIO 21 |
| OLED 驱动 | SSD1306 128×64（U8g2） |
| STM32 链路 RX2 / TX2 | **GPIO 16 / 17**（115200，V1 `JOYSTICK_DATA`） |

### STM32 UART（可选联调）

| STM32F407 | ESP32 B |
|-----------|---------|
| PA2 USART2_TX | GPIO16 RX2 |
| PA3 USART2_RX | GPIO17 TX2 |
| GND | GND |

B 固件 ≥ **1.2.0** 后，USB 串口会打印 `JOY seq=...`；若 OLED 仍接在 B 上会显示 Joystick 页。STM32 工程：https://github.com/Cchenyii/stm32-f407-joystick

两板需接同一 WiFi（可用手机热点）。

## 软件依赖

- Arduino IDE + **arduino-esp32 3.x**（本项目在 3.3.x 验证）
- 库：`DHT sensor library`、`NewPing`、`U8g2`

## 快速开始

1. 克隆或打开本仓库目录：
   `C:\Users\CJY13\Documents\esp32-ab-sensor`
2. 为每块板配置 WiFi（勿提交真实密码）：

```text
cd a_esp32
copy secrets.h.example secrets.h
# 编辑 secrets.h 填入 WIFI_SSID / WIFI_PASSWORD

cd ..\b_esp32
copy secrets.h.example secrets.h
# 同样编辑 secrets.h
```

3. 在 Arduino IDE 中分别打开：
   - `a_esp32/a_esp32.ino`
   - `b_esp32/b_esp32.ino`
4. 板型选择对应 ESP32 开发板，串口监视器 **115200**。
5. **先烧录并上电 B 板**，再烧录 A 板。

成功标志示例：

- A：`Protocol self-test: OK`，周期性 ACK 统计
- B：`Protocol frames=...` 增长且错误计数为 0，OLED 刷新温湿度与距离
- B OTA：浏览器访问 `http://esp32-b.local/` 可上传 `.bin`，OLED 显示进度（见 [`docs/OTA.md`](docs/OTA.md)）

## 协议要点（V1）

- 帧：`AA 55 | ver | type | seq | len | payload | CRC16-CCITT-FALSE`
- `SENSOR_DATA`：温度/湿度定点 ×100 + 距离 cm（6 字节）
- A 侧非阻塞等待 ACK，超时约 1200 ms，最多重传 2 次
- B 侧按序号去重，避免重复显示/统计

## 已知限制

- **手机热点不稳定**时可能出现短暂断连；当前设计约数秒内靠 mDNS/缓存 IP 与重连恢复，可接受偶发重连。
- mDNS 偶发返回 `0.0.0.0`：实现会校验后再缓存，并回退到上次有效 IP。
- `protocol.cpp` / `protocol.h` 在 A/B 目录各有一份相同副本（Arduino 草图不便跨目录共享）。

## 仓库结构

```text
esp32-ab-sensor/
├── a_esp32/                 # A 板草图
│   ├── a_esp32.ino
│   ├── protocol.h / .cpp
│   ├── secrets.h.example
│   └── secrets.h            # 本地文件，已 gitignore
├── b_esp32/                 # B 板草图
├── docs/
│   └── ESP32_AB_PROTOCOL.md
├── README.md
└── .gitignore
```

## 后续规划（可选）

1. ~~B 板 HTTP OTA + OLED 进度~~（已完成，见 [`docs/OTA.md`](docs/OTA.md)）
2. A 板 OTA 或经 B 集中升级
3. 稳定性压测报告 / Demo 视频写入作品集

## 许可证

个人作品集项目；如需开源许可可再补充。
