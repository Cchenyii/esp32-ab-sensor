# B 板 HTTP OTA 使用说明

## 功能

- B 板在 **TCP 8080** 接收传感器数据的同时，在 **HTTP 80** 提供固件升级页面
- 升级过程中 OLED 显示进度条
- 使用双 OTA 分区写入非当前运行槽位，重启后切换
- 启动时若新固件处于 `PENDING_VERIFY`，自动标记为有效（取消回滚）

固件版本号见 `b_esp32.ino` 中的 `FW_VERSION`。

## 一次性配置（Arduino IDE）

1. 打开 `b_esp32/b_esp32.ino`
2. **工具 → 开发板**：你的 ESP32 型号
3. **工具 → Partition Scheme**：选择带 **OTA** 的分区，例如：
   - `Minimal SPIFFS (1.9MB APP with OTA/190KB SPIFFS)`（4MB Flash 常用）
4. **USB 线烧录一次**（分区表变更后必须重新全量烧录）
5. 串口监视器 115200，确认打印 `Firmware 1.1.0` 和 `IP: x.x.x.x`

> 若未选 OTA 分区，`Update.begin()` 会失败，OLED 显示 `OTA Failed`。

## 升级步骤

1. B 板与电脑/手机连接同一 WiFi
2. 在 Arduino IDE：**项目 → 导出已编译的二进制文件**，得到 `b_esp32.ino.bin`
3. 浏览器打开：
   - `http://<B板IP>/`（串口里有 IP）
   - 或 `http://esp32-b.local/`（mDNS 可用时）
4. 选择 `.bin` 文件 → **Upload & Flash**
5. OLED 显示进度 → 成功后自动重启

## 验证

- 串口应打印新版本号 `Firmware x.x.x`
- 传感器链路与 OLED 显示应恢复正常
- 若升级后无法启动，可再次 USB 烧录恢复

## 与 A 板的关系

当前 OTA **仅 B 板**。A 板仍通过 USB 烧录；后续可扩展为 A 经 B 转发或独立 OTA 页面。
