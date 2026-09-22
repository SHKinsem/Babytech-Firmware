# 直通位置（FB / CB）实机接入完成报告

日期：2026-09-22 · 范围：`motion/`（控制器）与 `tools/motor-protocol-demo/`（实机页面）

直通位置以前在实机页面被标为「仅预览」，板端也拒绝这两个功能码。现在 FB/CB
作为**受监督的立即执行运动**接入：原始功能码与字节原样保留，完成判定由板端
的真实反馈决定。没有改动 CD（梯形）语义、WiFi、队列或 brain 代码。

## 1. 来源与线上约定

依据 `docs/ZDT_X42S/ZDT_X42S_Agent精简参考.md` 与同目录 V1.0.5 完整转录
（p54–p55 命令表、p71 读取实时设定目标位置）：

```text
FB:  A FB dir speed_u16 angle_u32 mode sync 6B                     (12 字节)
CB:  A CB dir speed_u16 angle_u32 mode sync current_u16 6B         (14 字节)
```

- 字段下标：speed 3、angle 5、mode 9、sync 10、current 11（CB 2 字节）。
- `dir` 0/1 = CW/CCW；`mode` 0 相对上一输入目标、1 绝对坐标零点、2 相对当前实际位置；
  `sync` 0 立即执行、1 缓存待 FF 触发。
- 命令**没有**加速度字段。板端不发明加速度：FB 保持 FB、CB 保持 CB，不翻译成 CD。
- 参考例：`02 FB 00 01 2C 00 00 03 84 02 00 6B`（电机 2、CW、300=30.0 RPM、
  900=90.0°、mode 2、立即执行）；CB 在 sync 后追加 `03 20`（800 mA）。

目标基准使用手册 p70 的 `0x33`「读取电机目标位置」（`A 33 6B` →
`[33][符号][幅值 u32][6B]`，7 字节，X 固件 0.1°）：它表示**上一条位置命令要求的目标**，
正是「相对上一输入目标位置」的基准，也是完成判定要核对的对象。

**不使用** p71 的 `0x34`「读取电机实时设定的目标位置」：那是实时设定值，
运动过程中可能是轨迹中间值，既不能当作 mode 0 的基准，也不能当作完成证明。
控制器只解码 `0x33`（`FeedbackField::Target`）；`0x34` 仍是可发送的只读查询
（网关与页面读取清单不变），但它的回包不影响任何运动许可或判定。
它与实际位置 `0x36` 是**两个不同的事实**，在控制器里也分开保存。

## 2. 控制器实现（motion/）

`motion/include/MotionCore.h`

- `decodeFeedback()` 新增 `0x33` → `FeedbackField::Target`（与实际位置 `0x36`
  共用布局、不共用字段；`0x34` 不在此列）。
- `DirectPositionRequest/Plan/Resolution`、`buildDirectPositionPlan()`（与当前位置
  无关的校验：方向、模式、sync、角度幅值、速度策略、CB 电流策略）与
  `resolveDirectTarget()`（按模式解析目标并施加行程策略）、`directDurationMs()`
  （恒速估计；只用于请求期拒绝与预算，**不作**为运行期定时器）。
- 全部用 int64 计算：mode 0/2 的「位置 + 位移」、mode 1 的坐标，最终都必须落在
  `±INT32_MAX`（驱动器能回传的反馈范围）内，否则 `target_out_of_range`，
  不会回绕。

`motion/include/ProtocolGate.h`

- 新 `CommandKind::DirectMove`；`0xFB`（12 字节）与 `0xCB`（14 字节）在
  `dir ≤ 1`、`mode ≤ 2`、`sync == 0` 时通过，其余一律 Invalid。
- `directPositionRefusal()` 给格式正确但被拒的缓存形式（sync 1）单独的原因
  `direct_sync_not_supported`，不再混成 `unsupported_or_invalid_command`。

`motion/src/MotorControl.{h,cpp}`

- `directPosition()`：与 `move()` 相同的准入门槛——已确认使能、新鲜位置与速度、
  速度在停止带内、单一受监督动作、无故障、无停止待确认。
- **mode 0** 必须用新鲜的 `0x33` 读值（600 ms 窗口）解析基准；没有新鲜读值时
  拒绝（`target_not_fresh`，503）并启动**有界**刷新（立即 1 帧 + 窗口 1500 ms 内
  至多 24 帧，且与四字段轮询**并行**发送，不改变位置／速度节奏）。绝不用实际
  位置或本地记住的目标代替。刷新期间重试即可成功。
- **行程策略**作用于 `|解析目标 − 当前实际位置|`：mode 0/2 的输入位移仍受
  `maxAngleDeg` 约束，mode 1 的绝对坐标不按行程裁剪，但实际行程必须在上限内。
  零位移是合法的受监督空操作（速度可为 0，判定容差 1 个反馈计数）；非零位移
  必须有非零速度（`speed_required`）且恒速估计不超过 `maxMoveSeconds`
  （`duration_too_long`）。
- **CB 电流**：手册范围 0–5000 mA，受板端 `maxCurrentMa` 约束，不设 100 mA 下限
  （0 与 50 mA 都接受）。**FB 没有电流字段**，因此无法施加单条命令的电流限制——
  页面与报告都按此说明，不伪造。
