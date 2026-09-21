# Brain / Motion UART 协议

范围：新仓库第一步通信闭环。下面先记录当前实现，再列出后续扩展方向。

## 当前帧格式

115200 baud、8N1；所有整数为 little-endian。

| 偏移 | 字段 | 长度 |
| --- | --- | --- |
| 0 | Magic `0x42 0x4D`（BM） | 2 bytes |
| 2 | Version `1` | 1 byte |
| 3 | Type | 1 byte |
| 4 | Payload length，最大 32 | 2 bytes |
| 6 | Sequence | 4 bytes |
| 10 | Payload | 0–32 bytes |
| 尾部 | CRC16，覆盖头部和 Payload | 2 bytes |

CRC 参数：多项式 `0x1021`、初值 `0xFFFF`、不反射、最终不异或，CRC 值以 little-endian 写入。
这是复用旧协议封包思路的新协议，不是旧 `BT / v3` 显示协议的兼容扩展。

接收端流式收帧；非法版本、类型、长度或 CRC 不会交给业务层。
字节间隔超过 100 ms 时调用方清除未完整接收的帧。无格式化文本日志写入板间 UART；日志走 USB Serial。

## 已实现消息

| Type | 名称 | 方向 | Payload |
| --- | --- | --- | --- |
| 1 | GetStatus | brain → motion | 空 |
| 2 | Status | motion → brain | 6 bytes，见下表 |

Status 沿用请求的 Sequence。brain 只接受当前待处理请求的响应。
自动查询与网页手动查询共享一个在途请求；每 500 ms 可重新查询。
小脑 1500 ms 没有有效响应时，brain 将其显示为离线。

| Status payload 偏移 | 内容 |
| --- | --- |
| 0–3 | 小脑 uptime，uint32 毫秒 |
| 4 | 状态：`0 = not_configured` |
| 5 | 能力位：bit 0 电机已接入、bit 1 传感器已接入；本阶段均为 0 |

小脑 uptime 是小脑本地时钟，不与大脑时钟比较。它不表示物理电机是否停止。

## 大脑网页接口

- `GET /`：固件内嵌页面。
- `GET /api/status`：读取大脑缓存的链路状态，不阻塞等待 UART。
- `POST /api/query`：触发或合并一次小脑查询；HTTP 202 仅表示请求已排入发送路径。

页面通过后续状态响应展示结果。链路离线时不展示缓存运行时间为实时值。

## 后续扩展，尚未实现

下一步先添加 `SET_PARAM`、`RUN_STAGE` 与 `STOP_ALL`，形成最小电机调试闭环。

- 参数在小脑 RAM 应用；只在 idle 时更新，参数保存到小脑 NVS。
- 执行动作先返回 `ACK` 接受/拒绝，再返回 `RESULT` 表示实际完成/失败。
- 状态包括当前动作、内部阶段、位置/速度/电流；大脑不重复推导机械状态。
- 忙碌时拒绝新的运动命令；停止优先。不做运动命令自动重试或排队。
- 大脑本地维护 App/云端信息，小脑只接收执行所需参数。

完整配方入口和更多调试指令按后续用户反馈增加，不预先实现空接口。
