# Motion 网页流程调试说明

描述对象：当前 Motion UART v4 固件中的网页“屏幕流程”、JSON 与 HTTP 调试接口。Milestone：V1 软件闭环，机械脚本与双板实机验收待完成。本文不描述 Brain 产品命令、旧 UART v3 操作或真实出料验收。

## 构建与接线

仅支持现有 Brain/Motion UART v4 配套构建，不新增 environment。旧 Brain 宏 0、Motion peer 1/2 编译入口已经退役并明确报错；历史实现从 Git 获取，不再提供旧演示烧录步骤。网页 CAN 控制、队列、流程调试、Wi-Fi、称重与 OTA 保持可用，不依赖 Brain 配对或 Cloud 在线。

在子仓库根目录只编译（不烧录）：

```sh
pio run -d main-controller -e brain
pio run -d device-controller -e motion
```

Brain 产品命令经 UART v4 的持久运行入口处理；网页流程是 Motion 工作台 owner，不等同于屏幕产品 Start/Initialize，不生成假宝宝/配方。真实流程总开关 `BABYTECH_ENABLE_NON_CONSUMABLE_PRODUCT_FLOW` 已删除；Motion `board_config.h` 的 `BABYTECH_V1_MOTOR_TEST` 默认 1，仅选择产品测试资源，不改变网页工作台所有权。NVS、SQLite schema 和 v4 线字段不变，Brain/App/Cloud 无需改动。软件编译不代表服务器切换或实机验收。

产品默认低液位有效且 false、剩余粉量固定 300 g（集中常量可调），启动/运行与 UART 上报同源。Cloud/App 沿既有字段显示 normal/300，无新增 UI 测试标识；这些是测试占位数据，不可实喂。宏 0 使用当前真实低液位/HX711 入口，需要 GPIO/极性配置及去皮/标定。温度不伪造，目标默认 45°C 或沿现有配方，water_temp/measured_water_temp 仍 null，温控未实现。两种选择保留真实 CAN、电机反馈/初始化、Stop、冲突、持久记录与身份保护；产品完成后 Initialize 再测试，不开机自动运动。Brain SIM 默认关闭，其 15 秒虚拟流程不运行真实 CAN，与 Motion 电机测试不同。

Motion GPIO43 TX 接屏幕 GPIO44 RX；Motion GPIO44 RX 接屏幕 GPIO43 TX，共地。115200 / 8N1 / 3.3 V；分别 USB 供电时不互接 5 V。依实际 GPIO 接线，不凭排针 TX/RX 丝印判断方向。

## 配置与操作

1. 使用上述默认 v4 构建，连接 Motion 热点，进入网页“屏幕流程”。无需选择旧宏；网页工作台和 Brain 产品入口保持各自的所有权与准入语义。
2. 内置 `device-controller/data/demo_flow.json` 保存台架配置；上电只校验并载入，不自动运动。网页 Load 与编辑只改变本机草稿。配置可解析不代表机构已验收或已经 Ready。
3. Apply 校验后整份替换 RAM 配置，不运动；失败保留旧配置。配置替换撤销旧软件参考。Export 导出编辑器中的配置，可保存到上述工程路径后重新编译烧录。没有文件系统上传、永久保存按钮或自动恢复中断流程。
4. 在 Apply 可接受配置后，由现场确认机构安全，点击网页“复位 / 初始化”。Brain 屏幕 Initialize 走产品运行入口，不按旧演示协议提交意图。入口只要求配置有效、执行器空闲且 CAN 可用，不以五轴位置/速度轮询完整为前提；队列的 `await`、已知故障及脚本超时仍可使本次初始化失败。
5. 先逐个调试业务阶段。单阶段结束停在 NotReady，不自动执行其他阶段；需回零时运行包含回程动作的混合阶段，再点复位重新检查。软件参考仍有效时复位不会重新碰撞找零。
6. Ready 时用网页“完整流程运行”：开盖 → 加水 → 加粉 → 关盖 → 混合（脚本内按相对位移回程）→ Complete 保持 3 秒 → Ready。流程层不再额外核验五轴实时位置/速度或混合后的零位；队列 `await` 与阶段超时仍生效。没有额外回起始位置阶段；展示计时不发送运动指令。下次 Start 前操作者自行换瓶。
7. 任何阶段可用顶部“全部停止”。取消剩余脚本并发送广播回零中断/停止；新鲜静止反馈才证明停止，超过 3 秒仍未确认则 Error。Error 由显式复位解除，参考失效时重新初始化，不续跑旧动作。

当前内置配置在关盖阶段使用 `sync begin trigger` 同步组；轴 1 的 68.2 mm 回程仅为位移账面平衡，两者均尚未完成这套演示流程的实机验收。

