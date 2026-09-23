# Motion / Display 流程演示开发计划

描述对象：工具 Motion 主控、完整产品 DisplayController 屏幕板、非阻塞流程状态机和网页调试工具的联动方案。

Milestone：V1 流程演示。更新日期：2026-09-23。

状态：开发约定，尚未实现。本文不把规划描述为现有功能，不代表真实出料、温控或机械安全已验收。

## 1. 范围与已有基础

目标是用工具已验证的电机能力尽快完成流程演示，不重构完整产品 Product，不转换 actuatorcfg，不统一两个仓库的底层驱动。

```text
Web workbench ---- stage scripts / initialize / run / existing Stop
       |
Motion ESP32 ---- non-blocking flow ---- existing command queue ---- CAN motors
       |
       +-------- display UART v3 -------- DisplayController ESP32
                                           stages / timeout / Start
```

- Motion 保留现有热点、网页、电机指令解析和执行能力；显示板继续烧录完整项目的 display 构建，不需要 Brain 板。
- 工具当前已有 [Brain/Motion UART v2](protocol-v2.md)、[电机队列](motor-queue.md)、[调试网页](motion-debug.md)。v2 不是 DisplayController 协议，不能直接混用，也不能因 v2 已实现就认为显示链路已接通。
- 不以完整产品中发现的 CD/FD 差异为前置条件，不主动改变工具电机底层。工具实际执行有问题时再定位。
- 不接入云端、App、MQTT、喂养记录或新屏幕 UI。不新增通用配方平台。
- 本文取代完整项目旧演示计划中的“LittleFS 持久保存”方案：调试用 RAM，演示用编译内置 JSON。

## 2. 编译与板间串口

保持单个 motion environment，通过宏选择 UART 对端：

```cpp
#define MOTION_UART_PEER_BRAIN 1
#define MOTION_UART_PEER_DISPLAY 2
#ifndef MOTION_UART_PEER
#define MOTION_UART_PEER MOTION_UART_PEER_BRAIN
#endif
```

- 默认 BRAIN 保留现有行为；演示编译增加 `-DMOTION_UART_PEER=MOTION_UART_PEER_DISPLAY`，非法值编译报错。
- 两个分支仅启用各自 UART 协议入口；DISPLAY 分支接入演示流程，不让原 Brain 解析器同时消费同一串口。
- 沿用主控 GPIO43 TX / GPIO44 RX，115200、8N1、3.3 V TTL。TX/RX 交叉，共地；各自 USB 供电时不互接 5 V。确认实际屏幕板引脚，不仅依赖排针丝印。
- 调试日志走 USB Serial，不混入板间二进制 UART。切换对端需重新编译烧录 Motion，不增加另一套 environment。

### 显示协议契约

权威源是完整项目 `Embeded_System/libraries/BabytechDisplayCore/src/display_protocol.*`、`display_model.h` 和 `Embeded_System/DisplayController/src/controller_link.*`。实施时固定其来源提交，以最小协议源文件复用并建立 golden frame 测试，不手工另造字段布局。

| 内容 | 约定 |
| --- | --- |
| 版本 | protocol version 3，snapshot schema 3 |
| 帧 | 原二进制编码和 CRC16；payload 上限 96 bytes，整帧上限 108 bytes |
| State | 阶段变化立即发送；无变化每 1 秒发送完整快照 |
| Intent | 现有 StartFeeding，屏幕和网页启动共用入口与门禁 |
| Ack | 回显请求 sequence，accepted 与最多 31 字符 reason；接受不代表完成 |
| 重试 | 屏幕每 500 ms 重试，最多额外两次；Motion 缓存近期请求与 ACK，重复请求不再次启动 |
| 离线 | 屏幕 2.5 秒无有效快照显示离线；主控不因屏幕断线自动中止动作 |
| 无效帧 | 错版本、CRC、长度及未知 intent 不触发运动 |

只传业务阶段、完成、阶段超时和必要停止/错误终态，不传单条指令、位置或详细电机反馈。详细日志留网页/USB。明确电机故障仍立即停止，不等超时才处理；屏幕使用通用错误。五个冲奶阶段超时映射既有对应错误，首次找零超时使用通用错误，不新增协议枚举。Stop 后不能还显示正在冲奶。

