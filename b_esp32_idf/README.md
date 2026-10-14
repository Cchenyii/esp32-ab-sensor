# ESP32 B 原生 ESP-IDF 网关

固定 ESP-IDF v6.1，目标 ESP32，当前应用 0.7.1。
A 板传感器通过 TCP 接入 B；B 与 STM32F407 双向 UART 通信，并接入 MQTT TLS、NVS 远程阈值配置和双应用分区 OTA。
OLED 位于 STM32，显示 A 板数据与摇杆状态。STM32 会话/DMA 固件应配套本原生 B 工程使用。

## 配置与编译

将 config/wifi_credentials.example.h、config/thingsboard_credentials.example.h 复制到同目录，去掉 .example 后缀，再在本机配置网络和设备 Token。
真实配置文件已忽略。使用 ESP-IDF v6.1 环境执行 `idf.py build`；Windows EIM 用户可用 `tools/idf.ps1 build`。
本地板上应用和备份包含配置，应保存在自己的电脑。

## 电脑端回归

```sh
cmake -S host-tests -B build-host -DCMAKE_BUILD_TYPE=Debug
cmake --build build-host --parallel
ctest --test-dir build-host --output-on-failure
```

Windows 已安装 MinGW C/C++ 与 CMake 时，可以执行 `host-tests/run.ps1`。
25 个 CTest 用例编译实际 protocol.cpp、protocol_self_test.cpp、session_gate.cpp 和 session_wire.c，覆盖 CRC、拆包、粘包、噪声恢复、会话重建、重复确认、旧会话拒绝、序号环绕及编码边界。
随机分片、CRC 恢复、C/C++ 编码一致性和会话去重使用固定种子 0x20261014，各运行 2000 轮，失败可复现。

## CI

仓库 .github/workflows/native-idf-ci.yml 在 push、pull_request 或手动触发时运行：

- Ubuntu 的 GCC/Clang 各执行 25 项回归，开启 AddressSanitizer / UndefinedBehaviorSanitizer。
- 官方 espressif/idf:v6.1 镜像编译完整 B 工程，使用示例网络配置与空云端 Token。
- 保存 JUnit 测试报告 14 天。

CI 结果证明软件回归与编译状态；热点恢复、DMA 中断时序、传感器和 OLED 需要板上验收。
本机已完成实际网页 OTA、健康失败回滚及 STM32 DMA 联调；电脑端回归的实时结果以 Actions 页面为准。

## OTA

ota_0 / ota_1 各 1.5 MiB，支持启动健康检查和失败回滚。USB OTAGET 查询状态，OTAARM 开启 300 秒的一次性口令窗口；网页在本地 HTTP 80 端口，云端 getOTAStatus RPC 只读查询。
使用实际本机配置重新编译并升级。文件校验使用 SHA-256，未配置固件签名或 Secure Boot。
