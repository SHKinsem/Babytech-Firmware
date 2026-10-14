# WSL 编译

描述对象：Windows/PowerShell 用户的 WSL 编译副本和产物导出。Milestone：V1，`V1-device`，唯一固件为成对 Brain/Motion UART v4；不描述烧录、设备安装或验收。

使用本流程时，Windows 目录是源码主目录；WSL 只维护自动同步的编译副本，不在副本中编辑源码。
无需移动整个 Git 仓库，也不在 `/mnt/d` 上编译。

## 使用

在仓库根目录用 PowerShell 7 执行：

```powershell
./tools/build-wsl.ps1
./tools/build-wsl.ps1 -Target motion
./tools/build-wsl.ps1 -Target brain
./tools/build-wsl.ps1 -Target test
```

默认发行版 `Ubuntu`，默认 8 个编译任务。可传 `-Distro Ubuntu -Jobs 4`。

Brain/Motion 仅支持成对 UART v4，无需额外协议宏；旧 Brain 宏 0、Motion peer 1/2 编译失败，WSL 脚本不另设协议版本。编译不会开启 `SIM ON`、安装配对或执行运动；Motion 默认产品资源仍是 `BABYTECH_V1_MOTOR_TEST=1` 的非饮用测试值，不代表真实传感器或食品安全闭环。历史 WSL 验证见文末，不作为当前镜像重建证据。

脚本会：

1. 按 Windows 工程路径创建独立的 WSL 编译目录，并取得构建锁。
2. 增量同步源码，清除镜像里已删除的源码，保留 `.pio` 缓存；不复制 `.git`、`out`。
3. 在 Linux 中运行协议主机测试，再编译选定板子。
4. 成功后把 `firmware.bin`、`firmware.elf`、`bootloader.bin`、`partitions.bin` 和构建信息写回 `out/wsl/<板子>/`。

Linux 缓存位置：

```text
~/.cache/babytech-firmware/platformio/                 # 独立工具链缓存
~/.cache/babytech-firmware/workspaces/<路径摘要>/source/ # 源码镜像与 .pio
```

同步仅清理脚本自己创建并记录来源的编译镜像；Windows 源码和其他 WSL 工程不受影响。
源码按校验和同步，未变化文件不重写，避免编辑器改变时间戳造成无意义重编译。
`out/wsl` 是最后一次成功构建的产物；失败构建不会覆盖它，查看 `build-info.txt` 确认时间。

## 首次环境

WSL 需要 `pio`、`rsync`、`flock`、`python3`、`g++` 和 `libssl-dev`。后者仅用于主机持久记录测试的真实 SHA-256 后端，设备仍使用各自 SDK 的 mbedTLS。脚本会搜索 `~/.local/bin`、
`~/.platformio/penv/bin` 和 `~/.venvs/platformio/bin` 中的 PlatformIO。
工具链以各自 `platformio.ini` 为准：Motion 为 Espressif32 6.4.0 / Arduino-ESP32 2.0.11；迁入屏幕的 Brain 为 pioarduino 55.03.30-2 / Arduino-ESP32 3.3.0。不要为了统一工具链而互相升降版本。
正常联网环境下，首次编译由 PlatformIO 下载缺失依赖。

2026-09-21 的验证环境曾无法从 WSL 直接访问软件源，采用 Windows 下载官方 Linux 工具包、校验 SHA256，再复用跨平台缓存；这是当时环境的替代办法，不是每台电脑的必做配置，也不表示使用 Windows 编译器在 WSL 中交叉调用。

## 烧录

此脚本只编译和导出，不连接 USB、不烧录、不修改 WSL USB 配置。
USB 继续留在 Windows。README 的原生 PlatformIO 上传会重新构建；如需上传已验证的 WSL 产物，使用现有 Motion 发布包与 [烧录工具](flash-workflow.md)，它不自动重编译。两条路径先核对板型、提交、配置、镜像和端口，保留 NVS；不把导出成功当作实机验收。

## 本机验证（2026-09-21）

以下为当时旧工程的历史基线，不代表当前屏幕 Brain、v4 网络或持久迁移已在 WSL 验证。

协议主机测试、motion 和 brain 的 Linux 编译均通过。首次完整编译的 PlatformIO 耗时分别为 5.38 秒、6.43 秒；紧接着无源码变化的增量检查分别为 1.33 秒、1.40 秒。这些数字不含 WSL 启动、源码同步和产物导出，不能视作端到端耗时。当前机器 WSL 启动仍可能有额外等待。

两块板的产物已导出到 `out/wsl/`；尚未烧录或做实机验证。
