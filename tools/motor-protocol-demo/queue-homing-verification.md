# 回零、停止使能与板端队列交付记录

日期：2026-09-22。实现由本机 Claude Code CLI、实际运行模型 `deepseek-flash[1m]`（api.deepseek.com）完成主体；Codex 负责协议合同、独立检查、集成修正、测试、打包和烧录。桌面页面，未投入手机布局。

## 交付

- 回零 9A 的六种模式接入板端监督。ACK 02 不当作完成；3B 默认闲置不当作完成；结合本次运行状态、完成应答和后续新鲜静止反馈。12/22 未运动单独报告。中断、超时、失败取消后续动作。
- 正常 FE 停止及试验计时结束保留已确认使能。取消在途使能后迟到 ACK 不能重新使能。主动失能和故障仍按各自语义处理。
- 板端 RAM 队列支持 enable、disable、move、home、torque、velocity、stop、wait、hex、can；可循环。网页提供可读文本、插入表单、语义预览、导入导出和板端进度。整段参数校验后才执行，不自动重试。
- rotation distance 按 ID 保存到 NVS，单位 mm/rev。mm 按实际保存值换算；deg/rev 不需要此配置。0 清除，保存失败不更改已确认值。
- 原始逻辑帧和实际 CAN 帧可以绕过功能码白名单，保留字节和 ID。只报告提交发送，不声称物理完成。队列与 HTTP、UART 互斥；有效 STOP 取消未来步骤，查询不抢走队列的反馈目标。
- 使用说明：`docs/motor-queue.md`。板端容量为64动作、8192字节程序、1–1000轮；速度等策略仍可在调试限制页修改。原始帧无运动监督。

## 已完成验证

- `npm test`：50项通过。
- 原生：MotionCore133检查、MotorControl892检查、CommandQueue307检查全部通过；原有 UART v1/v2、称重、BoardMotion 回归通过。
- 新增真实 UART endpoint + queue 集成测试：无效／错误 boot 的 STOP 无副作用，有效 STOP 取消，重复 STOP 不重新发送。
- 新增真实 `X42sProtocol.cpp` + 模拟 TWAI 边界测试：C6 分包与手算字节一致、原始标准／扩展 ID、数据原样、DLC 边界、发送记录和部分发送失败处理通过。不是只测另一个模拟协议实现。
- Playwright：最终 `qa-queue.mjs`、原有 `qa-device.mjs`（13项）、`qa-manual-v105.mjs` 通过；可配置限制流程也在本轮通过。队列检查覆盖单次提交、失败与未知结果、旧轮询、跨ID保存、取消、重载不启动和1280×800／1513×1039。API 为模拟，不是机构验收。
- `npm run build:device` 内嵌网页409690字节；已逐字节确认当前HTML包含在编译后的firmware.bin内。
- `tools/build-wsl.ps1 -Target device-controller` 成功，RAM89292字节，Flash1221817字节。编译过程自动运行上述原生和真实CAN驱动测试。

## 烧录结果

- 固件包：`out/releases/motion-queue-homing-20260922`。
- firmware.bin SHA256：`7cd4ab31e8f7b0fa0f45b5e999bf10ea4f1265ee6aef9ced2afcad51bc9678e8`。
- COM3，ESP32-S3，16MB Flash，MAC `3c:0f:02:c5:fe:64`，身份核验通过。
- 报告：`out/flash-runs/20260922T085152Z-746164e9/result.json`，`flash-verified`、`nvsUnchanged=true`、启动标志和HTTP就绪均已观察。
- 启动保留 HX711 DOUT47/SCK21 和已有校准，CAN 500kbit/s。新 motor-distance 命名空间首次不存在时使用未配置状态，不猜测行程值。
- 未发送使能或运动测试。当前电脑访问192.168.4.1的只读HTTP探测超时，因此未宣称实机网页API或机械运动已验收。烧录、原生验证、模拟浏览器验证与真实机构验证分开记录。

## 耗时与可避免返工

首个实现运行08:02 UTC，烧录完成08:52 UTC，约50分钟墙钟时间，前后端部分重叠。主要时间花在板端监督、队列时序和接口集成的复查／修复，页面布局和编译本身占比较小；没有为各项虚构精确百分比。

本次 Claude Code／DeepSeek 各轮日志没有网络重试或工具拒绝。实际问题主要是：测试用了CPP私有常量、poll参数漏传；前后端回零字段名漂移；完整表单指令比简写多一个单位参数导致计数错误；整数缩窄前校验遗漏；原生测试沿用旧反馈时序；大计划对象放在小型MCU栈上；旧HTTP响应覆盖新状态；重复实现UART校验时把小端boot误读为大端。最后一项由Codex改为在现有endpoint已经接受STOP之后，通过QueueBoardMotion取消队列，避免复制协议校验和去重逻辑。

改进：先冻结实际字段和完整表单输出样例，先编译一个完整动作及测试夹具，再扩展动作种类；明确状态归属，按所有写入口列出取消行为；检查嵌入式栈而非只看主机测试；至少一次测试真实传输实现；用延迟响应测试跨ID和提交竞争。

已更新个人 `deepseek-driven-dev/SKILL.md` 和 `references/assignment-and-checkpoints.md`，加入上述可复用检查。使用 PlatformIO Python 执行 skill-creator quick_validate，结果 `Skill is valid!`。系统默认Python缺少yaml，未为此改动全局依赖。

运行证据保留于本地忽略目录 `.deepseek-runs`，未提交凭据、原始运行日志或NVS备份。其余任务已有改动保留，本次未把整个混合工作区提交或推送。
