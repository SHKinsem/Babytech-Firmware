# 控制板命名与兼容标识

项目面向人的板名统一为 **主控板**（`main-controller`）和 **设备板**（`device-controller`）。旧讨论中的 `brain` 对应主控板，`motion` 对应设备板。产品仓库 `Embeded_System/DisplayController` 中的板卡称为 **显示板**；它是设备板的另一个 UART 对端，不是主控板的新名字。文档叙述、接线说明和新接口优先使用这三个名称。

| 用途 | 主控板 | 设备板 | 显示板 |
| --- | --- | --- | --- |
| 当前固件目录 | `main-controller/` | `device-controller/` | 产品仓库 `Embeded_System/DisplayController/` |
| 职责 | 交互、网络与主控板状态页 | 电机、传感器、执行和台架流程 | 产品显示与用户输入 |
| 设备板 UART 协议 | 主控板协议 v2，须显式选择 | 一次连接一个对端 | 显示板协议 v3，设备板当前默认选择 |

设备板只有一个用于上述协议的 UART 对端选择。默认 `device-controller/platformio.ini` 使用 `-DDEVICE_UART_PEER=2`，即显示板协议；与主控板联调时，须将构建标志改为 `-DDEVICE_UART_PEER=1` 后重新编译并烧录设备板。头文件中的取值名称为 `DEVICE_UART_PEER_MAIN=1` 和 `DEVICE_UART_PEER_DISPLAY=2`。旧 `MOTION_UART_PEER` 与旧枚举名保留为源码/构建兼容别名；若同时指定新旧选择宏，二者必须一致，不能在默认 `DEVICE_UART_PEER=2` 之外只叠加 `MOTION_UART_PEER=1`。

以下标识涉及已部署设备、构建工具、外部客户端或历史证据，本轮**不改其实际值**：

| 兼容标识 | 保留内容与原因 |
| --- | --- |
| OTA manifest board ID | 主控板 `brain`、设备板 `motion`；已经安装的 bootstrap 以此校验身份 |
| PlatformIO 环境与产物路径 | `[env:brain]`、`[env:motion]`，原生 `main-controller/.pio/build/brain`、`device-controller/.pio/build/motion`，WSL `out/wsl/brain`、`out/wsl/motion` |
| HTTP 与持久化数据 | 已有 `/api/...` 路径、表单/查询键、JSON 字段和 NVS 键保持原样；新板名不触发数据迁移 |
| Wi-Fi SSID | `Babytech-Debug`、`Babytech-Motion` 保持不变，避免已配置的客户端失联 |
| 线缆协议 | UART v2/v3 的帧编码、版本和字段，以及 CAN 报文保持不变；改名不表示协议互通 |
| 源码与工具旧名 | `motion::` 命名空间、旧宏别名、历史脚本名 `package-motion.py` 等在迁移期保留 |

原生构建使用 `pio run -d main-controller` 与 `pio run -d device-controller`。WSL `-Target` 同时接受新目录名与旧别名。OTA 打包的 `--board` 接受新目录名和旧 board ID，但签名 manifest 仍使用 `brain`/`motion`。曾以 `main-controller`/`device-controller` 作为 OTA 身份制作的开发包属于另一身份配置，不能与本仓库当前身份包互换；首次部署前核对实板 `/api/ota/status`，必要时按明确安排通过 USB 更新 bootstrap。

离线交付使用 `python tools/package-device.py --release <发布标识>`；`package-motion.py` 保留同参数兼容入口。工具验证当前网页已嵌入固件，不覆盖既有交付包，也不烧录设备。历史验证记录中的旧名称、提交 SHA 与 `out/wsl` 路径是当时证据，继续保留原样。
