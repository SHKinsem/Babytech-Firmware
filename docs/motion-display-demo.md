# 设备板与显示板演示操作说明

描述对象：设备板（`device-controller/`）的显示板 UART 协议配置、产品显示板（`Embeded_System/DisplayController`）和网页“屏幕流程”。产品显示板是独立设备，不是主控板（`main-controller/`）。Milestone：V1 软件接入完成，机械脚本与双板实机验收待完成。本说明不代表机构或真实出料已验收。

## 构建与接线

默认 `pio run -d device-controller` 构建设备板的显示板协议 v3 配置；`device-controller/platformio.ini` 的 `build_flags` 包含 `-DDEVICE_UART_PEER=2`。与主控板联调 UART v2 时，将这项构建标志改为 `-DDEVICE_UART_PEER=1`，重新编译并烧录设备板。PlatformIO 环境名 `motion` 是保留的构建兼容标识，不是板名。旧 `MOTION_UART_PEER` 构建宏仍兼容，但同时指定新旧宏时取值必须一致。

启动日志显示 `peer=display-controller protocol=3`（显示板协议）或 `peer=main-controller protocol=2`（主控板协议）；非法宏值不能编译。显示板配置不消费主控板协议帧，也不向显示板发送主控板协议帧。

显示板继续使用产品仓库 `Embeded_System/DisplayController` 的 `display` 固件；无需迁入 LCD、触摸或 LVGL。共享协议来源见 [SOURCE.md](../shared/BabytechDisplayCore/SOURCE.md)。设备板和显示板两端都必须是 protocol/schema 3。

设备板 GPIO43 TX 接显示板 GPIO44 RX；设备板 GPIO44 RX 接显示板 GPIO43 TX，共地。115200 / 8N1 / 3.3 V；分别 USB 供电时不互接 5 V。依实际 GPIO 接线，不凭排针 TX/RX 丝印判断方向。

## 配置与操作

1. 连接设备板热点 `Babytech-Motion`，进入网页“屏幕流程”。设备板若选择主控板协议 v2，网页会明确显示此功能不可用。
2. 内置 `device-controller/data/demo_flow.json` 保存台架配置；上电只校验并载入，不自动运动。网页 Load 与编辑只改变本机草稿。配置可解析不代表机构已验收或已经 Ready。
3. Apply 校验后整份替换 RAM 配置，不运动；失败保留旧配置。配置替换撤销旧软件参考。Export 导出编辑器中的配置，可保存到上述工程路径后重新编译烧录。没有文件系统上传、永久保存按钮或自动恢复中断流程。
4. 在 Apply 可接受配置后，由现场确认机构安全，点击网页“复位 / 初始化”，或配套新版显示板圆环右侧的 `Initialize`。两者共用设备板初始化入口；显示板找零中仍显示 NotReady，不增加 initializing 状态。入口只要求配置有效、执行器空闲且 CAN 可用，不以五轴位置/速度轮询完整为前提；队列的 `await`、已知故障及脚本超时仍可使本次初始化失败。
5. 先逐个调试业务阶段。单阶段结束停在 NotReady，不自动执行其他阶段；需回零时运行包含回程动作的混合阶段，再点复位重新检查。软件参考仍有效时复位不会重新碰撞找零。
6. Ready 时用屏幕 Start 或网页“完整流程运行”：开盖 → 加水 → 加粉 → 关盖 → 混合（脚本内按相对位移回程）→ Complete 保持 3 秒 → Ready。流程层不再额外核验五轴实时位置/速度或混合后的零位；队列 `await` 与阶段超时仍生效。没有额外回起始位置阶段；展示计时不发送运动指令。下次 Start 前操作者自行换瓶。
7. 任何阶段可用顶部“全部停止”。取消剩余脚本并发送广播回零中断/停止；新鲜静止反馈才证明停止，超过 3 秒仍未确认则 Error。Error 由显式复位解除，参考失效时重新初始化，不续跑旧动作。

当前内置配置在关盖阶段使用 `sync begin trigger` 同步组；轴 1 的 68.2 mm 回程仅为位移账面平衡，两者均尚未完成这套演示流程的实机验收。

显示板保留 Start 和状态显示，并通过新版 Initialize 入口请求设备板初始化。设备板提供的 `startEnabled` 由 Ready 派生，Cloud offline 提示允许保留。宝宝、品牌、水量、温度为演示数据，`thermalSimulated=true`，两个条件字段均为 None；产物不用于喂养。

## JSON 与脚本契约

格式基线见 [demo_flow.json](../device-controller/data/demo_flow.json)。总 JSON 最大 **16384 bytes**；最多 5 个轴，每段最多 64 条命令、8192 bytes、超时 100–3600000 ms。stage ID 固定为 `open_cap`、`water`、`powder`、`close_cap`、`mix`；JSON 顺序不改变执行顺序。重复/缺失/未知阶段、重复键、非法枚举与数值、过深嵌套、多行注入均被拒绝。

新增 `axes` 是全部参与执行与停稳检查的轴清单，例如：

```json
"axes": [
  {"motor_id": 1, "rotation_distance_mm": 8},
  {"motor_id": 3, "rotation_distance_mm": 40}
]
```

示例值不是实机参数。`rotation_distance_mm` 必须与该 ID 板端保存值一致；0 表示板端未配置此换算，只能使用 deg/rev。`initialization.zero_axes` 必须引用此清单，容差为 0.1–10 度。未声明轴的指令不能运行。

