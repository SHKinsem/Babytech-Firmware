# 控制板命名与兼容标识

本轮只统一工程目录，保留既有协议身份和构建产物名称。

| 用途 | 主控板 | 设备控制板 |
| --- | --- | --- |
| 当前项目目录 | main-controller | device-controller |
| 历史目录 | brain | motion |
| PlatformIO 环境名 | brain | motion |
| OTA manifest board ID | brain | motion |
| WSL 导出目录 | out/wsl/brain | out/wsl/motion |

原生构建使用 `pio run -d main-controller` 与 `pio run -d device-controller`；默认环境仍为 brain/motion。WSL `-Target` 同时接受新目录名与旧别名。直接读取原生构建产物时，路径为 `main-controller/.pio/build/brain` 和 `device-controller/.pio/build/motion`。

OTA 打包 `--board` 接受新目录名与旧 board ID，生成的签名 manifest 一律保留 brain/motion，以兼容已经安装的 bootstrap。目录改名不应改变板卡身份、版本、CAN/UART 编码、热点名或 NVS 键。原主目录曾使用 main-controller/device-controller 作为 OTA 身份的开发包属于另一身份配置；不能与本整合分支的包互换，首次部署前核对实板 `/api/ota/status`，必要时通过明确安排的 USB bootstrap 更新。

离线交付使用 `python tools/package-device.py --release <发布标识>`。`package-motion.py` 保留为同参数兼容入口；发布目录必须为新目录，脚本验证当前网页已包含在固件里，不覆盖既有交付包。工具本身不烧录。

显示演示源码、JSON 和新增同步模块都迁入 device-controller。代码中的 `motion::` 命名空间、brain UART 对端变量、协议常量保留原含义。历史验证记录中的旧提交 SHA 和 out/wsl 路径继续有效，不代表旧工程目录仍存在。