网页流程的 Ready 来自已应用的台架配置与软件参考，不证明真实出料。Brain 产品 Start/Initialize 使用既有 v4 运行路径及真实缓存，不使用旧演示数据；产物不用于喂养。

## JSON 与脚本契约

格式基线见 [demo_flow.json](../device-controller/data/demo_flow.json)。总 JSON 最大 **16384 bytes**；最多 5 个轴，每段最多 64 条命令、8192 bytes、超时 100–3600000 ms。stage ID 固定为 `open_cap`、`water`、`powder`、`close_cap`、`mix`；JSON 顺序不改变执行顺序。重复/缺失/未知阶段、重复键、非法枚举与数值、过深嵌套、多行注入均被拒绝。

新增 `axes` 是全部参与执行与停稳检查的轴清单，例如：

```json
"axes": [
  {"motor_id": 1, "rotation_distance_mm": 8},
  {"motor_id": 3, "rotation_distance_mm": 40}
]
```

示例值不是实机参数。产品/屏幕流程的毫米换算只使用当前 JSON `axes[].rotation_distance_mm`，不再要求与调试网页保存的 NVS 值相同。当前内置草案为轴1=2、轴3=40 mm/rev；其余轴0表示本脚本不提供毫米换算，只能使用 deg/rev。毫米指令缺少有效正数换算仍被拒绝；新板无需先在网页重复保存2/40，上电加载脚本但不自动运动，仍须现场确认参数并 Initialize。网页 Apply 只修改 RAM，永久修改内置参数仍需编辑 `device-controller/data/demo_flow.json` 后重编译烧录。`initialization.zero_axes` 必须引用此清单，容差为 0.1–10 度。未声明轴的指令不能运行。

“编排队列”中的手动程序仍使用 `/api/motor-distance` 保存的板端 NVS 值；此接口及原有数据不变。两种入口各自使用自己的已确认配置：产品使用 JSON，手动队列使用 NVS。不要把网页手动换算值当作产品脚本已更新；运行中仍不能替换配置或修改换算值。

每项 `commands` 是一行既有队列指令；演示额外支持软件零点指令：

```text
zero ID RPM ACCEL DECEL CURRENT
```

它按初始化完成时记录的驱动坐标发送同一 CD 定位指令的绝对模式，并等待目标位置、新鲜静止反馈；不触发碰撞，也不把 `move ID 0` 当作归零。只能用于声明为 zero_axes 的轴，不能放在初始化脚本中。当前台架配置暂不使用 `zero`，以相对位移回程。

演示脚本要求普通 `move/home` 显式带 `await`；仅业务阶段 `sync begin [trigger]` 与 `sync end` 之间的相对 `move` 不带 `await`。同步组必须完整，且只包含 2–8 个不同轴的相对运动。产品流程优先使用已有有效同步配置；未配置时使用 `productSyncDefaults()` 的 RAM 默认值：进度容差 0.02、时间容差 50 ms、反馈/准备/停止期限 5000/10000/3000 ms、响应预算 100 ms、完成容差 0.2 度。不写 NVS，也不覆盖有效配置；preflight、派发及回包关联使用同一运行快照。已有配置与查询预算不兼容仍拒绝，不偷偷替换。手动队列仍须按[队列说明](motor-queue.md)配置，产品默认值不替它放行。默认值不是实际响应时延或机械精度的验收结论。初始化碰撞找零必须为 `home ID 2 await`，每个 zero_axis 都需覆盖；初始化不接受同步组。碰撞速度、电流、返回角度等参数必须提前由现有工具配置并实机确认。Ready 不证明这些参数已验收。

允许 `enable`、`disable`、`move … await`、完整 `sync` 组、`stop`、`wait`、有持续时间的 torque/velocity；home 只用于初始化。演示不接受 raw hex/CAN、无限持续输出；这些能力仍留在原调试队列中。`disable` 会释放电机保持力，当前队列发送后不等待失能确认，Ready 也不检查使能状态；承重轴失能须先完成实机风险确认。加水/加粉可显式填写 `wait 毫秒数`，对应定时模拟，不代表真实出料检测。

普通队列的直接发送语义保持不变。演示队列增加严格回零证据：仅 ACK + 空闲状态不算找零完成；需要本次运行→完成状态或明确 `9F` 完成应答，再确认静止。`12/22` 未运动应答不建立零点。

## 参考、停止与故障

初始化脚本的 `await` 完成后，Motion 只对 `zero_axes` 设置 X 固件手册 p77 的易失掉电标志（`50 01`），读回 `3A.bit7=1` 才进入 Ready。之后标志变回 0 视为找零轴驱动重启，撤销软件参考并停止。实际 X28S/X42S 必须验证支持该标志；不支持时初始化不能通过。

