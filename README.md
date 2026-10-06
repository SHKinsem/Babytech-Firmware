# Babytech Firmware

## B1.0 屏幕迁入状态（2026-10-06）

两块板固件统一在本仓库管理：`main-controller` 已迁入父项目 DisplayController 的屏幕/触摸/UART v3 基线、显示库与 N16R8 板型，`device-controller` 继续作为 Motion。Brain 已通过本地及不含父项目的临时副本构建、LVGL host UI 测试，尚未烧录或验证真实触摸。原 DisplayController 的源码/构建入口已退役，仅保留迁移说明；旧 Brain 网页及其专属配置/页面测试也已移除，历史实现从 Git 获取。

默认 Brain 仍是 UART v3 屏幕角色；成对选择 v4 后已接入只读 Wi-Fi/MQTT 运行时和机械状态投影，尚无业务命令及双板记录桥接。Motion 默认路径仍直连 Cloud，v4 不启动产品 MQTT，独立调试网页不变。完整迁移按父项目 `Docs/refactoring/BRAIN_MOTION_CLOUD_APP_INTEGRATION_PLAN.md` B1–B3 推进，不把构建成功写成产品迁移完成。

App/屏幕文案唯一手写来源仍在父项目 `Shared/feeding_flow_ui/feeding_flow_ui.json`。父项目 `python3 Tools/generate_feeding_flow_ui.py` 自动更新本仓库生成 header，`--check` 检查内容及源哈希；生成物须随固件 commit 提交。独立 clone 编译锁定的 header，不读取父项目或下载文案，禁止手改 generated 文件。

## B1.1 共享协议基础（2026-10-06）

离线结果队列（2026-10-06，存储组件已实现、产品链路待接入）：Motion以BMS2单记录保存当前执行和最多4条未确认喂养结果，容量包含正在冲奶所预留的一条。`finishFeeding`冻结终态，`archiveFeeding`在确认静止后将其入队并释放执行槽，不等Cloud回执；队列满只阻止新prepare。历史结果确认删除不阻止另一个任务运行，仍须由上层验证Cloud stored回执。BMS1可只读加载，下次业务写入保留原记录内容并转换为BMS2，不在普通boot主动迁移。编码保守上界4073 bytes，未改Flash分区；真实20KiB NVS共存/替换峰值仍待实测。当前v4仍只读，不能据组件实现宣称屏幕已经能离线连续冲奶。

队列测试：`python3 tools/test_motion_result_queue.py --sanitize`；原记录/Store、commissioning及导出测试也必须通过。Motion Store主机实例现约17.2KiB，不得放入小任务栈；记录codec改用检查分配失败的堆暂存。USB导出缓冲上界6KiB，电脑端接受BMS1/BMS2和至多16KiB重组内容，仍只是诊断验证，不授权迁移或启动。

Brain v4 已接本地USB Wi-Fi/MQTT配置，见下方“Brain USB网络配置”。配置保存并读回后，由网络worker重连，不在UI线程操作无线接口。旧Wi-Fi两个键不是掉电事务：保存失败不改变当前实例的已验证连接配置，但重启后可能读到部分写入；重新配置即可，不擦电机/配对/业务NVS。MQTT使用原`cloudcfg/record`，写后重新打开核验才更新RAM，失败可重试；即使失败返回，Flash也可能已经改变，不声称回滚。测试：`python3 tools/test_brain_station.py --sanitize`及`python3 tools/test_brain_network.py --sanitize`。实际身份安装/MQTT账号退役交接仍未完成。

`shared/BoardProtocol/src/BoardProtocolV4.*` 已提供 v4 帧/CRC、固定容量分片重组和 Stop 编解码，`python3 tools/test_protocol.py` 同时运行 v1/v2/v4 回归。v4 的帧最大196 bytes、逻辑消息最大2047 bytes；Stop/心跳可在普通重组期间单独交付，未完成/错误输入不污染结果。接收完成只说明 bytes 齐全，仍须由后续 endpoint 核验 boot、身份、JSON、持久序号及执行门禁。

