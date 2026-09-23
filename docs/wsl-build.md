# WSL 编译

Windows 目录仍是源码主目录；WSL 只维护自动同步的编译副本，不在副本中编辑源码。
无需移动整个 Git 仓库，也不在 `/mnt/d` 上编译。

## 使用

在仓库根目录用 PowerShell 7 执行：

```powershell
./tools/build-wsl.ps1
./tools/build-wsl.ps1 -Target device-controller
./tools/build-wsl.ps1 -Target main-controller
./tools/build-wsl.ps1 -Target test
```

默认发行版 `Ubuntu`，默认 8 个编译任务。可传 `-Distro Ubuntu -Jobs 4`。

旧 `-Target brain` / `-Target motion` 仍分别映射到 `main-controller` / `device-controller`；输出只更新新名称目录，见 [命名迁移](controller-naming.md)。

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

WSL 需要 `pio`、`rsync`、`flock`、`python3`、`g++`。脚本会搜索 `~/.local/bin`、
`~/.platformio/penv/bin` 和 `~/.venvs/platformio/bin` 中的 PlatformIO。
工具链仍由两个 `platformio.ini` 固定为 Espressif32 6.4.0 / Arduino-ESP32 2.0.11。
正常联网环境下，首次编译由 PlatformIO 下载缺失依赖。

当前电脑的 WSL 无法直接连接软件源，因此初次配置通过 Windows 下载官方 Linux 工具包并校验 SHA256；
跨平台通用的 framework、platform 和 Python 工具从已有 Windows 缓存复制。没有使用 Windows 编译器在 WSL 中交叉调用。

## 烧录

此脚本只编译和导出，不连接 USB、不烧录、不修改 WSL USB 配置。
USB 继续留在 Windows。当前仍可使用 README 中的 Windows PlatformIO 烧录流程，
但该流程会执行 Windows 构建；直接烧录 WSL 导出产物的工具留待真实烧录阶段接入。

## 本机验证（2026-09-21）

协议主机测试、device-controller 和 main-controller 的 Linux 编译均通过。首次完整编译的 PlatformIO 耗时分别为 5.38 秒、6.43 秒；紧接着无源码变化的增量检查分别为 1.33 秒、1.40 秒。这些数字不含 WSL 启动、源码同步和产物导出，不能视作端到端耗时。当前机器 WSL 启动仍可能有额外等待。

两块板的产物已导出到 `out/wsl/`；尚未烧录或做实机验证。