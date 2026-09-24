# Babytech Firmware

两块 ESP32-S3 N16R8 的最小固件仓库。每块板是独立、标准的 PlatformIO Arduino 工程。

```text
浏览器 / 后续 App 与云端
          │ Wi-Fi
 main-controller：显示与网络 MCU
          │ 3.3 V UART
 device-controller：传感器与电机 MCU
```

## 功能与范围

当前提供两条调试路径：main-controller 的 UART 状态页，以及 device-controller 独立热点的 CAN 电机调试页。

- `main-controller/`：`Babytech-Debug` 热点与中文状态页，使用 v2 四指令查询状态、修改 RAM 参数、显式使能、执行单电机阶段和停止。
- `device-controller/`：`Babytech-Motion` 热点、可保存的路由器 Wi-Fi 配置、内嵌网页和 HTTP API；任意 CAN ID 1..255 的使能、失能、相对运动、停止、广播停止，以及真实位置/速度/电流反馈。设备控制板默认从 GPIO1/2 采样 HX711，网页可修改并持久化 DOUT/SCK，同时提供称重状态、去皮和标定 API。
- `shared/BoardProtocol/`：板间 UART v2 四指令协议、客户端与执行入口。完成状态来自真实电机反馈；机构尚未接入，称重数据暂未加入板间载荷。
- `tools/test_protocol.py`、`tools/test_motion.py`：主机协议、参数与反馈解析检查。

调试热点暂放设备控制板，便于独立台架调试；未来网络和显示由 main-controller 承担。当前只接入 HX711 称重，尚未接入其他传感器、LCD/触摸、云端、机构动作或多电机联动。上电不发送使能或运动指令。

详细操作和接口见 [设备控制板网页调试](docs/motion-debug.md)。

编排队列支持逐行发送和显式等待，详见 [队列用法](docs/motor-queue.md)。多轴同步与螺旋联动尚未实现。

## 目录

```text
main-controller/    主控板工程
device-controller/  设备控制板工程
shared/             两块板共同依赖的 BoardProtocol 库
docs/               使用说明、协议、实施计划与验证记录
tools/              构建、打包与主机验证工具
```

用 PlatformIO 打开 `main-controller` 或 `device-controller` 文件夹，分别编译。顶层没有第三个固件工程。
库通过相对路径引用，新仓库不依赖旧仓库路径。网页分别嵌入对应固件，不需要单独烧录文件系统。

## WSL 编译（推荐）

Windows 保留源码，编译在 WSL 的 Linux 文件系统进行：

```powershell
./tools/build-wsl.ps1
./tools/build-wsl.ps1 -Target device-controller
./tools/build-wsl.ps1 -Target main-controller
```

产物自动写回 `out/wsl/`，WSL 保留增量编译缓存。首次环境和路径说明见 [WSL 编译](docs/wsl-build.md)。
脚本不烧录设备。下面的原生 Windows 命令仍可作为备用方式。

## 编译与验证

工具链固定为 `espressif32@6.4.0`，使用 Arduino-ESP32 2.0.11。
使用 PlatformIO 标准 `esp32-s3-devkitc-1` 板型，并覆盖为 16 MB Flash / 8 MB OPI PSRAM。

在仓库根目录执行：

```text
pio run -d device-controller
pio run -d main-controller
python tools/test_protocol.py
python tools/test_motion.py
```

主机测试需要 `g++` 在 PATH 中，也可通过 `CXX` 指定兼容编译器。

## 接线

两块板的实际 GPIO 定义在各自 `include/board_config.h` 中：

| main-controller GPIO | device-controller GPIO |
| --- | --- |
| 43 / TX | 44 / RX |
| 44 / RX | 43 / TX |
| GND | GND |

UART 为 115200、8N1、3.3 V TTL。按实际 GPIO 连接；旧屏幕排针的 TX/RX 丝印可能按外部连接视角标注。
两板分别 USB 供电时共地，不互接 5V。只验 UART 时无需给电机上电。CAN 电机调试需要外接 CAN 收发器（TX GPIO4 / RX GPIO5，500 kbit/s）、共地、正确终端电阻和独立电机电源。

## 实机使用

确认端口对应的板子后分别烧录，例如：

```text
pio run -d device-controller -t upload --upload-port COM_DEVICE
pio run -d main-controller -t upload --upload-port COM_MAIN
```

`COM_DEVICE` / `COM_MAIN` 是占位符，替换为实际端口。

1. 连接 Wi-Fi `Babytech-Debug`，开发热点密码为 `babytech-demo`。
2. 浏览器打开 `http://192.168.4.1/`。
3. 点击「查询设备控制板状态」，应看到设备控制板在线、运行时间和响应计数更新。
4. 拔掉设备控制板 USB，约 1.5 秒后页面应显示设备控制板未连接，运行时间变为 `—`。
5. 恢复供电，应自动恢复在线，运行时间从新启动开始计数。

上面是 main-controller 状态页。调试电机时改连 `Babytech-Motion`（同一开发密码），访问 `http://192.168.4.1/`，输入驱动器 CAN ID，读取反馈后显式使能并试动。HTTP 202 只代表板卡已提交指令，不代表电机已执行。

## 来源与迁移边界

- 原项目：`hellowenshenghui/Babytech_Formula_Device`，参考本地 `V1-device` 的 `2ffcde2`。
- 网页交互参考：`SHKinsem/Project-Tenny`。
- CAN 传输库和 HX711 称重核心分别从旧工程 BabytechActuatorHal、BabytechSensorHal 按需迁入 `device-controller/lib/`；新工程不依赖旧仓库路径，旧仓库未修改。
- 新 UART v2 四指令协议要求主控板与设备控制板一起更新；不能与旧 v1 或 DisplayController/Product 协议混用。

详见 [当前板间协议 v2](docs/protocol-v2.md)；[v1 文档](docs/protocol.md) 仅供历史参考。

## 文档导航

| 文档 | 内容 |
| --- | --- |
| [仓库整合与协作计划](docs/integration-plan.md) | 分支现状、PR 依赖、整合顺序、协作者分工与验收清单 |
| [设备控制板网页调试](docs/motion-debug.md) | 基础 API、接线与问题排查 |
| [电机编排队列](docs/motor-queue.md) | 已实现的指令、参数与执行语义 |
| [板间协议 v2](docs/protocol-v2.md) | UART 指令与主控板与设备控制板接口 |
| [工作台交付说明](docs/motion-workbench-release.md) | 网页构建、烧录与交付边界 |
| [WSL 编译](docs/wsl-build.md) | 编译环境与构建步骤 |
| [Wi-Fi 固件升级计划](docs/wifi-ota-plan.md) | 双板 OTA 可行性、实施步骤与验收条件 |
| [Wi-Fi 固件升级使用说明](docs/wifi-ota-implementation.md) | 首次安装、签名打包与台架验收边界 |
| [电机修复与同步计划](docs/motor-sync-plan.md) | 待实现方案与验收标准 |
| [开发与验证记录](docs/development-notes.md) | 阶段安排及历史验证结果 |
| [控制板命名与迁移](docs/controller-naming.md) | 新旧工程名、构建入口与兼容边界 |
