# Babytech Firmware

两块 ESP32-S3 N16R8 的最小固件仓库。每块板是独立、标准的 PlatformIO Arduino 工程。

```text
浏览器 / 后续 App 与云端
          │ Wi-Fi
 brain：显示与网络 MCU
          │ 3.3 V UART
 motion：传感器与电机 MCU
```

## 当前可运行的内容

当前提供两条调试路径：brain 的 UART 状态页，以及 motion 独立热点的 CAN 电机调试页。

- `brain/`：`Babytech-Debug` 热点与中文状态页，使用 v2 四指令查询状态、修改 RAM 参数、显式使能、执行单电机阶段和停止。
- `motion/`：`Babytech-Motion` 热点、可保存的路由器 Wi-Fi 配置、内嵌网页和 HTTP API；任意 CAN ID 1..255 的使能、失能、相对运动、停止、广播停止，以及真实位置/速度/电流反馈。下板默认从 GPIO1/2 采样 HX711，网页可修改并持久化 DOUT/SCK，同时提供称重状态、去皮和标定 API。
- `shared/BoardProtocol/`：板间 UART v2 四指令协议、客户端与执行入口。完成状态来自真实电机反馈；机构尚未接入，称重数据暂未加入板间载荷。
- `tools/test_protocol.py`、`tools/test_motion.py`：主机协议、参数与反馈解析检查。

调试热点暂放下板，便于独立台架调试；未来网络和显示由 brain 承担。当前只接入 HX711 称重，尚未接入其他传感器、LCD/触摸、云端、机构动作或多电机联动。上电不发送使能或运动指令。

详细操作和接口见 [下板网页调试](docs/motion-debug.md)。

新版桌面协议工作台已接入真实电机接口与 Wi-Fi，网页随固件内嵌。最新能力范围、重建、烧录地址和验证边界见 [工作台交付说明](docs/motion-workbench-release.md)。

可选 DISPLAY 分支已接入产品显示板 UART v3、非阻塞流程、阶段调试和 JSON 配置。采用“调试用内存、演示用内置 JSON”，默认脚本未配置，上电不运动。见 [演示操作说明](docs/motion-display-demo.md) 与 [开发验收计划](docs/motion-display-demo-plan.md)。默认 BRAIN 构建仍使用 Brain/Motion v2；机械与双板实机验收尚未完成。

## 目录

```text
brain/       platformio.ini、src/、include/、lib/、test/、data/
motion/      platformio.ini、src/、include/、lib/、data/
shared/      两块板共同依赖的 BoardProtocol 库
docs/        板间协议和范围
tools/       主机验证工具
```

用 PlatformIO 打开 `brain` 或 `motion` 文件夹，分别编译。顶层没有第三个固件工程。
库通过相对路径引用，新仓库不依赖旧仓库路径。网页分别嵌入对应固件，不需要单独烧录文件系统。

## WSL 编译（推荐）

Windows 保留源码，编译在 WSL 的 Linux 文件系统进行：

```powershell
./tools/build-wsl.ps1
./tools/build-wsl.ps1 -Target motion
./tools/build-wsl.ps1 -Target brain
```

产物自动写回 `out/wsl/`，WSL 保留增量编译缓存。首次环境和路径说明见 [WSL 编译](docs/wsl-build.md)。
脚本不烧录设备。下面的原生 Windows 命令仍可作为备用方式。

## 编译与验证

工具链固定为 `espressif32@6.4.0`，对应当前电脑已安装的 Arduino-ESP32 2.0.11。
使用 PlatformIO 标准 `esp32-s3-devkitc-1` 板型，并覆盖为 16 MB Flash / 8 MB OPI PSRAM。
这不是 ESP-IDF 工程；目前无需为了 FreeRTOS 改换框架。

在仓库根目录执行：

```text
pio run -d motion
pio run -d brain
python tools/test_protocol.py
python tools/test_motion.py
```

主机测试需要 `g++` 在 PATH 中，也可通过 `CXX` 指定兼容编译器。

## 接线

两块板的实际 GPIO 定义在各自 `include/board_config.h` 中：

| brain GPIO | motion GPIO |
| --- | --- |
| 43 / TX | 44 / RX |
| 44 / RX | 43 / TX |
| GND | GND |

