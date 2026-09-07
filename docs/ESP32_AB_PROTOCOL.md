# ESP32 A/B 节点通信协议

## 1. 文档状态

- 当前候选版本：V1（二进制传感器帧）
- V1阶段：编码、CRC、环形缓冲解析、序号统计及非阻塞ACK已接入
- 服务发现：mDNS，主机名 `esp32-b.local`
- 传输层：TCP，端口 `8080`

V0文本协议已从A/B数据链路移除。V1格式保持冻结，后续只增加ACK状态机，
不修改现有SENSOR_DATA帧布局。

## 2. 设计目标

1. 明确消息边界，正确处理 TCP 拆包和粘包。
2. 检测应用数据损坏、长度异常和未知消息。
3. 使用序号识别重复帧和漏帧。
4. ACK 与重传不得阻塞 FreeRTOS 任务。
5. 网络中断后能够通过 mDNS 重新发现并恢复连接。
6. 协议可以增加新命令，而不破坏旧版本。

## 3. V1 帧格式

所有多字节整数使用网络字节序（大端）。

| 字段 | 长度 | 说明 |
|---|---:|---|
| Magic | 2 字节 | 固定为 `0xAA 0x55` |
| Version | 1 字节 | 当前为 `0x01` |
| Type | 1 字节 | 消息类型 |
| Sequence | 4 字节 | 发送方递增序号 |
| PayloadLength | 2 字节 | Payload 长度，最大 64 字节 |
| Payload | N 字节 | 消息数据 |
| CRC16 | 2 字节 | CRC-16/CCITT-FALSE |

固定头部长度为 10 字节；整帧长度为 `12 + PayloadLength`。

CRC计算范围从 `Version` 开始，到 `Payload` 最后一个字节结束；不包含
Magic和CRC字段本身。

CRC参数：

- Polynomial：`0x1021`
- Initial value：`0xFFFF`
- RefIn / RefOut：false
- XorOut：`0x0000`

## 4. 消息类型

| Type | 名称 | 方向 | Payload |
|---:|---|---|---|
| `0x01` | SENSOR_DATA | A → B（TCP） | 传感器数据 |
| `0x02` | ACK | B → 对端 | 处理结果 |
| `0x03` | HEARTBEAT | 双向 | 运行状态 |
| `0x04` | JOYSTICK_DATA | STM32 → B（UART） | 摇杆状态 |
| `0x10` | OTA_STATUS | 双向 | OTA状态，后续版本使用 |

### 4.1 SENSOR_DATA

使用定点整数，避免不同平台直接传输 `float` 产生兼容问题。

| 字段 | 类型 | 单位 |
|---|---|---|
| Temperature | int16 | 0.01 °C |
| Humidity | uint16 | 0.01 %RH |
| Distance | uint16 | cm |

Payload长度固定为6字节。

例如：温度27.35°C编码为整数2735。

### 4.2 JOYSTICK_DATA

与 SENSOR_DATA 共用同一 V1 帧信封（Magic/Version/Seq/CRC）；Payload 同样固定 6 字节，便于 STM32 与 ESP32 同构联调。

| 字段 | 类型 | 说明 |
|---|---|---|
| X | uint16 | ADC 原始值 0～4095 |
| Y | uint16 | ADC 原始值 0～4095 |
| Direction | uint8 | 0=CENTER,1=LEFT,2=RIGHT,3=UP,4=DOWN |
| SW | uint8 | 1=按下，0=松开 |

传输：STM32 USART2（115200）→ ESP32 B 板 `Serial2`（GPIO16 RX / GPIO17 TX），B 回复 ACK。

### 4.3 ACK

ACK帧的Sequence与被确认帧相同。

Payload长度为1字节：

| Status | 含义 |
|---:|---|
| `0x00` | 成功 |
| `0x01` | CRC错误 |
| `0x02` | 长度错误 |
| `0x03` | 不支持的版本 |
| `0x04` | 不支持的消息类型 |

## 5. 接收状态机

B板使用256字节环形缓冲区接收TCP字节流，解析器包含以下状态：

1. `SEARCH_MAGIC_1`：查找 `0xAA`
2. `SEARCH_MAGIC_2`：确认下一个字节为 `0x55`
3. `READ_HEADER`：读取Version、Type、Sequence和PayloadLength
4. `READ_PAYLOAD`：读取指定长度Payload
5. `READ_CRC`：读取并校验CRC
6. `DISPATCH`：按Type处理消息

若版本、长度或CRC非法，解析器丢弃当前帧并重新搜索Magic，不清空整个TCP
接收缓冲区。

## 6. ACK、去重与重传

A板同一时间最多保留一帧待确认数据：

1. 发送SENSOR_DATA后记录Sequence和发送时间，但TCP任务继续运行，不阻塞等待。
2. 1200ms内收到对应ACK：删除待确认帧。
3. 超时未收到ACK：重发同一Sequence，最多重发2次。
4. 连续失败后关闭TCP，通过mDNS重新发现B板并重连。
5. 新传感器数据到来而旧帧仍待确认时，只保留最新采样值。

B板保存最近一次成功处理的Sequence：

- 新Sequence：更新OLED并回复ACK。
- 重复Sequence：不重复更新业务状态，但再次回复ACK。
- 序号跳变：记录漏帧计数，仍处理最新帧。

这样即使“B已处理数据但ACK丢失”，A重发时也不会重复执行相同业务。

## 7. 安全边界

- PayloadLength不得超过64字节。
- 环形缓冲区满时丢弃最旧的未解析字节，并增加溢出计数。
- 所有字段在使用前必须完成长度与CRC校验。
- 禁止从Payload直接构造未限定长度的字符串。
- Sequence按无符号32位整数回绕处理。

## 8. 运行指标

系统维护以下计数器，便于OLED诊断页和串口输出：

- 已接收有效帧数
- CRC错误数
- 长度错误数
- 环形缓冲区溢出数
- 重复帧数
- 检测到的漏帧数
- ACK重传数
- TCP重连数
- 最近一次有效数据时间

## 9. 实施顺序

1. [已完成] 编写并验证CRC16函数。
2. [已完成] 实现帧编码器和解码器。
3. [已完成] 实现B板环形缓冲区和解析状态机。
4. [已完成] 将文本SENSOR_DATA替换为二进制SENSOR_DATA。
5. [已完成] 非阻塞ACK、超时重传、序号去重和漏帧统计。
6. [已完成] B板 HTTP OTA（双分区写入 + OLED 进度 + 启动确认）。
7. [进行中] STM32 → B：JOYSTICK_DATA UART 同构联调（CRC/ACK）。
8. 进行拆包、粘包、CRC错误和ACK丢失测试。