上电、产品脚本替换、已接受的手动运动/运动队列及原点或控制模式修改撤销参考；运动部分发送后报错同样撤销。独立手动 mm/rev 保存/清除、调试限值修改、参数校验或保存失败、使能本身、纯等待/只读队列不主动撤销参考，不要求重复 Initialize。原始队列中无法可靠识别的帧仍按可能影响参考处理，但不新增发送门禁。电流上限、反馈周期和不含上电回零的回零参数写入本身不撤销；暂时失去反馈后按原条件恢复。动作前已有故障或驱动重启仍使参考失效，不能被使能/等待队列清掉证据后掩盖；真实发送故障、显式 Stop/失能及控制状态重置保留原处理。队列执行失败仍会中止运行中的产品流程。普通轮询偶发缺样不撤销参考，但新鲜度仍影响可开始；Ready 不证明机构仍在机械零位。屏幕重启和 Wi-Fi 断线不改变参考；停止确认仍要求新鲜静止反馈。实机仍需验收方向、滑移与机械干涉。

自动流程、初始化、停止确认和 Complete 展示期间独占执行器，手动插入动作及配置写入会收到 `demo_busy`。演示的必要反馈查询不受普通“暂停自动查询”开关影响。日志记录 `demo.state`；详细位置、队列错误与 CAN 帧仍在原诊断页面。

产品就绪与产品队列 `move/home await` 使用按轴数和共享查询预算计算的 600–5000 ms 反馈有效期；默认五轴、10 次/秒为 5000 ms，不是额外等待。每类反馈接收时冻结有效期，之后扩大预算不能复活旧样本。就绪仍要求各配置轴位置/速度/状态有效、停稳且无故障；`await` 仍要求完成证明后的两组不同停稳样本和正确目标/回零证明。手动反馈 600 ms、手动队列 await 1000 ms 不变；Stop 使用冻结窗口及 Stop 后新反馈，原 3 秒确认期限不变。缺样到期仅让就绪停稳证据失效，正常反馈恢复后自动重建，不增加人工 Initialize 或配对步骤。

五阶段超时映射对应的屏幕超时错误；明确的 CAN/驱动故障使用 CanFault，队列 `await` 失败及首次初始化超时仍会进入 Error。普通轮询缺样本身不再直接报错。非 Error 阶段 error=None；不会产生没有真实依据的缺水、温控或瓶位错误。

## HTTP API

旧 UART v3 Intent/State 运行入口已经移除；共享 codec 单测仅保留内部回归。下表网页 HTTP 接口在当前 Motion v4 中保持，不以 Cloud 或 Brain 在线作为额外准入；原有机械安全、工作所有权、维护与 OTA 冲突检查不变。

POST 沿用 `application/x-www-form-urlencoded`。请求被接受不表示机械完成；客户端不自动重发 POST。

| 接口 | 内容 |
| --- | --- |
| `GET /api/demo` | available、stage、error、reason、startEnabled、busy、initializing、referenceValid、configured |
| `GET /api/demo/config` | 当前已应用 JSON，读取不运动 |
| `POST /api/demo/config` | `json`：整份校验并应用到 RAM |
| `POST /api/demo/action` | `action=initialize`：复位检查或首次找零 |
| 同上 | `action=start`：网页流程 Ready 入口 |
| 同上 | `action=stage&stage=mix` 等：单阶段调试 |
| 既有 `POST /api/stop-all` | 优先中止演示和余下队列，并请求停止 |

## 验证与实机边界

```sh
python3 tools/test_protocol.py
python3 tools/test_motion.py
python3 tools/test_demo.py
cd tools/motor-protocol-demo
npm test
npm run build:device
```

主机测试覆盖协议 v3、golden intent、重试去重、Ready/Start、初始化、阶段超时、Stop、Complete 计时、millis 回绕、JSON 校验、真实队列适配和软件零点报文。网页通过模拟 HTTP API 验证 Load 不 POST、Apply 不执行、Ready 启动、运行中锁定及已有 Stop；桌面尺寸为 1513×1039 / 1280×800。

原演示阶段软件交付验证：上述三组主机测试、网页 82 项测试、设备网页构建及 Motion 的 BRAIN / DISPLAY 双模式编译均通过。代码自审覆盖执行器独占、配置替换、停止确认、多轴反馈和掉电参考失效；未进行独立 Agent Review。浏览器模拟回归无控制台错误，不替代双板 UART 与真实电机验证；这些证据不代表本次默认 v4 构建或实机验收。

未烧录或驱动硬件。现场必须先确认电源/急停和机械区域，再逐轴低速验收碰撞找零、`50/3A` 掉电检测、软件 zero、单阶段、完整流程、停止未确认、屏幕断线/重启及连续循环。水粉等待演示不等于称重/流量闭环。App、Cloud、SQLite 无需升级或迁移；两板仅使用配套 UART v4，旧屏幕演示不得混烧。