UART 为 115200、8N1、3.3 V TTL。按实际 GPIO 连接；旧屏幕排针的 TX/RX 丝印可能按外部连接视角标注。
两板分别 USB 供电时共地，不互接 5V。只验 UART 时无需给电机上电。CAN 电机调试需要外接 CAN 收发器（TX GPIO4 / RX GPIO5，500 kbit/s）、共地、正确终端电阻和独立电机电源。

## 实机使用

确认端口对应的板子后分别烧录，例如：

```text
pio run -d motion -t upload --upload-port COM_MOTION
pio run -d brain -t upload --upload-port COM_BRAIN
```

`COM_MOTION` / `COM_BRAIN` 是占位符，替换为实际端口。

1. 连接 Wi-Fi `Babytech-Debug`，开发热点密码为 `babytech-demo`。
2. 浏览器打开 `http://192.168.4.1/`。
3. 点击「查询小脑状态」，应看到小脑在线、运行时间和响应计数更新。
4. 拔掉小脑 USB，约 1.5 秒后页面应显示小脑未连接，运行时间变为 `—`。
5. 恢复供电，应自动恢复在线，运行时间从新启动开始计数。

上面是 brain 状态页。调试电机时改连 `Babytech-Motion`（同一开发密码），访问 `http://192.168.4.1/`，输入驱动器 CAN ID，读取反馈后显式使能并试动。HTTP 202 只代表板卡已提交指令，不代表电机已执行。

## 接下来只做这些

1. 对当前 CAN 网页调试完成实机验收，再接入机构动作 Runtime。
2. 实机验收已实现的 UART「改 RAM 参数 → 单电机阶段 → 返回结果 → 停止」闭环，见 [v2 指令表](docs/protocol-v2.md)。
3. 将下板称重状态加入板间协议；下板调试页已接入称重、漂移诊断、去皮和标定。
4. 迁入已验证的屏幕/触摸驱动，在大脑上显示同一份小脑状态。
5. 根据真实调试反馈补齐开盖、关盖、混合与参数保存。

App、Cloud、多家庭权限、复杂恢复和全量测试平台不进入本阶段。
未来大脑负责页面和网络，小脑独占机械状态机与驱动；网络回调不直接操作电机。

## 来源与迁移边界

- 原项目：`hellowenshenghui/Babytech_Formula_Device`，参考本地 `V1-device` 的 `2ffcde2`。
- 网页交互参考：`SHKinsem/Project-Tenny`。
- CAN 传输库和 HX711 称重核心分别从旧工程 BabytechActuatorHal、BabytechSensorHal 按需迁入 `motion/lib/`；新工程不依赖旧仓库路径，旧仓库未修改。
- 新 UART v2 四指令协议要求上下板一起更新；不能与旧 v1 或 DisplayController/Product 协议混用。

Wi-Fi OTA 的设计与操作见 [实施计划](docs/wifi-ota-plan.md) 和 [使用说明](docs/wifi-ota-implementation.md)。

详见 [当前板间协议 v2](docs/protocol-v2.md)；[v1 文档](docs/protocol.md) 仅供历史参考。

## 验证记录（2026-09-21）

- UART v2 最小闭环：READ / WRITE / EXEC / STOP 已接通上下板，包括单电机阶段 RAM 参数、显式使能/失能、执行结果与优先停止。
- 21:20（香港时间）WSL 双板编译通过：motion Flash 1075561 bytes、RAM 72368 bytes；brain Flash 735717 bytes、RAM 45392 bytes。motion 构建已包含 HX711、NVS 标定和称重 API；产物位于 `out/wsl/motion/` 与 `out/wsl/brain/`，两板须一起更新到 v2。
- 协议测试：v1 回归及 v2 分段/粘包、CRC、批量写原子性、重复请求去重、丢回复结果补查、断线停止、重启恢复和查询限速通过。
- MotionCore 133 项检查、称重处理器 3 组场景、电机控制器 679 项检查，以及 UART 执行入口与真实控制器的模拟 CAN 联调通过；使能/失能缺 ACK、停止未确认不会报告成功。
- 上板嵌入页面 Playwright 测试通过，覆盖无自动动作、参数版本、未应用编辑、异步结果、停止、离线及重启。下板工作台的既有验证记录见其交付文档。
- 未进行硬件烧录、实际 UART 接线验收或浏览器实机测试。
