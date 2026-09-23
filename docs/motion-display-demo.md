# Motion / Display 演示操作说明

描述对象：工具 Motion 的 DISPLAY 编译分支、原产品 DisplayController v3 和网页“屏幕流程”。Milestone：V1 软件接入完成，机械脚本与双板实机验收待完成。本说明不代表机构或真实出料已验收。

## 构建与接线

默认 `pio run -d motion` 仍构建 BRAIN/v2。需要演示时，在 `motion/platformio.ini` 的 `build_flags` 增加 `-DMOTION_UART_PEER=2` 后构建同一个 `motion` environment。也可使用临时环境变量（须保留既有编译参数）：

```sh
PLATFORMIO_BUILD_FLAGS='-std=gnu++17 -DBOARD_HAS_PSRAM -DARDUINO_USB_CDC_ON_BOOT=1 -DMOTION_UART_PEER=2' pio run -d motion
```

启动日志 `peer=display-v3` 表示显示分支；`peer=brain-v2` 表示原协议。非法宏值不能编译。DISPLAY 分支不消费 Brain 协议或向屏幕发送 Brain 帧。

屏幕继续使用产品仓库 `Embeded_System/DisplayController` 的 `display` 固件；无需迁入 LCD、触摸或 LVGL。共享协议来源见 [SOURCE.md](../shared/BabytechDisplayCore/SOURCE.md)。两端都必须是 protocol/schema 3。

Motion GPIO43 TX 接屏幕 GPIO44 RX；Motion GPIO44 RX 接屏幕 GPIO43 TX，共地。115200 / 8N1 / 3.3 V；分别 USB 供电时不互接 5 V。依实际 GPIO 接线，不凭排针 TX/RX 丝印判断方向。

## 配置与操作

1. 连接 Motion 热点，进入网页“屏幕流程”。BRAIN 构建会明确显示功能不可用。
2. 内置 `motion/data/demo_flow.json` 故意留空机械脚本，`configured=false`。填写轴清单、初始找零和五阶段脚本，或 Load 已保存的 JSON。加载与编辑只改变本机草稿。
3. Apply 校验后整份替换 RAM 配置，不运动；失败保留旧配置。配置替换撤销旧软件参考。Export 导出编辑器中的配置，可保存到上述工程路径后重新编译烧录。没有文件系统上传、永久保存按钮或自动恢复中断流程。
4. 确认机构可运动、电机反馈新鲜且静止，点击网页“复位 / 初始化”，或配套新版显示板圆环右侧的 `Initialize`。两者共用 Motion 初始化入口；屏幕找零中仍显示 NotReady，不增加 initializing 状态。初始化成功才显示 Ready；若缺反馈、驱动拒绝、配置不匹配，不能进入 Ready。
5. 先逐个调试业务阶段。单阶段结束停在 NotReady，不自动执行其他阶段；需回零时运行包含 `zero` 指令的混合阶段，再点复位重新检查。软件参考仍有效时复位不会重新碰撞找零。
6. Ready 时用屏幕 Start 或网页“完整流程运行”：开盖 → 加水 → 加粉 → 关盖 → 混合（脚本内升降回软件零点）→ Complete 保持 3 秒 → Ready。没有额外回起始位置阶段；展示计时不发送运动指令。下次 Start 前操作者自行换瓶。
7. 任何阶段可用顶部“全部停止”。取消剩余脚本并发送广播回零中断/停止；新鲜静止反馈才证明停止，超过 3 秒仍未确认则 Error。Error 由显式复位解除，参考失效时重新初始化，不续跑旧动作。

屏幕只保留原 Start 和状态显示。Motion `startEnabled` 由 Ready 派生，Cloud offline 提示允许保留。宝宝、品牌、水量、温度为演示数据，`thermalSimulated=true`，两个条件字段均为 None；产物不用于喂养。

## JSON 与脚本契约

格式基线见 [demo_flow.json](../motion/data/demo_flow.json)。总 JSON 最大 **16384 bytes**；最多 5 个轴，每段最多 64 条命令、8192 bytes、超时 100–3600000 ms。stage ID 固定为 `open_cap`、`water`、`powder`、`close_cap`、`mix`；JSON 顺序不改变执行顺序。重复/缺失/未知阶段、重复键、非法枚举与数值、过深嵌套、多行注入均被拒绝。

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

它按初始化完成时记录的驱动坐标发送同一 CD 定位指令的绝对模式，并等待目标位置、新鲜静止反馈；不触发碰撞，也不把 `move ID 0` 当作归零。只能用于声明为 zero_axes 的轴，不能放在初始化脚本中。混合脚本须先按机构需要安排 zero，再执行混合。

演示脚本要求 `move/home` 显式带 `await`。初始化碰撞找零必须为 `home ID 2 await`，每个 zero_axis 都需覆盖。碰撞速度、电流、返回角度等参数必须提前由现有工具配置并实机确认。Ready 不证明这些参数已验收。

允许 `enable`、`move … await`、`stop`、`wait`、有持续时间的 torque/velocity；home 只用于初始化。演示不接受 disable、raw hex/CAN、无限持续输出。这些能力仍留在原调试队列中。加水/加粉可显式填写 `wait 毫秒数`，对应定时模拟，不代表真实出料检测。

普通队列的直接发送语义保持不变。演示队列增加严格回零证据：仅 ACK + 空闲状态不算找零完成；需要本次运行→完成状态或明确 `9F` 完成应答，再确认静止。`12/22` 未运动应答不建立零点。

## 参考、停止与故障

初始化结束后，Motion 对声明轴逐个设置 X 固件手册 p77 的易失掉电标志（`50 01`），读回新鲜 `3A.bit7=1` 才完成初始化。之后标志变回 0 视为驱动重启，撤销软件参考并停止。实际 X28S/X42S 必须验证支持该标志；不支持时初始化不能通过。

上电、配置替换、手动运动/原点相关操作与坐标换算修改撤销参考；故障、运行中反馈丢失、已观察到的驱动重启同样撤销。正常循环、屏幕重启、Wi-Fi 断线和位置可信的已确认 Stop 不撤销。位置与速度采样不能证明两次采样之间的全部机械行为；仍需实机验收方向、滑移与机械干涉。

自动流程、初始化、停止确认和 Complete 展示期间独占执行器，手动插入动作及配置写入会收到 `demo_busy`。演示的必要反馈查询不受普通“暂停自动查询”开关影响。日志记录 `demo.state`；详细位置、队列错误与 CAN 帧仍在原诊断页面。

五阶段超时映射对应的屏幕超时错误；CAN/驱动拒绝/缺反馈使用 CanFault，未分类失败和首次初始化超时使用 Unknown。非 Error 阶段 error=None；不会产生没有真实依据的缺水、温控或瓶位错误。

## HTTP API

屏幕 UART 的 v3 Intent 新增单字节 `Initialize=2`，原 `StartFeeding=1` 和 State 格式不变。Initialize 仅在 NotReady / Error 转交初始化入口，Ready / 运行态拒绝。ACK 表示接受请求，不表示完成；相同序号重复请求不重复执行，同序号换意图拒绝。找零期间再次点击的新序号由 busy 检查拒绝。屏幕离线或待 ACK 时禁用按钮，不自动恢复初始化请求；旧 Motion 不识别新指令，会导致屏幕 ACK 超时，须配套更新显示板和 Motion。旧屏幕配新 Motion 仍可使用网页初始化。

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
