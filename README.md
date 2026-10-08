# Babytech Firmware

## First-install offline restart validation (2026-10-08)

From the parent repository run `python3 Test/dual_main_install_check.py --sanitize`.
The host-only installation pipe starts both production mains without pairing,
accepts USB commands only on Brain, and relays original UART chunks. It imports
the full legacy context or tombstone, verifies both saved identities, exits
maintenance and remains activation-pending until both processes are rebuilt
with their original SDK disk. A second ordinary reboot reuses pairing without
installation or motion. Every report checks existing NVS bytes, runtime state,
CAN and control-frame prefixes; Brain's full context and Motion's barrier match
an independent canonical digest.

Both cases pass ASan/UBSan: active context 1044 steps, tombstone 950 steps,
three generations per case. CAN stationary packets and saved bench rotation
calibration are explicit SDK fixtures, not private safety-flag overrides.
No network worker is executed in this offline test. It does not prove physical
power cycling/Flash, old MQTT credential retirement, image upgrades or B4.
Default v3, non-consumable switch 0, production firmware and protocol are unchanged.

Pairing-commit interruption coverage uses the same parent tool with
`--install-storage-fault brain` or `--install-storage-fault motion`. Only explicit
host installation pipes accept `--install-nvs-fault pair-commit`; after actual
setup, the fake SDK fails the selected board's first pairing commit without
applying it. The already committed business record survives. Both processes
are reconstructed with their complete disks, then Brain USB explicitly resumes
installation using the original embedded identity/epoch. Two later offline
boots perform zero SDK blob Set/Commit calls and do not reinstall or move.
Same-boot Brain storage faults reject another install; a Motion storage fault
can accept an explicit Brain retry but still returns the fault, preserving all
bytes. Neither path clears records or adds a production gate.

Add `--install-storage-applied` in the parent tool to select host-only
`--install-nvs-fault pair-commit-applied`: the fake SDK applies the complete
pairing commit but returns an error. The failed run remains inactive. After
preserving both disks and restarting, a Brain fault leaves both boards fully
saved, so normal offline startup reuses pairing without reinstallation. A
Motion fault leaves Brain unimported; explicit Brain USB recovery keeps the
original pair/context and performs zero Motion blob writes. Ordinary offline
boots retain the full disk and do not execute product actions. Both fault
modes are rejected by non-installation pipes before setup. These are two
commit-error outcomes, not arbitrary torn Flash writes, physical power loss
or mixed firmware-image upgrades; production behavior is unchanged.

## 原执行上的 Cloud Stop（2026-10-08）

父项目 `Test/dual_main_broker_check.py --sanitize --prepare-fixture --prepare-contention cloud` 已通过604步实际双main/原UART/隔离broker与Cloud联测。先真实发行并接受Prepare序号1，再发行独立Stop序号2；Brain按当前Motion STATUS绑定原Product执行ID，二进制UART Stop使用Cloud来源、当前双方boot和准确目标，不混进普通COMMAND。

Stop ACK表示已收到，不等于电机停稳或Flash保存：缺轴/无新反馈时原intent、消费序号1和完整SDK disk仍保留；新鲜静止反馈后原Prepare归档为`stopped/E_STOPPED`，当前Cloud结果才变为`CloudStop`(kind2)/序号2。Prepare的原接受ACK、宝宝/配方和终态仍归属于序号1，不能要求新的当前结果仍是Prepare，也不能把Stop当一条喂养记录。Prepare pipe只读新增Stop序号/命令ID/执行ID，不改生产记录。

原终态三表commit及当前boot stored清对应项后，实际原TTL内Prepare/Stop MQTT重放、各自API重复/同ID冲突均无新UART控制前缀、ACK、CAN运动/停止或NVS写入。Stop重放尾段再次核对原Prepare账本及三表完整快照；Cloud HTTP ACK仅增加已持久保存的UTC`received_at`，其余七字段与原MQTT ACK精确一致。此处只证明SDK软件路径，不是触摸/Flutter/WebSocket、真实Flash/停稳/RTOS/ACL、丢Stop回执恢复、联合升级或B4验收；不改变生产门禁/契约/schema/PIO、默认v3或非食用0。

本轮原HTTP Stop518步、屏幕Stop533步、正常Prepare537步/run6回归通过，均SDK累计失败0；59专项Python、quick Cloud249（既有跳过1）/工具105/旧host22组/SQLite备份通过。独立限定Code/QA复审通过，reviewer复跑Python并以改写原账本/三表的反例核对尾审计，不运行C++或broker；实际联测由主Agent执行，未重建MCU、部署或烧录。

## 冲奶中三入口竞争与停止（2026-10-08）

父项目测试入口为 `Test/dual_main_broker_check.py --sanitize --prepare-fixture --prepare-contention http|screen`。运行两端实际main、UART原字节及隔离broker/Cloud；在原Prepare接受后，交错提交第二个Cloud Prepare、屏幕Start/Initialize意图，以及六个工作台动作/参数/称重/OTA写入。Cloud忙拒绝不取得发行序号，屏幕不占本地序号或持久pending，工作台诊断仍可读取。之后分别从Motion HTTP或Brain屏幕Stop停止原任务，检查广播Abort后Stop、新鲜停稳反馈、原请求的`stopped/E_STOPPED`终态、三表commit与匹配stored回执，不将Stop提交当完成。

Brain host real-pipe新增严格校验的`display_intent`和`stop_click`，仅向SDK view提供输入，决定仍由生产main/owner执行；不是实际触摸命中测试或固件串口命令。Motion Prepare pipe只读上报实际执行ID，UART二进制Stop观察仅在该场景显式启用，核对当前双方boot及原执行目标。竞争场景的单轴五段脚本使用`wait 2000`留出操作时间，正常Prepare仍为`wait 400`，不模拟每个电机动作。

stored后继续排空SDK与全部owner队列，以竞争前独立基线检查控制帧前缀，覆盖Stop提交/归档等待/回执全过程，再检查原ACK/ledger/序号及从Stop开始到结束的全部CAN；不允许晚到的回零/使能/重放。缺轴注入必须显式撤销才恢复反馈，不能放宽生产停稳判定。测试不新增生产门禁、PIO/仿真模式、协议/schema/NVS变更，不代替全阶段故障切点、联合升级/安装或实板Flash/UART/CAN/RTOS/ACL与B4验收。

终版HTTP/屏幕Stop分别519/533步通过，累计SDK发送失败均0；前次屏幕532步曾累计1、阶段未定位，不声称每次零背压或特定重传已经证明。正常Prepare537步/6次queue run、运行中Brain重建400步、默认历史1728步/4结果和Brain13类/16进程回归通过。独立限定Code/QA复审关闭缺轴恢复、尾段审计与过晚前缀基线三项测试P2，reviewer实际运行55项Python，未编译或运行broker。

## 新 Prepare 双主循环验证（2026-10-08）

父项目 `.venv/bin/python Test/dual_main_broker_check.py --sanitize --prepare-fixture --broker-python /tmp/babytech-mqtt-test-env/bin/python` 已通过517步实际双main/隔离Cloud联测：Motion不预存宝宝配置/历史结果，正常retained经Brain/UART同步；SDK HX711位流由真实HTTP去皮/100g标定得到300g，上传明确单轴/五wait脚本并经原HTTP初始化。实际API发行150ml/42C/25g每100ml，UART原命令/接受与MQTT ACK、永久账本精确一致；五阶段顺序和每段至少400ms、初始化加五段共6次队列运行，生成唯一37.5g目标终态。

原TERMINAL字节经Brain发布，独立SQLite读事务核对事件/喂养记录/永久receipt三表已提交。独立开关一直扣留stored，包括API线程等待期间，核对Motion仍保留结果后才放行；真实匹配UART CloudReceipt删除对应项，宝宝屏障、消费水位、原接受决策与请求摘要不变。Prepare成功来自终态，不能把原接受ACK的outcome改成喂养完成。重复API、同ID改水量冲突及原TTL内原MQTT重放不新增UART决策/ACK、执行或记录；SDK入队失败0。

仅显式host `--pipe --prepare-fixture` 编译既有非食用宏1；默认历史pipe仍0。SDK pipe只提供HX信号、排队HTTP和只读观察，普通模式拒绝这些输入及称重标量类型；Prepare模式快照仅额外允许实际去皮使用的U16(6)/I32(7)并校验长度。不新增PIO环境、设备仿真、固件串口命令或生产协议/schema/NVS格式。默认历史联测1728步/4结果及34个Motion main场景亦通过；单轴五wait和SDK I/O不是整机电机运动、真实App/Flash/UART/CAN/RTOS/ACL或B4验收，运行中断电和完整组合恢复仍开放。

## 双主循环结果与命令验证（2026-10-08）

父项目 `Test/dual_main_broker_check.py --sanitize` 通过 host-only `test_brain_main.py --real-pipe` 与 `test_motion_main.py --pipe`，运行两端生产 `setup()/loop()` 的独立进程。SDK UART 原字节双向中继，Brain 原始 MQTT 发布/订阅映射到隔离 Paho/broker 和真实 Cloud；不使用脚本 Motion peer、伪造 ACK 或落库回执。首次通过正常 Motion Store 接口准备四条历史 LocalTouch 完成/失败结果，不执行新冲奶。

覆盖 Brain、Motion 单板及双方退出后同时重建进程并保留 SDK disk、原事件重新发送、真实 Cloud 去重及提交后回执清空 Motion 队列。随后真实 Cloud session probe/发行两条无运动请求：`reset_error` 接受为 `already_clear`，`check_firmware_update` 拒绝为 `cloud_ota_not_supported`；实际 UART COMMAND/RESULT、MQTT ACK 与永久账本身份/序号一致。水位未知的 Settemp 在 Cloud 拒绝且不发行，不能伪造传感器让它通过。重复 API 返回原 ACK、冲突 ID 拒绝；原 MQTT 旧序号实际送入 Brain 后丢弃，不新增 UART 请求/查询/结果或 ACK。四条喂养记录仍唯一，无运动 CAN 输出。

核对每代 native 订阅/retained 配置、实际 HELLO boot nonce、原 JSON 字节/配方/宝宝、失败详情及消费水位。`--build-output` 只编译测试二进制，`--boot-id` 是 SDK 随机种子而非真实 boot ID；不新增固件环境或串口命令。联合 fixture 单独使用有界 3000 次 worker 预算及 Brain SDK 名义写容量 23 bytes；其他测试仍为 1000 次、原 7-byte 短写。7 bytes/15ms UI 节拍不足以在真实 50ms 首帧预算内发送请求，超时主动失效不是 CRC/parser 故障；联合测试不放宽生产预算或吞 CRC。23 bytes 只代表 host SDK 能及时发送，不证明 MCU UART/任务时序。