每项 `commands` 是一行既有队列指令；演示额外支持软件零点指令：

```text
zero ID RPM ACCEL DECEL CURRENT
```

它按初始化完成时记录的驱动坐标发送同一 CD 定位指令的绝对模式，并等待目标位置、新鲜静止反馈；不触发碰撞，也不把 `move ID 0` 当作归零。只能用于声明为 zero_axes 的轴，不能放在初始化脚本中。当前台架配置暂不使用 `zero`，以相对位移回程。

演示脚本要求普通 `move/home` 显式带 `await`；仅业务阶段 `sync begin [trigger]` 与 `sync end` 之间的相对 `move` 不带 `await`。同步组必须完整，且只包含 2–8 个不同轴的相对运动。运行前须按[队列说明](motor-queue.md)保存同步参数；默认全零时，关盖阶段会在首个动作前被拒绝。初始化碰撞找零必须为 `home ID 2 await`，每个 zero_axis 都需覆盖；初始化不接受同步组。碰撞速度、电流、返回角度等参数必须提前由现有工具配置并实机确认。Ready 不证明这些参数已验收。

允许 `enable`、`disable`、`move … await`、完整 `sync` 组、`stop`、`wait`、有持续时间的 torque/velocity；home 只用于初始化。演示不接受 raw hex/CAN、无限持续输出；这些能力仍留在原调试队列中。`disable` 会释放电机保持力，当前队列发送后不等待失能确认，Ready 也不检查使能状态；承重轴失能须先完成实机风险确认。加水/加粉可显式填写 `wait 毫秒数`，对应定时模拟，不代表真实出料检测。

普通队列的直接发送语义保持不变。演示队列增加严格回零证据：仅 ACK + 空闲状态不算找零完成；需要本次运行→完成状态或明确 `9F` 完成应答，再确认静止。`12/22` 未运动应答不建立零点。

## 参考、停止与故障

初始化脚本的 `await` 完成后，设备板只对 `zero_axes` 设置 X 固件手册 p77 的易失掉电标志（`50 01`），读回 `3A.bit7=1` 才进入 Ready。之后标志变回 0 视为找零轴驱动重启，撤销软件参考并停止。实际 X28S/X42S 必须验证支持该标志；不支持时初始化不能通过。

上电、配置替换、手动运动/原点相关操作与坐标换算修改撤销参考；已知故障、队列执行失败和已观察到的找零轴驱动重启仍会中止流程，其中已知故障和驱动重启会撤销参考。普通状态轮询偶发缺样不撤销 Ready；这也意味着 Ready 不证明机构仍在机械零位。屏幕重启和 Wi-Fi 断线不改变参考；停止确认仍要求新鲜静止反馈。实机仍需验收方向、滑移与机械干涉。

自动流程、初始化、停止确认和 Complete 展示期间独占执行器，手动插入动作及配置写入会收到 `demo_busy`。演示的必要反馈查询不受普通“暂停自动查询”开关影响。日志记录 `demo.state`；详细位置、队列错误与 CAN 帧仍在原诊断页面。

五阶段超时映射对应的屏幕超时错误；明确的 CAN/驱动故障使用 CanFault，队列 `await` 失败及首次初始化超时仍会进入 Error。普通轮询缺样本身不再直接报错。非 Error 阶段 error=None；不会产生没有真实依据的缺水、温控或瓶位错误。

## HTTP API

显示板 UART 的 v3 Intent 新增单字节 `Initialize=2`，原 `StartFeeding=1` 和 State 格式不变。Initialize 仅在 NotReady / Error 转交初始化入口，Ready / 运行态拒绝。ACK 表示接受请求，不表示完成；相同序号重复请求不重复执行，同序号换意图拒绝。找零期间再次点击的新序号由 busy 检查拒绝。显示板离线或待 ACK 时禁用按钮，不自动恢复初始化请求；旧设备板固件不识别新指令，会导致显示板 ACK 超时，须配套更新显示板和设备板。旧显示板固件配新版设备板固件时仍可使用网页初始化。

POST 沿用 `application/x-www-form-urlencoded`。请求被接受不表示机械完成；客户端不自动重发 POST。

| 接口 | 内容 |
| --- | --- |
| `GET /api/demo` | available、stage、error、reason、startEnabled、busy、initializing、referenceValid、configured |
| `GET /api/demo/config` | 当前已应用 JSON，读取不运动 |
| `POST /api/demo/config` | `json`：整份校验并应用到 RAM |
| `POST /api/demo/action` | `action=initialize`：复位检查或首次找零 |
| 同上 | `action=start`：与屏幕相同 Ready 入口 |
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

本次软件交付验证：上述三组主机测试、网页 82 项测试、设备网页构建及 Motion 的 BRAIN / DISPLAY 双模式编译均通过。代码自审覆盖执行器独占、配置替换、停止确认、多轴反馈和掉电参考失效；未进行独立 Agent Review。浏览器模拟回归无控制台错误，不替代双板 UART 与真实电机验证。

未烧录或驱动硬件。现场必须先确认电源/急停和机械区域，再逐轴低速验收碰撞找零、`50/3A` 掉电检测、软件 zero、单阶段、完整流程、停止未确认、屏幕断线/重启及连续循环。水粉等待演示不等于称重/流量闭环。App、Cloud、SQLite 无需升级或迁移；显示板需确认已烧录协议 v3。
