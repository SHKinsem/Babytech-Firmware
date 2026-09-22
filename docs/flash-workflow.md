# 可重复烧录工作流

## 2026-09-22 已有记录

依据 `out/flash-check/FLASH-RESULT.txt`、`backup.log`、`flash-motion.log` 和 `monitor.log`：

1. 确认 COM3 对应 ESP32-S3，16 MB Flash、8 MB PSRAM，MAC `3c:0f:02:c5:fe:64`。
2. 使用已打包的 `out/releases/motion-20260921T132548Z`，而不是现场重新编译。
3. 备份 Flash 前 64 KB，核对新旧分区布局相同。
4. 写入 bootloader `0x0`、partitions `0x8000`、boot_app0 `0xE000`、firmware `0x10000`，四段均通过 esptool hash 校验。
5. 没有全片擦除，保留 `0x9000..0xDFFF` 的 NVS。
6. 查看启动日志，确认 Motion、UART、CAN、HTTP 启动；HX711 提示需要校准。没有发送电机使能/运动，也没有完成传感器精度或 UART 对端验收。

## 新工具

`tools/flash-device.py` 支持当前项目 ESP32-S3 16 MB 分区布局。工具独立于前端和构建过程，不更改源文件、不执行 git 操作、不自动重编译，也不使用上次串口作为默认值。板卡的 8 MB OPI PSRAM 配置仍由构建配置负责；工具核对芯片和 Flash，不把它当作 PSRAM 功能测试。

使用本机 PlatformIO 的 Python（包含 esptool/pyserial 依赖），PowerShell 示例：

```powershell
$flashPython = "$env:USERPROFILE/.platformio/penv/Scripts/python.exe"
# 默认只读本地文件；不连接串口，不复位板卡
& $flashPython tools/flash-device.py --package out/releases/motion-20260921T132548Z

# 仅在确实要烧录时运行；端口和 MAC 必须是本次确认的目标
& $flashPython tools/flash-device.py --package out/releases/motion-20260921T132548Z --execute --port COM3 --expected-mac 3c:0f:02:c5:fe:64
```

预检要求包中有四个镜像和 `SHA256SUMS.txt`（sha256sum 格式）或 `SHA256SUMS.json`（文件名到摘要映射）。它验证摘要、ESP32-S3 镜像头、分区边界及应用容量。摘要只能检测包内容变化，不证明包的发布者身份或源码新旧。

执行时先复制镜像到唯一的 `out/flash-runs/<UTC时间>-<随机编号>/`，避免另一任务重新编译改变待烧录内容。核对 MAC 和 16 MB Flash 后，备份前 64 KB；分区不同则停止，不自动迁移。随后写入四段镜像，单独执行 verify-flash，回读比较 NVS，再复位并尝试采集 10 秒启动日志。可用 `--monitor-seconds 0` 关闭日志采集，`--baud 115200` 降低烧录速率，`--esptool PATH` 指定工具路径。本机验证的 esptool 版本为 5.0.2。

每次运行保存各步骤日志、备份与 `result.json`。`flash-verified` 只代表写入和 NVS 校验成功；启动日志可能因 USB 重新枚举或打开过晚而漏采，`bootBannerObserved`/`httpReadyObserved` 分别报告实际观察结果。不会自动寻找另一个串口，也不会自动重刷。失败后应先读报告，尤其注意 `writeStarted`；不自动恢复备份。

前 64 KB 备份含 NVS，可能包含 Wi-Fi 凭据，应仅留在本机忽略的 `out/` 下。它不是整个旧应用的备份，不提供固件回滚保证。新固件启动后自行迁移 NVS 的行为不属于烧录阶段的保留校验。

## 构建与后续验收

需要新固件时，先独立完成网页构建、测试、`tools/build-wsl.ps1` 和打包；选择该包运行预检。避免在主任务编辑源码时把“自动编译最新代码”混进烧录动作。

烧录后可人工访问设备网页检查状态。工具不连接 Wi-Fi、不修改系统网络、不发送电机/校准操作。自动烧录不能替代机构运动、CAN、UART、称重精度验收。

工具测试：`python tests/test_flash_device.py`。本次开发仅运行离线测试和已有包预检，没有重新连接或烧录板卡。
