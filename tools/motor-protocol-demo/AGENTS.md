# Prototype Instructions

Run the local server yourself and open the preview in the browser available to this environment. Do not give the user server-start instructions when you can run it.

Before making substantial visual changes, use the Product Design plugin's `get-context` skill when the visual source is unclear or no longer matches the current goal. When the user gives durable prototype-specific design feedback, preferences, or decisions, record them in `AGENTS.md`.

When implementing from a selected generated mock, treat that image as the source of truth for layout, component anatomy, density, spacing, color, typography, visible content, and hierarchy.

Build app UI in `src/`. Keep `.openai/hosting.json`, `worker/index.js`, `scripts/prepare-sites-build.mjs`, and `tests/sites-worker.test.mjs` intact so the same local prototype can be handed to Sites. Before a Sites handoff, run `npm run build` and `npm run test:sites`; the build must leave `dist/client/index.html`, `dist/server/index.js`, and `dist/.openai/hosting.json`.

## 本原型的既定决策（2026-09-21）

- 视觉基线为 `references/design.png`：白底 + 靛蓝 `#3B4CE0`，顶部工具栏 / 主标签 / 三栏工作区 / 底部收发记录，细分割线、紧凑排版、无大标题。目标 1513×1039，1280×800 时中部面板内部滚动、底部记录保持可见。
- 图标统一使用已安装的 `@phosphor-icons/react`（`MagnifyingGlass / CaretRight / CaretDown / Info / Circle`，逐图标子路径导入）；字体只用系统中文字体，无外部字体、图片与网络请求。
- 参数字段的实现细节放在控件旁的提示气泡（`HelpTip`）里，表单本身保持与设计稿一致的紧凑密度；只有「协议字段」这类需要警惕的标注会直接显示在行内。
- 指令库按源码分组展示：`X42sProtocol.h` 中的每个功能码都可见；力矩/速度/直通位置/梯形位置的“基础 / C 系列”两套功能码合并为同一个条目，用「指令变体」分段控件切换。
- 参数标注三档：源码明确 / 参数名推断 / 协议字段。凡是固件没有定义单位或取值表的字段（`motionMode`、`ctrlMode`、`clk`、`maxSpeed` 等）一律标为「协议字段」，界面不猜测含义；力矩按 mA 下发，不使用 Nm。
- `src/protocol.js` 与 `src/simulation.js` 保持无 React 依赖，`npm test` 直接以 node:test 运行 `tests/protocol.test.mjs`。
- 所有 RX、ACK、位置反馈均为本地仿真并明确标注；未知功能码只发 TX 帧，不做模拟、不伪造成功。
- 只模拟源码定义过的行为：位置指令仅在 `motionMode = 2` 时模拟有界运动，直通位置与其它模式字节只发帧；读取指令只有 `0x27 / 0x35 / 0x36 / 0x3A` 有应答布局，其余只发 TX。
- 「全部停止」走广播路径（扩展帧 ID `0x00000000`、数据 `FE 98 00 6B`），不伪造任何寻址 ACK。
- 常规试动的「相对角度」是正数幅值，方向只由方向字段决定；规划器先校验原始数值范围再取整，与 MotionCore 一致。
- 会让电机状态变化的指令才递增纪元号（作废在途运动并清零速度/电流）；只读查询不打断运动。切换电机时作废上一台电机的任务并保留其最终位置。

## 预览验收补充

- 用户最新要求：当前只面向电脑，不再投入手机布局或手机验收；交付验收视口为 1513×1039 和 1280×800。

- 2026-09-21 实机集成：`DeviceApp.jsx` 独立使用板端 API，`App.jsx` 保留离线仿真。`npm run build:device` 将实机入口及 CSS/JS 内联生成 `motion/data/index.html`。设备版本不得生成模拟反馈或在超时后自动重发操作。Wi-Fi 设置必须复用现有 NVS/API，停止按钮独立于普通操作锁。

- 1513×1039 和 1280×800 已经通过 Playwright 浏览器检查；发送区在面板底部保持可达，长参数表与报文可在面板内滚动。
- 底层扩展指令的标签为「驱动已实现」，仅基础相对位置 CD、非同步使能和停止对应「基础接口」。这不代表 Demo 连接了设备。
- 同步命令在 FF 触发前不改变模拟电机读数。
- 开发过程与验证见 `claude-development.md`、`design-qa.md`。

