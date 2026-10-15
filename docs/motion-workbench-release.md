# Motion 工作台使用与交付

描述对象：Motion 内嵌 CAN 工作台、调试网络与离线交付包。

Milestone：V1，`V1-device`，成对 Brain/Motion UART v4。本文不提供旧固件入口，不把网页提交或主机测试当作机械完成。路径保留供 `tools/package-device.py` 生成交付包 README。

## 打开工作台

连接 Motion 的 `Babytech-Motion` 热点（开发密码 `babytech-demo`），打开 `http://192.168.4.1/`；已保存路由器 Wi-Fi 时，也可从同一局域网使用其 STA 地址。工作台不要求 Brain 配对或 Cloud 在线，不会在上电、加载草稿或浏览器重连时自动运动。

CAN TX GPIO4、RX GPIO5，500 kbit/s，须外接 3.3V 逻辑收发器、共地和正确终端电阻。UART GPIO43/44 为 115200；接线和 HTTP/HX711 接口见 [调试说明](motion-debug.md)。

## 能执行什么

| 入口 | 当前行为 |
| --- | --- |
| 常规试动、指令实验室 | 显式使能/失能、停止、CD 相对移动、非缓存 FB/CB 直通位置、9A 回零、速度/力矩试验、合法参数写入与只读查询 |
| 编排队列 | 按原字节直发，支持显式 `move/home await`、定时动作、raw hex/CAN、sync/helix；见 [队列说明](motor-queue.md) |
| 网页“屏幕流程” | 应用 JSON、初始化、单阶段及完整流程；这是工作台 owner，不是 Brain 产品命令入口，见 [流程调试](motion-display-demo.md) |
| 称重 | HX711 采样、GPIO 配置、去皮与砝码标定，不代表出粉闭环 |
| Wi-Fi 与 OTA | AP+STA 配网、独立 Motion 签名维护升级；Brain 不提供当前 OTA |

指令实验室的 FB/CB 仅监督 `sync=0`，使用 `0x33` 目标证明，不能用 `0x34` 实时设定值替代；FB 没有单条电流上限字段。FF/未知帧不是指令实验室的受监督动作，可从普通队列 raw 路径发送，但只证明提交，不证明动作完成或未知节点安全。同步组有独立的准备、触发和反馈监督，不能与 raw 广播混为一谈。

调试策略通过 `/api/limits` 显式保存到 NVS。120RPM、240RPM/s、5 秒等是首次默认值，不是不可调整的驱动上限；协议字段范围仍有效。试验时长 0 表示不定时停止，反馈失效和故障处理仍保留。定时结束/正常 Stop 保留使能，失能会释放保持力；不要要求每次试验结束都重新使能。F5/F6 缺少某些 C 系列限速/限流字段，板端不能替驱动器补出这些能力。

HTTP 202 只表示请求受理。真实完成仍需对应 ACK、目标/回零证明和新鲜停稳反馈；Stop 提交不等于物理停止。网页断线不自动重发 POST，也不取消已接受动作。软件清状态不抹掉独立 OTA 运动证据。

## 数据与网络

TX 表示进入 TWAI 发送路径，RX 是实际接收帧；环形记录会覆盖，不是完整总线抓包。未知数值显示未知，不把 ACK、旧缓存或采样缺口补成成功。

Wi-Fi 扫描、连接、忘记和重连遵守现有运动/停机及所有权检查。保存网络不要求重装产品身份；热点保持开启，但 AP+STA 共用无线电，切信道可能暂时断开网页。Stop 入口保留不意味着无线链路当时必然可达；物理急停不能由 HTTP 代替。密码不从状态接口返回。

## 重建与打包

只改固件时直接编译已提交的网页。改前端后在 `tools/motor-protocol-demo` 执行：

```bash
npm ci
npm test
npm run build:device
```

回到固件根目录构建 Motion：

```bash
pio run -d device-controller -e motion
```

不要手改生成的 `device-controller/data/index.html`。Windows 可用 `tools/build-wsl.ps1 -Target motion`，详见 [WSL 编译](wsl-build.md)。

`python3 tools/package-device.py --release RELEASE_ID` 打包已有 `out/wsl/motion` 产物，不自动编译或烧录。它核对内嵌 HTML 与镜像一致，复制镜像、构建信息及既有 QA 材料，并创建源码包和摘要；缺产物/QA 文件或目标目录已存在时停止。打包成功不证明 QA 材料属于新镜像，更不代表新的实机验收。发布前核对提交、配置、时间和镜像哈希。

此文在包中成为顶层 README；包里的 `source.zip` 保留 `docs/` 和源码，用于查阅本文链接。OTA 的双文件包不是串口四镜像包，不能互换。

## 烧录与数据保留

仅在明确授权、核对目标且动力隔离后执行上传。普通升级保留 Wi-Fi、标定、换算、配对和业务 NVS，不擦整片 Flash、不重新登记或配网。

| 分离镜像 | Flash 地址 |
| --- | --- |
| bootloader.bin | 0x0000 |
| partitions.bin | 0x8000 |
| boot_app0.bin | 0xE000 |
| firmware.bin | 0x10000 |

`firmware.elf` 仅用于调试；`firmware.bin` 不烧到 0 地址。新构建上传与既有包烧录应分开，具体预检、MAC、分区、备份和 NVS 校验见 [烧录流程](flash-workflow.md)。

## 验证边界

主机、浏览器 mock 和构建验证各自有测试入口，不把历史测试数量写成新版本通过。已有限定实机运动成功证据，见 [同步记录](motion-sync-development.md)及产品仓库操作/验收说明；不代表全部负载、方向、连续精度、停止时限、AP/STA 切换、传感器、Flash 或 OTA 回退已验收。当前测试资源不可用于实际喂养，温控和真实出料闭环未实现。
