# Brain / Motion UART v2：四指令设计

状态：四指令最小闭环已实现，brain 和 motion 均使用 v2。当前支持系统信息/状态、单电机阶段 RAM 参数、阶段运行、电机使能/失能、优先停止、结果补查。机构动作、NVS 保存、清故障专用操作和传感器对象尚未实现。旧 [v1](protocol.md) 编解码仅保留作历史及回归测试，不用于板间运行。尚未进行硬件烧录和实机验收。

## 职责与指令

brain 负责交互和网络；motion 独占机构状态机、电机、传感器及执行保护。上板发送动作意图，下板判断能否执行、如何执行和是否完成。UART 与下板网页共用执行入口和资源互斥规则。

| CMD | 名称 | 请求用途 |
| --- | --- | --- |
| `0x01` | READ | 查询状态、参数、信息或保留的执行结果 |
| `0x02` | WRITE | 修改 RAM 配置；不触发运动，不自动保存 |
| `0x03` | EXEC | 执行动作、阶段、调试操作、保存参数或清故障（后二者为后续扩展） |
| `0x04` | STOP | 无参数，优先停止所有执行 |

请求和回复使用相同 CMD，不增加 ACK、RESULT、故障上报等指令码。禁止通过 WRITE 写入运行标志等方式隐式触发动作。

## 帧格式

继续使用 115200、8N1、小端整数和 BM magic。版本升至 2，不复用 v1 Type 的含义。

| 偏移 | 字段 | 字节数 |
| --- | --- | --- |
| 0 | Magic：`0x42 0x4D` | 2 |
| 2 | Version：2 | 1 |
| 3 | CMD | 1 |
| 4 | KIND：0 REQUEST、1 RESPONSE、2 EVENT | 1 |
| 5 | Payload length，最大 128 | 2 |
| 7 | Session ID | 8 |
| 15 | Sequence | 4 |
| 19 | Payload | 0–128 |
| 尾部 | CRC16 | 2 |

CRC 参数沿用 v1，覆盖头部和 payload：多项式 0x1021、初值 0xFFFF、不反射、不最终异或，小端存储。最大帧 149 bytes。逐字段编解码，不直接发送 C/C++ struct 内存。

保留 100 ms 字节间超时和流式恢复；日志仅走 USB Serial。版本、长度、CRC 或消息类别非法的帧丢弃；有效 v2 帧中的未知 CMD 返回 REJECTED/UNSUPPORTED。响应不得再次触发响应。

上板每次启动生成新的非零 64 位随机 Session ID；Sequence 从 1 单调递增，回绕前更换 Session ID。响应和操作事件回显原请求的 Session ID、Sequence。它们用于请求关联，不是认证凭据。

## 统一结果

所有 RESPONSE 和 EVENT 的 payload 前缀相同：

| 字段 | 类型 | 含义 |
| --- | --- | --- |
| outcome | uint8 | 0 OK、1 ACCEPTED、2 DONE、3 REJECTED、4 FAILED、5 CANCELLED |
| reason | uint16 | 具体原因；成功时为 0 |
| detail | 变长 | 按 CMD 和对象表定义 |

原因码：0 NONE、1 BUSY、2 INVALID_PARAM、3 INVALID_STATE、4 NOT_READY、5 FAULT_ACTIVE、6 UNSUPPORTED、7 CONFIG_MISMATCH、8 TIMEOUT、9 FEEDBACK_STALE、10 REQUEST_CONFLICT、11 RESULT_EXPIRED、12 BOOT_MISMATCH、13 STOP_UNCONFIRMED、14 INTERNAL_ERROR。

READ/WRITE 成功返回 OK，拒绝返回 REJECTED。EXEC/STOP 接受后返回 ACCEPTED，再通过 EVENT 返回 DONE、FAILED 或 CANCELLED。ACCEPTED 不表示电机已经启动；DONE 必须满足操作定义的完成条件。停止未确认应返回 FAILED/STOP_UNCONFIRMED，不能声称已物理停止。

## 对象与字段

对象地址为 `class:uint8 + instance:uint16`；字段 ID 和操作 ID 都为 uint16，在所属对象类别内解释。

| class | 对象 | instance | 可读内容 | 可写内容 | EXEC 操作 |
| --- | --- | --- | --- | --- | --- |
| 0x01 | 系统 | 0 | 信息、整机状态、保留结果 | 允许配置的系统参数 | 保存参数、清故障 |
| 0x02 | 机构 | 0 | 机构状态和动作参数 | 已定义机构参数 | 开盖、关盖、回零 |
| 0x03 | 动作 | 动作 ID | 参数、执行状态 | 已定义动作参数 | 运行 |
| 0x04 | 阶段 | 阶段 ID | 参数、执行状态 | 已定义阶段参数 | 运行 |
| 0x05 | 电机 | CAN ID | 反馈、有效性、采样年龄 | 允许配置的调试参数 | 使能、失能、相对运动 |
| 0x06 | 传感器 | 传感器 ID | 测量值、有效性、采样年龄 | 允许配置的校准参数 | 已支持的校准操作 |

