# Babytech 固件

描述对象：当前 Brain/Motion 固件、Motion 工作台和本地构建工具。

Milestone：V1，维护分支 `V1-device`，唯一产品链路为成对 UART v4。本文说明当前入口，不替代整机、食品安全或生产验收，不授权烧录和实际运动。历史开发过程从 Git 和下方有日期的记录查阅。

## 当前结构

```text
App -> Cloud -> MQTT -> main-controller（Brain：屏幕、网络、业务）
                            |
                          UART v4
                            |
                       device-controller（Motion：电机、资源、执行）
```

| 工程 | 当前能力 | 边界 |
| --- | --- | --- |
| `main-controller` / `brain` | LVGL 屏幕、触摸、Wi-Fi/MQTT、临时 AP 配网、本地产品请求、配置及结果转发 | 不恢复旧 `Babytech-Debug` 网页；当前没有 Brain OTA |
| `device-controller` / `motion` | 产品执行、CAN 工作台、队列/同步、HX711、独立 AP+STA 和签名 OTA | 不启动产品 MQTT；调试网页不要求 Brain 或 Cloud 在线 |

旧 Brain v3、Motion DISPLAY/v3 和 BRAIN/v2 固件入口已退役。Brain 的 `BABYTECH_BOARD_LINK_V4` 仅允许 1，Motion 的 `MOTION_UART_PEER` 仅允许 4；环境名仍为 `brain`、`motion`，不能混烧旧协议。保留旧 codec 单测和数据导入保护，不表示旧镜像仍可发布。

## 先读哪份说明

