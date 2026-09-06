# OLED 一页笔记（ESP32 B → 以后接到 F407）

## 1. 这块屏是什么

| 项 | 值 |
|----|----|
| 型号 | 0.96" SSD1306 |
| 分辨率 | 128×64 |
| 接口 | **I2C**（常见 4 针） |
| 地址 | 多为 **`0x3C`**（少数 `0x3D`） |
| 供电 | **3.3V**（F407 / ESP32 都可；不要接 5V 除非模块丝印写 5V） |

模块针脚（丝印）：

```text
GND  VCC  SCL  SDA
```

---

## 2. 现在在 ESP32 B 板上怎么接

来源：仓库 `README.md` + `b_esp32/b_esp32.ino`

| OLED 脚 | ESP32 GPIO | 说明 |
|---------|------------|------|
| GND | GND | 共地 |
| VCC | 3.3V | |
| SCL | **GPIO 22** | I2C 时钟 |
| SDA | **GPIO 21** | I2C 数据 |

这是 ESP32 Arduino 默认 I2C 脚。

---

## 3. 软件驱动用法（B 板现状）

- 库：**U8g2**（Arduino）
- 控制器类：`U8G2_SSD1306_128X64_NONAME_F_HW_I2C`  
  - `F` = 全缓冲（先画在 RAM，再一次性刷屏）  
  - `HW_I2C` = 硬件 I2C

关键调用顺序（记这 4 步即可）：

```c
u8g2.begin();           // setup 里初始化一次
u8g2.clearBuffer();     // 清空显存
u8g2.setFont(...);      // 选字体
u8g2.drawStr(x, y, s);  // 画字符串（y 是基线，不是左上角）
u8g2.sendBuffer();      // 真正刷到屏幕
```

B 板实际用途：

- `DisplayTask`：温湿度 / 距离，或 No WiFi / No client / Waiting
- OTA：进度条 + 成功/失败文字

相关文件：`b_esp32/b_esp32.ino`（`u8g2` 对象、`DisplayTask`、`drawOtaUi`）

> 注意：构造函数写成了 `u8g2(U8G2_R0, 22, 21, U8X8_PIN_NONE)`。  
> U8g2 官方 HW_I2C 参数顺序是 `(旋转, reset, clock, data)`，和 README 的「SCL=22 SDA=21」不完全一致。  
> **搬家时以杜邦线实际接法 + README 为准**；接到 F407 时重新配 I2C，不要照搬这个构造参数。

---

## 4. 拔下来接到 F407 时怎么做

硬件：

| OLED | STM32F407 |
|------|-----------|
| GND | GND |
| VCC | 3.3V |
| SCL | 选一组 I2C 的 SCL（CubeMX 里配，如 PB6 / PB8 等） |
| SDA | 同组 I2C 的 SDA（如 PB7 / PB9） |

软件（和 Arduino 不同）：

1. CubeMX：打开 **I2C1**（或 I2C2），Fast Mode 400 kHz 即可  
2. 用 HAL：`HAL_I2C_Mem_Write` 往 `0x3C` 发命令/显存  
3. 或找 STM32 版 SSD1306 / U8g2 移植；**不能直接复制** `b_esp32.ino` 里的 U8g2 Arduino API  
4. 同一块屏 **不能同时** 插在 ESP32 和 F407 上

最小验证：F407 上刷出一行 `F407 OLED OK` 即成功。

---

## 5. 面试一句话

> B 板用 I2C SSD1306（U8g2）做 UI；显示任务和网络任务通过队列解耦。同一块屏以后可接到 F407，换 HAL I2C 驱动即可。
