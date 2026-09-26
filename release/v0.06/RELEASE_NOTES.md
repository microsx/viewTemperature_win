# viewTemp v0.06 — 文字阴影

**发布要点**：所有渲染文字增加 8 方向 1px 半透明黑色阴影。在浅色背景（HWiNFO 面板、Task Manager、白底窗口）下悬浮窗文字可读性显著改善。

## 主要变更

### 文字阴影

- `UpdateLayered` 增加 `drawWithShadow` lambda：每行文字先用 alpha=180/255 的黑色在 8 个相邻位置画一遍，再在中心位置画前景色文字。模拟 1px 软阴影，无需 GDI+ 真正的 blur API（性能/兼容性更好）。
- 阴影 alpha 固定 180/255——**不跟随 `fgAlpha`/`opacity` 滑块**。半透明阴影视觉上"看不见"，而全局透明度已经由 `bgAlpha` 控制，阴影只需要"够黑"。
- 所有 DrawString 调用全部走 `drawWithShadow`（CPU/GPU 名字、Temperature、Usage、VRAM、RAM），无遗漏。
- 警示色（温度超阈值的红闪）保留——`drawWithShadow` 接 `Brush*` 参数，前景色由 `tempBrush(g_flashXxxOn)` 决定，阴影仍然统一黑色。

## 实现细节

- 阴影实现在 GDI+ `Graphics` `MeasureString` 测量阶段之后、绘制阶段之前。测量不受影响（阴影画在原位 +1px 处不改变排版宽度）。
- `yy` 变量必须在 lambda 定义之前声明——C++ capture list 在定义时解析，不在调用时。把 `int yy = 6;` 提到 `drawWithShadow` 之上。
- lambda 闭包内 `Graphics g` / `Gdiplus::SolidBrush shadow` / `Gdiplus::Font font` 均按引用捕获，符合 `UpdateLayered` 单次绘制作用域。

## 已知限制

- 无——与 v0.04 已知限制一致（iGPU 温度/VRAM 总量无法拿）。

## 升级注意

- ini 完全兼容，零迁移成本。

## 文件清单

- `viewTemp.exe`（218112 bytes）
- zip 含 `v0.06/viewTemp.exe`
- SHA-256 校验和文件同目录

## 安装

- 解压 zip 到任意目录
- 运行 `viewTemp.exe`
- 依赖：MSI Afterburner + RivaTuner Statistics Server（读 MAHM 共享内存）。DXGI / WDDM 自带 Windows API 无需额外安装。