| 用途 | 文档 |
| --- | --- |
| CAN 工作台、称重、网络和 HTTP 接口 | [工作台使用说明](docs/motion-workbench-release.md)、[调试接口](docs/motion-debug.md) |
| 普通队列、显式 await、同步/螺旋动作 | [电机编排队列](docs/motor-queue.md) |
| Motion 网页流程、JSON、初始化与参考 | [网页流程调试](docs/motion-display-demo.md) |
| FB/CB 直通位置的目标和完成判定 | [直通位置说明](docs/direct-position-implementation.md) |
| Motion 签名 OTA | [OTA 使用说明](docs/wifi-ota-implementation.md) |
| WSL 编译、离线发布包和烧录 | [WSL 编译](docs/wsl-build.md)、[烧录流程](docs/flash-workflow.md)、[命名约定](docs/controller-naming.md) |
| 2026-09-23 限定同步实机证据 | [同步开发记录](docs/motion-sync-development.md#2026-09-23-同步实机补充) |

在完整产品仓库内，工厂安装、Cloud 更新和 App 操作统一见父仓库 [操作手册](../../Docs/Architect/OPERATION_MANUAL.md)。独立 clone 本固件仓库时不包含该手册；仍可独立构建，不能据此自行重做生产登记或迁移。

## 取得源码与构建

完整产品仓库使用父仓库锁定的 submodule 提交；不要为构建擅自拉动固件版本。只有独立维护本仓库时，才在确认无未保存修改后更新分支：

```bash
git fetch origin V1-device
git switch V1-device
git pull --ff-only origin V1-device
```

每一步失败就停止处理；不使用硬重置覆盖本地工作。开发分支从这一基线建立。

在本子仓库根目录编译，不连接设备：

```bash
pio run -d main-controller -e brain
pio run -d device-controller -e motion
```

Motion 使用 `espressif32@6.4.0` / Arduino-ESP32 2.0.11；Brain 使用 pioarduino `55.03.30-2` / Arduino-ESP32 3.3、LVGL 8.4.0。两板均为 ESP32-S3 N16R8，工具链和板型以各自 `platformio.ini` 为准，不为了统一版本互相升降级。顶层没有第三个固件工程。

Motion 网页由 `tools/motor-protocol-demo` 的源码生成。前端没有改动时，直接编译已提交的内嵌 HTML；改动后先执行：

```bash
cd tools/motor-protocol-demo
npm ci
npm test
npm run build:device
```

再回到固件根目录编译 Motion。不要手改 `device-controller/data/index.html`。Windows/WSL 只编译和导出，命令及产物见 [WSL 说明](docs/wsl-build.md)。

App/屏幕文案唯一手写来源是父仓库 `Shared/feeding_flow_ui/feeding_flow_ui.json`。在产品仓库构建 Brain 时，pre-build 调用父生成器；内容未变不重写 header。独立 clone 使用已提交的 `shared/BabytechDisplayCore/src/generated/feeding_flow_ui.h`，不下载文案，也不手改生成文件。

## 接线与普通升级

| Brain GPIO | Motion GPIO |
| --- | --- |
| 43 / TX | 44 / RX |
| 44 / RX | 43 / TX |
| GND | GND |

UART 为 115200、8N1、3.3V TTL；按 GPIO 接线，不凭屏幕排针的 TX/RX 丝印猜方向。分别 USB 供电时共地，不互接 5V。Motion CAN TX/RX 为 GPIO4/5、500 kbit/s，须外接收发器、正确终端电阻和独立电机供电。

普通升级保留 NVS，不重新安装、登记、配网或认领，也不逐次执行双板人工校验。经明确授权并核对端口后，可使用：

```text
pio run -d main-controller -e brain -t upload --upload-port COM_BRAIN
pio run -d device-controller -e motion -t upload --upload-port COM_MOTION
```

端口是占位符，必须替换为本次核对的目标。此方式会构建再上传；发布既有已验证包应按 [烧录流程](docs/flash-workflow.md) 核对摘要、分区和 NVS，不擦除整片 Flash。升级期间先停机，断开电机动力电源；MCU 重启不等于驱动已断电或机构已静止。

## 首次安装与配网

仅新空白双板、换板或记录异常进入安装/受控恢复；普通启动自动复用已保存身份。

首次安装在可信 UART 台架上从单 Brain USB 发起：

```bash
python3 tools/install_brain.py --port /dev/cu.usbmodemBRAIN --device-id DEVICE_ID
```

工具自动发现 Motion、读取原记录、核对并保存双板身份；无需同时连接两板 USB。成功 `persisted_restart_required / activation_pending` 后退出维护，让整机含 USB 供电全部断电再上电一次。失败可能已有部分记录持久化，保留 NVS 和原身份，不擦除、不重新生成代次或盲目重复写入。

旧设备交接必须保留原业务配置、墓碑、未结事件和发行水位；旧 MQTT 会话退役/凭据交接由工厂流程处理。空白安装不要求电机反馈的有限许可只用于身份和配置保存，不是运动或物理静止证明。当前 Cloud schema 15 和设备登记按父手册确认；已完成的设备不因升级重复执行。

Brain 配网入口：

- 无有效 Wi-Fi 时自动开热点；已有配置只是暂时断网时继续重连。
- 正常启动后长按 BOOT 约 5 秒，连接开放热点 `Babytech-Brain-Setup`；未自动弹页时打开 `http://192.168.4.1/`。
- Wi-Fi 与 MQTT 分别保存，MQTT 默认 `101.33.219.108:1883`，已有自定义配置优先。页面不回传密码；保存后连接 Wi-Fi 会关闭热点，先复制 Device ID 和配对码。
- 页面六位码是开发派生码，不能代替 Cloud 已轮换的随机认领码；工厂提供有效码，不因缺少 Cloud 在线校验额外阻断配网。
- AP/HTTP 不加密，只在可信近距离环境使用。这里没有运动、配对安装或 OTA 操作。

USB 仍可分别保存网络；密码默认隐藏：

```bash
python3 tools/configure_brain_network.py --port /dev/cu.usbmodemBRAIN check
python3 tools/configure_brain_network.py --port /dev/cu.usbmodemBRAIN wifi
python3 tools/configure_brain_network.py --port /dev/cu.usbmodemBRAIN mqtt
```

`check` 只读，不进入维护。保存成功不等于联网/认证成功；维护退出未确认不等于保存失败。先处理退出和诊断，不重复写密码。串口打开可能复位板子，先关闭其他监视器并保证台架安全。不要分享网络 hex 命令、`OTA CODE` 或含宝宝数据的维护导出。

## 产品执行与测试边界

Motion 的 `BABYTECH_V1_MOTOR_TEST` 默认 1：产品低液位固定正常、余粉固定 300g，仅为非饮用电机联调占位。宏 0 使用实际低液位/HX711 入口，需要 GPIO、去皮和标定；无效数据不回退为测试值。两种模式都保留真实 CAN、身份、冲突、持久记录和 Stop 保护。

水温实测仍为 null，目标温度不是测温；温控、按水量/重量停止出料、瓶位检测和食品安全闭环未实现。不能把这些能力说成已迁移，也不为日常电机演示新增不存在的传感器门禁。

正常完成沿用 3 秒 Complete 展示，条件满足后恢复 Ready；下一瓶必须再次手动 Start，由操作者换瓶，不要求逐瓶 Initialize。参考失效、故障、实际冲突、结果容量满或存储失败仍按原规则处理。

Motion 保存原任务意图和最多四条待回执结果，容量包含本瓶预留；新宝宝配置不改旧瓶归属。网络断开不授权重放运动，未知结果只查询原身份；只有精确 Cloud `stored` 回执才删除对应结果。ACK、UART 发送成功、网页 `done` 都不能代替机械完成或 Cloud 落库。

反馈和 await 的当前预算见 [队列说明](docs/motor-queue.md#反馈有效期与停止后起点)，OTA 的独立停稳证据见 [OTA 说明](docs/wifi-ota-implementation.md)。窗口是样本有效期，不是人为延时；扩大预算不能复活旧反馈。

Brain USB `SIM STATUS / SIM ON / SIM OFF` 仅用于已有有效身份和缓存的隔离软件演示。SIM 默认关闭且不持久化，约 15 秒整体计时，不运行真实 CAN；四条结果在 RAM，复位会丢失。首次完成真实安装后，仿真时只供电 Brain、Motion 断电并拔开 UART；不伪造空白板配对或传感器。SIM 成功不等于真实喂养。

## 验证入口与已知限制

按修改范围选择测试，在固件根目录执行：

```bash
python3 tools/test_protocol.py --sanitize
python3 tools/test_motion.py
python3 tools/test_raw_can.py
python3 tools/test_demo.py
python3 tools/test_brain_main.py --sanitize
python3 tools/test_motion_main.py --sanitize
```

主机需要 C++ 编译器，可用 `CXX` 指定。需要 ArduinoJson 的测试使用 Motion 构建下载的头文件，或按 runner 参数提供已有头文件。CI 仅构建默认 v4 两板；旧 v1/v2/v3 codec 测试是内部回归，不是额外固件入口。完整跨端软件回归见父仓库 `Test/system_regression.py` 和正式回归文档。

已有限定低速、双轴同步及 V1 电机流程实机成功记录，不应再笼统写成“从未实测”；但历史镜像成功不证明当前所有工况、连续同步精度、Stop 时限、无线/触摸、Flash 掉电或签名 OTA 回退全部通过。正式操作前仍须核对提交、编译配置、镜像哈希与实际台架条件。硬件差距和后续优先级由父仓库当前手册维护，不在此叠加生产计划门禁。
