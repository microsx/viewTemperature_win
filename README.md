# viewTemp

一个轻量 Windows 悬浮窗，在屏幕右上角常驻置顶，显示 CPU / GPU 温度、
使用率、显存和系统内存。传感器数据来自
[MSI Afterburner 硬件监控](https://www.msi.com/Landing/afterburner) 的
共享内存块 —— viewTemp 本身不直接访问任何硬件。

## 功能

- CPU / GPU 温度、使用率、显存（已用 / 总量 / 百分比）、系统内存
- 阈值闪烁告警（每行 360 ms 心跳节奏，跨阈值触发）
- ini 驱动硬件库：启动时匹配 CPU / GPU 型号，自动选择合理温度阈值
- 窗口位置持久化、ini 设置热重载、置顶透明悬浮窗、悬停淡入交互
- 启动时按 Windows 用户界面语言自动识别 UI 文案（中 / 英已支持，新增语言请加 `LANG_IF`）

## 编译

需要 Visual Studio 2019 / 2022 附带 C++ 桌面工作负载和 Windows 10+ SDK。

```cmd
build.bat
```

产物：`viewTemp.exe`（单文件自包含二进制，无需额外 DLL）。

## 运行

先启动 **MSI Afterburner**（含 RivaTuner Statistics Server）—— viewTemp 从
其共享内存读传感器数据。未启动 Afterburner 时，只有系统内存和（只读）
显存总量字段会填充，温度和使用率显示 `--`。

## 配置

首次运行会在 exe 同目录生成 `viewTemp.ini`。包含窗口位置持久化、显示
开关和 `[threshold]` 段的 CPU / GPU 告警阈值。可自由编辑 —— 修改后约
1 秒内自动热重载，无需重启。

## 已知问题

见 [KNOWN_ISSUES.md](KNOWN_ISSUES.md)：运行依赖（必须开启 Afterburner）、
显存总量限制、有意为之的取舍。

## 协议

MIT —— 见 [LICENSE](LICENSE)。