`BoardSessionV4` 核对配对身份及本次 HELLO 的关联应答，分别管理心跳与机械状态的新鲜度；`BoardTransmitV4` 提供固定容量队列和逐帧高优先级调度，支持短写/背压。`shared/ProductBoardLink` 提供严格的 HELLO/STATUS JSON 编解码及共用只读链路，假串口覆盖两板握手、单/双向断线、重启和旧包重放。只读层始终关闭 Start，不派发任何动作；传输 ACK 不是业务接受。

同一模块另有普通 COMMAND 编解码与 Cloud v4 命令解码，复用 `ProductRequest`，保留序号、宝宝/版本和 float32 配方身份。`tools/test_board_commands.py --sanitize` 检查消息边界和精确往返；Stop 使用独立二进制通道，不加入普通动作枚举。这些 codec 尚未接入执行协调器，不验证当前网络会话或授予运动权限，不能因为解码成功就启用 Start。

`ReadOnlyLink` 为单 owner、非重入对象，约 8 KiB，不能放在 MCU loop 局部栈中；共享完整消息 scratch，短回执用紧凑帧。35 字段 STATUS 的 GCC8.4/14.2 静态接收调用链（receive/handle/decodeStatus）约 4 KiB，尚未包含外层适配器和 JSON 库调用余量；接入设备后仍须检查实际任务栈高水位，不能让网络回调并发操作链路。

当前默认固件仍使用原 UART v3。v4 已接入两端 MCU UART1 只读入口：Brain 使用 `BABYTECH_BOARD_LINK_V4=1`，Motion 使用 `MOTION_UART_PEER=4`。两端必须同时选择；这是迁移验证路径，不是已完成的产品固件。它读取 `productpair/record` 并检查本机角色/MAC，缺失或损坏即不可用，不自动认领陌生对端。受控配对写入和旧配置导入尚未完成，不能靠烧录这两个镜像直接完成迁移。

只读 v4 不启动 Motion 产品 MQTT，也不开放屏幕 Initialize/Start；Motion 独立 Wi-Fi、HTTP 调试网页及本地 OTA 保留。旧 `formulaevt/payload` 仅用只读方式检查，有记录或读取异常时保留原数据、标记待处理并监督停机，不调用会改写旧 journal 的 Outbox 初始化。新执行/事件身份和持久序号尚未接入，不将只读状态中的空身份/零水位当作可执行授权。默认 v3 行为不变。

在本子仓库根目录编译验证（不含烧录）：

```bash
PLATFORMIO_BUILD_SRC_FLAGS=-DBABYTECH_BOARD_LINK_V4=1 pio run -d main-controller -e brain
PLATFORMIO_BUILD_SRC_FLAGS=-DMOTION_UART_PEER=4 pio run -d device-controller -e motion
# Restore ordinary build outputs before packaging or later flashing.
pio run -d main-controller -e brain
pio run -d device-controller -e motion
```

共享协议运行 `python3 tools/test_protocol.py --sanitize`，配对记录运行 `python3 tools/test_pairing_record.py --sanitize`。先构建 Motion 获取 ArduinoJson 后运行 `python3 tools/test_board_messages.py --sanitize`、`python3 tools/test_board_link.py --sanitize`、`python3 tools/test_board_arduino.py --sanitize`。最后一个链接生产 MCU 适配器，仅用替身替换 NVS/UART/MAC；它不代表真实 Flash、UART 线速或 Stop 时限验证。CI 编译两套默认与迁移入口；WSL 默认目标仍只导出默认镜像。

## B1.2 共享网络基础（2026-10-06）

`shared/BabytechCloudLink` 是唯一 MQTT transport 实现，Motion 通过本地依赖构建，不再保留本地副本；共享模块也已在 Brain SDK 编译验证。默认 Motion 继续使用原 `begin()`；面向 Brain 的 `beginV4()` 增加每次建连随机会话、24 小时轮换和 5 秒请求时效检查。网络线程拥有 socket，经有界队列交付消息；它不调用 LVGL、UART 动作或电机接口。probe 与终态回执不覆盖 retained 配置槽，旧入口不接受新 probe 类型。v4 出站必须使用生成 payload 时的连接代次，入队及实际发送均检查，旧状态/探测回复不跨会话重放。

