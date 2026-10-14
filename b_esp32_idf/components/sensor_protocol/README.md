# sensor_protocol

原 ESP32 A/B V1 协议的独立 C++ 组件。`protocol.cpp` 从原 B 板仓库逐字节复制；`protocol.h` 将 `Arduino.h` 替换为 `stddef.h`、`stdint.h`。帧格式、枚举、CRC、编解码与流式解析实现保持原样。

## 接口

- C++ 业务模块包含 `protocol.h`，使用 `SensorProtocol` 命名空间。
- C 启动代码包含 `protocol_self_test.h`，调用 `sensor_protocol_run_self_tests()`；接口采用 C ABI。
- 协议核心与自测没有 Arduino、ESP-IDF、串口、socket 或日志依赖。日志由应用层输出。
- 每条字节流使用各自的 `FrameParser` 实例，由所属任务操作。

## 启动验证

共8组检查：原有自测、CRC标准向量、固定传感器/摇杆/ACK帧字节、全部拆包边界与逐字节输入、三帧粘包、CRC错误后恢复、前导噪声重同步、环形缓冲跨边界。

固定帧CRC由 Python 标准库 `binascii.crc_hqx(body, 0xFFFF)` 独立生成。三种帧长度18/18/13字节，拆包验证覆盖46个非空切分点及逐字节输入。环形缓冲验证连续处理54帧、882字节。

源码来源：`C:\Users\CJY13\Documents\esp32-ab-sensor\b_esp32\protocol.cpp/.h`。
原 `protocol.cpp` SHA256：`8A09CD7E12258B0EA704C86EC200DDB31593436FA1989F9E7C94508CA37F46E2`。

会话握手、去重、ACK发送、网络与UART驱动属于后续链路层迁移。本组件负责帧格式与字节流解析。
