# 控制板命名与兼容标识

描述对象：当前工程目录与保留的构建/打包标识。Milestone：V1，`V1-device`，只构建成对 Brain/Motion UART v4。历史目录名是工具兼容别名，不是旧固件入口。

| 用途 | 主控板 | 设备控制板 |
| --- | --- | --- |
| 当前项目目录 | main-controller | device-controller |
| 历史目录 | brain | motion |
| PlatformIO 环境名 | brain | motion |
| OTA manifest board ID | brain（历史兼容标识，当前无 Brain OTA） | motion |
| WSL 导出目录 | out/wsl/brain | out/wsl/motion |

原生构建使用 `pio run -d main-controller` 与 `pio run -d device-controller`；默认环境仍为 brain/motion。WSL `-Target` 同时接受新目录名与旧别名。直接读取原生构建产物时，路径为 `main-controller/.pio/build/brain` 和 `device-controller/.pio/build/motion`。

OTA 打包器解析新目录名与旧 board ID 别名，但当前只有 Motion 的 `ota_identity.h` 和 OTA 服务；参数列表保留 `brain` 不表示当前 Brain 可打 OTA 包。Motion manifest 使用 `motion`，见 [OTA 说明](wifi-ota-implementation.md)。历史使用 main-controller/device-controller 作为 OTA 身份的开发包属于另一身份配置，不能互换；明确做 Motion OTA 时核对实板身份，不将该检查变成普通启动或云端更新的关卡。目录改名不改变配对、CAN/UART、热点或 NVS。

离线交付使用 `python tools/package-device.py --release <发布标识>`。`package-motion.py` 保留为同参数兼容入口；发布目录必须为新目录，脚本验证当前网页已包含在固件里，不覆盖既有交付包。工具本身不烧录。

显示演示源码、JSON 和新增同步模块都迁入 device-controller。代码中的 `motion::` 命名空间、brain UART 对端变量、协议常量保留原含义。历史验证记录中的旧提交 SHA 和 out/wsl 路径继续有效，不代表旧工程目录仍存在。
