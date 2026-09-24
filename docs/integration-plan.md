# 固件仓库整合与协作计划

更新日期：2026-09-24（香港时间）。

本文供维护者和协作者安排本轮整合。第 1–3 节是检查时的状态快照；第 4–7 节是建议执行方案，不代表已合并、已验收或已分配负责人。开始工作前必须刷新远端状态，并在本文件更新负责人、提交 SHA 和验证结果。

## 1. 当前基线与本地成果

仓库：<https://github.com/SHKinsem/Babytech-Firmware>

- 远端 main：`d0435a71bdddcb92a234127fb790690f9d412349`。
- 检查机器的本地 main：`aae44462542ad0d8dfae31bb1b07b0aa6f8663c4`；比远端多 3 个提交、少 6 个提交。
- 本地独有提交：`39731a5`（文档整理）、`46e93dd`（控制板工程目录改名）、`aae4446`（命名和迁移文档）。
- 本地目录已改成 main-controller/device-controller；远端仍用 brain/motion，并新增了显示演示相关实现。
- 主工作目录有 15 个已跟踪文件修改、16 个未跟踪文件；暂存区为空、无 stash。内容混合了 OTA、电机反馈、文档、临时脚本与截图。
- 未发现正在进行的 merge/rebase、未合并索引项；所查源码没有冲突标记，git diff --check 通过。这不表示后续合并不会冲突。
- 本地未提交成果不是协作者 clone 仓库即可取得的内容。保全负责人需逐项核对并提交独有成果，不能只备份 tracked diff 而漏掉 untracked 文件。

检查机器有以下注册 worktree；这不是要求协作者采用相同的本地路径：

| worktree 目录名 | 分支 / 提交 | 状态与用途 |
| --- | --- | --- |
| Babytech-Firmware（主目录） | main / aae4446 | 不干净；先保全，不直接作为整合基线 |
| firmware-web-design | codex/firmware-web-design / 4685276 | 干净，对应 #5 |
| motor-command-feedback-pr | codex/motor-command-feedback / aca4446 | 干净但已过时；相对远端同名分支 ahead 4 / behind 7，禁止直接覆盖远端 |
| motor-feedback-clean-pr | codex/motor-command-feedback-clean / bffe467 | 干净，对应 #9 当前内容；本地没有 upstream |
| wifi-ota-pr | codex/wifi-ota / d49d507 | 干净，对应 #8 |

“干净”只表示没有未提交改动，不代表已验收或可以删除。

## 2. PR 与 issue 快照

