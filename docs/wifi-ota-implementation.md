# Wi-Fi 固件升级：实现与使用

描述对象：Motion 独立调试网页的签名 OTA 与本地打包工具。Milestone：V1。

2026-10-06 边界：屏幕基线迁入 Brain 后，`main-controller` 已不包含旧 Brain 的 OTA 服务和 `ota_identity.h`。当前可用目标是 Motion，Brain OTA 重新接入不在本次通信迁移范围。

## 当前状态

Motion 的 ESP32-S3 固件已接入共用 `WifiOta` 模块，提供 `/ota` 网页和 `/api/ota/*` 接口。应用镜像分块写入非运行中的 OTA 应用槽，签名、板卡类型、版本、大小及 SHA-256 全部校验后才切换启动槽。通过 `Babytech-Motion` 热点或其已配置的局域网地址访问。

这是**停机维护升级**。现有同步 `WebServer` 在上传过程中会阻塞主循环，不能保证及时处理 UART/CAN 或另一个 HTTP 停止请求。升级前必须停机并切断电机动力电源；网页勾选只是操作员确认，不是硬件互锁。不要在带电运动状态下试用。

2026-10-07：Motion 的 OTA 准入另检查独立 CAN 运动/Stop 证据。control reset、queue 软件 Done、raw 诊断失效及更换工作台执行 ID 不清此证据。已知参与轴须成功提交 Stop，并取得 Stop 后查询的有效位置/速度及静止反馈；新运动、反馈移动、过期或总线失效会撤销准入。保持使能和历史结果待回执本身不算运动，不因此拒绝更新。此检查只影响 OTA，不增加调试/配置/本地 Stop 门禁。

整批停稳和OTA证据复用同一个CAN查询预算，不另建轮询器。每个位置/速度样本在接收时取得独立有效期：`G=max(gapMs,ceil(1000/qps),ceil(timeoutMs/maxInflight))`，`W=clamp((2*N+4*D+3*P+maxInflight)*G+timeoutMs,600,5000)`毫秒。`N`分别为当前未收敛轴数/OTA历史参与轴数，`D`为Demo观察轴数，`P`表示页面查询开启且有选中轴。默认五轴无竞争为3500ms；窗口是样本有效期，不是延迟Stop或强制等待。调整预算/观察轴不延长已经收到样本的有效期，过期后必须重新取得位置与速度；移动、缺失/迟到回复或持续拥塞仍不能形成停稳证明。原单电机动作/Stop监督的600ms与全局查询频率不变；此上限为软件联调预算，真实反馈时序和安全阈值须台架验收。

已提交或部分提交的未知 opcode、非 X 地址帧、无法证明完整接收者的运动广播只留下 RAM 未知风险，不能靠软件 reset、轴1–5或调参表的反馈自动恢复网页 OTA。此时先物理隔离电机供电，再 USB 烧录并保留 NVS。没有 TX 的校验拒绝、已知读取和 Stop 广播不自动算未知运动。风险不写 NVS，**不保证重启后强制保留**；重新上电或 RAM 为空不是机械安全证明，不能作为免检操作。CAN 没有反馈请求关联 ID，不能完全排除旧缓冲响应恰好匹配后续查询；源码/host 测试不证明真实轴或上传全程安全，物理隔离要求始终保留。

## 首次安装与管理员代码

未带 OTA 接口的 Motion 固件，必须先用 USB/串口安装带 OTA 的固件。先核对实板为 ESP32-S3 N16R8、16 MB Flash，读回分区表并确认有 `otadata`、`app0`、`app1`。首次安装保留原有 NVS；不要擦除整片 Flash。现有 [`flash-device.py`](../tools/flash-device.py) 只覆盖设备板串口发布包，不能拿 OTA 的双文件包代替它。

Motion 首次启动时在 NVS 生成独立的 32 字符十六进制管理员代码。代码不会出现在 HTTP 响应或日常日志中。要查看或找回代码，用 USB 串口连接 Motion，波特率 115200，发送一行 `OTA CODE`，妥善保存终端返回的代码。NVS 擦除后会重新生成代码。不要把代码提交到仓库或加入公开测试记录。

## 生成升级包

1. 修改 `device-controller/include/ota_identity.h`，将 `BABYTECH_OTA_BUILD` 增大，并设置新版本。板端拒绝 `build` 小于或等于当前版本的镜像。
2. 编译并通过相关测试。本机编译可使用 `pio run -d device-controller`；WSL 构建使用 `tools/build-wsl.ps1`。
3. 用 [`package-ota.py`](../tools/package-ota.py) 生成发布目录。例如本机 PlatformIO 输出：

   ```powershell
   python tools/package-ota.py --board motion --build-dir device-controller/.pio/build/motion
   ```

   WSL 产物已导出到 `out/wsl/<board>/` 时，可省略 `--build-dir`。脚本核对芯片镜像头、双应用槽容量、源文件时间和私钥对应的公钥，生成 `firmware.bin`、`manifest.json`。私钥默认在忽略的 `out/ota/signing-key.pem`；发布时只交付这两个输出文件，不交付私钥。当前仓库嵌入的是开发公钥，正式产品必须改用离线保管的发布密钥并重建固件。

   源文件时间检查覆盖板端 `src/include/data/lib` 和共享的 `WifiOta`、`BoardProtocol`、`ProductBoardLink`、`BabytechCloudLink`、`BabytechDisplayCore` 的 `src`。其中任意文件比镜像新，都须重新构建后再打包。该检查是时间戳检查，不证明镜像完整复现了全部构建输入；修改编译参数或依赖后仍须主动重新构建。

4. 在 Motion 的 `/ota` 页面选择两个文件并输入管理员代码。发布包内的 `board` 与板卡必须一致。完成后等待重启，重新打开 `/api/ota/status` 检查版本和 build，并核对所用版本的 UART 通信恢复。

首次通过串口安装的 OTA 固件为 `build 1`；同为 `build 1` 的包只能用于首次安装或离线验证。后续 Wi-Fi 升级应从 `build 2` 起。

管理员证明使用随机挑战与 HMAC-SHA256，代码本身不经过 HTTP。网页仍通过普通 HTTP 提供，上传会话和固件内容没有传输加密；当前实现面向受控维护网络，不应把设备板 STA 地址暴露到不受信任的网络。

## 回退与验收边界

Motion 工具链的 ESP32-S3 bootloader 配置启用了 OTA rollback。本项目覆盖 Arduino 核心的默认提前确认行为：新镜像启动后须在 5 秒后通过本地服务检查才标记有效，30 秒内始终未通过则请求回退。Motion 检查 AP 与 CAN 控制器初始化。实板 bootloader 版本及回退行为仍需实测；本地检查不代表电机、称重或另一块板的物理功能均已验收。

至少在电机动力电源已断开的台架上验证：错误板卡/签名/摘要拒绝、上传中断及断电后旧固件可启动、首次启动失败回退、NVS 保留及升级后网页/UART/CAN/称重功能。本文的打包测试不代替上述实板验收。
