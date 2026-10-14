# 当前三板系统与验证口径

更新日期 2026-10-14。

## 架构与版本

- A 传感器板：Arduino 1.3.1 / Arduino-ESP32 3.3.11，DHT22 GPIO4、HC-SR04 GPIO5/18，每 2 秒采样并经 TCP 8080 发送。
- B 网关：ESP-IDF v6.1 / 应用 0.7.1，接收 A 数据，与 STM32 双向 UART 通信，使用 MQTT TLS 接入 ThingsBoard，保存 NVS 阈值并提供本地网页双分区 OTA。
- STM32F407VET6：FreeRTOS 采集摇杆、处理按键和显示 OLED。OLED 接 STM32 PB6/PB7；B GPIO16/17 与 STM32 PA2/PA3 交叉连接并共地，115200 bit/s。
- A/B 均使用 DHCP。A 自动发现 `esp32-b.local`，没有在代码里固定当前热点地址。
- 传感器本地采样约 2 秒，云端当前配置每 60 秒上传最新快照；离线历史补传未实现。

## 发布与验证

主工程合并提交：ESP32 `609ba69d47036f8675159d618c1f74d43ecc55c7`；STM32 `5f01d4688ac00e1ddbcaf232d5c67d772f634dbb`。

两仓库合计 48 个不同 CTest 用例：B 25、STM32 23。GCC/Clang 与 ASan/UBSan 矩阵会重复执行这些用例，不叠加成 96 个不同用例。CI 另编译完整 ESP32 和 ARM 固件。

10 次有完整记录的实板恢复验收包括 STM32 复位 4 次、B 复位 3 次、A 暂停后恢复 3 次。首次采集脚本回执误判的先导复位已执行但不计入成功率或恢复时间。12 类模拟故障来自已有 STM32 回归中的子集，不是新增 12 个独立用例。

OLED 白底黑字布局已由用户观察确认。网页 OTA 0.7.0 到 0.7.1、错误口令、哈希不匹配、上传中断和诊断版本主动健康失败回滚已有记录。物理断电 OTA、全带宽压测及 24 小时稳定性未完成；没有 CPU 利用率数据。

## 讲解顺序

架构与任务所有权 → 帧边界与 CRC → ACK 重试和幂等去重 → 重启后的会话恢复 → DMA 到任务的交接 → 云端状态机 → OTA 健康检查 → 验证方法和边界。

会话 token 区分通信世代，不是身份认证；SHA-256 检查镜像一致性，不代替数字签名。当前使用 ESP-IDF 提供的 bootloader，没有自研 STM32 bootloader，也没有启用 Secure Boot / eFuse 防降级。

## 公开证据

FAULT_INJECTION_STATUS.md 和 FAULT_INJECTION_SUMMARY.json 保留样本数、统计窗口、恢复时间与资源读数。公开副本移除了本机路径和局域网地址；原始串口日志、设备身份与 SWD/Flash 备份保存在本机。