现有屏幕没有 Stop、初始化或复位 intent，这些操作留在网页。没有独立初始化阶段和百分比进度字段；首次初始化使用 NotReady 且 startEnabled=false。

### Dummy 值与真实状态

- babyName、formulaBrand、waterMl、temperatureC 可以是演示配置；水量/温度不是实测值。
- 未接温控时 thermalSimulated=true，保留 sim 提示；cloudConnected=false，接受原屏幕 Cloud offline，不伪造联网。
- stage、startEnabled 和 error 必须对应实际执行情况。`startEnabled` 不维护第二套隐藏门禁，固定等于 `stage == Ready`；Motion 收到 Start intent 时重新确认当前仍为 Ready。条件提示只传有依据的值，不伪造传感器读数。
- 加水/加粉初版建议非阻塞等待模拟，具体是否模拟及等待时间待确认。屏幕无专用模拟出料标志，网页须标明演示边界，演示产物不用于喂养。

### V1 最小状态、条件与错误映射

- `DemoFlowController.stage` 是对外生命周期的唯一状态源；`startEnabled` 只由 `stage == Ready` 派生，`error` 只由 Error 终态及其锁存原因派生。不得再维护平行的 UI 状态、可启动标志或页面专用阶段。
- DISPLAY 构建直接用 DemoFlowController 和演示 JSON 生成 `DisplaySnapshot`，复用既有协议编码/解码；不原样调用完整产品中依赖云连接门禁的 snapshot 组装逻辑，避免 `cloudConnected=false` 把演示 Start 永久禁用。
- V1 没有真实传感器证据时，`primaryCondition=None`、`footerCondition=None`。控制器离线和协议不匹配继续由 DisplayController 本地判断，不伪造 LowWater、OverTemperature、BottleRemoved 或传感器异常。
- 非 Error 阶段一律发送 `error=None`。Error 锁存到网页明确执行“复位/初始化”，不因心跳、屏幕重连或超时展示结束自行清除。复位时参考仍可信且轴在零位/静止/反馈新鲜，只重新检查后进入 Ready；参考已失效才重新碰撞找零。

| 原因 | 对屏幕发送的 error |
| --- | --- |
| 开盖阶段超时 | `CapUnscrewTimeout` |
| 加水阶段超时 | `WaterDispenseTimeout` |
| 加粉阶段超时 | `PowderDispenseTimeout` |
| 关盖阶段超时 | `CapScrewTimeout` |
| 混合阶段超时 | `MixingTimeout` |
| 已确认的 CAN/电机故障，或 Stop 因 CAN/反馈故障无法确认 | `CanFault` |
| 初始化超时、无法分类的执行失败、非 CAN 原因的 Stop 未确认 | `Unknown` |
| 用户 Stop 已确认且位置仍可信 | `None`，流程转 `NotReady` |

演示版只主动产生 `None`、上述五个阶段超时、`CanFault` 和 `Unknown`。不产生缺少真实依据的 `LowWater`、`OverTemperature`、`BottleRemoved`、`WaterSensorInvalid`、`PowderSensorInvalid`、`TemperatureSensorInvalid`、`NetworkLost`，也不额外区分 `PowderMotorFault`；后续接入真实传感器时再扩展。

## 3. 初始化与流程状态机

```text
Boot -> Load embedded JSON -> Reference invalid -> NotReady
Web Initialize (once after coordinate reset)
  -> Collision homing -> Establish software zero/start position -> Ready

Screen/Web Start (Ready only)
  -> Open cap -> Water -> Powder -> Close cap
  -> Mix: lift axes return to software zero, then mix
  -> Verify script done + required axes at zero/stationary/fresh
  -> Complete -> hold 3 seconds without motion -> Ready

Stop / fault / timeout -> Cancel remaining stages -> Stop handling -> Latched state
```

