# 统一电机控制入口与状态模型，修复编排/实验室使能不一致及板端过度门禁

目标仓库：SHKinsem/Babytech-Firmware

## 问题

通信、硬件反馈、命令确认、任务所有权与保护策略混在 MotorControl 中。编排为实现直接发送绕过了该路径，导致同一 CAN 动作经不同入口执行，产生不同的板端状态和操作权限。

## 已确认的入口差异

| 入口 | 接口 | 行为 |
| --- | --- | --- |
| 实验室使能 | /api/command | 调用 enable，登记请求，通过 ACK 和新的 3A 状态确认 |
| 独立使能 | /api/enable | 调用 enable，但外围门禁不同 |
| 编排 enable | /api/queue/start | 直接发送 F3，不建立确认事务 |
| 全部使能 | /api/enable-all | 广播发送，不逐台确认 |

相同地址、非同步单台使能的逻辑报文均为 `01 F3 AB 01 00 6B`。

已复现：编排 enable 后，即使注入 F3/02 和实际使能反馈，仍为 `driverEnabled=true`、`enabled=false`、`state=disabled`；切回实验室被网页门禁阻挡，直接调用 move 也返回 `not_enabled`。启动仅含 `wait 10` 的编排，同样清除此前实验室的使能确认。

原因：CommandQueue::start 调用 clearControlState 重置全部节点；编排发送不设置 enablePending，而 ACK 与 3A 更新确认状态依赖该事务。网页使用内部 enableConfirmed 对应的 enabled 作为运动许可。

审查基线：aae44462542ad0d8dfae31bb1b07b0aa6f8663c4。

相关位置：

- device-controller/src/CommandQueue.cpp：648、787、994 行。
- device-controller/src/MotorControl.cpp：308、363、523、750、1025、1658、1909 行。
- device-controller/src/BoardMotion.cpp：48 行。
- shared/BoardProtocol/src/BoardEndpoint.cpp：124 行。
- device-controller/src/main.cpp：901 行。
- tools/motor-protocol-demo/src/DeviceApp.jsx：248 行。

## 一并整改的问题

1. UART Exec 在识别操作前检查 Busy，连失能也拒绝；HTTP /api/enable 允许 UART 忙时失能，但实验室 /api/command 会拒绝。独立 STOP 仍可用。
2. 向不存在的 2 号地址发 stop，10 秒后仍阻塞 1 号使能。停止未知事实应保留，阻塞范围应按机械关联与任务明确限定。
3. MotorControl 静止阈值为 0.5 RPM，BoardMotion 为 0.2 RPM；0.3 RPM 反馈使底层清除停止等待，上层仍不接受。
4. anyStopPending 将 job.active 视为停止等待，导致正常运动被 Endpoint 上报为 Stopping。
5. move await 固定到位容差 ±0.1°；目标 90°、实际稳定 89.8°且反馈正常仍等待。容差应基于机构精度要求配置，不直接等于反馈分辨率。
6. 单条命令的 600 ms 反馈失效策略与四目标约 480 ms 的理想查询周期余量过小。结合查询预算与实测延迟校准，不凭静态审查指定新超时值。

## 职责调整与边界

- 通信层负责编码、收发和传输结果。
- 电机状态层按地址统一保存真实反馈、时间和有效性，不区分命令来源。
- 动作层提供共同的使能、移动、停止、失能发送与请求记录。
- 执行策略层决定顺序、等待、任务超时、保护及所有权。
- 网页与 UART 负责适配和展示，复用状态及操作语义。

真实使能、请求状态、ACK、反馈新鲜度和任务占用应分开表达。发送成功不能直接设置真实使能，广播不能伪造逐台确认。

保留编排直接发送语义：仅显式 await/wait/持续时间等待，不自动补使能、不自动重发、不在发送结束时追加停止。队列接管应处理原任务所有权，不能无差别清空硬件观测。显式清状态保持独立语义。

保留报文校验、旧反馈不证明新动作完成、ACK 不等于机械完成等必要约束。与 docs/motor-sync-plan.md 的反馈分层和查询预算方向协调。本 issue 不声称已解释“双电机 move 只有一台运动”的实机根因。

## 验收标准

- [ ] 同一使能经实验室、API、编排、UART 发送，报文与真实状态解释一致。
- [ ] 编排使能后有真实反馈，切回实验室无需仅为修复软件标记重复使能；反馈缺失明确为未知。
- [ ] wait-only 编排不丢弃有效硬件观测；任务接管和显式清状态职责明确。
- [ ] 广播、迟到 ACK、外部失能、反馈过期、多地址切换不制造虚假确认。
- [ ] HTTP/UART 停止与失能权限一致，抢占后的原任务结果正确。
- [ ] 正常运动报告 Running；停止判据统一；离线节点不造成无解释、无退出路径的全局 Busy。
- [ ] 到位容差和反馈失效策略有明确依据；不把缺少证据解释为成功。
- [ ] 加入跨入口回归测试，保留编排直发与停止幂等语义。
- [ ] 完成相关测试和固件编译；网页修改通过 build:device 生成内嵌页面；实机验证状态、页面切换和反馈。

## 验证记录

现有 `python tools/test_motion.py` 的 8 组主机测试全部通过。使用实际控制器/队列/板端类配合 fake CAN，临时程序复现了上述使能状态分裂、wait-only 清状态、手动 move 拒绝、忙时 UART 失能拒绝、离线停止阻塞、阈值冲突、正常运动 Stopping 和 await 容差等待。

临时复现程序未提交，需补为正式回归测试。本次未修改固件、未烧录、未进行实机验收。