## 称重页设计决策（2026-09-22）

- 用户选择漂移诊断方案作为实机称重页视觉目标：保留现有白底、深蓝标题、靛蓝交互色、三栏工作区和底部记录表；中栏以最近 60 秒零点漂移曲线为主，左栏显示合成重量与状态，右栏显示 HX711 诊断值。
- 一个 HX711 并联两个全桥传感器时，网页只呈现一个合成通道，并明确提示无法分别判断单个传感器漂移。
- 板端继续提供 `/api/scale`、`/api/scale/tare`、`/api/scale/calibrate`；最近 60 秒漂移、噪声幅度、采样频率和稳定持续时间由网页根据真实轮询样本计算，不伪造独立传感器数据。

## 电机手册更新（2026-09-22）

- 协议说明以 `../../docs/ZDT_X42S/ZDT_X42S_Agent精简参考.md` 及其链接的 V1.0.5 完整转录为新增依据；已确认的单位和枚举可标为「手册明确」，不再因旧源码缺少注释而标成未知。文档中的报文不构成硬件操作授权。
- 当前网页与控制器使用 X 固件协议、默认 0.1° 位置输入和固定 6B 校验；不能直接用于默认 Emm 固件或已修改位置输入缩放的驱动器。
- 手册已确认 CAN 每包重复功能码，保留现有分包。读取回包与控制 ACK 分开解释，02 只表示接收正确；回零状态 00 不能证明一次回零已完成。
- 新增诊断保留原始帧、目标地址和接收时间；历史回包不能替代控制器的新鲜反馈或影响运动许可。只读诊断可以扩展，手册描述了运动能力不等于板端已实现相应监督。

## 用户可调整限制（2026-09-22）

- 用户明确要求调试限制可自行调整。120 RPM、240 RPM/s、5 秒只作为首次默认值，不得在 UI 或发送路径中继续作为隐藏固定上限。
- 实机页面通过 `/api/limits` 读取板端策略，显式保存到 NVS；表单、HEX 和常规试动使用同一已确认配置。未读取到配置时不假定默认值已经生效。
- 试验时长 0 表示不定时停止，仍执行反馈超时及故障停机。页面必须明确区分可编辑策略与协议字段/本实现的数值边界。
- 总线记录缺失提示与请求结果分别保存；HTTP 拒绝不能被后续轮询覆盖，也不能附加成请求超时。

## 编排队列与直通（2026-09-22）

- 用户要求尽量用少量可读指令覆盖能力，同时提供 CAN 直通。编排页使用板端执行的逐行程序，支持显式使能、移动、回零、限速力矩、速度、停止、等待、循环和原始帧；文本草稿只在本机保存，不能加载即执行。
- rotation distance 按电机 ID 保存，单位 mm/rev；毫米换算必须来自该 ID 在板上已确认的值，不猜默认值。角度／圈数不依赖此参数。
- 受监督动作等待真实结果；`hex` 和 `can` 跳过功能码白名单并保留原始字节，只表示提交发送，不承诺动作完成。未知响应不能被解释为成功。
- 正常停止／定时试验结束保留已确认使能，主动失能与故障另行处理。回零默认闲置状态不能判为本次回零完成。
- 队列运行状态由板端维护，浏览器断开不取消，重启板卡不恢复。页面轮询不得覆盖更新的提交／取消应答；跨电机设置的旧响应不得覆盖新表单。

## 驱动器限流与碰撞阈值（2026-09-22）

- 指令实验室需提供手册 p82 的 `45 66` 闭环最大相电流配置，不能仅按旧驱动类的方法清单判断手册能力是否已覆盖。
- `4C` 的碰撞检测电流是判定阈值，不是电流上限。界面应直接说明，不能只藏在提示气泡里。
- `45` 是所选电机的全局闭环相电流上限，影响回零及其它闭环运动；是否掉电保存由用户选择。输入框示例值不是设备读回值。
- 若电流上限等于碰撞检测阈值，严格“大于阈值”的条件可能无法成立；不得自动把两个字段绑定。尚未决定回零临时覆盖及恢复策略，不能默认实现或自动下发。