- 初始化找零是独立按钮，通过堵转/碰撞建立软件零点；只在 Motion 上电后或坐标参考明确失效时执行一次。上电不自动动作，不恢复中断流程。
- 后续每轮不重新碰撞找零，也不增加独立的起始归位阶段。只有首次初始化成功并处于 Ready 才接受 Start。
- 初始化结束以及混合结束时，在配置容差内且静止才算处于软件零位。缺失/过期反馈不得当作在零位；混合阶段本身负责让升降轴回零后再混合。
- 当前工具 `move` 为相对实际位置，不可用 `move ... 0` 冒充绝对归零。实施时核对现有绝对定位/位置反馈能力，明确工具原点和软件零点关系；没有现成入口时只补薄适配。不在未验证情况下硬套 home 模式代替软件归位。
- 混合脚本完成、所需轴仍在软件零位、反馈新鲜且静止后才能进入 Complete。没有独立“回到起始位置”动作；Complete 到 Ready 的 3 秒仅用于屏幕展示，不发送电机指令。
- Motion 或驱动器重启、坐标换算/轴配置变化、改变原点的手动或 raw 操作、检测到位置跳变或其他坐标不可信情况会清除参考。正常完成、屏幕重启、网络断开以及位置仍可信的已确认 Stop 不清除参考。
- Stop、故障或阶段超时先退出运行状态并锁存结果。复位后参考仍有效、所需轴在零位且反馈新鲜静止时可恢复 Ready；参考不可信才要求重新初始化找零，不自动续跑中断流程。
- Complete 固定展示 3 秒；期间故障/Stop 仍优先，结束时再次确认参考有效、所需轴在零位且反馈新鲜静止，满足才回到 Ready，全程不发送电机指令。无可靠瓶位检测时，Ready 只表示控制器可以接受下一次 Start，不表示已经检测到换瓶；操作者须在下一次启动前自行换瓶。
- 原开盖脚本含 home/disable，拆分初始化时人工确认哪些动作迁移，不自动删改；承重轴失能风险须实机确认。

### Ready 与 Start

- 对外只使用 NotReady、Ready、五个业务阶段、Complete 和 Error；Idle、Cleaning、Offline、Unknown 不作为正常 Motion 流程阶段发送。屏幕链路离线仍由 DisplayController 自行判断。
- `stage == Ready` 是唯一启动资格，快照中的 `startEnabled` 直接由该条件生成；其他所有阶段一律为 false。屏幕仅用 startEnabled 控制 Start 按钮，不从阶段名称自行推导额外规则。
- 配置、首次找零、零位/静止确认、故障和所有权检查都在 Motion 进入 Ready 之前完成，不再维护与 Ready 并行的第二套启动条件。
- Ready 期间只维护同一状态：参考、零位、静止或反馈条件失效时立即转回 NotReady，因此 startEnabled 同步变为 false。
- 屏幕快照可能已经过期，因此收到 Start intent 时仍须原子地重读当前阶段：仍为 Ready 则接受并立即离开 Ready，否则只返回 `not_ready`，不启动动作。

### 不可简化的执行底线

- 上电、Load JSON 和 Apply JSON 都只更新状态/配置，绝不触发找零或任何电机运动。
- 任一阶段故障或超时立即取消剩余阶段并请求停止；停止未确认时保持 Error，不用自动进入下一阶段或 Ready 掩盖问题。
- 复用的 Stop 始终可用，优先级高于阶段完成、切换和 Complete 计时；物理急停仍是独立安全手段。
- 自动流程独占队列；运行中不允许手动插入动作、替换 JSON、改变坐标换算或原点。这些是执行安全约束，不增加屏幕交互复杂度。

### 非阻塞要求

首次初始化、单阶段、完整流程、Complete 展示计时及阶段切换都采用 tick 状态机：

```text
loop: Web/Stop -> UART -> CAN feedback / queue poll -> flow tick -> State heartbeat
```

- 进入阶段提交一次脚本并记录开始时间；后续 tick 读取执行状态，不用循环等待返回。
- await 仅暂停队列推进，wait/阶段超时用时间戳，不用 delay 或忙等；检查现有内部路径同样非阻塞。
- 阶段完成后记录下一阶段，后续 tick 再推进；Stop/故障优先于完成和切换。
- 每次 tick 有界，不递归跑完整套流程；网页、CAN、UART 和停止始终能响应。
- 调试队列和流程单一所有权；自动流中禁止插入手动动作、改变换算或应用新 JSON，查询与现有 Stop 保留。
- 队列结束不等于电机已停。需等待的 move/home 使用 await；末尾非等待运动和无限持续输出不能让阶段提前完成。
- 现有队列 await 缺反馈会继续等待，本版增加阶段超时监督，超时取消余下队列并请求停止。停止未确认必须留故障，不宣称安全停稳。
- Stop 不等于 disable；复用原停止能力，新增取消流程和清除待执行阶段。保留物理急停，网页不是硬件安全回路。

