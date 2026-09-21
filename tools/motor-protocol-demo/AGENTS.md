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
