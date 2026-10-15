# Motion 测试入口

描述对象：Motion 的生产主循环、CAN、队列和网页流程测试。

Milestone：V1，`V1-device`，UART v4。在固件根目录按修改范围选择：

```bash
python3 tools/test_motion_main.py --sanitize
python3 tools/test_motion.py
python3 tools/test_raw_can.py
python3 tools/test_demo.py
python3 tools/test_protocol.py --sanitize
```

相关 runner 需要 C++ 编译器及已有 ArduinoJson 头文件；构建入口为 `pio run -d device-controller -e motion`。

生产主循环测试替换 SDK/NVS/CAN I/O，不等于物理运动、断电或 Stop 时限验收。已有实机证据及当前操作边界见 [固件入口](../../README.md)。