SDK、UI、NVS、时钟和 TCP 适配仍是替身；Brain 网络凭据重新 seed，Cloud session 随机源未随重启轮换。默认无水位配置的联测不证明新Settemp接受；显式水位夹具的限定证据见下文。两种配置均不证明凭据 Flash、旧 Cloud session 失效、新 Prepare、实际加热、运行中双板断电、完整 HTTP/OTA 矩阵、真实 CAN/UART/RTOS/ACL 或首次物理激活。默认 v3、非食用宏 0 和协议字段不变，完整 B3.3/B4 仍待验收。

测试同时输出 SDK 输入/owner 队列与累计发送失败计数。历史回执集中释放的背压依靠重传恢复，失败数独立报告；新命令及重放阶段必须不新增失败，并核对输入/全部队列排空、原 TTL 与 6-byte 控制帧前缀，不能把丢包或未完成发送算作去重成功。这些只读观察不改变生产队列或门禁。

## Motion 主循环验证（2026-10-08）

显式主机 Prepare 场景单独运行，不加入默认45进程或默认历史结果 pipe；新 Prepare 联测使用上方的显式参数：

```bash
python3 tools/test_motion_main.py --sanitize --prepare-fixture \
  --case sdk-prepare-flow --case sdk-prepare-home-missing --case sdk-prepare-marker-missing
```

仅这个host构建使用既有非食用宏1和GPIO21，不新增PIO环境、设备仿真或串口切换。SDK在DOUT1/SCK2提供有转换间隔的24位有符号HX711位流及第25个增益时钟；真实HTTP去皮/100g标定、滤波/超时及正常NVS保存得到300g。实际UART同步宝宝配置并Initialize，CAN SDK仅按精确home/F3/marker帧回复；失联粉量不能就绪，恢复可自动继续，缺home完成或marker不能误报Ready。

正向使用明确上传的单轴测试配置和五个`wait 400`阶段，真实Queue/Flow/Runtime依次执行，150ml/42C/25g每100ml生成37.5g目标的持久终态；原请求重放不新增阶段或改变NVS。旧结果未收到Cloud回执，正常Complete显示hold结束后仍可Initialize并做第二瓶180ml/43C、目标45g，两条身份/结果分开保存且正常Store重开一致。粉比保持已同步配置，不能借改请求粉比绕过context契约。

三个独立进程通过ASan/UBSan，共108项检查；默认34进程亦通过。此处Brain是脚本ReadOnlyLink peer，不运行真实Brain main/Cloud/broker；五段等待不是完整机械脚本或电机运动/传感器闭环证明。完整新Prepare双main链路、真实Flash/CAN/UART/RTOS和B4仍待验证，默认设备非食用宏0及v3不变。

`python3 tools/test_motion_main.py --sanitize` 直接编译生产 `device-controller/src/main.cpp`，执行真实 `setup()/loop()`、已注册 HTTP handler、UART 回调及持久恢复 owner；保留 MotorControl/X42s、Queue、ProductSession/Runtime/Recovery/Store、WiFiSetup 和 WifiOta。45个独立进程包含原八项启动/恢复与SDK crypto、12项产品占用/历史结果HTTP、4项USB/UART维护、9项OTA认证/预约/上传失败、一项GPIO采样/输入校验及11项恢复中HTTP。嵌入资产来自实际网页/流程 JSON；v4 Motion 不创建产品 MQTT worker，非食用宏仍为 0。

恢复中HTTP测试用正常Motion Store接口预存一条Prepare intent，再运行实际setup/loop；不是Cloud重新发行任务。`http-recovery-guards`覆盖诊断读取、动作/参数/称重/Wi-Fi写入拒绝、合法raw只读与畸形raw不取消、OTA安全拒绝；原请求/事件/执行ID、消费水位及完整SDK disk不变。`http-recovery-stop-0..8`逐进程验证九种既有Stop/disable/reset入口，核对完整广播Abort后Stop及对应F3帧，不因软件reset的200或Stop的202删除intent或宣称停稳。

`http-recovery-stop-failure`注入FE发送失败并保留503与原证据；取消注入后实际重发Abort/Stop，原CanFault仍锁存，接口仍返回503。五轴新鲜反馈齐全后恢复正常结束，原Prepare按`reboot_during_feed/E_REBOOT_DURING_FEED`归档一次；缺轴/仍运动不得提前归档。全恢复区间禁止使能/回零/新队列，持久变化仅允许业务record。归档后未收到Cloud回执也不阻普通polling/距离设置/队列运行取消或OTA manifest验证。host JSON DOM容量32KiB仅用于解析完整48帧诊断trace，不改变固件响应或设备内存。45进程和显式Prepare三进程108检查通过ASan/UBSan；SDK停稳信号不是真实机械停稳、Flash断电或完整多owner/联合升级验收。

新增 `http-product-guards` 经生产 UART 接受 Clean，核对15个读取入口和动作/配置写入拒绝；合法raw Read可用，畸形Stop/Interrupt/disable不能取消，新增CAN只允许读取，持久任务和Clean等待状态不变。`http-product-stop-0..8` 分别覆盖Stop、Stop-all、queue cancel、单轴/广播disable、raw Stop/Interrupt/disable及control reset；实际广播Abort/Stop和对应F3完整帧必须出现，未停稳不删持久决策、不误报OTA安全，reset的200只表示软件状态清理及Stop提交。`http-product-stop-failure` 注入FE提交失败，核对503、成功Abort和原证据保留。

`sdk-water-sampling` 默认检查未配置的水位输入不能变成valid；`--water-fixture --case sdk-water-sampling` 仅在host编译时选GPIO21主动高低水位，SDK只提供digitalRead电平，生产100ms/五次去抖计算valid和恢复，pipe输入必须为整数0/1。HX711仍不存在，非食用执行开关仍0，不直接改业务资源或授权。`--water-fixture`不是PIO环境、固件烧录或设备仿真入口。父双main工具的同名参数覆盖低水位拒绝不发行、恢复后Settemp=46接受及目标回流、重复/冲突API和原TTL内重放；实际MQTT ACK、UART及永久账本对照通过，没有新运动或喂养记录。

2026-10-08批准并修复v4目标投影：空闲显示当前设备目标；冲奶显示本次Prepare锁定的温度。原宝宝配方不变，下一Prepare仍使用自身请求/缓存配方。修复仅在`MotionProductRuntime::project`，共享`ProductSession::displaySnapshot`及默认v3保持原样；不添加目标NVS持久化、加热或门禁。`python3 tools/test_motion_product_runtime.py --sanitize --case temperature`检查这一区分及新配置/运行快照隔离；机械/Flash为替身，不证明完整main的新Prepare成功或实际温控。

`http-history-debug` 用Store正常接口预存已停稳历史结果，读取和距离/限制配置写入、队列运行/取消仍可用，RAM/NVS/HTTP读回一致，历史业务记录字节不变。OTA安全前置可达，空manifest返回400而非因历史结果被阻挡；不证明认证成功、OTA预约或Flash安装。新增HTTP夹具显式保存与嵌入草案匹配的轴1=2mm、轴3=40mm/rev，未改变默认标定或证明回零/机械就绪；无匹配标定时boot配置可能未应用，不能拿此夹具当默认就绪证明。完整恢复/认证OTA矩阵及物理联调仍待验证。

`http-maintenance-usb` 通过实际USB `MAINT BEGIN/END`，`http-maintenance-uart-release/expiry` 通过实际Discovery及维护帧进入预约；明确SDK五轴静止反馈是前置，不用手动设置busy。维护中15读取入口和独立raw `01 1F 6B`可用，16写入/reset入口拒绝，8停止/disable入口逐请求核对完整非查询CAN帧的地址、DLC、payload、顺序及数量。USB无三秒TTL，缺反馈END拒绝，补新反馈后退出；UART持续实际续租超过三秒再释放，或以最后完整帧SDK读取时刻检查2999ms仍有效/3000ms释放。仅两个边界loop冻结host时钟，避免原XMotor/loop延时移动观察，不改变生产预算或其他用例时钟。进入、续租、退出/到期全过程保持完整SDK disk；进入/退出不发取消或运动，退出后无害polling配置恢复。`http-maintenance-busy` 核对实际Clean占用时USB/UART拒绝，不夺取产品任务。不是Brain安装协调器或写入中断、真实USB/UART/CAN/Flash/RTOS验证，也不证明成功认证OTA。

OTA场景通过实际USB `OTA CODE` 和已注册HTTP接口取得nonce、验证管理员HMAC、签名并预约；仅host SDK在完整匹配现行固件PEM后显式替换为一次性测试公钥。测试只保留公开签名，不读取生产私钥、不修改固件公钥；默认SDK不替换，实际系统P-256验签保留。覆盖关闭/等长错PEM/坏公钥/错签名/坏DER/改hash负例、nonce消费和60秒期限，token/IP/字段匹配、START安全重查、Ready期间读取/写入/专用Stop权限，以及120秒正常/回绕到期当轮START拒绝。begin失败、短写、hash不符、SDK end拒绝及256/1024字节后ABORT均检查状态/原因、释放预约和重新认证恢复；ABORT不伪造最终HTTP响应。Uploading没有自动TTL释放，不能据Ready到期测试声称上传超时已恢复。此项不证明生产密钥、ESP32 mbedTLS/TCP、有效固件安装/回滚、UART产品拒绝矩阵或Brain/Motion联合升级。

主循环测试发现默认 10 query/s 无法让五轴位置/速度/flags 同时满足原 600ms 停稳窗口。经用户批准，Stop/reset 等待和恢复停稳证明使用 Stop 时冻结的预算窗口（600–5000ms），仍要求全部配置轴反馈严格晚于 Stop 且速度在原静止阈值内；普通动作、启动/回零检查仍为 600ms，查询速率和 3 秒 Stop 等待期限不变。缺反馈、运动和过期仍不能确认。测试 SDK 不代表真实 CAN 采样关联、电气、NVS Flash、RTOS 时序或带电 OTA 安全；Flash替身可核对begin/write/abort，但end始终拒绝，不伪造安装成功。完整双 main + broker 恢复矩阵和实机验收仍待完成。

## B1.0 屏幕迁入状态（2026-10-06）

两块板固件统一在本仓库管理：`main-controller` 已迁入父项目 DisplayController 的屏幕/触摸/UART v3 基线、显示库与 N16R8 板型，`device-controller` 继续作为 Motion。Brain 已通过本地及不含父项目的临时副本构建、LVGL host UI 测试，尚未烧录或验证真实触摸。原 DisplayController 的源码/构建入口已退役，仅保留迁移说明；旧 Brain 网页及其专属配置/页面测试也已移除，历史实现从 Git 获取。

默认 Brain 仍是 UART v3 屏幕角色；成对选择 v4 后已接 Wi-Fi/MQTT、状态投影、Cloud命令单次UART派发、优先Stop和屏幕本地持久请求。屏幕 Initialize 无需Cloud/宝宝缓存，Start使用已保存且与Motion匹配的有效缓存；两者由Motion最终机械准入。Motion v4 产品命令接收/持久执行和监督Stop已接验证路径，默认非食用产品开关仍为0。Brain自动配置缓存/同步及双板记录与Cloud回执运行桥接已接；组合恢复、最终安装激活和实机验收未完成。Motion默认路径仍直连Cloud，v4不启动产品MQTT，独立调试网页保留。完整迁移按父项目 `Docs/refactoring/BRAIN_MOTION_CLOUD_APP_INTEGRATION_PLAN.md` B1–B4推进，不把构建成功写成产品迁移完成。