这些是命名空间，不是实现清单；未知或未接入对象返回 UNSUPPORTED/NOT_READY。系统信息必须提供实际支持能力，不能因为分配了对象 ID 就宣称可用。

每个字段须定义类型、单位、范围、默认值、读写权限和允许修改状态；每个操作须定义参数、前置条件、完成判据、超时和失联行为。优先使用定点整数，如角度 0.01°、转速 0.1 rpm、时间 ms、电流 mA；具体换算和有符号性由字段表固定。

## 请求内容

所有请求 payload 先携带 `target_boot_id:uint64`。READ 系统信息时可为 0，用于发现下板当前启动标识；STOP 也允许为 0。其他请求必须匹配下板本次启动的非零随机 boot_id，否则拒绝 BOOT_MISMATCH。

- **READ**：随后为 `count:uint8` 和 count 个 `(class:uint8, instance:uint16, field:uint16)`。回复 detail 为 count 及对应的 `(class, instance, field, value_length:uint16, value)`，顺序与请求相同。整个批次成功或拒绝；不拆包、不截断。预计回复超过 128 bytes 时拒绝 INVALID_PARAM，上板拆分查询。已知对象的暂不可用测量值通过有效标志表达。
- **WRITE**：随后为 `expected_revision:uint32, count:uint8` 和 count 个 `(class, instance, field, value_length:uint16, value)`。全量验证后原子更新 RAM，返回 `new_revision:uint32`。任一字段不合法、重复字段、版本不匹配或忙碌则全部拒绝。首版仅 IDLE 允许配置更新。
- **EXEC**：随后为 `class:uint8, instance:uint16, operation:uint16, expected_revision:uint32` 和操作参数。参数长度由剩余 payload 与操作定义共同校验。首版统一核对配置修订号，启动时冻结参数快照。接受后使用请求的 Session ID/Sequence 作为执行标识。
- **STOP**：除 target_boot_id 外没有参数。无论忙碌、配置版本或故障锁定状态均可请求，绕过普通业务队列。有效停止请求优先处理；这不等同于独立硬件急停。

每个批次 count 至少为 1，必须精确消耗 payload，禁止尾随未解释字节。变长值的类型由字段表确定，接收端不能信任发送端自报类型。首版只实现阶段对象的参数写入。

## 状态与事件

整机状态为 NOT_CONFIGURED、IDLE、RUNNING、STOPPING、FAULT；当前动作和阶段使用独立 ID，不扩大状态枚举。

系统状态提供 boot_id、运行时间、配置修订号、当前执行标识、阶段 ID、故障码及最近结果。上板另维护链路新鲜度。详细电机/传感器反馈独立查询，并携带有效性与采样年龄。没有反馈不等于数值为 0，上板不得从超时推断动作完成或电机停止。

首版 READ 轮询即可，不增加订阅机制。异步操作最终结果使用原 CMD 的 EVENT。首版故障通过轮询状态获取；后续自发故障如需主动上报，使用 READ EVENT 携带系统状态字段，Session ID 和 Sequence 均为 0；事件前缀为 OK/NONE，故障内容在状态字段内表达，不能误当成某个动作成功。故障必须锁存于可查询状态，不能只发一次事件。

## 执行、重复请求与恢复

