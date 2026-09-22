# FB / CB 接入验证（2026-09-22）

Claude Code 使用用户已配置的 api.deepseek.com / deepseek-flash[1m] 实现，Codex 核对手册、审查和执行验证。会话 4ae90a73-745c-456e-9347-4918ab551acf。

已接入指令实验室 FB 基础和 CB 限流，支持立即执行的 mode 0/1/2，保留原始功能码。mode 0 以驱动器 0x33 目标位置为基准，缺失时明确拒绝并启动只读刷新，不自动重发运动。mode 1 按实际行程而非绝对坐标幅值检查可调限制。正常完成保持使能。

同步缓存仍未接入运动监督。FB 不带单条电流限制，CB 电流范围遵循手册和已保存的限制。当前控制板用 int32 解码位置，±214748364.7° 是本实现边界，并非驱动器符号加 uint32 协议的完整边界。

验证结果：
- npm test：56 项通过。
- MotionCore：133 项；MotorControl：1122 项；CommandQueue：307 项，全部通过。UART、称重、队列停止联动回归通过。
- 实际 X42sProtocol.cpp 配合 fake TWAI：FB/CB 两包的 ID、字节及对应公开驱动函数均通过。
- Playwright qa-direct-position：8 项通过（mock HTTP），验证单次发送、FB/CB 精确字节、同步拦截、mode 0 提示和零位移。
- Playwright qa-device：13 项通过，含 Wi-Fi、称重、常规运动及 1513×1039 / 1280×800 桌面回归。
- Motion 固件构建通过；发布包见 out/releases/motion-direct-position-20260922，SHA256SUMS.json 校验文件。包内网页需与当前 motion/data/index.html 逐字节匹配。

本轮没有烧录，没有发送真实电机运动。浏览器用模拟 API 验证请求，不作为机械效果验证。之前 Wi-Fi / 主循环疑似阻塞的问题未在本次改动中修复。

审查修正：原任务描述将 0x34 实时设定值当作上一目标，复核手册 p70/p71 后纠正为 0x33，并补上 0x34 不可替代的测试；持续刷新运行中目标以取得不同反馈样本；补齐短报文长度检查；修正将双 CAN 分包误算为一次物理帧的测试和浏览器输入框定位器。保留工作区其它任务改动，没有混合提交。