2026-10-08 真实模式就绪上报：公开 `can_start` 不再固定 false，而按新鲜且空闲停稳的 Motion Start 权限、双方精确同步的宝宝/配方，以及现有冲突请求/维护状态计算。每次周期/probe 编码在 UI loop 重新求值，避免前面的配置/命令回调使旧布尔值过时。屏幕离线启动不依赖 Cloud，Cloud 发行不依赖本地触控序号容量；信息性 Stop 查询、历史待上传结果本身不阻止下一瓶，Motion 队列满与机械检查仍生效。Unknown 动作不重放，本地持久 pending 不因此清除；未开启非食用能力的 Motion 仍不能启动，不代表食用或实机验收。下面按日期排列的开发证据中固定 false 是当时边界，以本段为当前行为。

`python3 tools/test_brain_main.py --sanitize` 包含13个case/16个独立进程，包括新增真实 pipe 输入校验。`--case real-readiness` 的离线缓存/最大本地序号fixture运行生产main和实际UART适配器，测试peer返回编码身份、配置证明和状态；各38个probe核对true/false，覆盖配置更新/墓碑、接受后旧idle、ACK未入队、本地Stop及恢复。历史outbox/信息性查询不过拦，屏幕离线实际发送，本地序号满不挡Cloud。测试peer不是Motion main，SDK/物理时序/Flash/broker仍不在此证据内，真实Motion默认非食用能力仍关闭。

`--case real-results --case real-results-write-failure` 将生产Brain主循环/网络/UART适配器接到实际Motion `ReadOnlyLink`、`MotionStateStore` 和 `MotionResultDelivery`。夹具用Store正常接口预存四条历史完成/失败结果（Cloud/local交替），不模拟执行电机。测试事件发布失败、无新鲜STATUS/配置同步时补传、当前宝宝已换而原JSON/宝宝/配方不变、非stored/错设备/不匹配回执、UART回执丢失、Motion owner重建及删除commit失败后重载；精确回执逐项删除，非outbox持久证据保持不变，Brain不发Command/Stop。SDK/NVS/网络/时钟是替身，stored是注入输入，不证明Cloud事务、真实broker、Motion setup/loop、双板重启、CAN或Flash原子性；不能据此关闭完整B3.3/B4。

`--case real-auth-offline` 用三个独立冷启动进程分别模拟MQTT CONNECT被拒绝、command订阅失败、config订阅失败。每次重连继续注入同一拒绝；本地Initialize在NotReady阶段派发，脚本Motion回覆Ready后才派发Prepare，生产Brain保留原宝宝/配方及序号，两个动作各只发送一次。持续三次连接尝试后仍无Cloud会话/消息处理/发布、屏幕本地可用，不擦配对或改凭据。覆盖实际main/网络worker/本地owner和UART适配器，不复制派发逻辑；SDK拒绝返回值和Motion回复是测试输入，不证明真实密码校验、Mosquitto ACL、机械执行、触摸布局、Flash或RTOS时序。

App/屏幕文案唯一手写来源仍在父项目 `Shared/feeding_flow_ui/feeding_flow_ui.json`。父项目 `python3 Tools/generate_feeding_flow_ui.py` 自动更新本仓库生成 header，`--check` 检查内容及源哈希；生成物须随固件 commit 提交。独立 clone 编译锁定的 header，不读取父项目或下载文案，禁止手改 generated 文件。

## B1.1 共享协议基础（2026-10-06）

离线结果队列（运行桥接已接，实机待验收）：Motion以BMS2单记录保存当前执行和最多4条未确认喂养结果，容量包含正在冲奶所预留的一条。运行器在RAM冻结终态/时间，停稳后`finishFeeding`持久保存、`archiveFeeding`入队并释放执行槽，不等Cloud回执；队列满只阻止新prepare。历史结果确认删除不阻止另一个任务运行，上层核对精确Cloud stored回执后由Motion删除对应项。BMS1可只读加载，下次业务写入保留原记录内容并转换为BMS2，不在普通boot主动迁移。编码保守上界4073 bytes，未改Flash分区；真实20KiB NVS共存/替换峰值仍待实测。Brain Cloud/触控已单次发送，不能据组件实现宣称完整离线连续冲奶已实机验收。

队列测试：`python3 tools/test_motion_result_queue.py --sanitize`；原记录/Store、commissioning及导出测试也必须通过。Motion Store主机实例现约17.2KiB，不得放入小任务栈；记录codec改用检查分配失败的堆暂存。USB导出缓冲上界6KiB，电脑端接受BMS1/BMS2和至多16KiB重组内容，仍只是诊断验证，不授权迁移或启动。

Brain v4 已接本地USB Wi-Fi/MQTT配置，见下方“Brain USB网络配置”。配置保存并读回后，由网络worker重连，不在UI线程操作无线接口。旧Wi-Fi两个键不是掉电事务：保存失败不改变当前实例的已验证连接配置，但重启后可能读到部分写入；重新配置即可，不擦电机/配对/业务NVS。MQTT使用原`cloudcfg/record`，写后重新打开核验才更新RAM，失败可重试；即使失败返回，Flash也可能已经改变，不声称回滚。测试：`python3 tools/test_brain_station.py --sanitize`及`python3 tools/test_brain_network.py --sanitize`。实际身份安装/MQTT账号退役交接仍未完成。

`shared/BoardProtocol/src/BoardProtocolV4.*` 已提供 v4 帧/CRC、固定容量分片重组和 Stop 编解码，`python3 tools/test_protocol.py` 同时运行 v1/v2/v4 回归。v4 的帧最大196 bytes、逻辑消息最大2047 bytes；Stop/心跳可在普通重组期间单独交付，未完成/错误输入不污染结果。接收完成只说明 bytes 齐全，仍须由后续 endpoint 核验 boot、身份、JSON、持久序号及执行门禁。

`BoardSessionV4` 核对配对身份及本次 HELLO 的关联应答，分别管理心跳与机械状态的新鲜度；`BoardTransmitV4` 提供固定容量队列和逐帧高优先级调度，支持短写/背压。`shared/ProductBoardLink` 提供严格的 HELLO/STATUS JSON 编解码及共用链路，假串口覆盖两板握手、单/双向断线、重启和旧包重放。Motion显式注册产品回调后才处理动作，未注册入口仍拒绝；Brain现单次发送Cloud及触控COMMAND，传输ACK不是业务接受。原始STATUS保留Motion的startEnabled观察值，显示/Cloud消费者另加自身准入，遥测不直接授权动作。

同一模块另有普通COMMAND/COMMAND_RESULT与Cloud v4命令编解码，复用`ProductRequest`并保留序号、宝宝/版本和float32配方身份。Motion现接执行协调器，只有持久接受后才启动；codec本身不授予运动权限。`tools/test_board_commands.py --sanitize`及`test_product_command_result.py`检查边界，Stop使用独立二进制通道，不加入普通动作枚举。

`ReadOnlyLink` 为单 owner、非重入对象，约 8 KiB，不能放在 MCU loop 局部栈中；共享完整消息 scratch，短回执用紧凑帧。旧35字段版本的 GCC8.4/14.2 静态接收调用链（receive/handle/decodeStatus）约4 KiB；这是历史预算参考，不是36字段版本的栈测量。尚未包含外层适配器和 JSON 库调用余量；接入设备后仍须检查实际任务栈高水位，不能让网络回调并发操作链路。

当前默认固件仍使用原 UART v3。v4 已接入两端 MCU UART1：Brain使用 `BABYTECH_BOARD_LINK_V4=1`，Motion使用 `MOTION_UART_PEER=4`。Brain已接Cloud/触控owner，Motion已注册产品接收/Stop回调。两端必须同时选择；这是迁移验证路径，不是已完成的产品固件。它读取 `productpair/record` 并检查本机角色/MAC；缺失或损坏不能进入正常握手、联网或产品操作，但可选v4入口允许仅打开UART诊断发现。不自动认领陌生对端。受控配对写入和旧配置导入及单USB工具已接单Brain持久安装，首次激活采用下文已批准的整机手动重新上电一次；动作/配置/结果桥接已接，完整组合恢复和实机验收未完成，不能靠烧录这两个镜像直接完成迁移。

v4不启动Motion产品MQTT；屏幕Initialize/Start使用下方本地owner，Motion独立Wi-Fi、HTTP调试网页及本地OTA保留。旧`formulaevt/payload`仅只读检查，有记录或读取异常时保留数据、标记待处理并监督停机，不调用旧Outbox初始化。新记录开机恢复、Motion动作接收及Brain Cloud/触控派发、两板结果补传已接源码；遥测本身不授予运动权限。默认v3行为不变。

2026-10-07工作台Stop补充：v4 STATUS现在要求36字段，新增`execution_owner=none|product|workbench`，Brain/Motion配套升级，不能混用旧35字段v4。工作台queue/Demo用一个RAM执行ID，App与屏幕Stop共用原目标绑定，旧目标不能停止新批次；网页本地Stop仍独立可用。OTA运动证据与软件owner分开，详见[OTA说明](docs/wifi-ota-implementation.md)。

在本子仓库根目录编译验证（不含烧录）：

```bash
PLATFORMIO_BUILD_SRC_FLAGS=-DBABYTECH_BOARD_LINK_V4=1 pio run -d main-controller -e brain
PLATFORMIO_BUILD_SRC_FLAGS=-DMOTION_UART_PEER=4 pio run -d device-controller -e motion
# Restore ordinary build outputs before packaging or later flashing.
pio run -d main-controller -e brain
pio run -d device-controller -e motion
```

共享协议运行 `python3 tools/test_protocol.py --sanitize`，配对记录运行 `python3 tools/test_pairing_record.py --sanitize`，只读发现运行`python3 tools/test_board_discovery.py --sanitize`。先构建 Motion 获取 ArduinoJson 后运行 `python3 tools/test_board_messages.py --sanitize`、`python3 tools/test_board_link.py --sanitize`、`python3 tools/test_board_arduino.py --sanitize`。最后一个链接生产 MCU 适配器，仅用替身替换 NVS/UART/MAC；它不代表真实 Flash、UART 线速或 Stop 时限验证。CI 编译两套默认与迁移入口；WSL 默认目标仍只导出默认镜像。

## B1.2 共享网络基础（2026-10-06）

`shared/BabytechCloudLink` 是唯一 MQTT transport 实现，Motion 通过本地依赖构建，不再保留本地副本；共享模块也已在 Brain SDK 编译验证。默认 Motion 继续使用原 `begin()`；面向 Brain 的 `beginV4()` 增加每次建连随机会话、24 小时轮换和 5 秒请求时效检查。网络线程拥有 socket，经有界队列交付消息；它不调用 LVGL、UART 动作或电机接口。probe 与终态回执不覆盖 retained 配置槽，旧入口不接受新 probe 类型。v4 出站必须使用生成 payload 时的连接代次，入队及实际发送均检查，旧状态/探测回复不跨会话重放。