| 项目 | 检查时状态 | 后续处理 |
| --- | --- | --- |
| [PR #1：同步组与查询预算](https://github.com/SHKinsem/Babytech-Firmware/pull/1) | Draft；GitHub 报冲突；head 49bc874 | 与 #7 作为同一条功能线整合，仍需实机验收 |
| [PR #2：显示演示规划](https://github.com/SHKinsem/Babytech-Firmware/pull/2) | 已合并 | 保留主干已有的显示演示功能 |
| [PR #5：网页工作区改进](https://github.com/SHKinsem/Babytech-Firmware/pull/5) | 非 Draft；可自动合并；head 4685276 | 先处理 P1 输入换算问题，再整合布局 |
| [PR #7：同步反馈修正](https://github.com/SHKinsem/Babytech-Firmware/pull/7) | Draft；GitHub 报冲突；head dafbf81 | 已包含 #1 基础实现，另有后续修正和瓶盖流程；最新组合未完成实机验收 |
| [PR #8：Wi-Fi OTA](https://github.com/SHKinsem/Babytech-Firmware/pull/8) | Draft；可自动合并；head d49d507 | 对应 #3；维护门禁需适配最终状态模型，台架验收未完成 |
| [PR #9：电机命令反馈](https://github.com/SHKinsem/Babytech-Firmware/pull/9) | 非 Draft；可自动合并；head bffe467 | 先处理 P2 温度符号问题；不等于解决 #6 |
| [Issue #6：统一控制入口与状态](https://github.com/SHKinsem/Babytech-Firmware/issues/6) | Open | 优先复现和修复跨入口状态不一致 |
| [Issue #3：双板 OTA](https://github.com/SHKinsem/Babytech-Firmware/issues/3) | Open | 软件实现与实板回退等验收分开记录 |
| [Issue #4：mDNS 发现](https://github.com/SHKinsem/Babytech-Firmware/issues/4) | Open；未见对应开放 PR | 建议留到本轮整合后独立开发 |

三个 issue 当时均没有 assignee、label 或 milestone。上表顺序不是已确认的人员分工。

### 合并前已知审查项

- **#5 / P1**：[审查意见](https://github.com/SHKinsem/Babytech-Firmware/pull/5#discussion_r4081863142)。`field-display.js` 对 `60.`、`+60`、`1e2` 等未匹配格式原样返回，下游数值转换可能接受它们并绕过 ×10 换算。例如显示 60 单位却发送 60 个协议计数（6 RPM 或 6°）。应明确拒绝或正确规范化，并添加从显示值到编码结果的回归。
- **#9 / P2**：[审查意见](https://github.com/SHKinsem/Babytech-Firmware/pull/9#discussion_r4084538700)。`0x43` 批量状态温度仍使用 `0x39` 的特殊符号规则；手册示例 `00 22` 应为 +34℃，现有写法会解释为 −34℃。应修正并验证正负温度。审查针对旧 head，但检查当前 bffe467 时该写法仍存在。
- **#6**：重点是编排使能与实验室状态分裂、wait-only 编排清状态、HTTP/UART 权限与停止状态不一致。应在整合后的代码重新复现，不能默认其他 PR 已经修好；完整验收以 issue 为准。

## 3. 已验证的合并风险与测试边界

使用 Git merge-tree 在临时对象目录模拟，未修改工作区或分支：

| 组合 | 结果 |
| --- | --- |
| 本地 main + origin/main | 12 条冲突：4 条内容冲突、8 条目录改名导致的新文件位置冲突 |
| #5 + #9 | motion/data/index.html、DeviceApp.jsx、CommandPanel.jsx 冲突 |
| #8 + #9 | motion/data/index.html 冲突 |
| #8 + #5 | motion/data/index.html、DeviceApp.jsx 冲突 |

本地主干内容冲突位于 README.md、device-controller/src/main.cpp、docs/motor-queue.md、main-controller/platformio.ini。位置冲突涉及远端新加的 demo_flow、DemoFlowConfig、DemoFlowController、DisplayLinkCore、UartPeer、DemoMotorExecutor 等文件。

这些结果针对上面的提交快照，不包含主目录未提交内容。#1/#7 的冲突状态来自 GitHub；本次未对它们做本地冲突逐文件模拟。各 PR 单独可自动合并，不代表彼此可以连续无冲突合并，也不代表逻辑兼容。

本次在主目录工作副本实际通过：

- 前端 npm test：84 项。
- Sites worker：4 项；OTA 页面：3 项；烧录工具单元测试：8 项。
- tools/test_protocol.py、tools/test_motion.py、tools/test_raw_can.py。
- git diff --check。

本次未重新执行 ESP32 完整编译、浏览器验收或实机测试。PR 描述中的历史验证结果不等于最终组合的验证结果。检查时 GitHub Actions 运行记录为 0、main 未受保护、仓库 rulesets 为空。

## 4. 推荐整合顺序

整合负责人从刷新后的远端 main 建立独立 `codex/` 分支和干净工作目录。不要从当前脏的本地 main 直接整合。每一阶段保持独立、可审阅的提交；阶段验证失败时不继续叠加无关改动。

1. **保全与去重**：核对本地未提交内容与 #8/#9，分别记录已在远端、仅在本地、可重建产物。保存本地独有的 3 个提交与所有独有文件；先不清理 worktree。
2. **同步功能线 #1 + #7**：建议以 #7 的最终功能为整合对象，保留 #1 的基础提交及后续修正历史，避免重复 cherry-pick。与最新 main 解决冲突，保留显示演示功能。维护者确认采用一个收敛 PR，还是继续两级 PR；确认替代关系前不关闭原 PR。
3. **状态一致性 #6**：在同步整合基线上复现 issue 场景，补正式跨入口回归，修复已确认的问题。保持独立提交，避免扩展成无边界的大重构。
4. **反馈 #9，然后界面 #5**：先修两条已知审查项，再把命令反馈整合进最终页面布局，保留显示演示、同步和现有调试入口。
5. **OTA #8**：基于最终的使能、忙、停止、任务所有权定义接入维护门禁。完成软件验证后仍单独记录未完成的台架验收；开发签名配置不能作为正式发布完成的证据。
6. **目录改名**：功能收敛后单独迁移 brain/motion → main-controller/device-controller。审阅原有改名提交后适配新增文件，不盲目重放；同步修复所有构建、打包、测试和文档路径。
7. **最终验收与清理**：验证最终提交，再通过 PR 更新主干。确认无独有成果后删除过时 worktree/分支，归档证据并补充 CI。

mDNS #4 建议在本轮之后独立开发；若提前开展，需明确基线并协调 WiFiSetup 的文件归属。

## 5. 协作者如何开展工作

### 认领与交接

所有负责人当前待认领。开始前在对应 PR/issue 或本表记录负责人、基线 SHA、开发分支和范围：

| 工作包 | 负责人 | 交付物 | 验收状态 |
| --- | --- | --- | --- |
| 本地成果保全与去重 | 待认领 | 文件清单、独有改动提交、备份位置（不含密钥内容） | 未开始 |
| #1/#7 同步整合 | 待认领 | 冲突已解决的功能分支、同步与显示回归记录 | 未开始 |
| #6 状态一致性 | 待认领 | 复现测试、最小修复、跨入口验收结果 | 未开始 |
| #9 反馈与 #5 界面 | 待认领 | 审查项修复、最终页面及浏览器验证 | 未开始 |
| #8 OTA | 待认领 | 维护门禁适配、双板台架记录 | 未开始 |
| 改名、最终组合与 CI | 待认领 | 路径迁移、双板构建、可复现检查入口 | 未开始 |

交接至少包含：基线/head SHA、修改范围、验证命令和结果、未验证项、依赖 PR、下一位接手者。整合负责人串行接入共享文件的改动；其他人可在独立分支提前修复局部问题，但须说明所基于的版本。

### 共享文件和行为边界

- DeviceApp.jsx、CommandPanel.jsx、控制板 main.cpp、MotorControl/CommandQueue 是容易重叠的文件，修改前协调归属，不用整文件覆盖解决冲突。
- 内嵌 `data/index.html` 从前端源码生成。先解决源码，再运行 `npm run build:device`；检查脚本指向当前阶段的正确工程目录，不手工拼接压缩 HTML。
- 普通编排保留直接发送语义：仅显式等待/持续时间/await 等要求等待；不偷偷补使能、重发或在发送结束追加停止。同步任务、显式 await 与普通直发分别验证。
- HTTP 提交、CAN TX、ACK、真实反馈、机械完成分开表达；缺少反馈不能伪造成功，广播不能伪造逐台确认。
- 不因整合丢弃远端已实现的显示演示流程、轮询修正、停止路径、配置和草稿行为。
- 不对共享主干或旧反馈分支强推，不执行硬重置/清目录来“变干净”；先确认独有成果已有可恢复副本。
- 测试通过不等于允许烧录或运行机械。实机验证单独安排负责人和设备条件，软件工作不自动执行硬件操作。

## 6. 验收检查单

以下清单尚未完成，执行者应填写对应提交和结果，而不是沿用旧 PR 的“通过”。

- [ ] 本地独有成果完成保全；#1/#7 的包含和替代关系已核对。
- [ ] #5 数值换算和 #9 温度符号修复有回归。
- [ ] #6 各入口真实状态、wait-only、忙时停止/失能、离线节点等场景完成验证。
- [ ] 同步、await、普通编排和显示演示在同一组合中正常工作。
- [ ] 在最终源码下运行 protocol、motion、raw CAN、前端和相关 OTA/烧录工具测试。
- [ ] 重新生成内嵌网页，并验证实际浏览器交互；至少覆盖 1513×1039、1280×800。
- [ ] 两块 ESP32 工程完整编译通过，记录工具链、提交 SHA 和产物对应关系。
- [ ] 改名后 build:device、WSL 构建、打包、测试、文档没有指向旧目录的失效入口；有意保留的兼容别名单独说明。
- [ ] 同步最终补丁和完整瓶盖流程完成受控实机验收，记录未覆盖边界。
- [ ] OTA 按 #3/#8 完成错板/签名/摘要拒绝、中断、首次启动失败回退、NVS 保留和更新后功能验证；区分开发与正式签名。
- [ ] 最终 diff 不包含临时截图、无关产物、私钥；源码与内嵌页面一致。
- [ ] CI 至少覆盖可在主机执行的测试，明确双板构建和合并检查策略。
- [ ] 主干更新后，协作者切到统一基线，原 PR/issue 状态按真实完成情况更新。

参考执行入口（使用各分支自带脚本，不假设所有分支具有相同测试集）：

```text
python tools/test_protocol.py
python tools/test_motion.py
python tools/test_raw_can.py
python -m unittest discover -s tests -p test_flash_device.py
node --test tests/ota-page.test.cjs
```

在 tools/motor-protocol-demo 内运行 npm test、npm run test:sites、npm run build:device；需要发布原型时再按该目录说明运行 npm run build。OTA 页面测试在接入 OTA 后才存在。改名前用 pio run -d brain / motion；改名后用 pio run -d main-controller / device-controller，或该版本 tools/build-wsl.ps1 的对应目标。这些命令不包含烧录。

## 7. 文件整理与后续维护

检查时 out/ 约 224 MB、.deepseek-runs/ 约 102 MB、两个 .pio/ 合计约 93 MB、node_modules/ 约 73 MB；主要缓存目录已被忽略。根目录仍有 .fix-polling.py、QA 截图等未跟踪文件，应核对后归档。

**out/ 内并非全是可重建缓存：本地 OTA 签名私钥也在其中。** 清理前单独安全备份签名材料，不将私钥、凭据写进 Git、PR 或共享日志。构建证据也应先确认保留需求。

当前 1,211 个已跟踪文件中，969 个来自 STM32 参考工程，包含部分编译产物。可另开整理任务明确参考源码与可重建产物的保留范围，不与本轮功能整合混在一起删除。

每次阶段完成，在下表添加记录，并更新前面的负责人和验收状态。快照过期时保留历史 SHA，新增当前基线，避免把旧测试结论套到新代码上。

| 日期 | 阶段 / PR | 基线与结果 SHA | 验证结果和未覆盖项 | 记录人 |
| --- | --- | --- | --- | --- |
| 2026-09-24 | 初始检查与整合建议 | 远端 d0435a7；本地 aae4446 + 未提交改动 | 见第 3 节；尚未执行整合 | Codex |