- 复用 `MoveJob`，新增 `opcode`（默认 `0xCD`）与 `targetProof`。ACK 只在
  **功能码与作业一致**时置 `ackSeen`：CD 或 CB 的 ACK 不能确认一条 FB 作业
  （反之亦然）。`02` 仍只代表「收到」，`9F` 是手册 p36/p40 记录的到位返回，
  二者都只是 ACK。
- **完成判定**＝匹配的 FB/CB ACK ＋ 两对互不相同的新鲜样本，每对包含新鲜
  `0x33`（严格新于作业开始且新于上一对）、新鲜位置与速度、`|位置−解析目标|`
  在容差内、`|驱动器报告目标−解析目标|` 在容差内、速度在停止带内。
- 运行期截止时间＝板端配置的 `maxMoveDurationMs`（与回零一致），不是自造的
  加速曲线或试验计时器。失败／超时／部分发送沿用既有行为：尽力停止 + 锁存故障
  （`move_timeout` / `move_ack_timeout` / `move_tx_failed` / `feedback_stale`），
  停止或失能取消作业，正常结束不自动失能。
- `0x33` 样本的作废时机：运动命令（CD、FB/CB、速度／力矩试验）、回零触发与
  失败／取消、停止与全部停止、显式失能、参数写入（配置类）、以及任何原始帧
  （`noteRawTransmission`）。作废后 mode 0 会老实地要求重新读取。
- `statusJson` 新增 `targetDeg`（无新鲜读值时为 `null`），让「模式 0 被拒」有据可查。

## 3. 实机页面（tools/motor-protocol-demo/）

- `src/device-api.js`：FB/CB 从「仅预览」列表移除，改为按板端同一套规则校验
  长度、方向、模式、sync、速度、电流（无 100 mA 下限）与 mode 0/2 的行程幅值；
  mode 1 只校验 int32 可回传范围。新增 `directPositionBoardNote()` 明确标注
  「由板端解析」的坐标相关检查（mode 0 的 0x33 基准、mode 1 的实际行程、
  mode 2 的当前实际位置），并补齐 `target_not_fresh`、`travel_out_of_range`、
  `target_out_of_range`、`speed_required`、`direct_sync_not_supported` 等
  错误文案。
- `src/DeviceApp.jsx`：FB/CB 进入「需要使能」列表（与其它受监督运动一致），
  gate 由 `supportReason` + 已确认限制决定，没有残留的硬编码预览名单；
  「试验边界」文案更新为「直通位置已接入」，并显示上面的板端解析说明。

## 4. 验证

- `tests/test_motor_control.cpp` 新增 10 组用例：功能码／字节（含手册示例
  12/14 字节逐字节比对，CAN 两包均重复功能码）、三种模式的目标解析（含
  「不是当前+位移」）、mode 0 缺新鲜读值／过期读值／`0x34` 不能代替 `0x33`、
  完成判定（ACK 单独不算、只有 `0x34` 不算、`0x33` 目标不符不算、两对才完成）、
  `directPosition()` 公开入口在三种模式下的 CB 派发（含电流 0、sync 拒绝、
  超限拒绝）与字节入口一致性、拒绝原因的边界（截断帧不越界读取，校验码／
  广播地址不算同步策略决定）、错误功能码 ACK 不确认并超时、行程／溢出／时长／
  速度／CB 电流边界、零位移空操作与 mode 1 零目标、取消／截止时间／发送失败／
  反馈中断／样本作废（停止与原始帧）。
- `tests/fakes/fake_x42s.cpp|h` 增加 `makeTarget()`（`0x33`）、`makeSetpoint()`
  （`0x34`，用于证明其不可代替）、`TxKind::Direct` 与逐字节逻辑帧记录。
- `tools/motor-protocol-demo/tests/device-limits.test.mjs` 增加 6 组用例：
  编码逐字节、布局／sync／模式、限制镜像、零速规则、板端解析说明（含 `0x33`
  与「`0x34` 不可代替」）。
- `tools/motor-protocol-demo/qa-direct-position.mjs`：桌面 Playwright 模拟板端
  流程，验证 FB／CB 各只提交一次且字节精确、同步缓存仍被拦截、模式 0 已可用并
  标注 `0x33`／`0x34` 区别、零速只在零位移时放行；输入框按 `#field-*` 定位，
  避免标签文本同时命中提示气泡按钮。运行前需先 `npm run build:device` 生成
  `motion/data/index.html`。

## 5. 明确的边界

- FB **不能**施加单条命令的电流限制（协议无该字段）；需要限流请用 CB。
- 缓存形式（sync 1）不在板端监督范围内，仍被明确拒绝；需要时可用队列的原始帧。
- 完成判定会比较驱动器 `0x33` 报告的目标位置与本次解析目标（容差同位置判定）。
  若某台驱动器以其它单位或语义回传该字段，判定会**超时失败**而不是假装完成——
  这是有意的失败方向，现场若遇到应记录真实回包再决定是否放宽。
- `0x34`（p71 实时设定值）只作为可读诊断保留：它不参与 mode 0 解析，也不参与
  完成判定，因为运动过程中它可能是轨迹中间值。
- 只按默认 0.1° 位置输入解释报文；0.01° 缩放的驱动器需另算。
- 未做手机布局；仅在 1513×1039 桌面视口验收。