`CloudSession` 是网络新鲜度门禁，不是持久去重或动作授权。Brain v4 在有效配对后调用 `beginV4()`；网络 worker 只读既有 `wifi-cfg` 和 `cloudcfg`，进行有界等待/退避的 Wi-Fi 重连及 MQTT 服务，UI loop 不调用网络 I/O。坏凭据不自动擦除，没有配对记录则不启动。受控凭据配置入口仍属 B1.3，不应手工猜写 Flash。

`brain_network` 每次 MQTT 新会话立即、其后每2秒尝试发布状态；回复 config topic 中匹配本机和当前session的只读probe。可选产品handler现交付Cloud派发owner，未注册时仍明确拒绝集成未就绪；不在网络回调写业务NVS或更新UI。Motion STATUS包含产品阶段、故障、资源有效性和完整宝宝ID；Brain只在新鲜链路开放命令入口，`can_start=false`仍保留。过期状态显示unknown并保留已知故障，Cloud仅新鲜恢复状态才解除错误。父仓库已实现Cloud探测、SQLite14持久发行/回执；触控、配置和结果转发已接运行桥接，组合恢复/激活及实机验收未完成，源码实现不代表生产部署已验证。

网络依赖现已随实际 v4 入口加入 Brain ini；即使默认不运行网络，SDK 依赖也会增加镜像和静态 RAM。最终队列、显示 DMA、任务栈和内部堆需实板测量。正常状态只保留最新值，探测保持独立队列；实际发送前核对会话及原始机械采样有效期。JSON 完整转义后超过 2047 bytes 则整条拒绝，绝不截断身份；极端转义字段组合可能超限。只读代码不能用于替换现场产品固件。

运行 `python3 tools/test_cloud_session.py --sanitize` 验证生产会话逻辑；`python3 tools/test_cloud_contract.py` 覆盖设备 ID/完整 topic、旧编解码及 Outbox。主机状态测试与固件编译不证明真实 broker、FreeRTOS 调度或网络断线时延。

`python3 tools/test_cloud_link.py --sanitize` 直接链接生产 MQTT worker，确定性 I/O 替身测试覆盖启动失败清理/重试、单 worker、Stop/配置队列、断线/会话轮换和发送前时效检查。`test_brain_network.py --sanitize` 检查实际 Brain 运行时、只读凭据及状态编码；`test_brain_controller.py --sanitize` 检查实际 ControllerLink 失联/换 boot 的历史故障保留。它们不运行真实 FreeRTOS/TCP/Flash；任务的大消息缓冲是 worker 独占成员，不能改为多个生产者共用，也不能从网络回调重入 transport。

## B1.3 配对迁移准备（2026-10-06）

`BoardPairingStore` 已提供共享只读加载器和首次安装原语。安装只允许本机角色/MAC，相同内容重试不写 Flash；已有不同身份、损坏或读取异常均阻止覆盖。set/commit/关闭后重新读取核验出现不确定结果时锁存失败，不自动擦除或重试。该原语尚未暴露为 USB/HTTP/MQTT 命令，当前固件仍不会在开机或握手时自动创建配对。

离线清单工具（macOS/Linux/WSL，Python 标准库，无密码、联网或烧录）：

```bash
python3 tools/prepare_board_pairing.py generate --output /tmp/new-pairing.json \
  --device-id bt-example --brain-physical-id 112233445566 \
  --motion-physical-id aabbccddeeff
python3 tools/prepare_board_pairing.py check --input /tmp/new-pairing.json
```

以上为格式示例，不是实际设备身份。Device ID 必须沿用整机身份，physical ID 使用各板原始 STA MAC 的12位小写hex，不是 Wi-Fi IP 或 AP MAC。清单包含同一个随机epoch和互为对端的两份CRC记录；0600文件排他创建，已有文件/符号链接拒绝覆盖。中断迁移应校验并复用原清单，不能换路径重新生成epoch。CRC不是签名或设备授权。

尚待接入旧 journal 结清、上下文/清除墓碑导入、凭据交接与最终受控安装入口。**生成清单或存储测试通过，不代表两板已经迁移；当前不要自行写 NVS，也不开放产品动作。** 迁移前须备份，真实服务器/Flash操作需单独授权。

测试：`python3 tools/test_pairing_store.py --sanitize` 对生产适配器注入存储故障；`python3 tests/test_prepare_board_pairing.py` 检查文件保护及与生产 C++ codec 的互操作。替身不是两SDK真实NVS断电与剩余页验证。

旧配置预检已增加 `ProductContext` 和 `LegacyContextStore`：只读旧 `productctx/payload` STRING，区分不存在、格式损坏和读取失败。完整保存宝宝 ID/姓名/品牌、配方及原版本；`cleared` 保留清除版本，不当成空缓存。严格检查 UTF-8、重复键、类型/范围及整条 2047-byte 上限，失败不改输出，不默默截断 UI 名称或把错误类型降级成默认值。旧版缺省姓名/品牌仍归一化为空/Friso，`updated_at` 只作元数据、不参与语义比较。

兼容边界：粉水比沿用旧 Motion 的 float32 转换后范围校验，水量/温度先按范围检查再取整。旧 ArduinoJson 会在字符串内直接保存少见 ASCII 控制字符；仅旧 NVS 读取入口将这种已知写法转成合法 JSON 转义，再交严格解码器检查。NUL、坏转义、重复字段等仍拒绝，转义后超过 2047 bytes 也拒绝，不修改旧键。新的 MQTT/UART JSON 不接受这些原始控制字符。

`encodeContextIdentity` 输出固定 little-endian、带字符串长度的语义字节，`contextDigest` 据此计算摘要；固定编码本身不是 Flash 记录或配对授权。旧数据读取尚未接入两板安装入口；`Missing` 不能证明设备从未运行。测试（先构建 Motion 获取 ArduinoJson）：`python3 tools/test_product_context.py --sanitize` 和 `python3 tools/test_legacy_context_store.py --sanitize`。原键不会被这些组件改写或删除。

Brain 持久状态组件已增加 `BrainStateRecord`/`BrainStateStore`：完整缓存、清除版本、本地发行序号和单个在途请求保存在同一 `brainstate/record`。显式受控安装后才可使用；普通加载不创建、不擦除记录。存储返回 `Stored` 仅代表提交/重新打开/读回核验成功，不是 Motion 已接受。已保存的在途请求重启后只能查询，不自动重新发送；清除请求槽保留序号，新宝宝配置不重写旧请求快照。读取/写入不确定时锁存故障，重复 `load()` 不能把同一运行实例倒退到旧 NVS。

请求/上下文 SHA-256 使用 SDK mbedTLS，对固定字段编码计算，不依赖 JSON 字段顺序，也不是消息认证。Brain记录编码上界1491 bytes、当前最大合法本地记录1421 bytes，包含CRC和配对身份。实际Brain v4 setup现通过已核对本机MAC/角色的配对，只读加载与安装器共用的Store一次，保留完整配置/墓碑、本地序号和pending；UART初始化随后失败仍可读取本地证据，不要求Cloud在线。Missing/损坏/身份不符不创建或擦除记录，pending未知时不报成无请求，不因业务加载失败额外关闭网络诊断。普通loop不反复load、不重发恢复的请求；实际UART原结果查询及匹配后持久清pending已接入，见下文在途恢复说明。触控本地持久占号及MQTT派发现已接验证路径，首次安装后整机重新上电联调及配置/终态桥接待验，不能据此声称产品完成。测试：`python3 tools/test_product_state.py --sanitize`、`python3 tools/test_brain_state_store.py --sanitize`、`python3 tools/test_brain_controller.py --sanitize`；主机 SHA 使用 macOS CommonCrypto 或 Linux/WSL OpenSSL（`libssl-dev`），只替换SDK调用边界，不自写哈希实现。

Motion新增 `MotionStateRecord`/`MotionStateStore`，在单个 `productstate/record` 中保存配置屏障、cloud/local各自消费水位及最近结果、一个执行意图或待确认喂养终态。新拒绝同样消费序号；重复结果不授权再次运动，新配置与新拒绝不改旧执行快照。喂养终态须验证Cloud stored回执和新鲜静止证据才清除，水位保留。initialize/clean记录独立执行结果，不生成喂养事件；重启加载不是继续动作的许可。Cloud Stop先走立即安全停机，静止后再保存序号屏障；存储失败不能阻止Stop。

当前BMS2记录上界4073 bytes（旧BMS1为1728），目标粉量继续采用原0.1g舍入。加载只读，写入不确定会锁存失败；不自动创建记录或覆盖旧NVS。两板持久导入和Motion新动作接收已接入，Brain派发及完整结果桥接仍待完成，不改变默认v3行为。测试：`python3 tools/test_motion_state_record.py --sanitize`、`python3 tools/test_motion_state_store.py --sanitize`。主机故障注入不等于实板Flash/NVS容量或MCU栈验收；Store使用静态成员缓冲，实板仍须测完整调用链的栈高水位。

Motion v4开机通过UART适配器核对过本机MAC/角色的配对读取`productstate/record`，`MotionStateRecovery`已接到实际setup和`pollDemo`。空执行槽及已归档历史队列不触发Stop或写Flash；未完成意图/未归档终态、已配对但记录缺失/损坏/读取失败，走既有`ProductSession::recoverAfterRestart`广播Stop、撤销参考并监督新鲜反馈，不续跑。确认静止后，prepare意图冻结为`reboot_during_feed / E_REBOOT_DURING_FEED`并入队；initialize/clean记Interrupted、清意图，不生成喂养事件。已有终态保留原成功/失败快照，历史队列不会套用旧单槽`eventPending`全局门禁。

恢复期间只对尚未确认停稳的冲突运动保留busy；确认静止后，即使持久写入故障，独立调试不因此永久禁用，产品仍保持关闭、旧证据不清除。停机证据要求CAN可用和停止请求之后的新鲜静止测量，不要求产品Ready、清除故障或关闭静止保持使能。开机从已解析内置JSON独立取得轴列表；即使旋转参数不匹配、脚本不能运行，仍可查询并确认停机，不妨碍之后修复配置。所有采样与同一保守时间界限比较，时钟推进不能让停止前样本变新。

STATUS读取真实持久水位、执行/event ID及配置屏障/墓碑；存储异常报告`E_STORAGE_FAULT`，保留已有更具体的机械错误，不能将故障状态里的零占位水位当作同步成功。测试：`python3 tools/test_motion_state_recovery.py --sanitize`，包含NVS故障和生产ProductSession/Flow的主机替身；`tools/test_demo.py`验证真实MotorControl/Executor与假CAN的停机检测，`tests/test_maintenance_wiring.py`仅检查实际入口源码连线。它们不代表真实CAN、Flash或断电验收。完整任务派发、配置同步、Cloud结果补传和安装仍待完成。

