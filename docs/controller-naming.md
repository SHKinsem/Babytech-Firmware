# 控制板命名与工程迁移

2026-09-23 起，工程按板卡职责命名：

| 原名称 | 工程目录 / PlatformIO 环境 | 中文名称 |
| --- | --- | --- |
| brain | main-controller | 主控板：界面、网络、业务协调 |
| motion | device-controller | 设备控制板：电机、传感器、执行状态 |

## 构建与产物

使用 `pio run -d main-controller` 或 `pio run -d device-controller`。WSL 脚本使用同名 `-Target`，旧 `brain` / `motion` 参数仍接受并映射到新工程；旧工程目录不保留。

新产物写到 `out/wsl/main-controller/` 和 `out/wsl/device-controller/`，工程内环境目录为 `.pio/build/<新名称>/`。旧 `out/wsl/brain/`、`out/wsl/motion/` 及旧 WSL 缓存可能仍存在，只是历史文件，不能用来判断新构建是否成功。构建镜像中旧目录可能因保留的 `.pio` 缓存出现 rsync 非空目录提示，不参与新工程编译。

设备网页仍通过 `tools/motor-protocol-demo` 中的 `npm run build:device` 生成，输出改为 `device-controller/data/index.html`。

已验证构建的打包入口改为：

```text
python tools/package-device.py --release 20260923-rc1
```

发布标识必须显式填写，输出为 `out/releases/device-controller-<标识>/` 及对应 zip；已存在同名目录时拒绝覆盖。脚本仍要求现有 QA 证据和嵌入网页与固件完全匹配，不执行编译、测试或烧录。旧 `package-motion.py` 不再提供，历史发布包不重命名。

## 兼容边界

- UART 报文、HTTP 接口及 JSON 字段（包括 `motionState`）、NVS 键和浏览器存储键保持原样。
- Wi-Fi 热点仍为 `Babytech-Debug` 与 `Babytech-Motion`，不用重新配置热点名称。
- `motion` 命名空间、`MotionCore`、`motionMode` 等是运动控制术语，继续使用。测试脚本及历史文档文件名中相同术语也不统一替换。
- 页面显示和板卡身份日志改为主控板 / 设备控制板；未改变运动逻辑、GPIO、分区或硬件型号。
- 历史开发记录中的旧名称指当时版本，不能机械套用其中的旧构建路径。当前操作以 README 和本页为准。