- 首版只允许一个普通 EXEC 在途，不排运动队列；已经使能且空闲允许写 RAM 配置和运行阶段；正式动作与调试动作互斥，任何入口均遵守同一规则。
- STOP 打断动作时，原 EXEC 最终报告 CANCELLED 或 FAILED；STOP 自己单独报告停止结果。没有活动动作也不能直接推断电机静止，DONE 仍须符合停止判据。
- 下板在副作用发生前登记请求，以 `(boot_id, session_id, sequence)` 去重。同标识且内容相同返回已有接受状态或终态，不重复执行；内容不同拒绝 REQUEST_CONFLICT。WRITE 和保存参数也必须去重。
- 对每个已接纳会话记录最高请求序号；已过期序号不能当新请求执行，返回 RESULT_EXPIRED。采用首版单请求发送顺序，避免乱序普通请求；STOP 可越过等待中的普通请求，后到的旧普通请求据此拒绝。
- 下板保留当前执行及最近 8 个修改类请求结果（WRITE、EXEC、STOP，包含拒绝和终态），包含会话与流水号；READ 可读取这些固定结果槽位。结果被淘汰时不能编造结果或重新执行。去重回复缓存与终态结果槽位分开管理。
- 首版最多记录 4 个会话。槽位耗尽时，新会话的普通请求返回 BUSY，不通过静默淘汰会话而重新接纳旧运动请求；STOP 始终可进入停止处理路径且必须幂等。后续如需无重启释放会话，再明确会话生命周期设计。
- 首版 STOP 在另一次 STOP 处理中收到不同执行标识时返回 BUSY，已有停止流程继续；重发同一 STOP 返回其已有状态。过期 STOP 在去重记录已淘汰后允许幂等地再次请求停止。
- 上板不自动重试 EXEC/WRITE；响应丢失后查询当前执行或保留结果。STOP 可以重发；READ 可以重试。
- 下板重启更换 boot_id，不恢复或重放旧动作。上板发现 boot_id 变化后废弃旧实时缓存，旧操作结果标为未知，重新读取信息和状态。
- 失联处理在下板执行。首版每 500 ms 发送一次查询（状态与参数/反馈/结果交替，状态通常每秒更新），1500 ms 无有效执行所属会话请求时，下板广播停止并将原执行报告为 FAILED/TIMEOUT。STOP 独立等待反馈，3 秒未确认则 FAILED/STOP_UNCONFIRMED。超时意味着执行失败或状态未知，不意味着物理停止。上述时间及策略仍需实机验证。查询响应丢失仍可能使上板离线而下板继续收到请求，上板在线状态不能替代下板保护。没有定义失联策略的动作不得开放执行。

## 实现顺序与验收

1. v2 编解码、READ 系统信息/状态，确认双板升级与版本不匹配提示。
2. WRITE 参数、EXEC 单阶段、STOP，打通调参和执行反馈；沿用现有执行核心。
3. READ 补查结果、重复请求去重、重启与失联处理。
4. 经机构实测后开放完整动作、参数保存和更多对象。

上下板必须一起更新到 v2，旧板不会响应 v2 查询；上板将显示离线，目前不单独识别旧板版本。v1 源文件和测试保留，不自动在不同版本间执行运动命令。

验收至少覆盖分段/粘包/坏 CRC/截断恢复、批量写原子性、超长回复拒绝、丢失接受回复及终态事件、重复和冲突请求、旧会话/旧启动请求、结果过期、忙碌时停止、停止反馈缺失、双入口互斥、执行中断线和下板重启。主机测试验证协议与状态逻辑，物理完成和停止判据必须另做硬件验收。


## 首版已实现字段与操作

以下是当前代码的精确布局；所有多字节整数为 little-endian。READ 回复仍带公共结果前缀、count 和每个字段的地址/长度。保留结果仅覆盖本次下板启动，不跨重启持久化。

| 对象 / 实例 / 字段 | 值布局 | 长度 |
| --- | --- | --- |
| SYSTEM / 0 / 1 INFO | boot_id:u64, protocol_version:u8=2, capabilities:u32=0x0F | 13 |
| SYSTEM / 0 / 2 STATUS | 见下表 | 50 |
| SYSTEM / 0 / 0x100..0x107 | session:u64, sequence:u32, cmd:u8, outcome:u8, reason:u16 | 16 |
| STAGE / 1 / 1..6 | 单个参数：4 字节，类型/单位见参数表 | 4 |
| MOTOR / 1..255 / 1 | flags:u8, position:i32, velocity:i32, current:u16, position_age:u32, velocity_age:u32, current_age:u32 | 23 |

结果槽位 0x100 最新，依次向旧排列。空槽返回 RESULT_EXPIRED。capabilities bit 0=系统信息/状态，bit 1=阶段 RAM 参数，bit 2=单电机阶段及使能/失能，bit 3=停止；不宣称机构或传感器已接入。

MOTOR 位置单位 0.1°，速度单位 0.1 rpm，电流 mA，年龄 ms。flags bit 0/1/2 分别表示位置/速度/电流在 600 ms 内有效，bit 3=使能已确认，bit 4=使能待确认，bit 5=停止待确认，bit 6=该电机故障。无效数值必须忽略，未知年龄为 UINT32_MAX。上板继续计算缓存年龄，不能把旧 flags 当实时反馈。

| STATUS 值偏移 | 字段 |
| --- | --- |
| 0 | boot_id:u64 |
| 8 | uptime_ms:u32 |
| 12 | config_revision:u32 |
| 16 | state:u8（0 NOT_CONFIGURED、1 IDLE、2 RUNNING、3 STOPPING、4 FAULT） |
| 17 | fault:u16（当前将控制器故障映射为 FAULT_ACTIVE） |
| 19 | motors_available:u8 |
| 20 | active_session:u64 |
| 28 | active_sequence:u32 |
| 32 | last_session:u64 |
| 40 | last_sequence:u32 |
| 44 | last_outcome:u8 |
| 45 | last_reason:u16 |
| 47 | last_valid:u8 |
| 48 | stage_id:u16（0 无 / 1 单电机相对运动） |