`BoardCommissioning` 已把本机身份核对、Motion旧事件/配置预检、业务状态导入和最后的配对写入串在一起。中断后只允许同内容且未使用的初始记录续装；已有配对但业务记录丢失时拒绝重新从零开始。Guard必须在各持久阶段提供排他维护、人工授权、新鲜静止及MQTT交接证据，Brain还须验证对应Motion导出的完整配置。Motion实际预约/静止Guard已接专用UART目标；Brain自动安装入口、一次性交接确认和双板完成切换仍待实现，不能随手传入true跳过前置条件。组件测试：`python3 tools/test_board_commissioning.py --sanitize`，不写真实Flash、不代表迁移已启用。

### v4 本地维护入口

两板USB支持`MAINT BEGIN`、`MAINT STATUS`、`MAINT END`，115200 baud、换行结束。响应`[maint] active/inactive/unsafe`；未配对Brain也可锁住本地UI，但这不证明Motion已静止。Motion进入/退出需有新鲜静止反馈，且没有产品待办、队列、驱动操作、Wi-Fi设置、OTA或HX711去皮。维护中网页运动/参数和网络设置拒绝，OTA不能开始；Stop、Stop all、queue cancel和只读查询仍可用。不自动停机，也不因长时间未操作自动退出。

Motion v4统一读取USB并保留`OTA CODE`，避免两处抢读；默认固件仍使用原OTA命令入口。命令限767个ASCII字符，每loop最多读64 bytes；超长、控制字符或2秒读取间隔的输入整行丢弃到换行，不执行后缀。已处理或拒绝的行缓冲清零，未知输入不回显。`OTA CODE`是既有本地管理员恢复功能，不要分享其输出。

### Brain UART安装前读取

Brain v4支持可选只读`PAIR STATUS`与维护内`PAIR DISCOVER <device_id>`。Motion经同一UART自动返回实际MAC及配对状态，无需第二USB；独立发现请求域、1000ms截止、无NVS写入/动作。`found`不创建配对或授予Start，损坏/读错/冲突不冒充Missing；超时可重试。自动持久安装已内部调用此路径，该支持入口不是额外安装或日常验证步骤。单USB工具已接，实板联调仍未完成，正常配对使用轻量HELLO自动复用。

发现成功后，维护内`PAIR READ <device_id>`使用同一UART逐块拉取既有`MaintenanceExport`完整快照；只读`PAIR RECORDS`报告idle/pending/complete/invalid/unavailable/timed_out，不打印宝宝数据或密码。协议kind18，每块155 bytes、1秒截止，整体5秒；旧查询ID不能推进来源。同一Brain新的显式读取可用新挑战、递增ID和offset0立即替换丢包后的旧捕获，不重试动作。Brain最多临时分配16KiB+NUL字节区、另分配完整快照与codec解析区，失败局部返回，不写NVS或影响Motion调试；实际堆峰值/时序仍须实板测量。完整UTF-8上下文/墓碑、水位和四结果队列保留，读取状态不冒充Missing；complete仍不是安装授权。真实自动安装将内部调用此路径，而不是要求用户执行多条支持命令。

验证入口：`python3 tools/test_motion_export_snapshot.py --sanitize`、`python3 tools/test_board_export_transfer.py --sanitize`和生产adapter/controller测试。前两者运行生产codec/导出及SHA/NVS边界替身；adapter短写测试另有内存导出fixture，不冒充全NVS实板。UART不是加密通道，接可信台架；只读捕获不证明后续未改变。

### Brain UART安装前维护预约

Discovery成功后，可选支持命令`PAIR HOLD <device_id>`在Brain本地维护内明确请求Motion预约；`PAIR HOLD`只读查看，`PAIR RELEASE`或Brain `MAINT END`经同一UART结束，不需要Motion USB。kind19绑定双方boot、Brain MAC、设备、nonce和递增ID。Motion复用当前静止/无工作条件，并与USB维护/OTA互斥；网页冲突写入/运动、网络改配和OTA启动暂拒绝，HTTP Stop/Stop all/queue cancel、合法原始Stop/Interrupt/disable和读取继续可用，CAN监督照常。Motion产品接收/Stop已接入，但Brain派发和完整产品入口仍未开放。

Brain仅为显式预约每500ms续期，响应截止1秒，Motion预约3秒到期自动释放；普通启动、配网、读取和冲奶不自动预约。release无需Ready/传感器/Cloud恢复，迟到或同nonce请求不重新打开已结束会话，失败不自动acquire。到期只释放RAM预约，不回滚Flash。核心`tools/test_board_maintenance.py --sanitize`和实际adapter/controller主机测试不替代UART/HTTP/Flash实机验收；自动安装已内部调用此路径。这些命令只是支持入口，不是额外安装步骤。

### Motion UART持久安装入口

可选v4的kind20安装通道已接同一UART收发owner及Motion实际导入目标，不新增USB诊断写命令。完整配对记录、旧档案/墓碑经有界分片传入；目标复用现有`productState`，每个持久阶段重新核对预约设备/nonce/双方身份与当前静止条件，调用原导入协调器，业务记录先写并读回、配对最后写。旧事件未结清、已使用水位/结果、内容冲突或存储错误不被覆盖，缺失上下文按全零编码。不确定写入错误只锁存导入组件，不借此永久禁用独立调试；超时保留部分写入证据，不自动重试或擦除。

本板Installed不在当前boot激活运行UART身份、网络或运动；Brain自动持久协调入口见下节。首次成功后采用整机手动重新上电一次，复用已有握手、不新增完成协调协议；重启联调仍待验。一次性交接位是受控入口的操作声明，固件不能据此证明broker账号已经撤销。测试`tools/test_board_install.py --sanitize`、`tools/test_motion_install.py --sanitize`及实际adapter测试使用生产codec/导入器，边界I/O为替身，不是实板Flash或迁移验收。

### 单Brain自动持久安装

Brain v4唯一loop已接`BrainInstaller`。本地维护期间，`PAIR INSTALL <device_id> HANDOFF_CONFIRMED`一次发起后，内部自动发现、预约、读取完整Motion记录、选择/复用配对代次、写Motion、再次读取核对、最后保存Brain。空记录才生成新epoch；孤立初始业务记录只按相同身份/上下文续装，不覆盖已使用水位、pending或结果。`BrainStateStore::inspectForCommissioning`只读完整现存证据，不创建记录、赋予ready或清故障；实际MAC及双方配对由安装owner核对。

该入口仅供已授权安全台架上的首次迁移，不是MQTT/普通启动入口。确认词表示操作者已停止旧Motion产品会话并完成一次性账号撤销/换密与Brain配置，不能用“Motion v4不连MQTT”代替。`PAIR INSTALL STATUS`返回阶段、实际失败类别及`writes_may_have_persisted`，`PAIR INSTALL CANCEL`尽力结束预约；失败/超时不擦NVS、不自动重发。同boot显式重试保留原预期身份/完整配置，先做受控恢复读取：允许尚未写配对或已完整写入同一配对，仍拒绝外来记录，不借重试清除可能已写入的提示。普通诊断读取保持原发现匹配规则。安装中原支持写操作/网络改配暂拒绝，退出本地维护会终止协调并释放预约，Motion Stop/读取不受影响。正常启动不调用安装器。

`persisted_restart_required reason=activation_pending`仅表示双板持久记录已核对，**不代表产品控制已完成**。2026-10-07已确认采用最简首次激活流程：成功后退出维护，整机停稳时用户让 **Brain和Motion一起断电再上电一次**（USB不能让单板保持供电），保留全部NVS；两板随后读取原配对/业务状态，通过现有HELLO/STATUS自动连接。只连接Brain USB，无需Motion USB、FINALIZE、重启回执或双板重启协调协议。

**日常开机和保留NVS的重新烧录不需要重新配对或人工双板校验**；换板、记录缺失/损坏或身份冲突仍按既有受控恢复处理，不清空或静默认领。失败/超时不能报安装完成或用重新上电代替核对，先保留证据按同身份恢复。此决定替代原自动运行切换要求，不新增固件重启或运动命令。

2026-10-08安装恢复修复：Motion的安装发现回复在收到Discovery支持帧时重新只读加载当前持久配对，而不是一直报告开机快照。否则Motion已保存、Brain单独重启后会发现Missing却读到Ready，报`records_invalid`无法续装。刷新只改变安装发现信息；不重启UART、不更改boot/运行时配对、不启动产品动作或MQTT、不写NVS，已排队的回复保持原字节。读取错误明确返回对应诊断，不沿用旧Ready。首次成功后整机重新上电规则不变。父项目`Test/dual_main_install_reset_check.py --sanitize --restart brain|motion`分别覆盖三个切点及档案/墓碑，存活板RAM/UART/预约不被重建；这是软件证据，不是Flash断电或联合镜像升级验收。

2026-10-08已加入最终电脑端单Brain USB入口（在本子仓库根目录执行）：

```bash
python3 tools/install_brain.py --port /dev/cu.usbmodemBRAIN --device-id DEVICE_ID
```

需要兼容的成对v4固件、可信UART/USB、停稳且已授权的台架及安装了pyserial的Python。替换端口和已有整机Device ID，关闭其他串口监视器。先在broker完成旧Motion产品会话退役及账号撤销/换密；输入`INSTALL`仅确认这次交接，不代表工具验证了broker。可以在同一USB会话隐藏输入Wi-Fi/MQTT凭据，也可保留原配置。工具一次发起安装、只读查询阶段并退出维护，不需Motion USB或手动支持命令；不发送运动、擦除、FINALIZE、自动重试或复位命令。

成功仅为`persisted_restart_required / activation_pending`及本地维护退出，随后用户按上文让整机（含USB供电）手动重新上电一次。失败、超时或取消可能已有网络/配对/业务记录保存，不是跨板回滚：保留NVS，必要时在Brain USB查看`PAIR INSTALL STATUS`、`MAINT END`，核对后仅明确重试原身份。已有安装在运行或等待重新上电时，工具不重新发起、不取消原安装。普通开机、保留NVS的烧录不运行本工具；串口打开仍可能被驱动复位。

工具与运行桥接已有软件实现，但不能直接用于生产迁移；真实账号交接、重新上电、main/broker组合恢复及整机验收仍待完成。`python3 -m unittest discover -s tests -p test_install_brain.py`检查电脑端假USB故障；完整`tools/test_brain_installer.py --sanitize`额外让电脑工具通过管道操作生产USB解析器/协调器和双NVS替身（空档案、活动档案、墓碑、旧事件拒绝），不是实际Arduino main、无线网络、Flash、UART电气或机械验收。CI/WSL/源码包包含工具；本次未运行远端CI/WSL、烧录、整机断电或生产操作。

### 只读请求结果查询与重启恢复

