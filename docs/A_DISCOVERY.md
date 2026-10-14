# A 板自动发现与构建

当前 A 应用版本 1.3.1，Arduino-ESP32 固定 3.3.11。本次从已运行的独立工程同步源文件，没有改动板上固件。

## 自动发现

A/B 地址均由 DHCP 分配。A 的 TCPTask 独占 WiFiClient 和 PeerDiscovery。发现顺序为 mDNS 主机查询、最近成功地址缓存、`_esp32b._tcp` 服务查询、可选同子网备用地址。

- 初始化 `MDNS.begin("esp32-a")`；`MDNS.queryHost("esp32-b", 1500)` 使用裸主机名。
- TCP 单次连接限时 1500 ms；缓存有效期 5 分钟，连接失败、网络变化或过期时清理。
- Arduino-ESP32 3.3.11 的服务查询限时 3000 ms，最多 20 个结果；本应用最多尝试 3 个候选地址。整轮发现可能包含多个查询和连接，不能写成整轮 1500 ms。
- 校验端口、主机名及同子网 IPv4 单播地址，排除自身、网络和广播地址。
- 可选 `SERVER_IP_FALLBACK` 默认留空；设备没有配置静态 IP。
- 1.3.1 关闭 Wi-Fi modem sleep。原 V1 数据协议保持兼容。

## 可复现构建

安装 Arduino CLI，将其加入 PATH，然后执行：

```powershell
arduino-cli config add board_manager.additional_urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli core update-index
arduino-cli core install esp32:esp32@3.3.11
arduino-cli lib install 'DHT sensor library@1.4.7' 'Adafruit Unified Sensor@1.1.15' 'NewPing@1.9.7'
Copy-Item ./a_esp32/secrets.h.example ./a_esp32/secrets.h
# 编辑本机 secrets.h 后编译
./tools/build-a.ps1
```

已有 Arduino IDE 配置的 Windows 用户可向脚本传 `-ArduinoCli` 和 `-ConfigFile` 的本机路径。源码断言要求核心版本 3.3.11。编译输出在 build-a/output，不提交。

本轮没有重新运行 A 编译；已有实板记录验证了主机发现及数据链路。服务查询、地址缓存和跨热点切换没有分别完成故障验收。A 初次启动仍等待 Wi-Fi 连接后才启动采样。

参考：[固定版本 ESPmDNS 实现](https://github.com/espressif/arduino-esp32/blob/3.3.11/libraries/ESPmDNS/src/ESPmDNS.cpp)。