IDLE/RUNNING 等为调试执行器状态，不代表整机机构已完成配置；传感器、开盖和混合状态不得据此推导。

| STAGE 1 字段 | 类型 | 单位 | 默认值 | 范围 |
| --- | --- | --- | --- | --- |
| 1 motor | u32 | CAN ID | 1 | 1..255 |
| 2 angle | i32 | 0.1°，相对行程 | 100 | -36000..36000，非零 |
| 3 speed | u32 | 0.1 rpm | 50 | 1..1200 |
| 4 accel | u32 | rpm/s | 10 | 1..240 |
| 5 decel | u32 | rpm/s | 10 | 1..240 |
| 6 current | u32 | mA | 800 | 100..5000 |

整个参数组合还必须满足预计运动时间不超过 60 秒。角度目前采用电机驱动的 0.1° 精度，不是示例中的 0.01°。RAM 参数初始修订号 1，每个成功 WRITE 加 1；达到 UINT32_MAX 后拒绝继续写入，不静默回绕。

已实现 EXEC 均无附加操作参数：

- STAGE / 1 / operation 1：使用参数快照做一次相对运动。必须显式使能、位置和速度反馈新鲜且近似静止。完成需要驱动接受确认，以及两组不同时间、执行后的近目标位置和近零速度反馈。
- MOTOR / ID / operation 1：使能。只有收到对应驱动 ACK 才完成；1.5 秒缺 ACK 报 TIMEOUT。
- MOTOR / ID / operation 2：失能。需要驱动 ACK 和执行后静止反馈；缺 ACK 或 3 秒停止未确认均不报告成功。
- STOP：广播停止，下板清除运动跟踪及软件使能确认。需要所有被跟踪停止对象的执行后新鲜位置/速度反馈，并且速度绝对值不超过 0.2 rpm。没有已知目标、CAN 不可用或缺少反馈均不能报告 DONE。DONE 的范围是控制器已知对象，不是对未发现 CAN 节点的证明。

使能仍复用现有控制器的故障恢复规则：故障对象有新鲜静止反馈后，显式使能可尝试恢复；本轮没有另增 CLEAR_FAULT 操作。UART 执行期间，下板网页拒绝使能、运动和原始普通命令；停止入口保留。下板网页停止若打断 UART 运动，原 UART 执行报告 CANCELLED。

## 上板网页接口与操作

- GET /api/status：缓存状态、boot（16 位十六进制字符串）、revision、parameterRevision、params、当前指令状态；不阻塞等 UART。
- POST /api/query：请求一次查询。
- POST /api/params：表单字段 boot、revision、motor、angleTenths、speedTenths、accel、decel、current；批量应用六个参数，不保存 NVS。
- POST /api/exec：boot、revision、action=enable/disable/run；enable/disable 另带 motor。
- POST /api/stop：无表单参数；即使上板缓存已离线也可发 STOP。

POST 返回 HTTP 202 只表示已提交 UART 路径，界面必须继续等下板 RESPONSE/EVENT 或结果补查。POST 没有自动重试，只有 STOP 可以显式重发。浏览器表单版本与下板不同会被拒绝，需重新读表单；未应用的编辑不会作为运行参数。

两板更新后，连接 Babytech-Debug，打开 http://192.168.4.1/。等待系统状态与参数读取，调整参数并应用，等待反馈，再点使能、运行阶段；停止全部始终可用。操作不触发上电自动使能。

当前限制：四个会话槽位不自动淘汰，上板累计四次不同启动会话后继续重启，普通查询可能返回 BUSY，需重启下板；STOP 不受该限制。流水号 UINT32_MAX 时客户端拒绝发新请求，需重启上板更换会话。UART 传感器对象、机构动作、参数持久化仍待后续实现；不表示其他接口没有独立的传感器开发。

## 本轮验证

- tools/test_protocol.py：v1 回归和 v2 编解码、批量写原子性、重复/冲突/过期请求、丢回复结果补查、停止、失联、重启和客户端恢复。
- tools/test_motion.py：现有运动核心/控制器测试，以及 UART Endpoint → BoardMotion → 真实 MotorControl → 模拟 CAN 联调，包含完成判据、网页停止打断、失联、缺 ACK 和故障。
- tools/build-wsl.ps1 -Target all：上下板固件编译；产物在 out/wsl/brain 和 out/wsl/motion。
- node tests/brain-web.cjs：实际嵌入页面的模拟 API 测试，验证无自动动作、参数版本、未应用编辑、异步结果、停止、离线及重启。
- 无硬件烧录、无实际 UART/CAN/机构验收；模拟测试不能代替物理停止与机构参数验收。