`ProductResultQuery`实现既有RESULT_QUERY/RESULT的严格JSON codec，按设备/source/seq/command ID读取同一个Motion Store已验证的RAM记录。最近ACK、在途执行槽或四结果队列能返回原接受/拒绝和独立执行结果；水位只区分过期/未知，不能假定接受。身份矛盾返回request_conflict，未加载/存储故障不伪装零水位历史；普通请求返回SHA-256摘要供后续Brain核对，Cloud Stop不借用普通请求摘要。

查询组件本身不load/写NVS、不重放或清除请求。两板v4运行入口已在唯一UART owner上接入查询/回复：当前HELLO会话及双方boot必须匹配，Brain只接受当前查询的设备/source/seq/ID结果；超时、断链、传输失败或对板重启只结束瞬态查询，不假定原请求失败。Motion以原运行Store作只读回答，发送器忙时保留一条回复；查询不要求机械Ready、Cloud在线或新鲜屏幕STATUS，不影响独立调试。

Brain主循环的`BrainPendingRecovery`用开机已读入的同一个Store自动查询原在途请求，不重占号、不发COMMAND。只有known结果同时匹配完整请求身份及原SHA-256摘要，才持久清除本地pending；原接受或拒绝均可解析接受在途，**不等于冲奶完成，也不清Motion终态队列**。unknown/过期/冲突/存储故障/摘要不符/超时保留pending，每秒只读重查；本地维护暂停查询，清槽存储失败保留故障证据，不反复写Flash。不增加安装或日常人工步骤。

主机验证：`python3 tools/test_product_result_query.py --sanitize`验证lookup/codec；`python3 tools/test_board_link.py --sanitize`、`test_board_arduino.py`、`test_brain_controller.py`验证生产UART core/适配器/屏幕controller；`python3 tools/test_brain_pending_recovery.py --sanitize`验证生产Store在途恢复。SDK/NVS/UART边界替身不代表实板Flash断电、UART时序或完整产品验收；Brain动作派发、配置和终态桥接仍待完成。

### B2 Motion产品执行接收（2026-10-07）

实际v4入口复用同一`MotionStateStore / ProductSession / DemoFlowController`，通过`MotionProductRuntime`完成initialize/prepare/clean的接受先落盘再执行。重复仅返回原决定，set_target_temp沿用参数更新，reset_error只可already_clear，未实现OTA明确拒绝。首片/重组时间计入TTL，重复首片或超时重传不刷新；持久接受后截止耗尽则记录未启动失败，不倒改原接受。

运行期忙碌拒绝只冻结一条完整请求和拒绝结果，停稳后落盘才发最终COMMAND_RESULT，不重检为接受或排队执行。此前断电只能报告unknown、只查询原身份。终态在RAM冻结内容/时间，停稳后写入4条结果队列，Cloud桥接尚未接通。Stop/心跳不等普通回复或Flash；失败/监督超时可显式重试同目标，正常重复不刷新停机窗口。HTTP Stop/取消通知运行器，非喂养中止记Interrupted。D1终态未停稳仍Stop；D2不调用旧Cloud断网中止路径。

持久故障保留journal，不等于仍拥有后续调试动作：停稳或已准入的网页动作接手后释放旧RAM机械身份，旧product目标不能停止新工作台，也不永久锁网页。工作台UART目标化尚未实现，直接HTTP安全Stop仍可用。`python3 tools/test_motion_product_runtime.py --sanitize`验证生产组件/边界替身，main接线另作静态检查；不是实板CAN、NVS、HTTP或完整App验收。默认非食用能力仍关闭；不因本项安装、烧录或宣称产品可供喂养。

### Brain 单次UART发送接口

Brain的共享UART core、Arduino适配器和Controller现提供单次`requestCommand`/精确COMMAND_RESULT及优先`requestStop`接口。发送排入不等于Motion接受，LINK_ACK不完成普通请求；1000ms超时、断链、换boot或取消仅保留未知，随后查询原身份，不自动重发。首帧/调度预算集中为`kCommandFirstFrameBudgetMs=50`，从编码TTL扣除并约束本地写入；过期残帧只改坏尚未写出的CRC后排空，保留Stop/心跳且不交织字节，不宣称已发帧可撤回或实板时限已验收。local Stop由唯一owner生成既有transient ID、seq0，不等待NVS；Received不是停稳。

业务owner在调用前负责local持久占号、Cloud会话/原截止核验和Stop新鲜目标绑定。BrainNetwork/main现已调用Cloud及local触控owner；配置和终态桥接待完成，故不是完整产品已启用。测试`tools/test_protocol.py --sanitize`、`test_board_link.py`、`test_board_arduino.py`、`test_brain_controller.py`、`test_brain_local_dispatcher.py --sanitize`及`test_motion_product_runtime.py --sanitize`覆盖对应生产组件/SDK边界替身，不替代真实Flash/CAN/RTOS或完整App验证。

### Brain Cloud派发owner（2026-10-07，软件子项）

`BrainCloudDispatcher`是UI loop唯一Cloud请求owner，不是网络worker或动作队列。原会话/代次/5秒TTL核对含SHA耗时，单次COMMAND后只用精确业务结果ACK；未知只查询原身份及摘要，Expired重复不伪造拒绝。Cloud负责永久发行，Motion负责持久接受，不新增Brain Cloud账本或修改local pending。

Stop绑定新鲜product执行ID或明确静止idle，独立序号不受更高普通水位误挡；不等待Store/普通结果/旧信息ACK。main在可能Flash路径前再次服务UART，短写/FIFO忙时不保证已完整发送。Received不是停稳。`publishAck=true`仅入队，入队失败的已有回复槽保留重试，busy额外回复best-effort，新Stop可替换旧信息ACK；不保证wire/broker/Cloud落库。测温未知保持null，不以目标温度冒充实测；`commands_enabled`仅新鲜链路开放，`can_start=false`保留。

Stop传输结果未知时，只按原device/source/seq/ID经既有RESULT_QUERY重查，不重发Stop、不绑定后来任务。精确Known且空普通摘要/outcome=none才补原ACK/reason/session；unknown/过期/冲突/故障不伪造结果。只用有界RAM跟踪最后一笔Stop；新命令/Stop可抢占信息查询，普通接受恢复、本地持久pending或维护优先使用唯一查询槽。查询不要求Cloud在线或新鲜STATUS，也不写Flash；Brain复位后由Cloud永久账本保留原未知，不自动恢复/重发Stop。

取消仅匹配当前ResultQuery的kind/message ID，未发字节的查询立即释放普通槽，不误删后来的COMMAND。若已有半帧上UART，单次作废CRC后先排空、不交织下一帧；这段真实背压仍可返回busy，不另排动作或自动重试已消费请求。Stop/心跳优先槽保留。

`python3 tools/test_brain_cloud_dispatcher.py --sanitize`双SHA各247场景，保留205并新增42项Stop恢复（`--case stop_recovery`），每版含10项真实UART core/codec/Store组合；静态接线21另测，不合称生产main/Network/runtime/broker动态闭环。新增三项持久Stop丢回执/查询、换Motion boot及五类identity/摘要反例，未发查询同loop被Clean抢占、半帧CRC排空不交织且只执行新的显式Clean。查询无Store写入，Stop不重发；Motion handler是测试回调，不是生产Runtime或真实停稳证明。Stop原TTL边界/回绕、Unknown放行反例、ID保护、Known优先及持久证据保留继续覆盖。production mode3保留local pending只证明退出不清证据，不代表main允许此时新Cloud命令（仍由原admission拒绝）；生产Controller连接仍需新鲜STATUS，FakeLink无STATUS不代表实际main允许。真实Flash/CAN/RTOS/Stop时限仍待台架。

Cloud Unknown最小放行已获用户2026-10-07确认：原TTL耗尽，过期后收到的新鲜Motion状态无活动/无busy且已停稳时，仅退出RAM执行在途；不同ID/更高序号的新请求可进入。原结果仍Unknown、无伪造ACK、不重发/清持久证据；已到达精确Known优先处理，旧/未来/失联状态不放行。最后retired ID仅有界RAM guard，所有历史ID由Cloud永久账本防复用；local NVS pending不因此清除。工作台UART目标化、版本配置和终态/Cloud回执仍待接；仅本地软件验证交付，未推送、烧录或部署。

### Brain本地触控owner（2026-10-07，软件子项）

`BrainLocalDispatcher`与Cloud/pending恢复共用UI loop、UART及唯一BrainStateStore，不新增线程、动作队列或持久格式。Initialize使用原屏幕NotReady/Error准入，不要求Cloud或宝宝缓存，也不以缺少停稳证书阻止Motion既有监督恢复。Start仍要求新鲜、空闲停稳的Motion及匹配版本/宝宝的有效完整缓存，由Motion核验配方/资源/四条结果队列容量。原始STATUS保留Motion readiness，Controller显示和Cloud投影独立准入，不能把遥测本身当动作授权。

触控先检查UART可入槽，再持久保存下一local_seq、canonical ID及完整配方快照，随后仅发送一次COMMAND；原5秒TTL包含准备/Flash/SHA时间。精确COMMAND_RESULT接受或拒绝才清接受pending，不等于冲奶完成或删除Motion事件。超时、Stop取消、换boot及未知保留pending交原只读恢复；已持久但后续未发送同样保留，不套用Cloud RAM放行。对此明确未发送情形的简化清理尚待单独决策，未实现。

新显式触控可让信息性Stop查询退出，不取消正在恢复的普通Cloud请求或实际Stop传输；维护/坏Store/持久pending仍只阻止普通请求。`tools/test_brain_local_dispatcher.py --sanitize`使用生产Store/codec/core及边界替身，不是实板触摸、完整main/RTOS/CAN/Flash或Cloud上传验收。公开can_start仍false，自动配置/记录桥接等待B3；受控动作仅显式非食用验证能力，默认关闭，不允许食用结果。

本地runner双SHA各107场景（每版5项真实core/Store组合、Motion为脚本handler），Code独立复跑通过；Cloud owner新增3项触控抢占边界、双SHA各250通过。保留旧owner与替换pending的回归，清理必须匹配冻结原seq/ID；不同请求回执不清后来证据。静态接线22项、原pending恢复及协议/Arduino/Controller/LVGL回归通过；Code和QA限定范围复审通过，不等于完整设备/生产链路。CI/WSL runner已接，远端未执行。

### v4 配置发送与 Motion 持久确认

同一UART core及Brain Arduino/Controller adapter现提供显式`requestContext`，Motion main注册到同一`MotionProductRuntime / MotionStateStore`，v4不再读旧`productctx`作为RAM档案。CONTEXT复用完整feeding_context JSON；CONTEXT_RESULT严格包含`reply_to/device_id/profile_version/cleared/context_digest/status`。精确当前会话回复才完成传输，业务层还须检查`stored/unchanged`；busy/conflict/storage_fault或LINK_ACK都不是配置已保存。换boot或失联使旧确认失效，Stop可取消本配置传输，不擦缓存或重放运动。