`CloudSession` 是网络新鲜度门禁，不是持久去重或动作授权。Brain v4 在有效配对后调用 `beginV4()`；网络 worker 只读既有 `wifi-cfg` 和 `cloudcfg`，进行有界等待/退避的 Wi-Fi 重连及 MQTT 服务，UI loop 不调用网络 I/O。坏凭据不自动擦除，没有配对记录则不启动。受控凭据配置入口仍属 B1.3，不应手工猜写 Flash。

`brain_network` 每次 MQTT 新会话立即、其后每 2 秒尝试发布状态；只回复 config topic 中匹配本机和当前 session 的只读 probe，其余命令/配置不派发或写 NVS。UI 仅由原任务更新。Motion STATUS 增加产品阶段、故障、资源有效性与完整宝宝 ID；Brain 固定禁止启动，过期机械状态显示 unknown，保留已知故障。Cloud 配套处理 `motion_status_stale=true` 时不解除已有设备错误，只有新鲜恢复状态才解除。Cloud 探测/发行闭环与 SQLite14 尚未实施。

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

请求/上下文 SHA-256 使用 SDK mbedTLS，对固定字段编码计算，不依赖 JSON 字段顺序，也不是消息认证。Brain记录编码上界1491 bytes、当前最大合法本地记录1421 bytes，包含CRC和配对身份。该组件尚未接普通启动/触控/MQTT入口，Motion持久状态及双板受控迁移仍待接入，不可据此开放动作或自行写Flash。测试：`python3 tools/test_product_state.py --sanitize`、`python3 tools/test_brain_state_store.py --sanitize`；主机 SHA 使用 macOS CommonCrypto 或 Linux/WSL OpenSSL（`libssl-dev`），只替换SDK调用边界，不自写哈希实现。

Motion新增 `MotionStateRecord`/`MotionStateStore`，在单个 `productstate/record` 中保存配置屏障、cloud/local各自消费水位及最近结果、一个执行意图或待确认喂养终态。新拒绝同样消费序号；重复结果不授权再次运动，新配置与新拒绝不改旧执行快照。喂养终态须验证Cloud stored回执和新鲜静止证据才清除，水位保留。initialize/clean记录独立执行结果，不生成喂养事件；重启加载不是继续动作的许可。Cloud Stop先走立即安全停机，静止后再保存序号屏障；存储失败不能阻止Stop。

记录上界1728 bytes，目标粉量继续采用原0.1g舍入。普通加载只读，写入不确定会锁存失败；只提供受控安装原语，不自动创建记录或覆盖旧NVS。该组件尚未接产品运行/两板导入，不改变默认v3行为，也没有开放v4动作。测试：`python3 tools/test_motion_state_record.py --sanitize`、`python3 tools/test_motion_state_store.py --sanitize`。主机故障注入不等于实板Flash/NVS容量或MCU栈验收；虽然Store持有成员缓冲，codec仍有栈临时对象，运行集成必须测栈高水位。

`BoardCommissioning` 已把本机身份核对、Motion旧事件/配置预检、业务状态导入和最后的配对写入串在一起。中断后只允许同内容且未使用的初始记录续装；已有配对但业务记录丢失时拒绝重新从零开始。Guard必须在各持久阶段提供排他维护、人工授权、新鲜静止及MQTT交接证据，Brain还须验证对应Motion导出的完整配置。此处尚未实现真实Guard、USB安装操作入口或双板完成确认，不能通过随手传入true跳过前置条件。组件测试：`python3 tools/test_board_commissioning.py --sanitize`，不写真实Flash、不代表迁移已启用。

### v4 本地维护入口

