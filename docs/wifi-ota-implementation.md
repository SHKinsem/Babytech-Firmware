# Wi-Fi 固件升级：实现与使用

## 当前状态

两块 ESP32-S3 固件已接入共用 `WifiOta` 模块，提供 `/ota` 网页和 `/api/ota/*` 接口。应用镜像分块写入非运行中的 OTA 应用槽，签名、板卡类型、版本、大小及 SHA-256 全部校验后才切换启动槽。主控板通过 `Babytech-Debug` 热点访问；设备板通过 `Babytech-Motion` 热点或其已配置的局域网地址访问。

这是**停机维护升级**。现有同步 `WebServer` 在上传过程中会阻塞主循环，不能保证及时处理 UART/CAN 或另一个 HTTP 停止请求。升级前必须停机并切断电机动力电源；网页勾选只是操作员确认，不是硬件互锁。不要在带电运动状态下试用。

## 首次安装与管理员代码

旧固件没有 OTA 接口，必须先用 USB/串口分别安装带 OTA 的两板固件。先核对实板为 ESP32-S3 N16R8、16 MB Flash，读回分区表并确认有 `otadata`、`app0`、`app1`。首次安装保留原有 NVS；不要擦除整片 Flash。现有 [`flash-device.py`](../tools/flash-device.py) 只覆盖设备板串口发布包，不能拿 OTA 的双文件包代替它。

每块板首次启动时在 NVS 生成独立的 32 字符十六进制管理员代码。代码不会出现在 HTTP 响应或日常日志中。要查看或找回代码，用 USB 串口连接**对应板卡**，波特率 115200，发送一行 `OTA CODE`，妥善保存终端返回的代码。NVS 擦除后会重新生成代码。不要把代码提交到仓库或加入公开测试记录。

## 生成升级包

1. 分别修改对应板的 `include/ota_identity.h`，将 `BABYTECH_OTA_BUILD` 增大，并设置新版本。板端拒绝 `build` 小于或等于当前版本的镜像。
2. 编译并通过相关测试。Windows 本机编译可使用 `pio run -d main-controller` 和 `pio run -d device-controller`；WSL 构建使用 `tools/build-wsl.ps1`。
3. 用 [`package-ota.py`](../tools/package-ota.py) 分别生成发布目录。例如本机 PlatformIO 输出：

   ```powershell
   python tools/package-ota.py --board brain --build-dir main-controller/.pio/build/brain
   python tools/package-ota.py --board motion --build-dir device-controller/.pio/build/motion
   ```

   WSL 产物已导出到 `out/wsl/<board>/` 时，可省略 `--build-dir`。脚本核对芯片镜像头、双应用槽容量、源文件时间和私钥对应的公钥，生成 `firmware.bin`、`manifest.json`。私钥默认在忽略的 `out/ota/signing-key.pem`；发布时只交付这两个输出文件，不交付私钥。当前仓库嵌入的是开发公钥，正式产品必须改用离线保管的发布密钥并重建固件。

4. 在对应板卡的 `/ota` 页面选择两个文件并输入管理员代码。发布包内的 `board` 与板卡必须一致。完成后等待重启，重新打开 `/api/ota/status` 检查版本和 build。两板逐一升级，每次确认 UART v2 通信恢复。

首次通过串口安装的 OTA 固件为 `build 1`；同为 `build 1` 的包只能用于首次安装或离线验证。后续 Wi-Fi 升级应从 `build 2` 起。

管理员证明使用随机挑战与 HMAC-SHA256，代码本身不经过 HTTP。网页仍通过普通 HTTP 提供，上传会话和固件内容没有传输加密；当前实现面向受控维护网络，不应把设备板 STA 地址暴露到不受信任的网络。

## 回退与验收边界

工具链的 ESP32-S3 bootloader 配置启用了 OTA rollback。本项目覆盖 Arduino 核心的默认提前确认行为：新镜像启动后须在 5 秒后通过本地服务检查才标记有效，30 秒内始终未通过则请求回退。设备板检查 AP 与 CAN 控制器初始化，主控板检查 AP 初始化。实板 bootloader 版本及回退行为仍需实测；本地检查不代表电机、称重或另一块板的物理功能均已验收。

至少在电机动力电源已断开的台架上验证：错误板卡/签名/摘要拒绝、上传中断及断电后旧固件可启动、首次启动失败回退、NVS 保留、两板逐一升级及升级后网页/UART/CAN/称重功能。