新版本在运动/维护忙碌时只观察版本/摘要，返回busy且不写Flash；下一次旧Prepare被阻止，已接受的快照不变，Initialize/Stop/独立调试不增加配置门禁。已停稳的清洁提示、故障或Complete显示保持不挡配置保存；同内容重发核对持久屏障并恢复空RAM，不重新写Flash。默认v3保持原行为。

Brain v4现从既有MQTT配置槽解析完整档案/墓碑，回调只暂存最新合法版本；同一UI loop在紧急UART服务后保存到安装器共用的`BrainStateStore`，再由`BrainContextSync`发送。离线开机或UART换会话自动同步已保存缓存，不等待Cloud、不重配。精确stored/unchanged才允许新的Prepare；配置更新不修改已预留或已接受任务的快照。Busy/丢回执/超时等只重试幂等配置，间隔至少1秒，不重发动作。维护或Stop传输未决时暂缓配置Flash，合法非Prepare请求可抢占配置传输，Initialize不要求配置确认。终态/Cloud回执桥接仍未接通，公开can_start仍false，不宣布App全通。

测试：`python3 tools/test_product_context_messages.py --sanitize`和`python3 tools/test_motion_product_runtime.py --sanitize`，后者可用`--case contexts`或`--case context_uart`限定范围。它们直接运行生产codec/store/core，Flash、机械和串口I/O为替身，不能证明实板或完整main/Cloud链路。

自动同步专项：`python3 tools/test_brain_context_sync.py --sanitize`（同一Store、缓存/版本与双UART core/Motion runtime组合）和`python3 tools/test_brain_network.py --sanitize`（生产Network/CloudLink收件，SDK I/O替身）。两组不是实际MCU/broker或完整App验收；台架须核对保留NVS的冷启动、离线缓存、换宝宝/解绑、丢回复与Stop背压，不自行手写NVS绕过缺失/冲突。

同版本不同内容会记录本次运行的版本冲突，仅暂停该版本的新Prepare；重发原内容不能解除，需Cloud更高版本保存并精确确认。低版本旧包不撤销有效缓存，Initialize/Stop/调试和已接受快照不受这个门禁影响。冲突观察只保存在RAM，不新加NVS格式，也不承诺断电后的跨板原子撤销；故障配置仍须受控核对。

### v4 终态与 Cloud 回执编解码

共享`ProductEventMessages`实现已确认kind12/13的纯编解码。终态从持久执行快照提取原宝宝、配方、event ID和冻结uptime；Cloud来源附原`command_seq`，local_touch不附云序号。float32数值以9位有效数字往返，JSON转义后最多2047 bytes；失败不修改输出。收到新配置、重连或补报不能改写原终态；Brain转发应保留原JSON字节，不重新编码。回执严格为既有type/device_id/event_id/status=stored四字段，不把LINK_ACK、MQTT发送成功或命令接受当落库证明。

共享UART core、Arduino适配器及Brain Controller现有显式`publishTerminal`/`forwardCloudReceipt`和按角色验证的回调，复用kind12/13及唯一普通分片槽；同一已配对会话即可补历史终态，不等当前宝宝、Ready或新鲜STATUS。回调只在同一loop交付，原JSON不改写；没有NVS清槽、自动重新投递或新动作。Stop/心跳走原优先通道，可发送的业务回复优先于终态，尚未准备好的延后回复不挡历史终态。普通槽释放后优先预约到期回复和已有STATUS，避免连续补传挤掉遥测；没有样本时仍可补传，不增加队列或准入门禁。

两板main现已接`MotionResultDelivery`/`BrainResultDelivery`及BrainNetwork真实终态发布。Motion复用四条持久结果，成功入UART队列后按一秒间隔轮转；槽忙/断链未入队时保留原项，下轮重试，不消耗周期而与STATUS相位锁定。转换失败的槽按同一周期让行；后续Cloud拒绝不退回轮转索引。传输失败不改原JSON/身份/水位；Intent不发布成喂养记录。Brain按当前MQTT代次发布原字节，无第二份持久outbox；旧结果不要求当前宝宝、Ready、TTL或新鲜STATUS。真实Cloud stored回执按精确config topic/当前代次接收，匹配模拟结果先交其owner，其他回执经UART到Motion。两侧仅复制一条待处理回执，不覆盖不同项；丢回执由原事件补传和Cloud幂等回执恢复。

Motion回调不写Flash，loop在紧急UART/电机服务之后处理精确匹配删除，维护/OTA只暂缓。已归档旧结果可在下一任务运行时确认，保持新意图及消费水位；未归档执行槽还须运行器/恢复owner释放且新鲜停稳，不能抢在其归档前清槽。主机测试不能证明Flash写入期间真实Stop时限或CAN反馈余量。

测试：`tools/test_result_delivery.py --sanitize`测生产Store/codec/UART core与relay组合，NVS/Network边界为替身；`tools/test_brain_network.py --sanitize`另测生产网络worker的原JSON发布及代次，`tools/test_board_link.py --sanitize`/`tools/test_product_event_messages.py --sanitize`覆盖传输/codec。父仓库`Test/brain_event_contract_check.py --sanitize`独立验证真实隔离Cloud事务及回执decoder。它们不构成完整main+broker+App或实机验收。真实can_start仍false；默认v3及旧单槽事件路径不变，B3.3/安装激活与B4继续待做。

### Brain-only 仿真（Brain v4 USB入口）

`main-controller/src/brain_simulation.h`只模拟一个整体计时，默认关闭；冻结原宝宝、配方、事件身份和终态uptime，Stop生成失败而非成功。四条RAM结果容量包含当前动作预留，未上传的旧结果不阻止下一瓶，只有满容量才限制新请求。真实Cloud stored回执经解码并匹配后才移除对应结果；没有UART、运动或NVS接口，RAM结果不承诺掉电补传。

已有有效配对、业务缓存和网络配置的Brain v4，在自己的115200波特率USB监视器发送以下命令（每条以换行结束）：

```text
SIM STATUS
SIM ON
SIM OFF
```

不需要额外仿真编译环境或修改Motion固件；默认v3入口不支持这些命令。模式默认off且不持久化。ON/OFF仅切Brain请求路由，实际模拟任务或未决真实任务存在时返回busy，不取消或抹掉原证据。成功切换轮换MQTT会话，等待Cloud重新完成现有probe即可，不需人工配对；迟到的旧模式命令不能转为真实UART动作。有效保存的身份/缓存即可让Motion断电或UART未接时测试，不要求Motion在线，但不为未安装的空白Brain伪造配对。

开启后只支持App经Cloud发送Prepare/Stop：默认整体15秒计时，不逐项模拟电机；Stop生成失败记录。屏幕只观察仿真状态，本地Initialize/Start不进入UART或模拟本地序号。Cloud下发的配置仍保存Brain缓存，但不转发Motion；退出后恢复原真实同步。真实UART心跳/只读恢复仍属于原链路，不是仿真UART消息。Motion独立网页不改。

状态明确为`hardware_profile=simulation`、`motion_connected=false`，水温、水量、余粉及物理有效性仍未知/false。Cloud和新App仅对这一完整v4 Brain模式采用显式commands_enabled/can_start决定启动，不放宽真实固件。MQTT模拟终态包含`execution_mode=brain_simulation`并保存原宝宝、配方和uptime；不修改UART kind12/13或SQLite schema。仅用于隔离测试家庭/数据库，模拟记录不是实际喂养。结果在RAM中，断网时继续计时、重连补传并等待真实stored回执；Brain复位会丢失计时/结果，Cloud的completed_at仍为首次接收时间，不承诺离线真实UTC完成时间。

测试：`tools/test_brain_simulation.py --sanitize`、`tools/test_brain_simulation_dispatcher.py --sanitize`及`tools/test_brain_network.py --sanitize`，父仓库另运行`Test/brain_event_contract_check.py --sanitize --brain-simulation`。`python3 tools/test_brain_main.py --sanitize`直接编译生产Brain `setup()/loop()`、ControllerLink/Arduino UART适配器、CloudLink worker和业务owner：七个隔离进程覆盖USB开启、完成/重复、期限点触屏Stop优先、同批墓碑/旧请求及冻结结果、SIM OFF后匹配回执、ACK前/后断网再补结果和屏幕初始化失败。主循环不复制，UI对象/USB/UART/NVS/Wi-Fi/MQTT为替身，worker让行时按确定性时钟执行loop；配网写入不在此fixture范围。测试stored消息是注入输入，不冒充Cloud事务证明；不运行Motion main、真实FreeRTOS/TCP/broker，也不证明触摸布局、电气、Flash或时限。真实终态UART桥接已有源码与限定验证，完整多owner/双板main+broker组合及整机迁移验收仍未完成。

同一host runner的`--pipe`仅提供测试JSON行接口：运行实际主循环，导出原始MQTT发布字节并接收外部驱动的MQTT输入、模拟USB和时钟；不新增固件编译环境或设备串口命令。父仓库`Test/brain_main_broker_check.py`使用此接口将SDK传输替身接到本机隔离broker和真实Cloud API/SQLite，验证模拟任务的完整数据往返。它不是MCU PubSubClient TCP、Motion UART、电机、FreeRTOS或真实App验收；合成安装身份与测试绑定也不证明首次实机安装。

### Brain USB网络配置

仅适用于开发中的Brain v4，不是默认v3或Motion网页。先核对USB端口并关闭其他串口监视器，使用可信电脑/USB线；Python需要`pyserial`，可使用已安装PlatformIO的Python。工具只写网络设置，不烧录、重启、安装配对或修改Mosquitto ACL。

```bash
python3 tools/configure_brain_network.py --port /dev/cu.usbmodemBRAIN wifi
python3 tools/configure_brain_network.py --port /dev/cu.usbmodemBRAIN mqtt
```

按提示输入SSID/密码或broker/端口/账号/密码；密码隐藏输入，不写命令行、文件或串口日志。工具先确认Brain，再进入本地维护、保存、退出；失败也尝试退出，若USB断开则重新连接后用`MAINT END`解除本地UI锁。成功仅表示存储读回通过，需另查连接状态。Wi-Fi与MQTT均可在未配对时预存，不创建网络任务、不联网或发布设备身份；有效配对后网络启动时读取已保存凭据。已运行时配置成功由worker重连。首次UART安装入口已接，网络预存不等于身份安装或实机验收。旧账号撤销与新Brain独占产品MQTT仍需单独受控交接。

底层命令为`NET STATUS`、`NET WIFI <ssid_hex> <password_hex或->`、`NET MQTT <host_hex> <port> <user_hex> <password_hex>`；除STATUS外要求本地维护。hex是编码不是加密，不要将完整命令贴聊天。Wi-Fi/MQTT配置由各自NVS格式保存，不是跨两项事务；一项成功另一项失败时仅重试失败项。凭据保留在设备NVS/RAM中，本工具不提供存储加密。USB打开仍可能因驱动/适配器复位板子；实机验证须在授权安全台架执行。此入口未改变Motion独立网页，也不开放产品动作。