两板USB支持`MAINT BEGIN`、`MAINT STATUS`、`MAINT END`，115200 baud、换行结束。响应`[maint] active/inactive/unsafe`；未配对Brain也可锁住本地UI，但这不证明Motion已静止。Motion进入/退出需有新鲜静止反馈，且没有产品待办、队列、驱动操作、Wi-Fi设置、OTA或HX711去皮。维护中网页运动/参数和网络设置拒绝，OTA不能开始；Stop、Stop all、queue cancel和只读查询仍可用。不自动停机，也不因长时间未操作自动退出。

Motion v4统一读取USB并保留`OTA CODE`，避免两处抢读；默认固件仍使用原OTA命令入口。命令限767个ASCII字符，每loop最多读64 bytes；超长、控制字符或2秒读取间隔的输入整行丢弃到换行，不执行后缀。已处理或拒绝的行缓冲清零，未知输入不回显。`OTA CODE`是既有本地管理员恢复功能，不要分享其输出。

### Brain USB网络配置

仅适用于开发中的Brain v4，不是默认v3或Motion网页。先核对USB端口并关闭其他串口监视器，使用可信电脑/USB线；Python需要`pyserial`，可使用已安装PlatformIO的Python。工具只写网络设置，不烧录、重启、安装配对或修改Mosquitto ACL。

```bash
python3 tools/configure_brain_network.py --port /dev/cu.usbmodemBRAIN wifi
python3 tools/configure_brain_network.py --port /dev/cu.usbmodemBRAIN mqtt
```

按提示输入SSID/密码或broker/端口/账号/密码；密码隐藏输入，不写命令行、文件或串口日志。工具先确认Brain，再进入本地维护、保存、退出；失败也尝试退出，若USB断开则重新连接后用`MAINT END`解除本地UI锁。成功仅表示存储读回通过，需另查连接状态。Wi-Fi未配对也能预存；MQTT需有效配对且网络worker已启动，当前完整安装入口尚缺，不能通过这个工具绕过身份安装。旧账号撤销与新Brain独占产品MQTT仍需单独受控交接。

底层命令为`NET STATUS`、`NET WIFI <ssid_hex> <password_hex或->`、`NET MQTT <host_hex> <port> <user_hex> <password_hex>`；除STATUS外要求本地维护。hex是编码不是加密，不要将完整命令贴聊天。Wi-Fi/MQTT配置由各自NVS格式保存，不是跨两项事务；一项成功另一项失败时仅重试失败项。凭据保留在设备NVS/RAM中，本工具不提供存储加密。USB打开仍可能因驱动/适配器复位板子；实机验证须在授权安全台架执行。此入口未改变Motion独立网页，也不开放产品动作。

维护中可发`MAINT EXPORT <device_id> <32位非零小写hex挑战值>`，导出实际MAC、启动识别号、配对及业务记录；Motion另读旧完整上下文/墓碑并检查旧事件是否存在。缺失、损坏、IO错误和身份冲突分别报告，错误记录不冒充空状态。导出只读，不含Wi-Fi/MQTT密码；可能含宝宝姓名/配方和任务信息，须私密保存，不提交Git或贴聊天。重组后的内容以`[maint-export] `开头，是一行schema=1 JSON，三类记录以hex承载；USB实际使用`[mx] offset:crc:hex`分片，每片至多16 bytes，采集工具校验连续偏移和CRC后重组，可忽略片间普通日志。它不是迁移资格或新机证明，更不是日常冲奶前置步骤。

包括短提示和OTA CODE在内的USB回复均排队，每loop最多64 bytes，按可写容量处理短写；5秒未完成或本地安全条件丢失则中止导出，不自动解除维护锁。Brain的Arduino 3.3 HWCDC设1ms超时，避免零超时重试计数下溢；Motion旧SDK保留0ms。不是严格实时USB保证，背压或帧内日志干扰仍可能导致本次采集失败。发送期间不处理下一条USB命令，UART、网络、Stop和反馈轮询继续。文件采集工具`tools/capture_board_export.py`使用随机挑战关联本次请求，排他保存0600文件，保留维护锁，不自动安装、退出或重启。关闭其他串口监视器；串口适配器打开端口仍可能复位板子，必须在已批准的安全台架上使用。需pyserial，可使用已安装PlatformIO的Python环境。

