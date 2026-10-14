# Brain 测试入口

描述对象：`main-controller` 的屏幕、网络与产品主循环测试。

Milestone：V1，`V1-device`，唯一运行协议 UART v4。从固件根目录构建 `pio run -d main-controller -e brain`。主机测试按修改范围选择：

```bash
python3 tools/test_brain_main.py --sanitize
python3 tools/test_brain_network.py --sanitize
python3 tools/test_brain_portal.py --sanitize
python3 tools/test_display_view.py
python3 tools/test_protocol.py --sanitize
```

`test_brain_main.py` 编译生产 setup/loop，SDK、屏幕、USB、网络和 NVS 为替身，不是实际 ESP32 或手机验收。需要 ArduinoJson 的 runner 使用 Motion 的已有依赖头文件，先构建 Motion 或按其参数提供头文件。

`test_protocol.py` 同时验证 v4 帧/会话/发送调度与保留的 v1/v2 codec；`test_demo.py` 包含旧显示 codec 和当前 Motion 网页流程回归，不把旧 codec 当成 Brain 当前运行协议。真实触摸、UART 电气、无线共存、Flash 与整机停止仍需单独验收，参见 [固件入口](../../README.md)。