维护中可发`MAINT EXPORT <device_id> <32位非零小写hex挑战值>`，导出实际MAC、启动识别号、配对及业务记录；Motion另读旧完整上下文/墓碑并检查旧事件是否存在。缺失、损坏、IO错误和身份冲突分别报告，错误记录不冒充空状态。导出只读，不含Wi-Fi/MQTT密码；可能含宝宝姓名/配方和任务信息，须私密保存，不提交Git或贴聊天。重组后的内容以`[maint-export] `开头，是一行schema=1 JSON，三类记录以hex承载；USB实际使用`[mx] offset:crc:hex`分片，每片至多16 bytes，采集工具校验连续偏移和CRC后重组，可忽略片间普通日志。它不是迁移资格或新机证明，更不是日常冲奶前置步骤。

包括短提示和OTA CODE在内的USB回复均排队，每loop最多64 bytes，按可写容量处理短写；5秒未完成或本地安全条件丢失则中止导出，不自动解除维护锁。Brain的Arduino 3.3 HWCDC设1ms超时，避免零超时重试计数下溢；Motion旧SDK保留0ms。不是严格实时USB保证，背压或帧内日志干扰仍可能导致本次采集失败。发送期间不处理下一条USB命令，UART、网络、Stop和反馈轮询继续。文件采集工具`tools/capture_board_export.py`使用随机挑战关联本次请求，排他保存0600文件，保留维护锁，不自动安装、退出或重启。关闭其他串口监视器；串口适配器打开端口仍可能复位板子，必须在已批准的安全台架上使用。需pyserial，可使用已安装PlatformIO的Python环境。

单独维护/导出不写配对或业务记录，也不调用BoardCommissioning；持久安装和网络配置各走上文明确入口。Brain网络仍发布只读状态/probe；`active`不能当作网络已停止或导入获准。测试`python3 tools/test_maintenance_console.py --sanitize`运行解析器/锁；`python3 tools/test_maintenance_export.py --sanitize`运行生产导出/USB调度但使用MAC/NVS/字节端口替身；`python3 -m unittest discover -s tests -p test_maintenance_wiring.py`只是入口连线静态回归，不代替串口/网页/机械实测。

## V1 Motion 集成状态（2026-10-01）

`device-controller` 的 DISPLAY 构建正在接入现有 Babytech Cloud/App：Motion 自行连接 MQTT，使用设备专属 topic、产品状态、命令 ACK 和终态事件；屏幕经 UART v3 提交本地操作。旧 Brain/v2 协议库保留兼容回归，已不是 `main-controller` 的运行入口。

产品链路保存运行 journal 后才允许动作，终态在 Cloud 落库回执后才清除；MCU 重启补报失败并请求受监督停机，不自动续跑。有效缓存允许屏幕离线本地启动，资源/机械/存储门禁不变；本地流程不依赖网络，远程流程仍在 Wi-Fi 丢失时中止。单槽未确认前不能再次产品启动，须先部署支持回执的 Cloud。称重无效上报 `unknown`，不是 `low`。主机测试不代替真实断电、NVS 和停止反馈验收。

这一默认v3集成**尚未完成验收**。目前通过的是 Motion 构建、主机协议/动作测试、实际 JSON 编码器及 Stop 长载荷优先级测试；父项目 `Test/motion_contract_check.py` 也用生产编码器输出验证了 Cloud 的状态、审计、SQLite 和 WebSocket 处理。尚未用新 Motion 板完成 MQTT 往返、屏幕联调和安全台架验证。默认构建不授权非食用固定转数产品流程，低水位输入未配置时也保持 `noready`，不可用于真实喂养。父项目的 `Docs/refactoring/BRAIN_MOTION_CLOUD_APP_INTEGRATION_PLAN.md` 保留旧 M0–M3 证据，并记录 2026-10-06 新 B0–B4 迁移计划：现有屏幕板兼任 Brain，Motion 保留独立调试网页。可选v4迁移已有上述部分实现，默认v3仍是Motion直连MQTT；完整迁移尚未完成。

## 原有主线基线（2026-09-24）

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
App ── Cloud ── MQTT ── device-controller（Motion、传感器与电机）
                              │ UART v3
                              └── main-controller（Brain 屏幕基线）

后续目标：App ── Cloud ── MQTT ── Brain ── UART ── Motion
```

## 当前可运行的内容

当前可构建屏幕 Brain 和独立 Motion。旧 Brain UART v2 库/测试仍保留兼容证据，但旧状态网页不再编入 Brain；Cloud 产品链路仍处于 Motion 直连基线的集成验证阶段。

- `main-controller/`：480×320 ST7796 屏幕、GT1151 触摸、LVGL 页面及 UART v3 状态/意图；沿用原 Initialize/Start、离线与忙碌门禁。暂不提供旧 `Babytech-Debug` 热点、网页或 Brain OTA。
- `device-controller/`：`Babytech-Motion` 热点、可保存的路由器 Wi-Fi 配置、内嵌网页和 HTTP API；任意 CAN ID 1..255 的使能、失能、相对运动、停止、广播停止，以及真实位置/速度/电流反馈。下板默认从 GPIO1/2 采样 HX711，网页可修改并持久化 DOUT/SCK，同时提供称重状态、去皮和标定 API。
- `shared/BoardProtocol/`：板间 UART v2 四指令协议、客户端与执行入口。完成状态来自真实电机反馈；机构尚未接入，称重数据暂未加入板间载荷。
- `tools/test_protocol.py`、`tools/test_motion.py`：主机协议、参数与反馈解析检查。

调试热点保留在 Motion 板，便于独立台架调试；产品链路暂由 Motion 直连 Cloud，显示由迁入 `main-controller` 的屏幕基线负责。HX711 已接入调试和产品状态，完整 MQTT、显示板与机械流程仍待联调验收。上电不自动执行运动。

详细操作和接口见 [下板网页调试](docs/motion-debug.md)。

直发队列、执行诊断、全局查询预算及同步/螺旋动作见 [电机编排队列](docs/motor-queue.md)。软件验证和待实测项目见 [同步开发与验收记录](docs/motion-sync-development.md)；已完成部分低速、小幅及多圈同步实测，完整负载与机械验收仍待完成。

新版桌面协议工作台已接入真实电机接口与 Wi-Fi，网页随固件内嵌。最新能力范围、重建、烧录地址和验证边界见 [工作台交付说明](docs/motion-workbench-release.md)。

默认 DISPLAY 构建已接入产品显示板 UART v3、非阻塞流程、阶段调试和内置 JSON 配置；上电不自动运动。见 [演示操作说明](docs/motion-display-demo.md) 与 [开发验收计划](docs/motion-display-demo-plan.md)。Brain/Motion v2 需显式以 `-DMOTION_UART_PEER=1` 编译；机械与双板实机验收尚未完成。

## 目录

```text
main-controller/       platformio.ini、src/、include/、lib/、test/、data/
device-controller/      platformio.ini、src/、include/、lib/、data/
shared/                BoardProtocol、DisplayCore、DisplayLvgl、PanelSt7796、WifiOta
boards/                Brain 屏幕板 N16R8 配置
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

Motion 工具链固定 `espressif32@6.4.0`（Arduino-ESP32 2.0.11），使用标准 `esp32-s3-devkitc-1` 并覆盖 16 MB Flash / 8 MB OPI PSRAM。Brain 沿用屏幕平台 pioarduino `55.03.30-2`（Arduino 3.3 / IDF 5.5）、自定义 N16R8 板型及 LVGL 8.4.0；不互相升级或降级。
这不是 ESP-IDF 工程；目前无需为了 FreeRTOS 改换框架。

在仓库根目录执行：

```text
pio run -d device-controller
pio run -d main-controller
python tools/test_protocol.py
python tools/test_motion.py
python tools/test_raw_can.py
python tools/test_demo.py
python tools/test_cloud_contract.py
python tools/test_display_view.py
```

主机测试需要 `g++` 在 PATH 中，也可通过 `CXX` 指定兼容编译器。目录已改名，但 PlatformIO 环境仍为 `brain` / `motion`，OTA board ID 和 WSL 导出目录也保留这些兼容标识。

`test_cloud_contract.py` 的 `event_outbox` 套件把生产 Outbox、NVS 适配器、journal、产品会话和停机监督放在一起验证，仅替换平台 I/O。覆盖存储读写/提交/删除失败、离线持久化、回执到达但 Stop 未确认后再次复位、同一终态重发及 QoS 0 不清除记录；测试专用头文件位于 `tests/fakes/outbox/`，不会进入固件构建。它不验证真实 Flash 掉电行为、FreeRTOS 队列或 MQTT worker。

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

Motion GPIO 仍在 `device-controller/include/board_config.h`；Brain UART 在 `main-controller/src/display_board_profile.h`，LCD/触摸在 `shared/BabytechPanelSt7796`：

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

默认配套为屏幕 Brain + Motion UART v3。屏幕查看机械状态；断开 UART 后不得仍显示可启动，恢复后应以 Motion 最新状态为准。迁移v4是上文单独列出的验证路径，已接Cloud派发但完整产品链路未完成，不与默认镜像混用。

电机调试连接 Motion 的 `Babytech-Motion` 热点（开发密码 `babytech-demo`），访问 `http://192.168.4.1/`。先读取反馈，再在受控无负载条件下显式使能和试动；HTTP 202 只表示提交，不表示已执行或已停稳。旧 Brain 的 `Babytech-Debug` 网页入口已经退役，不再作为当前操作步骤。实际操作前仍须确认接线、安全条件和固件版本。

## 后续迁移

唯一开发顺序和验收出口在父项目 `Docs/refactoring/BRAIN_MOTION_CLOUD_APP_INTEGRATION_PLAN.md`。当前继续 Brain 网络/身份、双板动作与持久记录桥接；本仓库不再维护另一份相互冲突的优先级清单。Motion 独立调试网页保留，网络回调不直接操作电机。

## 来源与迁移边界

- 原项目：`hellowenshenghui/Babytech_Formula_Device`，参考本地 `V1-device` 的 `2ffcde2`。
- 网页交互参考：`SHKinsem/Project-Tenny`。
- CAN 传输库和 HX711 称重核心分别从旧工程 BabytechActuatorHal、BabytechSensorHal 按需迁入 `device-controller/lib/`；新工程不依赖旧仓库路径，旧仓库未修改。
- 当前默认显示协议为v3，迁移验证协议为v4（Cloud派发已接，触屏动作及结果桥接未完成）；两板按对应版本配套，不能混装。UART v2四指令协议只保留兼容回归，不是当前屏幕入口。

Wi-Fi OTA 的设计与操作见 [实施计划](docs/wifi-ota-plan.md) 和 [使用说明](docs/wifi-ota-implementation.md)。

历史兼容协议见 [v2 文档](docs/protocol-v2.md) 和 [v1 文档](docs/protocol.md)；新双板契约以父项目 `Docs/refactoring/BRAIN_MOTION_BOARD_CONTRACT.md` 为准。

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