这仍不是完整迁移：无安装、无凭据写入、未调用BoardCommissioning。Brain网络仍发布只读状态/probe；`active`不能当作网络已停止或导入获准。测试`python3 tools/test_maintenance_console.py --sanitize`运行解析器/锁；`python3 tools/test_maintenance_export.py --sanitize`运行生产导出/USB调度但使用MAC/NVS/字节端口替身；`python3 -m unittest discover -s tests -p test_maintenance_wiring.py`只是入口连线静态回归，不代替串口/网页/机械实测。

## V1 Motion 集成状态（2026-10-01）

`device-controller` 的 DISPLAY 构建正在接入现有 Babytech Cloud/App：Motion 自行连接 MQTT，使用设备专属 topic、产品状态、命令 ACK 和终态事件；屏幕经 UART v3 提交本地操作。旧 Brain/v2 协议库保留兼容回归，已不是 `main-controller` 的运行入口。

产品链路保存运行 journal 后才允许动作，终态在 Cloud 落库回执后才清除；MCU 重启补报失败并请求受监督停机，不自动续跑。有效缓存允许屏幕离线本地启动，资源/机械/存储门禁不变；本地流程不依赖网络，远程流程仍在 Wi-Fi 丢失时中止。单槽未确认前不能再次产品启动，须先部署支持回执的 Cloud。称重无效上报 `unknown`，不是 `low`。主机测试不代替真实断电、NVS 和停止反馈验收。

这一集成**尚未完成验收**。目前通过的是 Motion 构建、主机协议/动作测试、实际 JSON 编码器及 Stop 长载荷优先级测试；父项目 `Test/motion_contract_check.py` 也用生产编码器输出验证了 Cloud 的状态、审计、SQLite 和 WebSocket 处理。尚未用新 Motion 板完成 MQTT 往返、屏幕联调和安全台架验证。默认构建不授权非食用固定转数产品流程，低水位输入未配置时也保持 `noready`，不可用于真实喂养。父项目的 `Docs/refactoring/BRAIN_MOTION_CLOUD_APP_INTEGRATION_PLAN.md` 保留旧 M0–M3 证据，并记录 2026-10-06 新 B0–B4 迁移计划：现有屏幕板兼任 Brain，Motion 保留独立调试网页。该迁移尚未实施，当前代码仍是 Motion 直连 MQTT。

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

默认配套为屏幕 Brain + Motion UART v3。屏幕查看机械状态；断开 UART 后不得仍显示可启动，恢复后应以 Motion 最新状态为准。迁移 v4 是上文单独列出的只读路径，不与默认镜像混用。

电机调试连接 Motion 的 `Babytech-Motion` 热点（开发密码 `babytech-demo`），访问 `http://192.168.4.1/`。先读取反馈，再在受控无负载条件下显式使能和试动；HTTP 202 只表示提交，不表示已执行或已停稳。旧 Brain 的 `Babytech-Debug` 网页入口已经退役，不再作为当前操作步骤。实际操作前仍须确认接线、安全条件和固件版本。

## 后续迁移

唯一开发顺序和验收出口在父项目 `Docs/refactoring/BRAIN_MOTION_CLOUD_APP_INTEGRATION_PLAN.md`。当前继续 Brain 网络/身份、双板动作与持久记录桥接；本仓库不再维护另一份相互冲突的优先级清单。Motion 独立调试网页保留，网络回调不直接操作电机。

## 来源与迁移边界

- 原项目：`hellowenshenghui/Babytech_Formula_Device`，参考本地 `V1-device` 的 `2ffcde2`。
- 网页交互参考：`SHKinsem/Project-Tenny`。
- CAN 传输库和 HX711 称重核心分别从旧工程 BabytechActuatorHal、BabytechSensorHal 按需迁入 `device-controller/lib/`；新工程不依赖旧仓库路径，旧仓库未修改。
- 当前默认显示协议为 v3，迁移只读协议为 v4；两板按对应版本配套，不能混装。UART v2 四指令协议只保留兼容回归，不是当前屏幕入口。

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