## 4. 工具页面与 JSON

### 最小交互

- 屏幕沿用现有阶段/详情显示和唯一的 Start，不增加初始化、Stop、复位、JSON 或调试按钮；初始化和异常恢复留在网页。Cloud offline 提示可以保留，不影响由 Ready 派生的 Start。
- 网页只新增 Load / Apply / Export JSON、单一“复位/初始化”（首次或参考失效时找零，参考仍有效时只重新检查）、五个阶段的脚本编辑与单阶段运行，以及“完整流程运行”；复用现有 Stop，不增加重复按钮。
- 不增加独立“回到起始位置”、永久保存到设备、配方平台或恢复向导。混合阶段负责升降回零，网页通过现有状态和日志说明拒绝/故障原因。
- “完整流程运行”调用与屏幕相同的 Start 入口，只在 Ready 接受并按固定顺序运行。
- 单阶段检查必要前提，不偷偷执行其他业务阶段；不满足条件时明确拒绝。单阶段调试不改变完整流程的唯一状态源和队列所有权规则。
- Load JSON 恢复全部阶段和参数到编辑器；Apply 前校验并在空闲时整份替换 RAM 配置，Load / Apply 均不启动电机。
- Export JSON 导出全部配置用于下次继续调试、备份和 Git 留档；不是执行日志。

### 同一格式，两种来源

```text
调试：Load/edit JSON -> explicit Apply -> Motion RAM -> stage/full run -> Export
演示：Export -> motion/data/demo_flow.json -> build embedding -> flash -> boot parse
```

- RAM 调试配置断电丢失；重启读取本次固件内置版本，不自动恢复网页草稿。
- JSON 放进工程后，通过构建自动内嵌，不要求人工转换 C++。可以沿用当前 embed_txtfiles 的机制，保留原 data/index.html 条目并增加 JSON。
- 本轮不实现 LittleFS、SPIFFS 或 JSON 的 NVS 持久化，不增加“保存到设备永久生效”的按钮承诺。
- 导入电脑文件不会自动修改固件内置配置；最终演示版本需重新编译烧录一次。
- 两种来源共用同一个解析器。有效配置在空闲时整份替换，失败保留原 RAM 配置；本轮开始锁定快照。
- 工具已有换算参数 NVS 不删除；软件零点、当前位置和初始化资格不随 JSON 恢复。

### JSON 结构草案

这是配置格式示例，时间/轴号/误差非验收参数；空脚本允许编辑，但不能启用为完整演示。

```json
{
  "schema_version": 1,
  "name": "v1-demo",
  "display": {
    "baby_name": "DEMO",
    "formula_brand": "NOT FOR FEEDING",
    "water_ml": 180,
    "temperature_c": 45
  },
  "initialization": {
    "timeout_ms": 60000,
    "zero_axes": [
      {"motor_id": 1, "zero_tolerance_deg": 1.0},
      {"motor_id": 3, "zero_tolerance_deg": 1.0}
    ],
    "commands": []
  },
  "stages": [
    {"id": "open_cap", "timeout_ms": 120000, "commands": []},
    {"id": "water", "timeout_ms": 30000, "commands": []},
    {"id": "powder", "timeout_ms": 15000, "commands": []},
    {"id": "close_cap", "timeout_ms": 60000, "commands": []},
    {"id": "mix", "timeout_ms": 60000, "commands": []}
  ]
}
```

commands 中每项就是一行原工具指令，例如：

```json
{
  "id": "open_cap",
  "timeout_ms": 120000,
  "commands": [
    "enable 1",
    "move 1 -2 mm 300 300 300 200 await"
  ]
}
```

上述片段只演示包装，不是完整开盖动作。原脚本顺序和 await 语义不变；转换不自动删除 home 或 disable。增加电机动作只改 commands，不改 C++ 阶段接口。

