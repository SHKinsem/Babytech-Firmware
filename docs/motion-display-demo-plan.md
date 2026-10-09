# Motion / Display 旧演示退役说明

描述对象：旧 UART v3 DISPLAY 与 UART v2 BRAIN 演示计划的退役边界。当前 Milestone：V1 仅支持 Brain/Motion UART v4；本文不提供旧固件构建、烧录或验收步骤。

2026-10-09 旧演示运行分支和宏选择已移除；原计划从 Git 历史查阅。不可通过 Brain 宏 0、Motion peer 1/2 恢复旧发布入口。

保留的 Motion 网页流程、JSON、CAN 调试和 HTTP 操作见 [当前调试流程说明](motion-display-demo.md)，产品安装与 Brain/Motion 构建见 [README](../README.md)。共享 display model、协议类型、旧 codec 单测及持久导入保护不随演示退役删除；它们不代表旧固件仍可发布。
