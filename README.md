# Babytech Firmware

## 当前主线（2026-09-24）

[PR #12](https://github.com/SHKinsem/Babytech-Firmware/pull/12) 已合入 `main`，包含同步运动、命令反馈、紧凑工作台、Wi-Fi OTA 和控制器目录统一。主线现作为协作者共同调试的基线。

**sync 优先级最高，尤其要保留 9 月 23 日实机验证过的行为。** 已记录双轴反向三圈完成、触发后目标确认及查询反馈结果；完整工况与边界见 [同步实机记录](docs/motion-sync-development.md#2026-09-23-同步实机补充)。整合后的镜像仍需复验，不能把历史实测或软件测试等同于最终机械验收。

- 已接入：同步组与查询预算、真实命令反馈、单位输入修复、部分跨入口 disable 修复、OTA 软件实现。
- 待跟进：[控制状态问题 #6](https://github.com/SHKinsem/Babytech-Firmware/issues/6) 的剩余策略与实测、[OTA #3](https://github.com/SHKinsem/Babytech-Firmware/issues/3) 的签名/健康确认/回滚验收。
- **mDNS 尚未实现**，由 [#4](https://github.com/SHKinsem/Babytech-Firmware/issues/4) 跟踪；目前通过热点地址或路由器分配的 IP 访问。

具体交接见 [整合进度与待验收项](docs/integration-progress-20260924.md)，兼容标识见 [控制板命名](docs/controller-naming.md)。[原始整合计划](docs/integration-plan.md) 保留整合前快照，不代表当前分支和 PR 状态。

## 协作者开始调试

确认工作区干净后更新主线，再建立自己的调试分支：

```bash
git switch main
git pull --ff-only origin main
git switch -c codex/your-task
```

如果本地 main 已分叉，先保留独有工作再整合，不要强制覆盖。优先复验昨晚相同的 sync 程序与参数；修改反馈轮询、目标确认、查询预算或停止判定时，记录提交 SHA、构建配置、镜像哈希与实测结果。其他功能的整合不得静默改变这些行为。

两块 ESP32-S3 N16R8 的最小固件仓库。每块板是独立、标准的 PlatformIO Arduino 工程。

```text
浏览器 / 后续 App 与云端
          │ Wi-Fi
 main-controller：显示与网络 MCU
          │ 3.3 V UART
 device-controller：传感器与电机 MCU
```

## 当前可运行的内容

当前提供两条调试路径：主控板的 UART 状态页，以及设备控制板独立热点的 CAN 电机调试页。

- `main-controller/`：`Babytech-Debug` 热点与中文状态页，使用 v2 四指令查询状态、修改 RAM 参数、显式使能、执行单电机阶段和停止。
- `device-controller/`：`Babytech-Motion` 热点、可保存的路由器 Wi-Fi 配置、内嵌网页和 HTTP API；任意 CAN ID 1..255 的使能、失能、相对运动、停止、广播停止，以及真实位置/速度/电流反馈。下板默认从 GPIO1/2 采样 HX711，网页可修改并持久化 DOUT/SCK，同时提供称重状态、去皮和标定 API。
- `shared/BoardProtocol/`：板间 UART v2 四指令协议、客户端与执行入口。完成状态来自真实电机反馈；机构尚未接入，称重数据暂未加入板间载荷。
- `tools/test_protocol.py`、`tools/test_motion.py`：主机协议、参数与反馈解析检查。

调试热点暂放下板，便于独立台架调试；未来网络和显示由 brain 承担。当前只接入 HX711 称重，尚未接入其他传感器、LCD/触摸或云端；同步组与 DISPLAY 演示已有软件实现，完整机械流程仍待实机验收。上电不发送使能或运动指令。

详细操作和接口见 [下板网页调试](docs/motion-debug.md)。

直发队列、执行诊断、全局查询预算及同步/螺旋动作见 [电机编排队列](docs/motor-queue.md)。软件验证和待实测项目见 [同步开发与验收记录](docs/motion-sync-development.md)；已完成部分低速、小幅及多圈同步实测，完整负载与机械验收仍待完成。

新版桌面协议工作台已接入真实电机接口与 Wi-Fi，网页随固件内嵌。最新能力范围、重建、烧录地址和验证边界见 [工作台交付说明](docs/motion-workbench-release.md)。

默认 DISPLAY 构建已接入产品显示板 UART v3、非阻塞流程、阶段调试和内置 JSON 配置；上电不自动运动。见 [演示操作说明](docs/motion-display-demo.md) 与 [开发验收计划](docs/motion-display-demo-plan.md)。Brain/Motion v2 需显式以 `-DMOTION_UART_PEER=1` 编译；机械与双板实机验收尚未完成。

## 目录

```text
main-controller/       platformio.ini、src/、include/、lib/、test/、data/
device-controller/      platformio.ini、src/、include/、lib/、data/
shared/                BoardProtocol、BabytechDisplayCore、WifiOta
docs/        板间协议和范围
tools/       主机验证工具
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

工具链固定为 `espressif32@6.4.0`，对应 Arduino-ESP32 2.0.11。
使用 PlatformIO 标准 `esp32-s3-devkitc-1` 板型，并覆盖为 16 MB Flash / 8 MB OPI PSRAM。
这不是 ESP-IDF 工程；目前无需为了 FreeRTOS 改换框架。

在仓库根目录执行：

```text
pio run -d device-controller
pio run -d main-controller
python tools/test_protocol.py
python tools/test_motion.py
python tools/test_raw_can.py
python tools/test_demo.py
```

主机测试需要 `g++` 在 PATH 中，也可通过 `CXX` 指定兼容编译器。目录已改名，但 PlatformIO 环境仍为 `brain` / `motion`，OTA board ID 和 WSL 导出目录也保留这些兼容标识。

修改工作台后，先重建内嵌页面，再编译设备固件：

```bash
cd tools/motor-protocol-demo
npm ci
npm test
npm run build:device
cd ../..
pio run -d device-controller
```

GitHub Actions 执行前端与主机回归，并编译主控、设备 BRAIN 和 DISPLAY 配置，见 [CI 配置](.github/workflows/firmware-checks.yml)。浏览器 mock 测试与编译通过不代表实机验收完成。

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
pio run -d device-controller -t upload --upload-port COM_MOTION
pio run -d main-controller -t upload --upload-port COM_BRAIN
```

`COM_MOTION` / `COM_BRAIN` 是占位符，替换为实际端口。

1. 连接 Wi-Fi `Babytech-Debug`，开发热点密码为 `babytech-demo`。
2. 浏览器打开 `http://192.168.4.1/`。
3. 点击「查询小脑状态」，应看到小脑在线、运行时间和响应计数更新。
4. 拔掉小脑 USB，约 1.5 秒后页面应显示小脑未连接，运行时间变为 `—`。
5. 恢复供电，应自动恢复在线，运行时间从新启动开始计数。

上面是 brain 状态页。调试电机时改连 `Babytech-Motion`（同一开发密码），访问 `http://192.168.4.1/`，输入驱动器 CAN ID，读取反馈后显式使能并试动。HTTP 202 只代表板卡已提交指令，不代表电机已执行。

## 后续工作优先级

1. 优先复验主线 sync，保住昨晚验证的触发后目标确认、查询预算、目标窗口和真实完成判定，再扩展负载与机构流程。
2. 实机验收已实现的 UART「改 RAM 参数 → 单电机阶段 → 返回结果 → 停止」闭环，见 [v2 指令表](docs/protocol-v2.md)。
3. 将下板称重状态加入板间协议；下板调试页已接入称重、漂移诊断、去皮和标定。
4. 迁入已验证的屏幕/触摸驱动，在大脑上显示同一份小脑状态。
5. 根据真实调试反馈补齐开盖、关盖、混合与参数保存。

App、Cloud、多家庭权限、复杂恢复和全量测试平台不进入本阶段。
未来大脑负责页面和网络，小脑独占机械状态机与驱动；网络回调不直接操作电机。

## 来源与迁移边界

- 原项目：`hellowenshenghui/Babytech_Formula_Device`，参考本地 `V1-device` 的 `2ffcde2`。
- 网页交互参考：`SHKinsem/Project-Tenny`。
- CAN 传输库和 HX711 称重核心分别从旧工程 BabytechActuatorHal、BabytechSensorHal 按需迁入 `device-controller/lib/`；新工程不依赖旧仓库路径，旧仓库未修改。
- 新 UART v2 四指令协议要求上下板一起更新；不能与旧 v1 或 DisplayController/Product 协议混用。

Wi-Fi OTA 的设计与操作见 [实施计划](docs/wifi-ota-plan.md) 和 [使用说明](docs/wifi-ota-implementation.md)。

详见 [当前板间协议 v2](docs/protocol-v2.md)；[v1 文档](docs/protocol.md) 仅供历史参考。

## 验证记录

2026-09-24 整合：协议、运动、raw CAN、demo 主机回归通过；前端 92/92、Sites 4/4、OTA 页面 3/3、刷写工具模拟测试 8/8、设备浏览器 QA 14/14 通过。主控与设备 BRAIN/DISPLAY 固件编译通过，最终页面嵌入校验通过；代码提交 `754e893` 的远端 push/PR CI 均通过。实机证据另见上述同步记录，本轮整合未烧录设备。

### 历史记录（2026-09-21）

以下是当日结果，体积和检查数量不代表当前固件：

- UART v2 最小闭环：READ / WRITE / EXEC / STOP 已接通上下板，包括单电机阶段 RAM 参数、显式使能/失能、执行结果与优先停止。
- 21:20（香港时间）WSL 双板编译通过：motion Flash 1075561 bytes、RAM 72368 bytes；brain Flash 735717 bytes、RAM 45392 bytes。motion 构建已包含 HX711、NVS 标定和称重 API；产物位于 `out/wsl/motion/` 与 `out/wsl/brain/`，两板须一起更新到 v2。
- 协议测试：v1 回归及 v2 分段/粘包、CRC、批量写原子性、重复请求去重、丢回复结果补查、断线停止、重启恢复和查询限速通过。
- MotionCore 133 项检查、称重处理器 3 组场景、电机控制器 679 项检查，以及 UART 执行入口与真实控制器的模拟 CAN 联调通过；使能/失能缺 ACK、停止未确认不会报告成功。
- 上板嵌入页面 Playwright 测试通过，覆盖无自动动作、参数版本、未应用编辑、异步结果、停止、离线及重启。下板工作台的既有验证记录见其交付文档。
- 未进行硬件烧录、实际 UART 接线验收或浏览器实机测试。