- 新配置层读取 JSON、检查阶段、超时及命令，再交现有指令解析器，不重新实现电机协议。
- 初始化/五阶段的业务 ID 与顺序固定，屏幕映射在固件中维护；重复/缺少/未知阶段不得启用。
- 最终 schema 需记录或校验 rotationDistance 等影响指令含义的配置；与设备实际参数不匹配则拒绝执行，不静默换算。字段从现有工具配置提取，不另造一套参数含义。
- 保持执行器既有容量限制（当前单队列 64 动作、8192 字节），另限定总 JSON 大小和超时范围。配置解析仅在空闲进行，不影响运行中的停止监督。
- 自动演示只允许能够定义结束条件的指令组合；raw CAN 和无限持续输出留在原手动工具，不默认进入完整流程。

## 5. 实施拆分

| 步骤 | 工作 | 验收 |
| --- | --- | --- |
| 1 | 编译宏、协议复用、DisplayLink、State/Start/ACK | BRAIN 原功能不回归；DISPLAY 与原显示板互通、重试不重复启动 |
| 2 | JSON schema、内置构建、RAM 配置解析、fake executor 状态机 | 同 JSON 两种来源结果一致；无电机验证首次找零、Ready/Start、阶段顺序、Complete 展示计时及非阻塞超时 |
| 3 | 对接现有队列/反馈、阶段监督、Stop 取消流程 | 混合完成时真实确认回零/静止；失败/停止不进入下一阶段，不重构电机驱动 |
| 4 | 阶段编辑/运行、独立找零、完整运行、Load/Apply/Export | 上次 JSON 可继续调试；导入不运动；忙碌不能替换配置；无需新增停止按钮 |
| 5 | 双板实机与文档交付 | 逐轴、单阶段、完整流程通过；断线/重启/故障可恢复；内置版本可脱离电脑演示 |

建议文件：motion 中新增 DisplayLink、DemoFlowController、DemoFlowConfig；main.cpp 只接轮询；platformio.ini 增加宏/嵌入资源；motion/data/demo_flow.json 提供内置配置；网页源文件增加最小流程控件并重建内嵌页面。已有队列只补必要接口。文档同步 motor-queue、motion-debug、构建说明，不能把新功能写成旧功能已具备。

初始内置配置应明确未配置或禁止运动，不能附带未经验收的通用机械脚本。每个子任务验证完成后按一笔逻辑提交组织，实际提交需授权。

## 6. 验证与待确认

- 协议：CRC/截断/版本、golden bytes、重复/冲突 sequence、ACK 丢失重试、重启和屏幕离线。
- 状态机：上电参考无效、首次找零成功/失败、DemoFlowController 为唯一状态源、Ready 与 startEnabled 始终一致、非 Ready 的 Start 被拒绝、反馈过期、零位容差边界、混合结束未回零、Complete 3 秒内无运动及每阶段超时；Stop 与完成同时到达不推进。
- 快照：非 Error 阶段始终 `error=None`，V1 只产生约定的最小错误集合；无真实传感器时两个 condition 均为 None；`cloudConnected=false` 不覆盖 Ready 派生的 startEnabled。
- JSON：导入导出等价、内置/RAM 同语义、未知版本/非法指令/过大文件/换算不匹配拒绝；无效配置不替换原配置；上电、Load、Apply 和重启回内置均不运动。
- 交互：屏幕只保留现有 Start；网页只出现约定的最小操作，不新增独立归位、重复 Stop、永久保存或恢复向导。
- 调度：await/wait/长动作/阶段切换时网页、CAN 和 UART 持续响应；原停止入口仍有效。
- 实机：确认首次找零顺序/干涉、混合阶段升降回零、承重保持、单位换算和实际到位；先低速逐轴，再单阶段，最后完整演示。自动测试不能代替机械验收。
- Review：代码逻辑与 QA 分别检查；独立 reviewer 不可用时说明自审限制。

仍需落实：最终阶段脚本、需确认回零的轴及位置容差、软件零点与工具坐标对应关系、各阶段超时、承重轴停机策略，以及加水/加粉是否使用模拟等待。软件骨架和协议测试可先推进，不要求先重构 Product。
