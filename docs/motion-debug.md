# Motion 调试接口

描述对象：当前 Motion 工作台网络、接线和基础 HTTP/HX711 接口。

Milestone：V1，`V1-device`，Brain/Motion UART v4。产品命令从 Brain 经 UART v4 到 Motion；本页是独立台架调试，不描述旧六字节 UART 状态或旧 Brain 网页。队列/同步见 [队列说明](motor-queue.md)，网页流程见 [流程调试](motion-display-demo.md)，交付见 [工作台说明](motion-workbench-release.md)。

## 网络与接线

Motion 使用 AP+STA。热点 `Babytech-Motion`，开发密码 `babytech-demo`，默认地址 `http://192.168.4.1/`；保存路由器配置后也可使用 STA 地址。没有互联网转发，不依赖 CDN 或外部网页服务。

| 信号 | GPIO/设置 |
| --- | --- |
| UART TX/RX | 43/44，115200、8N1、3.3V TTL |
| CAN TX/RX | 4/5，500 kbit/s，外接 3.3V 逻辑收发器 |
| HX711 DOUT/SCK | 默认 1/2，可通过网页保存 IO 配置 |

ESP32-S3 没有片上 CAN 收发器。控制板、收发器和驱动共地；总线两端各有 120Ω 终端，断电后跨 CANH/CANL 通常约 60Ω。避免长分支，先检查供电、接线、`busState` 和错误计数再试动。分别 USB 供电的 Brain/Motion 不互接 5V；按实际 GPIO，不凭排针 TX/RX 丝印猜方向。

## 操作原则

- 上电、读取、恢复草稿、切换页面不自动使能或运动；停止/失能会影响机构保持力，须由操作者明确执行。
- HTTP 202 只表示受理；ACK 表示驱动回复，不等于机械到位或停止。缺失/过期数值为 null，不是零。
- 同一受监督动作、普通直发队列、sync 和产品流程各自有所有权与完成判定，不能把一个入口的状态用作另一个入口的许可。
- 断线后只读恢复状态，不重发结果未知的 POST；关网页不取消已接受的运动。Stop 入口保留，HTTP/无线不是硬件急停。
- 调试工作台只在可信本地网络使用；不因 Cloud 暂时离线或没有产品配对全局关闭调试。

POST 通常使用 `application/x-www-form-urlencoded`，响应 JSON 的 `ok/message` 与 HTTP 状态反映受理或拒绝。写入前校验完整数字和协议布局；未知功能码只在明确的队列 raw 入口按原字节发送。

## 基础接口

| 接口 | 用途 |
| --- | --- |
| `GET /api/status?id=1` | 当前 CAN、控制器、位置/速度/电流、目标和新鲜度；不是整机产品 Ready |
| `POST /api/enable` | `id/enabled`，显式使能或失能 |
| `POST /api/move` | `id/angle/speed/accel/decel/current`，相对当前位置 CD 运动，不自动使能 |
| `POST /api/command` | 指令实验室的完整逻辑 HEX，板端再次校验并监督受支持动作 |
| `POST /api/stop` / `/api/stop-all` | 停止目标/当前所有者或广播停止；继续检查新停稳反馈 |
| `GET /api/trace` | 实际 TX/RX、序号和时间，有限环形记录会覆盖 |
| `GET/POST /api/limits` | 读取/显式保存调试策略，首次默认值不是永久上限 |
| `GET /api/config-result` | 参数写入的提交、读回与差异；不是新的动作完成证明 |

常规 `/api/move` 要求已确认使能、新鲜静止反馈、无冲突/故障；参数范围受已保存调试策略和协议字段约束。普通队列不套用这套试动策略，显式 await 和 sync 另有监督。FB/CB 的模式、限流边界和 `0x33/0x34` 区别见 [直通位置](direct-position-implementation.md)。

`enabled` 是控制器已确认的状态，不是点击成功。广播发送无法逐台证明应答；旧缓存、CAN TX 成功或软件清状态不能证明未知节点已停。停止后需要新反馈，缺失就保留未确认状态，不能通过自动使能或重放旧动作恢复。

## HX711

称重在 Motion 采样，可用于调试与产品资源投影；尚无按重量停止出粉闭环。一个 HX711 通道呈现合成重量，不拆成没有依据的两路传感器数据。

| 接口 | 用途 |
| --- | --- |
| `GET /api/scale` | IO、available/calibrated/stable、原始和换算值、采样年龄、去皮/标定状态；无效数值为 null |
| `POST /api/scale/config` | `doutPin/sckPin`，保存到 `scale-io` 并重新初始化 |
| `POST /api/scale/tare` | 空载且电机静止时启动异步 16 样本去皮 |
| `POST /api/scale/calibrate` | `knownWeightG`，1–5000g；要求本次启动去皮已完成、读数新鲜 |

先轮询确认去皮完成，再放已知砝码标定。标定结果保存到 `scale-cfg`；保存成功不是传感器精度或实际剂量验收。

IO 允许 GPIO1、2、6–18、21、38–42、47、48；DOUT/SCK 不同。GPIO45 仅在固件确认 Flash 供电 eFuse 固定为 3.3V 时开放。CAN、UART、USB、启动绑带及外部 Flash/PSRAM 引脚保留，不能用换 IO 绕过。

产品 `BABYTECH_V1_MOTOR_TEST=1` 的 normal/300g 是测试占位，与此处真实 HX711 读数不是同一证明；温度仍未知，不虚构测温或温控。

## Wi-Fi

网页支持扫描、手动/隐藏 SSID、保存连接、读取 STA 地址和忘记网络；空密码表示开放网络，不支持企业认证。保存到 NVS，失败按现有重连策略恢复，运动/冲突期间改网遵守既有检查；不为改网重装配对或产品配置。

| 接口 | 用途 |
| --- | --- |
| `GET /api/wifi` | 当前网络与扫描状态，不返回密码 |
| `POST /api/wifi/connect` | `ssid/password` 保存并尝试连接 |
| `POST /api/wifi/forget` | 只删除本模块 Wi-Fi 配置 |
| `POST /api/wifi/scan` / `GET /api/wifi/scan` | 启动扫描/读取结果 |

AP+STA 共用单无线电，扫描或 STA 切信道可能暂时断开热点页面。受理不等于连接成功，保存不等于 MQTT 认证；网络恢复不授权重放运动。Motion 是独立工作台网络，产品 MQTT 由 Brain 负责。当前维护网络为 HTTP，不宣称 TLS 已启用；Motion OTA 的隔离要求见 [OTA 说明](wifi-ota-implementation.md)。

## 验证边界

相关入口为 `tools/test_motion.py`、`tools/test_raw_can.py`、`tools/test_demo.py`和工作台 `npm test`；浏览器 mock 只证明界面与模拟 API 交互。已有实机运动成功不等于全部 CAN 工况、称重精度、物理停止、无线切换或传感器闭环验收。真实测试先确认急停、行程、动力与安全区域，版本及参数单独记录，不把未来硬件计划当作普通启动的新门禁。
