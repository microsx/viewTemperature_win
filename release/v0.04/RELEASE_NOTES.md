# viewTemp v0.04 — GPU 集合化（多 GPU 支持）

**发布要点**：从独显直连 / 单 GPU 模型重写为多 GPU 集合模型。本子在混合模式（iGPU + dGPU）下，**两张卡都会显示**——每张卡独立 4 行（名字 / 温度 / 使用率 / VRAM）。

## 主要变更

### GPU 集合化重写

- viewTemp 不再写死读 `MAHM GPU entry 0`。启动时从 MAHM 共享内存动态枚举所有 GPU entry，按 `szGpuId[0] != 0` 过滤，**只保留 MAHM 实际填数据的卡**。
- 每张 GPU 独立渲染 4 行块（name / temp / usage / VRAM）。笔记本独显 + iGPU 双卡显示 8 行；单卡系统显示 4 行。
- 菜单每张 GPU 一个独立勾选项（ID = `1015 + i`），点击立即写入 `viewTemp.ini`。**关闭某张卡只影 响显示，不影响采集**。
- ini key 用 PCI 设备路径（`VEN_10DE&DEV_28A0` → `VEN_10DE_DEV_28A0`），**跨重启稳定**——BIOS 切换主显 / 增减卡时之前设置的显示开关不丢失。

### VRAM 总量从 DXGI 拿

- 老代码 `g_dxgiVramBytes`（单卡）改成 `g_dxgi[]`（按 VendorId 匹配 MAHM GPU entry）。
- 启动时枚举所有 DXGI adapter，按 `VEN_xxxx` 前缀匹配到对应 GPU 节点写入 `vramTotalGB`。
- **dGPU（NVIDIA RTX 4060 等）能拿到 7.77GB / 8.00GB 这种 OS 可见 VRAM 总量**——继续按 256MB WDDM 预留规则显示。
- **iGPU 拿不到**：DXGI 报 `DedicatedVideoMemory = 0`（Intel iGPU 共享系统 RAM）。VRAM 行退化为只显示 `12.3%` 百分比，无总量——这是有意为之，无公开 API 能拿到 iGPU 共享内存的硬上限。

### 已知限制（iGPU 温度）

- **MAHM 不给 Intel iGPU 填 `MAHM_SRC_GPU_TEMPERATURE`**——已在 WSL 端 `enumerate_gpus.exe` 测试程序验证（见 commit log）。
- iGPU 的 Temp 行显示 `--`；Usage 和 VRAM 百分比正常显示。

## 升级注意

- **旧 ini 完全兼容**——老 key `ShowCpu / ShowGpu / ShowUsage / ShowVram / ShowRam / TopMost / Transparent / FontSize / FgColor / Opacity` 全部保留行为。
- 老 ini 里没有的新 key（每张卡的 `ShowGpu_<PCI-path>=`）**默认 1（显示）**。
- 第一次启动会向 ini 写入 `ShowGpu_VEN_10DE_...=1` 和 `ShowGpu_VEN_8086_...=1` 之类的新 key——右键菜单取消勾选后立刻落盘，下次启动按 ini 决定。

## 文件清单

- `viewTemp.exe`（217088 bytes）
- SHA-256: `d6be257e9c10e69ca62ca76451ba3cde6f7d2849c35088d3ba46dc15c7c0e462`
- zip 含 `v0.04/viewTemp.exe`

## 安装

- 解压 zip 到任意目录
- 运行 `viewTemp.exe`
- 依赖：MSI Afterburner + RivaTuner Statistics Server（读 MAHM 共享内存）。DXGI / WDDM 自带 Windows API 无需额外安装。