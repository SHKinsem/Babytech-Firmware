# 参数草稿保留验收（2026-09-22）

- Claude Code / `deepseek-flash[1m]` 实现，Codex 审查、测试和打包。
- 会话：`4ae90a73-745c-456e-9347-4918ab551acf`；本轮记录：`.deepseek-runs/20260922-103249-76c748c8`。任务边界见 `draft-retention-brief.md`。
- 实机指令表单、编辑模式和未发送 HEX 按电机 ID / 指令 / 变体保存；常规试动按电机 ID 保存。同源 localStorage 恢复本机草稿，不恢复设备状态，不自动发送。
- 保留空字符串及未完成输入；未知版本、损坏或被拒绝的存储不会阻塞页面。存储被拒绝时只在当前页面内保留。不同浏览器或设备访问地址不共享草稿。
- 本轮未改 Wi-Fi、称重、调试限制和队列草稿的保存机制。

## 已执行

- `npm test`：67 项通过，含草稿隔离、变体、空输入、存储损坏、手动试动 round-trip。
- `npm run build:device`：成功，内嵌 HTML 426011 字节，SHA256 `424e033b373d56c2100d37bebc8389c03d0e4462ce35da44bbb187b438a820c0`。
- `node qa-drafts.mjs`：13 项通过。真实 Chromium + 模拟板端 HTTP，验证切换指令/变体/ID/标签、刷新恢复、原始 HEX、存储失败和真实限制校验，0 自动 POST、0 浏览器错误。
- `node qa-current-limit.mjs`：6 项通过，恢复草稿后的发送路径仍按手册编码且一次点击一次提交。
- 已检查 `qa/drafts-lab-1513.png`；本机预览 `http://127.0.0.1:4175/?device` 可访问。浏览器检查不等于实机验证。
- `./tools/build-wsl.ps1 -Target motion`：原生测试全部通过，Motion 控制器 1175 checks、队列 308 checks；固件构建成功。RAM 92460 / 327680，Flash 1243733 / 6553600。

交付目录：`out/releases/motion-drafts-20260922`。本轮没有烧录或发送硬件指令。固件包含构建时工作区现有代码，未丢弃其它任务的修改。
