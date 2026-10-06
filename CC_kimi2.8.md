# OptiScaler-MFG-Ada 交接文档（CC → Kimi）

> 日期：2026-09-28
> 仓库：`D:\CODE\HoyoDLSS5\OptiScaler-MFG-Ada`
> 当前分支：`feat/hsr-dx11-stability`
> 部署目标：`D:\APPS\HoYoShadeHub\OptiScaler\mfg-ada\mfg-ada-0.1.5\OptiScaler.dll`（根目录！启动器注入的是这个，不是内层，见 9.4）
> 测试游戏：崩坏：星穹铁道（`hkrpg_bilibili` / StarRail.exe，DX11）

---

## 0. 一句话现状

崩铁在 OptiScaler 的 **Dx11wDx12 swapchain wrapper**（D3D11 游戏桥到 D3D12 以支持 FG/NR）下严重卡顿。根因已定位：**wrapper 的 Present 桥路径本身有 ~35-50ms 纯开销，与用哪个 upscaler 无关（FSR/DLSS w/Dx12 都卡）**。已加 fast-path 跳过桥，但有两个遗留问题需修。本文件把所有上下文交给你接手。

---

## 1. 架构背景（必读）

### 1.1 Dx11wDx12SC 是什么
崩铁是 DX11 游戏。OptiScaler 要给它加 D3D12-only 的 FG（帧生成）和 FinishedPicture NR，于是在启动时用 `Dx11wDx12SC` 包装游戏的 IDXGISwapChain：

- 游戏看到的 swapchain 是 wrapper（D3D11 面）
- wrapper 内部维护一个真正的 D3D12 swapchain（`_fgSwapChain`）用于 FG/NR
- **wrapper 一旦在启动时套上就无法运行时拆除**（游戏持有指针）——这是所有问题的根源

关键文件：
- `OptiScaler/with_dx12/dx11_with_dx12_sc.cpp` — wrapper 实现，**Present 是性能主战场**
- `OptiScaler/with_dx12/dx11_with_dx12.cpp` — D3D11↔D3D12 互操作 helper
- `OptiScaler/upscalers/IFeature_Dx11wDx12.cpp` — upscaler evaluate 桥（DLSS/FSR 共用）
- `OptiScaler/upscalers/IFeature_Dx11wDx12.h:12` — `DX11WDX12_NUM_OF_BUFFERS = 2`

### 1.2 wrapper 何时被套上
profile `[FrameGen] Enabled = true` → 启动时套 wrapper。崩铁 profile：
`D:\APPS\HoYoShadeHub\OptiScaler\mfg-ada\mfg-ada-0.1.5\profiles\hkrpg_bilibili.ini`
关键配置：`FrameGen.Enabled=auto`（auto 在有 DLSSG 时解析为 true）、`AdaMfgUnlock=true`、`InterpolationCount=5`、`SoftPause=true`、`PausePresentGap=600`、`DlssNr.Enabled=auto`、`DlssNr.FinishedPicture=true`、`Dx11Upscaler=dlss_12`。

### 1.3 Present 桥完整路径
`dx11_with_dx12_sc.cpp` 的 `Dx11wDx12SC::Present`（约 294 行起）：
```
DXGI_PRESENT_TEST 检查 → (fast-path) → _InitInteropObjects
→ _GetDx11BackBufferIndexForPresent → _RequestSharedBackBuffer
→ _CopyDx11BackBufferToShared      // D3D11→shared
→ _WaitDx11ThenDx12                // fence wait
→ _CopyDx11SharedToDx12FGBackBuffer // shared→D3D12
→ _WaitForInteropCopyOnPresentQueue // fence wait
→ DlssNr::ApplyToFinishedPictureBridge  // NR（若有 owner）
→ MenuOverlayDx::Present
→ _real->Present(0)                // 隐藏的 D3D11 present，SyncInterval=0
→ _fgSwapChain->Present(SyncInterval)  // ★ D3D12 swapchain，真正的上屏，~36ms 主体
```

---

## 2. 已确认的根因

### 2.1 卡顿在桥本身，不在 upscaler evaluate
- 早期误判：以为只有 DLSS w/Dx12 卡（NGX evaluate 慢），FSR 不卡。
- **证伪**：用户实测 FSR w/Dx12 同样卡。
- 结论：卡顿是 wrapper Present 桥的 D3D11↔D3D12 互操作 + D3D12 `_fgSwapChain->Present` 的同步开销，纯跑就 35-50ms/帧，即使 FG/NR 都关、桥无消费者也照跑。

### 2.2 FG 关 + NR 关 = 应该不卡，但实际 20fps
因为 wrapper 套上后每帧无条件跑完整桥。**当 FG 和 NR 都没有消费者时，整条桥是纯开销**——这是 fast-path 要解决的。

### 2.3 NR 配置标志 vs 真实 owner
- `DlssNrEnabled` / `DlssNrFinishedPicture` 是 profile 静态标志，菜单关 NR 只动 `DlssNrEnabled`，但 NR owner 可能根本没创建。
- 日志铁证：`NR bridge: no active NR owner`（owner 不存在，NR 实际 no-op），但配置标志仍为 true。
- **判断 NR 是否真在跑，必须查 `activeNrOwner` 指针，不能用配置标志。**

### 2.4 DLAA(1:1) 比 DLSS-off 慢一倍
- 用户实测：游戏 DLSS 关 = 120fps 上限；DLAA(1:1) = 60-80fps。
- DLAA 是 1:1（render size == target size），NGX evaluate 仍每帧跑全模型（~8ms）。
- 已加 1:1 passthrough（copy color→output 跳过 NGX），**但 0 次触发**——见遗留问题 P1。

---

## 3. 已做的改动（当前 working tree）

### 3.1 `dx11_with_dx12_sc.cpp::Present` — fast-path（核心修复）
在 DXGI_PRESENT_TEST 之后、`_InitInteropObjects` 之前：
```cpp
const bool nrActive = DlssNr::HasActiveFinishedPictureOwner();
if (!Config::Instance()->FGEnabled.value_or_default() && !nrActive)
{
    MenuOverlayDx::Present(_real, SyncInterval, Flags, nullptr, nullptr, _handle, false);
    auto directResult = _real->Present(SyncInterval, Flags);
    // ... LOG_WARN on failure, LOG_DEBUG on success ...
    return directResult;
}
```
**演进史（避免你重蹈覆辙）**：
- v1 条件：`!FGEnabled && !DlssNrFinishedPicture` → 永不触发（FinishedPicture 是 profile 静态 true）
- v2 条件：`!FGEnabled && !(DlssNrEnabled && DlssNrFinishedPicture)` → 仍误判（DlssNrEnabled 配置 true 但 owner 不存在）
- v3（当前）：`!FGEnabled && !HasActiveFinishedPictureOwner()` → 用真实 owner

### 3.2 `dx11_with_dx12_sc.cpp::Present` — 桥逐步计时（诊断，未生效，见 P2）
每 300 帧打一条 `bridge breakdown: copyShared X waitDx11 X copyFG X waitInterop X NR X realPres X fgPres X TOTAL X`。

### 3.3 `IFeature_Dx11wDx12.cpp::Evaluate` — DLSS 1:1 passthrough（未生效，见 P1）
1:1 时 copy color→output 跳过 NGX evaluate。条件用 `GetUpscalerType()==DLSS && RenderWidth==TargetWidth && RenderHeight==TargetHeight`。
配套加了 `ratio diag` 诊断日志（每 300 帧）。

### 3.4 `DlssNrFeature_Dx12.h` + `DlssNr_Dx12.cpp` — 新增 `HasActiveFinishedPictureOwner()`
```cpp
// header
bool HasActiveFinishedPictureOwner();
// cpp（DlssNr namespace 内，访问匿名命名空间的 activeNrOwner/nrOwnersMutex）
bool HasActiveFinishedPictureOwner()
{
    std::lock_guard lock(nrOwnersMutex);
    return activeNrOwner != nullptr;
}
```

### 3.5 其他（原神相关，与崩铁无关，别动）
- `FSRFG_Dx12.cpp` / `.h`：原神 FSRFG 格式对齐（`_backBufferFormat`），未解决但与崩铁无关。
- 这些是上一轮原神工作残留，崩铁场景全部跳过。

---

## 4. 遗留问题（你的任务）

### P1【高优先】DLSS 1:1 passthrough 不触发
- 现象：日志 `Dispatch!!` 1835 次（evaluate 在跑），`1:1 passthrough` 0 次，`ratio diag` **0 次**。
- `ratio diag` 0 次很反常——它在 `Dispatch!!` 之前几行，如果 evaluate 路径在跑，ratio diag 必然该打。
- **怀疑**：spdlog 1.x bundled fmt 对多参数格式串（6+ `{}`）的编译期有问题，或 LOG_INFO 宏在这个 TU 有特殊行为。`Dispatch!!` 是 LOG_DEBUG 单参数能打，ratio diag 是 LOG_INFO 多参数打不出。
- **你需要做**：
  1. 确认 `LOG_INFO` / `LOG_DEBUG` 宏定义（在 `OptiScaler/Logger.h` 或 `OptiScaler/logging/`，本仓库 spdlog 1.x bundled fmt）。
  2. 先把 ratio diag 改成最简单的单参数 `LOG_INFO("ratio diag test {}", 1)` 验证是否是 fmt 参数数问题。
  3. 拿到 ratio diag 后，看 DLAA 下 `type/render/target` 实际值。passthrough 不触发可能因为：
     - `GetUpscalerType()` 不返回 `Upscaler::DLSS`（崩铁走 `dlss_12` 但可能 bridge 侧 upscaler 类型枚举不同）
     - `RenderWidth()/TargetWidth()` 在 DLAA 时不相等（崩铁内部可能 DLAA 仍按 DLSS quality ratio 报，或 target 是 swapchain 尺寸而非 render 尺寸）
     - `RenderWidth()/TargetWidth()` 返回 0（未初始化）
  4. 注意：DLAA 1:1 passthrough 跳过 NGX evaluate 后，DLAA 应从 60 → 接近 120。

### P2【中优先】bridge breakdown 诊断不触发
- 同样 0 条，和 ratio diag 一样的反常——进一步佐证 P1 的 fmt 多参数怀疑。
- breakdown 格式串有 8 个 `{}`。已简化但仍 0。
- 修好 P1 的 logging 问题后这个大概率自愈。这个数据有用：能定位 NR/FG 开着时桥的哪一步最贵（fence wait vs fgPresent vs NR）。

### P3【验证】fast-path 用真实 owner 后是否修好"关 NR/FG 20fps"
- 最新部署（04:34）用 `HasActiveFinishedPictureOwner()`，**尚未验证**。
- 用户报告"关 NR/FG 又变 20fps"是基于 v2（配置标志）的回归。v3 应修好，但需用户重测确认。
- 验证方法：日志搜 `fast-path direct Present`，应大量出现（>0）；Frametime 应从 ~40-64ms 掉回 <10ms（120fps 区间）。

### P4【低优先，已知】NR 开 + FG 关时桥仍跑
- NR 开（有 owner）时 fast-path 不触发是正确的（NR 需要桥）。
- 但此时桥 + NR = 64ms/帧（~15fps 真实帧，被 AdaMfgUnlock driver 层 MFG 插值帧拉高 fps 计数）。
- 这个场景的优化需要 P2 的 breakdown 数据定位哪步最贵。可能方向：
  - FG 关时 `_fgSwapChain->Present` 是否可跳（NR 改的是 D3D12 backbuffer，但若 FG 关，上屏能否走 `_real` 而非 `_fgSwapChain`？需查 NR 输出落到哪个 buffer）
  - fence wait 是否可异步化

### P5【低优先，原神】FSRFG 格式对齐
- `FSRFG_Dx12.cpp` 的 `_backBufferFormat` 对齐逻辑，WARN 日志从未出现（同样的 logging 谜题？）。
- 与崩铁无关，除非你想顺手用 P1 的发现一起修。

---

## 5. 编译/部署/测试流程

### 5.1 编译（必须用这个工具链）
```bash
# 仓库根目录 D:\CODE\HoyoDLSS5\OptiScaler-MFG-Ada
powershell -NoProfile -Command "& 'D:\VS2022BuildTools\MSBuild\Current\Bin\MSBuild.exe' OptiScaler.sln /p:Configuration=Release /p:Platform=x64 /t:OptiScaler /m:1 /nodeReuse:false /v:minimal"
```
- **必须用 powershell wrapper**，Git Bash 会吃掉 `/p:` 参数。
- **必须编 .sln 不是 .vcxproj**：vcxproj 的 IncludePath 用 `$(SolutionDir)`，直接编 vcxproj 时 SolutionDir 退化，会报 `nvsdk_ngx.h` 找不到。
- 产出：`x64/Release/a/OptiScaler.dll`（注意是 `a/` 子目录）

### 5.2 部署
```bash
cp -f x64/Release/a/OptiScaler.dll "D:/APPS/HoYoShadeHub/OptiScaler/mfg-ada/mfg-ada-0.1.5/OptiScaler.dll"
```
- **崩铁和原神共用同一个 dll**（启动器注入）。

### 5.3 日志
- 崩铁独立日志：`D:\APPS\HoYoShadeHub\OptiScaler\mfg-ada\mfg-ada-0.1.5\hsr_optiscaler.log`
- profile 已设 `[Log] LogFileName` 避免被原神覆盖。
- LogLevel=0（DEBUG 全开）。
- 常用 grep：
  ```bash
  LOG="D:/APPS/HoYoShadeHub/OptiScaler/mfg-ada/mfg-ada-0.1.5/hsr_optiscaler.log"
  grep -c "fast-path direct Present" "$LOG"   # fast-path 触发次数
  grep -c "_CopyDx11BackBufferToShared" "$LOG" # 完整桥次数
  grep -c "ratio diag" "$LOG"                  # passthrough 诊断
  grep -c "bridge breakdown" "$LOG"            # 桥计时
  grep -c "1:1 passthrough" "$LOG"             # passthrough 触发
  grep -c "Dispatch!!" "$LOG"                  # evaluate 跑了
  grep -c "!FGEnabled" "$LOG"                  # FG 关着
  grep "Frametime" "$LOG" | tail -20           # 帧时间
  ```

### 5.4 验证 dll 含你的字符串（排除没编进去）
```bash
grep -c "你的日志字符串" "D:/APPS/HoYoShadeHub/OptiScaler/mfg-ada/mfg-ada-0.1.5/OptiScaler/OptiScaler.dll"
```

---

## 6. 关键数据快照（2026-09-28 04:32 那轮）

- fast-path direct Present: **0**（v2 条件误判 NR 活跃）
- _CopyDx11BackBufferToShared: 7723（完整桥在跑）
- ratio diag: **0**（P1 谜题）
- bridge breakdown: **0**（P2 谜题）
- 1:1 passthrough: **0**
- Dispatch!!: 1835（evaluate 在跑）
- !FGEnabled: 3186（FG 确实关着）
- Frametime 分布（NR 开着的一轮）：<20ms 占 63%（AdaMfgUnlock 插值帧）、20-40ms 16%、>40ms 20%。真实游戏帧 ~64ms。
- 用户实测：DLSS-off=120fps、DLAA=60-80fps、开 NR+FG 卡、关 NR/FG 又 20fps（v2 回归，v3 待验）。

---

## 7. 注意事项 / 已踩的坑

1. **`DlssNrFinishedPicture` 是 profile 静态标志**，菜单关 NR 不动它。判断 NR 活跃用 `HasActiveFinishedPictureOwner()`。
2. **AdaMfgUnlock=true** 时 driver 层 DLSSG 独立于 OptiScaler 菜单运行——菜单关 FG 但 fps 仍高（插值帧），真实游戏帧看 Frametime 不是 fps 计数器。
3. **SoftPause**：Deactivate 不发 eOff，保留 NGX DLSSG feature。`!FGEnabled` 日志打不代表 driver 层 FG 真停。
4. **两个游戏共用 dll**：改崩铁路径前确认不影响原神。原神走 Genshin FSR Bridge sidecar 注入。
5. **spdlog 多参数日志可能有问题**（P1/P2）——这是当前最大的未解谜题，可能是 fmt 编译期参数数限制。先验证这个再改逻辑。
6. memory 文件在 `C:\Users\thx11\.claude\projects\D--CODE-HoyoDLSS5\memory\`，`hsr-dlss-wdx12-stall.md` 记录了完整根因演进（已更新为"FSR 也卡、桥本身开销"）。

---

## 8. 建议的接手顺序

1. **先解 P1 logging 谜题**（ratio diag / breakdown 为何 0 条）——这是所有诊断的前置。改单参数 `LOG_INFO("test {}", 1)` 验证。
2. 拿到 ratio diag，修 1:1 passthrough 条件，让 DLAA 跳过 NGX（60→120）。
3. 验证 P3：v3 fast-path 是否修好"关 NR/FG 20fps"（让用户重测，看 `fast-path direct Present` 出现 + Frametime 正常）。
4. P4：NR 开 + FG 关场景，用 breakdown 数据定位优化点。
5. P5 原神 FSRFG 顺手修（如果 P1 发现是通用 logging 问题）。

每个改动后按 5.1-5.4 流程编译部署，让用户重测拿日志。

---

## 9. Kimi 接手第一轮进展（2026-09-28 05:05）

### 9.1 P1 logging 谜题已破解——是 red herring
- 现有日志（04:29:53–04:32:33）用的是**旧 dll**；`ratio diag`/`bridge breakdown` 代码 04:33 才写入源文件、04:34 才首次部署，**从未被运行过**。
- INFO 日志一直正常：旧日志里 `[I]` 共 3487 条。`LOG_INFO` 多参数没问题，fmt 怀疑排除。
- 教训：查"为什么我的日志没打"前先确认产生日志的 binary 版本（对比源文件 mtime / dll 部署时间 / 日志时间范围）。

### 9.2 本轮代码改动（已编译部署，dll 05:05:35）
1. **`dx11_with_dx12_sc.cpp` breakdown 计时修复**：`tNr` 原在 NR 桥**之前**采样，导致 NR 耗时被算进 `realPres`、`NR` 恒 ~0。已移到 NR 桥之后；`tPrePresent` 移到 `_fgSwapChain->Present` 之前。现在 copyShared/waitDx11/copyFG/waitInterop/NR/ovReal/fgPres 归因正确。
2. **`IFeature_Dx11wDx12.cpp` passthrough 加锐化门控**：`!dx12Feature->SharpenEnabled()`——NGX 开锐化时 evaluate 会磨光，不能跳过。
3. **`ratio diag` 增加 sharpen 字段**，便于判断 passthrough 不触发是否因锐化。

### 9.3 待用户测试（按此顺序收集日志）
当前部署 dll（05:05:35）= v3 fast-path + 修正版 breakdown + 锐化门控 passthrough，**从未运行**。需要 3 轮测试：

**T1 验证 P3（fast-path）**：游戏内 DLSS 关、FG 关、NR 关 → 进游戏几分钟退出。
- 预期：`fast-path direct Present` 大量出现（>0，LOG_DEBUG 每帧一条）；`Frametime` 回 <10ms 区间；`_CopyDx11BackBufferToShared` 不再出现。
- 注意 AdaMfgUnlock：fps 计数器不可信，看 Frametime。

**T2 验证 P1（DLAA passthrough）**：DLSS 开 + DLAA 模式（1:1）、FG 关、NR 关 → 几分钟退出。
- 预期：`ratio diag` 每 300 帧一条（`type 0 sharpen 0 render X Y target X X` 同值）；`1:1 passthrough` 前 4 帧各一条；`Dispatch!!` 不再出现；fps 应回 120 档。

**T3 收集 P4 数据（NR 开 + FG 关）**：DLSS 开（任意档）、FG 关、NR 开 → 几分钟退出。
- 预期：fast-path 不触发（正确，NR 需要桥）；`bridge breakdown` 每 300 帧一条，看 NR/fgPres/waitDx11 哪项最贵。

每轮后 grep 方法同第 5.3 节；`bridge breakdown` 字段名现为 `ovReal`（原 realPres）。

### 9.4 重大发现：部署路径错了（05:14 修正）
- 启动器界面显示注入路径 = `...\mfg-ada-0.1.5\OptiScaler.dll`（**根目录**）。
- 内层 `OptiScaler\OptiScaler.dll` 是包依赖目录里的文件，**启动器从不加载**。
- 原文档部署路径写成内层是错误的：CC 近期所有构建（含 04:34 v3）**从未进过游戏**，04:29 日志跑的是根目录 1:52 的旧 dll（无 fast-path 代码，字符串验证 fast-path=0）。
- 已修正：新构建（05:05 编译）现部署在根目录 `OptiScaler.dll`，旧文件备份 `OptiScaler.dll.bak-prefastpath`。
- 以后部署一律用根目录路径；两个 dll 同时存在是正常现象（内层是依赖目录的一部分），不要删内层。
- 05:14 已同步更新本文档头部部署目标与 5.2 节命令。

### 9.5 fast-path 空指针崩溃修复（05:21 部署）
- 现象：新 dll 首次进游戏即崩溃（事件查看器：unknown 模块、偏移 0；日志最后一行 `MenuOverlayDx::Present`）。
- 根因：fast-path 调用 `MenuOverlayDx::Present(_real, ..., nullptr, _handle, false)` 把 pDevice 传了 nullptr，而该函数首行就 `pDevice->QueryInterface(...)` → 空指针调用必崩。
- 修复：`pDevice` 传 `_dx11Device`（构造函数持有、已 AddRef），overlay 走正常 D3D11 路径（与普通 DX11 游戏路径一致）。
- 已重新编译部署（根目录 dll 05:21:03），根目录与内层 dll 已同步为同一构建。
- 注意：MenuOverlayDx::Present 的签名是 `(swapchain, SyncInterval, Flags, pPresentParams, pDevice, hWnd, isUWP)`，第 5 参才是 device，别再把 nullptr 塞进去。

### 9.6 薄桥重写 v1（05:37 部署，待测）
架构决策：砍掉 fast-path 和 OptiScaler FG 在热路径的位置，保留最小薄桥。

关键事实（重写前代码阅读结论）：
- wrapper 创建时**真实 D3D11 swapchain 建在隐藏窗口**（DxgiFactory_Hooks.cpp `realDesc.OutputWindow = hiddenHwnd`），可见窗口挂 D3D12 `_fgSwapChain`。所以 fast-path present `_real` = 黑屏 → 这就是 05:22 "未响应"的根因（游戏可能因窗口不可见停止渲染）。**每帧拷贝不可省略**。
- 旧桥其实已基本是 GPU 异步（`_WaitDx11ThenDx12` = D3D11 signal + copyQueue GPU wait；`_WaitForInteropCopyOnPresentQueue` = present queue GPU wait），无 CPU fence 阻塞（除 allocator 复用等待）。
- 35-50ms 大头来自 OptiScaler DLSSG feature 本身（FGEnabled=true 时），不是拷贝。

重写内容（dx11_with_dx12_sc.cpp）：
1. **删除 fast-path**（ broken by design：对隐藏窗口 present）。
2. **NR 门控改为 `HasActiveFinishedPictureOwner()`**，且 FG feature 不在时 queue 回退 `_dx12CommandQueue`——NR FinishedPicture 在 OptiScaler FG 完全关闭时也工作（用户硬需求）。
3. `_WaitForInteropCopyOnPresentQueue` 同步加 FG null 回退。
4. breakdown 计时保留（NR 归因= waitInterop→tNr，ovReal= tNr→tPrePresent，fgPres= present）。

热路径现状（NR off 时）：拷贝对 + overlay(D3D12) + 隐藏 real present + 可见 D3D12 present。FG 由游戏原生 DLSSG / driver MFG 负责，OptiScaler 不参与。

待测（游戏内 FG 菜单保持关闭）：
- T1：DLSS 关/FG 关/NR 关 → 应正常显示不黑屏，帧率接近游戏原生（拷贝开销 ~1-2ms）。
- T3：NR 开 + FG 关 → `bridge breakdown` 每 300 帧一条，看 NR/fgPres 哪项贵；确认 `NR bridge: applied picture` 出现（NR 真在跑）。

### 9.7 薄桥验证通过 + 焦点暂停（05:46 部署）
验证数据（05:38-05:41 会话）：
- 稳态 breakdown：copyShared ~0.00 / waitDx11 0.02 / copyFG 0.07 / waitInterop 0.08 / NR 0.01 / ovReal 0.11 / fgPres ~1.0，**TOTAL 0.8~1.5ms**，采样间隔 2.7s ≈ 真实帧 111fps。薄桥达标。
- 首帧 TOTAL 32.98ms 是 NR 一次性资源创建（waitInterop 1.71 + fgPres 29.35），非稳态。
- `NR bridge: applied picture (epoch 5668)` 确认 FinishedPicture NR 真实在跑（用户硬需求已满足）。

用户反馈与处理：
1. "NR 15.43ms" = NR 模型的 GPU 执行成本（4060 在该分辨率下固有），不在桥内。NR 只随游戏 SR evaluate 运行（Dispatch/ProcessSeam 激活 owner），与 OptiScaler FG 无关。
2. "开启 NR 自动开帧生成"：菜单/热键均无 FG 耦合。真相：驱动层 MFG 随帧流自动激活拉高 fps 计数（AdaMfgUnlock 特性，dll 无法暂停）；NR 真正依赖的是**游戏内 DLSS 开着**（SR evaluate 提供 NR 输入）。**NR 单独测试法：游戏内 DLSS 开任意档 + Opti 菜单 FG 关 + NR 开**。
3. alt-tab 闪屏：05:46 构建加了 `GetForegroundWindow() != _handle` 时跳过 NR（看不见帧时跑模型纯烧 GPU 且返回时状态失步）。驱动层 MFG 不在我们控制范围。

### 9.8 NR 降分辨率优化（05:54，待测）
- 现状：NR on + 菜单 FG off + 游戏内 DLSS 开，真实帧 ~39fps（ReShade 基线 41），桥开销仅剩 ~2fps 当量。
- 05:50 会话 breakdown：稳态 TOTAL 2.0~2.4ms（NR 激活时 waitInterop ~0.8-1.0，fgPres ~1.0）。
- 大头是 NR 模型 GPU 15.43ms。**当时 WorkingScale 已是 0.5**（profile 里已有，满分辨率约需 ~60ms）。
- 已将 profile + 根 ini 的 `DlssNr.WorkingScale` 从 0.5 降到 **0.25**（代码硬下限，DlssNr_MenuInput 范围 25~200%）。预期 NR 15.43 → ~4-5ms，真实帧有望接近 55-60。
- 画质代价：模型在 1/4 分辨率运行，NR 效果变弱/变软。不满意可回 0.35 或 0.5 折中。菜单里也有此选项（Neural Rendering 面板）。
- 备注：日志 `ApplyFinishedColor ... decision 3 (pending 2, ready 0)` 是 finished-picture 队列采样状态，暂无需处理。

### 9.9 NR 界面独立 + 节点图重构（06:22 部署，待游戏内验证）
- 新独立窗口 "NR / Neural Rendering Center"：主菜单右列按钮开关（_showNrCenterWindow）。
- 布局：顶部 NR 主开关行（DlssNr::RenderCenterToggles）；左侧窄节点栏（railWidth=字号x9，竖排按钮）；右侧选中节点配置面板。
- 节点（CenterNode 枚举，DlssNr_MenuSections.h）：Upscale / FrameGen / NrInput / NrModel / NrBlend / NrPlacement / Status。
- 移动：Ada MFG unlock 整块抽成 MenuCommon::RenderAdaMfgUnlock -> 超分节点；RenderFrameGenerationRuntimeSettings -> FG 节点。主菜单不再渲染这两块。
- 删除：DlssNr::RenderMenu 与 PipelineUi::Draw 节点图弃用（PipelineUi.h 仅留 CheckboxWrapped/DrawTimingBar）。
- 关闭逻辑同步：HandleMenuShortcuts、Close 按钮、ToggleMenu 及主窗口 focus 检查均已加 _showNrCenterWindow。

### 9.10 四项功能需求的进展（06:50 中断交接，代码改了一半）

**背景**：9.9 的 NR 中心窗口已部署验证。用户提出 4 个需求：1) 多级 NR 级联（第一层 25% → 合并游戏帧 → 第二层 60%）；2) pass 数上限提至 5；3) 学习 renodx-dlss5.addon64；4) 学习其他分支 NR opt。研究已完成，实现进行中（见"已改/未改"）。

**需求 3 结论（renodx-dlss5.addon64 v8.5.0-rc10，strings 分析）**
- Hook NGX EvaluateFeature；设置存 ReShade.ini [RenoDX.DLSS5]。关键项：NRHookPoint(0=Upscaled/1=Render/2=Present)、NRPasses 1-4（Stack passes，串行堆叠，每 pass 独立 history=NRChainedHistory 默认 ON）、NRResolutionScale（工作分辨率缩放）、NRIntensity、NRPresentFrames（FG 下每游戏帧只跑第一个 present）、NRPresentPreFg（FG 前跑 NR）、NRCodecMode、分阶段 GPU 计时、"NR Faster"自动调优（自动降 HookPoint/ResolutionScale）。
- Present 模式对 finished frame 跑 NR、不依赖游戏开 DLSS；异步私有 D3D12 设备 + deep submission ring + watchdog。
- **注意：renodx 所有 pass 共用单一分辨率**；用户的 25%→60% 分层分辨率想法超越 renodx。

**需求 4 结论（git 分支调查）**
- 远端只有 origin/main 且已完全并入我们分支。有价值的 NR 提交全在本地 `archive/main-experiments`：
- 强耦合 finished-picture 修复链（需整体按序 cherry-pick）：9938746(arm SL present hook) → 4fcfbfd(FG pre-present compose) → ca46a32(TLS 守卫+slot 去重) → acd3555(slot 自身队列提交) → 0df86df(MFG 饱和防闪烁 slot 池) → efe8c94(池 6→4 省显存)。
- 独立可搬：176be47+0bf7325（NGX CreateFeature 失败退避 600→38400 帧，防拖垮游戏 upscaler）。
- 3cbb303（FG compose 门控修复+DWM forcer，体量大需评估）。
- 建议：`git cherry-pick 9938746 4fcfbfd ca46a32 acd3555 0df86df efe8c94 176be47 0bf7325` 逐个解冲突。

**多 pass 架构关键事实（读码结论）**
- `DlssNrFeature_Dx12.h`: MaxPassCount=30, DefaultMaxPassCount=3（需求2只需改 5 + 菜单放开）。
- 每 pass 独立 NGX feature/history（`nr.models[pass]`, Models.cpp Prepare 按 pass 建）；pass 间用 mode 8 ClampProxy 串联（纯逐像素 Load，不做重采样）。
- PassTuning/PassStyle/PassPreset（dlssnr/PassProfiles.h）：pass0 用主配置，pass1 用 Pass2* 配置，pass2 用 Pass3* 配置，pass3+ 用 DlssNrExtraPasses[pass-3]（已支持到 30 层，Config.h:319）。
- Run 循环在 `shaders/dlssnr/DlssNr_Dx12_Run.cpp`（pass 循环 ~line 340-375），所有 pass 目前共用单一 workWidth/workHeight（~line 146-151 从 DlssNrWorkingScale 算）。
- 资源：nr.output/passScratch/passClamp 在 PrepareRunModels 按 workWidth 分配（Models.cpp）；encode 写 colorCopy=全尺寸编码 proxy、gKeep=hdrCopy=未动副本；reduced 时 mode 2 Downsample 生成 colorSmall=modelInput。resolve 用 UV SampleLevel 读 gModel（尺寸无关，final pass 任意分辨率安全）。
- mode 8 shader 修改点：dlssnr.hlsl gMode==8 分支。

**已完成的改动（未编译！）**
1. `DlssNr_Common.h`: DlssNrConstants 尾部加 `float ClampMerge`（在 256 字节 padding 内，static_assert 仍过；Vk 共享头自动同步）。
2. `dlssnr.hlsl`: cbuffer 加 gClampMerge；mode 8 改为源尺寸≠目标尺寸时 UV 重采样 + 按 gClampMerge 混入 gOriginal（编码游戏帧）。
3. `Config.h/cpp`: 新增 DlssNrLaterPassScale（无默认=跟随主 scale）+ DlssNrPassMerge（默认 0.5），INI 键 LaterPassScale/PassMerge，读写已挂。

**原"未完成的改动"清单（已于 2026-09-28 07:06 全部完成，详见 9.11）**
1. `DlssNrFeature_Dx12.h`: DefaultMaxPassCount 3→5。✅
2. ModelState.h nr 结构加 laterWorkWidth/Height 字段；State.h PrepareRunModels 签名加 later extent。✅
3. Models.cpp：output/passScratch/passClamp 按 max(work, later) 分配；每 pass Prepare 用各自尺寸（pass0=work，passN=later）；resolutionChanged 纳入 later 尺寸。✅
4. Run.cpp：算 laterScale/laterWidth/Height；pass 循环内 per-pass 宽高 + per-pass mvToWork 因子；ClampProxy dispatch 用下一 pass 尺寸、InOriginal=nr.colorCopy、ClampMerge 设值。✅
5. 菜单：Input 节加"后续层分辨率"（DlssNrLaterPassScale）；Model 节加 PassMerge 滑条 + pass 上限 2→5 + Pass3-5 树（绑 ExtraPasses）。✅
6. 重编译 shader：跑 OptiScaler/shaders/precompile 下 `..\shader_tools\build_precompiled_shader.bat dlssnr`（需 python；会生成 DlssNr_Shader.cso/.h 和 Dx11 变体）；Vk spv 用 build_precompiled_shader_vk.bat（我们游戏走 Dx12 路径，Vk 可暂不重编但常量布局已兼容）。✅
7. 编译部署（坑#4/#5 流程），游戏内验证：DlssNr.WorkingScale=0.25 + LaterPassScale=0.6 + Passes=2，看日志 "DLSS-NR model passes: configured 2, effective 2" 和画质/帧数。✅（编译部署完成；游戏内验证待用户跑游戏）

**验证要点**：两级联期望 NR GPU 成本 ≈ 25% 层(~4ms) + 60% 层(~9ms?) < 单层 100%(~15.4ms) 或单层 25%（太糊）；画质应比单层 25% 明显好。

### 9.11 多级级联实现完成记录（2026-09-28 07:06 编译部署）

- **实际路径勘误**：所有 dlssnr 源码在 `OptiScaler/shaders/dlssnr/`（非 upscalers/dlssnr）；hlsl 与预编译产物在 `OptiScaler/shaders/dlssnr/precompile/`；shader 生成器在 `OptiScaler/shaders/shader_tools/build_precompiled_shader.bat`。
- **代码改动**（前 3 项为 9.10 已改，本次编译验证无误）：
  1. `DlssNr_Common.h` ClampMerge 常量、`dlssnr.hlsl` mode 8 重采样+混入、`Config.h/cpp` LaterPassScale/PassMerge —— 已核实。
  2. `OptiScaler/dlssnr/DlssNrFeature_Dx12.h:13` DefaultMaxPassCount=5（Vk 路径共用同常量，默认上限同步变为 5）。
  3. `DlssNr_Dx12_ModelState.h`：nr 加 laterWorkWidth/Height；CachedSurfaces 同步加字段；`DlssNr_Dx12_State.h` PrepareRunModels 签名加 `DlssNr::ColorExtent laterWork`。
  4. `DlssNr_Dx12_Models.cpp`：output/passScratch/passClamp 按 max(work,later) 分配（passWidth/passHeight）；Prepare 循环 per-pass 尺寸（pass0=work，passN=later，日志同步）；resolutionChanged 纳入 later 尺寸；`DlssNr_Dx12_Resources.cpp` 的 altSurfaces 缓存比较/拷贝纳入 later 尺寸。
  5. `DlssNr_Dx12_Run.cpp`：laterScale 缺省=workScale（均 clamp 0.25–2.0）；pass 循环 per-pass passW/passH 与 per-pass mvToWork 因子；ClampProxy dispatch 目标尺寸=下一 pass（=later 尺寸）、InOriginal=nr.colorCopy（全尺寸编码帧）、ClampMerge=clamp(PassMerge,0,1)；因 per-pass 常量不同，弃用原 clampSlots 描述符复用（每次取新 slot）。resolve 仍走 UV SampleLevel，尺寸无关，未动。
  6. 菜单：`DlssNr_MenuInput.cpp` Input 节加 "Separate later-pass resolution" 复选框 + "Later pass resolution" 25–200% 滑条（松手提交，NoDefault 语义=跟随主 scale）；`DlssNr_MenuModel.cpp` pass 上限 2→5（combo 显示实际数字）、加 "Pass merge" 0–1 滑条、新增 Pass 3 树（绑 DlssNrPass3* 旧键）与 Pass 4/5 树（绑 DlssNrExtraPasses[0/1]）。
- **Shader 重编**：必须用大写参数跑 `build_precompiled_shader.bat DlssNr`（生成符号 `DlssNr_cso`，与代码引用一致；小写 `dlssnr` 会生成 `dlssnr_cso` 导致 C2065）。Dx12 CSO + Dx11 变体已重编（Dx11 仅 warning X4000 原样存在）；Vk spv 未重编（常量布局兼容）。
- **编译**：`MSBuild OptiScaler.sln /p:Configuration=Release /p:Platform=x64 /t:OptiScaler` exit 0，0 error（29 条原有 C4250 warning）。产出 `x64/Release/a/OptiScaler.dll`（26,484,224 字节，07:06）。注意：第一次后台跑 MSBuild 曾假成功（exit 0 但未链接），重跑 /v:normal 才暴露 2 个 error（C7500 value_or_default 用于 NoDefault optional、上述 cso 符号大小写），均已修。
- **部署**：已 cp 到 `/d/APPS/HoYoShadeHub/OptiScaler/mfg-ada/mfg-ada-0.1.5/OptiScaler.dll`（时间戳 09-28 07:06）；内层 `OptiScaler/OptiScaler.dll`（06:30）与 ini/profiles 未动。
- **验证**：`grep -c LaterPassScale` 和 `grep -c PassMerge` 部署 dll 各=1；"Separate later-pass resolution"=1。
- **ini 现状（未改用户配置）**：根 ini `WorkingScale=0.5`、`Passes=2`、`UnlockPasses=auto`，无 LaterPassScale/PassMerge 键（→默认：跟随主 scale、merge=0.5）；profiles/hkrpg_bilibili.ini `WorkingScale=0.25`。若要跑 9.10 的 25%→60% 两级联验证，需用户自行加 `LaterPassScale=0.6`。
- **待办**：游戏内实测（日志 "DLSS-NR model passes: configured N, effective N"、画质/帧数对比）尚未做。

### 9.12 绝区零（ZZZ）链路测试首轮（2026-09-28 07:11 会话，进行中）

崩铁停服更新，改用绝区零（`D:\APPS\miHoYo Launcher\games\ZenlessZoneZero Game\ZenlessZoneZero.exe`，Unity，**D3D11 设备** + sl.interposer D3D12）验证同一 dll（07:06 构建）。日志 = 默认 `OptiScaler.log`（非 hsr_optiscaler.log），profile = `profiles/nap_cn.ini`（WorkingScale=0.5、Passes=2）。

**关键差异：ZZZ 没走 Dx11wDx12 wrapper 桥**。
- 日志无任何 Dx11wDx12/wrapper 创建记录，bridge breakdown / model passes / Dispatch!! 均 = 0。
- 走的是 OptiScaler Dx12 原生路径：`WrappedIDXGISwapChain4`、`FGHooks::FGPresent`、`Hudfix_Dx12::PresentEnd`。
- 原因：ZZZ 渲染经 sl.interposer 的 D3D12 设备（hkD3D12CreateDevice caller=sl.interposer.dll），OptiScaler 视其为 Dx12 游戏（Dx12Upscaler: dlss），不套 Dx11 wrapper。→ ZZZ 链路不等价于崩铁桥路径，不能用来验证桥/级联；只能验证 NR/FG 的 Dx12 通用链路 + 多级 pass 代码是否稳定。

**ZZZ 会话已确认**：
- 注入成功，MFG unlock 生效（5 插值帧 patch ×2 模块）；FG 在跑（FGPresent 大量，Frametime 尾部 11-13ms 含插值）。
- NR owner 生命周期出现过一次：`DlssNr_Dx12::Retire ... retaining retired GPU owner` → `reclaimed`（07:12:47）——owner 建过又被回收，NR 是否持续运行**待确认**。
- ExposureScan 正常（Adopt 4 候选 buffer；near-miss 为正常过滤）。
- **非致命错误**：游戏目录自带 `nvngx_dlssnr.dll` 加载失败（签名验证错误 0xBAD00000）——是 ZZZ 自己的 NGX 加载游戏目录 snippet 失败，不影响 OptiScaler 自己的 nvngx_dlssnr.dll。

**下一步**：
1. grep 最新日志 `ApplyFinishedColor|decision|model passes|effective` 确认 NR 是否持续跑；若 owner retire 后不再建，查 ZZZ 游戏内 DLSS 开关（NR 输入依赖游戏 SR evaluate）。
2. NR 确认在跑后，可让 nap_cn.ini 加 `LaterPassScale=0.6` 验证多级级联。
3. 崩铁桥路径只能等开服再验。

### 9.13 绝区零 08:40 日志复核（2026-09-28）

- 用户界面当前显示“等待超分器运行”。这不是 OptiScaler 注入失败：`OptiScaler.log` 在 08:40 仍持续出现 `NVSDK_NGX_D3D12_EvaluateFeature DLSS-NR route: handle 1000000, NGX feature 1, upscaler true`，说明 ZZZ 的超分评估入口确实被捕获。
- 同一时段 `FGHooks::FGPresent Present finished` 持续出现，MFG/FG 链路仍在工作。
- 08:40:53 出现 `DlssNr_Dx12::Retire ... (wasActive=true, kept active=true)`，随后游戏/Streamline 批量销毁 DLSS 资源；因此 NR owner 曾经建立并运行过，之后因游戏重建或关闭当前超分资源而进入等待状态。
- 目前没有证据表明需要修改 OptiScaler 代码来解决“等待超分器运行”。优先在绝区零图形设置中明确启用 DLSS/超分（不要使用关闭、原生或仅 TAA 的模式），应用后重启游戏，再观察是否重新出现持续的 NR owner/`ApplyFinishedColor` 日志。
- `profiles/nap_cn.ini` 已明确指定 `Dx12Upscaler = dlss`、`DlssNr/FinishedPicture = true`；无需先改 profile。游戏目录自带 `nvngx_dlssnr.dll` 的签名错误仍是 ZZZ 自身加载提示，不影响 OptiScaler 注入的 NGX DLL。

### 9.14 关键修复：ZZZ + DLSSG 下 Finished Picture 被错误取消（2026-09-28）

用户确认 ZZZ 实际为 `1280x800 -> 2560x1600 (2.0)`，且关闭“应用最终画面”时 NR 正常工作，因此“等待超分器运行”不是游戏超分关闭。

根因在 `OptiScaler/shaders/dlssnr/DlssNr_Dx12.cpp::DlssNr_Dx12::ApplyFinished`：原条件把 `externalFrameGeneration` 或 `activeFgOutput == FGOutput::DLSSG` 当成取消 finished-picture NR 的条件。ZZZ 正是原生 D3D12 + DLSSG/MFG 路径，所以每次 FG present 都会执行 `_state->late.Cancel()`，菜单自然一直显示等待最终画面。

已修改：native D3D12 `ApplyFinished` 不再因 DLSSG/MFG 取消；Dx11wDx12 bridge 的重复调用保护仍由 `ApplyToFinishedPicture` 自己保留。这样 ZZZ 的 DLSS 输出可以继续进入 finished-picture 合成。

已按本文 5.1 的专用工具链完成编译部署（2026-09-28 09:28:56）：
`D:\VS2022BuildTools\MSBuild\Current\Bin\MSBuild.exe OptiScaler.sln /p:Configuration=Release /p:Platform=x64 /t:OptiScaler /m:1 /nodeReuse:false /v:minimal`
构建成功，只有既有 warning；产出与部署根目录 DLL 均为 26,486,272 字节、时间戳 09:28:56。现在可以直接重启绝区零复测“应用最终画面”。重点观察 `FinishedPictureStatus` 是否从 waiting 变为 running，以及是否出现 `ApplyFinishedColor`。

### 9.15 ZZZ DLSSG 黑帧/闪烁修复（2026-09-28 09:49）

复核 09:32 日志发现，DLSSG 并非 MFG 解锁失败：`Activate` 已发生，但之后每次 `DLSSG_Dx12::Dispatch` 都报告 `Depth or Velocity is not ready`，`FGHooks` 因此走 `FG feature exists but is inactive/paused; pass-through present only`。同时 `StreamlineHooks::hkslSetTagForFrame` 持续记录 `Skipping the FG tagging of potential DLSS resources`。

原因是绝区零的 `IgnoreTagsWithoutHudlessForFG` quirk 会把“深度 + MV、暂时没有 HUDless”的资源当作 DLSS 专用资源而跳过；在当前 `FGInput=DLSSG`、`FGOutput=DLSSG` 配置下，这正是 DLSSG 需要捕获的资源，导致深度/MV 永远未就绪并表现为 FG off、闪烁/黑帧。

已修改 `OptiScaler/hooks/Streamline_Hooks.cpp`：当 `activeFgOutput == FGOutput::DLSSG` 时禁用该过滤，仅对 FSR/XeFG 保留 ZZZ workaround。这样 native DLSSG 能接收深度/MV，避免因资源未就绪而 pass-through。

已按专用 MSBuild 工具链编译成功并部署到启动器实际加载的根目录：

`D:\APPS\HoYoShadeHub\OptiScaler\mfg-ada\mfg-ada-0.1.5\OptiScaler.dll`

本次 DLL 时间戳为 2026-09-28 09:49:28，大小 26,486,272 字节。下一次复测应重点确认 `Depth or Velocity is not ready` 数量下降、出现 `SetOptions before/after` 与有效 DLSSG presents，同时继续观察 `ApplyFinishedColor` 和闪烁/黑帧。

### 9.16 回归保护：保留崩铁 DX11wDx12 DLSSG 6X 路径（2026-09-28 10:00）

为避免 9.15 的 ZZZ 修复影响星穹铁道 DX11→DX12 bridge，资源过滤条件已进一步收窄：只有 `swapchainInteropApi == None`（native D3D12）且 `FGOutput == DLSSG` 时才绕过 ZZZ 的 `IgnoreTagsWithoutHudlessForFG` 过滤。DX11wDx12 bridge 即使使用 DLSSG 输出，仍保持原过滤逻辑和资源路径，不改变现有 6 倍配置。

已重新编译并部署，实际加载 DLL 时间戳为 2026-09-28 10:00:01。此版本优先保证崩铁 DX11 DLSSG 6X 不受影响；ZZZ 仍需重启后观察 NR 命中率和 DLSSG 资源就绪情况。

### 9.17 回退到 0.1.3 的 ZZZ 工作配置（2026-09-28 10:22）

对比 `mfg-ada-0.1.3\profiles\nap_cn.ini` 与当前 0.1.5 配置后确认，旧版稳定运行 Finished Picture NR 的关键不是显式接管 DLSSG，而是：

```ini
Dx12Upscaler = auto
[FrameGen]
FGInput = auto
FGOutput = auto
```

当前 0.1.5 已将上述三项恢复为 `auto`；保留 `AdaMfgUnlock = true`、`DlssNr/FinishedPicture = true`、`WorkingScale = 0.5`、`Passes = 2`，并保留 0.1.5 的 `OptiDllPath`。原配置已备份为：

`D:\APPS\HoYoShadeHub\OptiScaler\mfg-ada\mfg-ada-0.1.5\profiles\nap_cn.ini.bak-before-auto-20260928`

本次仅变更 ZZZ profile，没有改动已部署的代码 DLL；因此 9.16 中针对星穹铁道 DX11wDx12 DLSSG 6X 的 native/bridge 隔离仍保持不变。需要完全退出绝区零后重新启动，确认日志回到 `OptiScaler FG false`，并持续出现 `DLSS-NR finished picture: Applying NR to the finished picture.`。

### 9.18 修复 HSR NR finished picture 隔帧闪烁（2026-09-28 22:12 部署）

**症状**：崩铁 DX11 bridge 下，FG+NR 时 1 秒闪一下；不开 FG 一直闪；2 倍闪得更明显；关 NR 帧数不变。

**日志证据**（hsr_optiscaler.log 21:33-21:36 会话，490 次 decision）：
- `decision 1`（应用 NR）与 `decision 3`（跳过，pending 2 ready 0）精确 1:1 交替（各 245 次）。
- 配置：`FrameGen.FGInput=Upscaler, FGOutput=DLSSG, DLSSG.InterpolationCount=5, SoftPause=true, DlssNr.FinishedPicture=true, WorkingScale=0.25, Passes=2`。
- bridge breakdown：NR CPU 侧仅 0.5-1ms；FG 开启时 fgPres 19-30ms（GPU 饱和）；FG 关闭时帧时间 28-29ms（游戏渲染瓶颈）。

**根因**：`FinishedInputReady` 对同 queue 直接放行、跨 queue 必须等 fence。HSR bridge 的 NR 输入准备（depth/MV/residual copy）在 producerQueue 上，present 时（realQueue）当前帧的 fence 必然未完成 → decision 3 跳过；下一帧上一帧的 fence 完成 → decision 1 应用。NR 版/原生版交替 = 闪烁。ZZZ native 路径 `same producer queue true` 所以不闪。

**修复**（`DlssNr_Dx12_FinishedCompose.cpp` + `DlssNr_Dx12_State.h`）：
1. NR 成功帧在 `cmd->Close()` 前把 finished picture 快照 copy 到持久 `fallbackFinished` 纹理（与 NR 同一 command list，realQueue 执行，零额外同步）。
2. decision 3（无 ready slot 且存在有效快照且 shape 匹配）时，用独立的 fallback command list 把上一帧 NR 结果 copy 回当前 backbuffer，画面始终是 NR 版（滞后 1 帧），不再交替。
3. fallback allocator 复用由 fallbackFence 保护（有界 spin 2000 次，正常零等待；GPU 积压时放弃 fallback 退回跳过）。
4. 分辨率/格式变化时按 desc 重建快照纹理；residualOnly（RunBeforeSR）路径不参与。

**安全性**：NR compose 本就在 DLSSG Dispatch 之前 in-place 修改 finished picture（现有行为），DLSSG 生成输入来自 Streamline tags 不受影响；fallback 只改 decision 3 帧的内容来源。

**未改动**：星穹铁道 6 倍 MFG 的 DLSSG 路径、ZZZ native 修复、Streamline quirk 隔离均保持。

**帧率说明**（非 bug）：原生 28-29ms 是游戏渲染瓶颈；6 倍 MFG 下 fgPres 20-30ms 是 4060 在 2560x1600 的 GPU 上限（游戏渲染+5 帧生成+NR）。关 NR 帧数不变是预期（NR CPU 仅 1ms，GPU 侧与渲染重叠）。

部署：根目录与内层 `OptiScaler.dll` 均为 2026-09-28 22:12:18，26,488,320 字节；旧版备份 `OptiScaler.dll.bak-noflicker`。

待验证：重启崩铁后 NR 全程启用（日志应出现一次 `replaying last finished frame while cross-queue input is in flight`，之后 decision 3 不再导致裸帧）；FG 开/关、2 倍/6 倍切换下画面无闪烁。

### 9.19 界面优化：NR 中心窗口状态跨菜单开关保留（2026-09-28 22:23 部署）

用户反馈：NR 中心（NR / Neural Rendering Center）在关闭 Opti 菜单再打开后丢失。

原因：`menu_common.cpp` 有三处在关闭菜单时重置 `_showNrCenterWindow = false`：快捷键切换分支（原 1543 行）、主窗口 Close 按钮（原 7672 行）、`HideMenu()`（原 8487 行）。`_showNrCenterWindow` 是 static 变量，只有这些显式重置才清零。

修改：上述三处不再重置 `_showNrCenterWindow`，保留 NR 中心窗口的显隐状态；用户点 NR 中心自己的关闭按钮（`RenderNrCenterWindow` 内 `!open` 分支）仍可关闭。渲染入口 `RenderMainMenuWindow` 在 `_isVisible=false` 时整段跳过，因此菜单隐藏期间 NR 中心不绘制，重开菜单时按保留的状态恢复显示。

部署：根目录与内层 `OptiScaler.dll` 均为 2026-09-28 22:23:53，26,488,320 字节。

### 9.20 修复 Later pass 双画面 + NR 菜单全面中文化（2026-09-28 22:53 部署）

**症状 1**：NR 中心开启"Separate later-pass resolution"并设置与 Model resolution 不同的值后，屏幕出现两个画面：左上角一个小画面 + 一个整屏大画面，两者没有叠加。

**根因**：多 pass 链的 rasters（nr.output/passScratch/passClamp）按 `max(work, laterWork)` 分配（`DlssNr_Dx12_Models.cpp` passWidth=std::max），但每个 pass 只写自己的工作区域。later != work 时：
- clamp（mode 8）用 `gSource.GetDimensions` 拿到的是分配尺寸而非内容尺寸 → 误判不需要 resample → `Load(id.xy)` 读到未初始化区域；
- resolve（mode 1）用 `gModel.SampleLevel(cmpUv)` 采样整个纹理 → 只占左上角的小 answer 被拉伸/错位。

**修复**（默认 later=work 时行为不变）：
1. `DlssNr_Common.h`：DlssNrConstants 的 padding 内新增 `SourceContentWidth/Height`、`ModelContentWidth/Height` 4 个 uint32（static_assert(256) 仍成立）。
2. `dlssnr.hlsl`（含 precompile 副本，已用 dxc cs_6_0 / fxc cs_5_0 重编译为 DlssNr_Shader.h 与 dlssnr_Shader_Dx11.h）：
   - cbuffer 新增 4 字段；
   - clamp（mode 8）：用 `gSourceContent*` 判断 resample，为 0 时回退 `GetDimensions`；
   - resolve（mode 1）：`gModelContent* != 纹理尺寸` 时把 cmpUv 按内容区域比例校正后再采样 gModel。
3. `DlssNr_Dx12_Run.cpp`：clamp pass 填 `SourceContentWidth/Height = passW/passH`；resolve 填 `ModelContentWidth/Height = effectivePasses>1 ? later : work`，superDown 与 enlarged 路径改为填全分辨率 width/height。
4. VK 路径常量填 0，走 GetDimensions 回退 = 旧行为，未改动。

**症状 2**：NR 中心大量选项无中文（用户语："这些选项没有中文"）。

**修复**：
1. `Localization.cpp` kChineseTable 新增 49 条（含 Model resolution / Separate later-pass resolution / Later pass resolution / Pass merge / Pass 1-5 / Intensity / Local structure / Local tone / Skin structure / Standard / Natural / Cinematic / Auto 等全部缺失的 Tr 与 HelpMarker 字符串）。
2. `DlssNr_MenuModel.cpp`：
   - `DeferredSlider` 内部对 label 调 `I18n::Tr`（rawLabel 保留给 Reset##id 与 HelpMarker 的 strcmp 分支）——一次覆盖 Intensity/Local structure/Local tone/Skin structure 全部滑条；
   - `InheritedProfileCombo` 同样翻译 label；
   - Style 下拉项数组改为逐帧翻译（Standard/Natural/Cinematic/Auto）；
   - TreeNodeEx 的 "Pass 1/2/3" 包 Tr；pass 4/5 的动态 label `Tr(nodeLabel)`（表内补 "Pass 4"/"Pass 5" 精确键）。

**未改动**：9.18 闪烁修复、9.19 窗口状态、HSR 6 倍 DLSSG quirk 隔离、ZZZ native 路径均保持。VK NR 路径行为不变。

部署：根目录与内层 `OptiScaler.dll` 均为 2026-09-28 22:52:35，26,493,952 字节。

待验证：
1. ZZZ 开 Separate later-pass resolution 并设 later < work（如 model 100% / later 50%）：画面单一且为 NR 版，无左上角小图。
2. 恢复 later=work：与 9.18 修复后表现一致。
3. NR 中心各选项悬浮提示与标签显示中文。

### 9.21 修复 Later pass 双画面（真正根因）+ 中文主窗口无限拉宽（2026-09-28 23:24 部署）

**问题 1：Later pass 双画面（9.20 未修好，用户复现"左上角小画面 + 全屏大画面"）**

9.20 只给 shader 传了内容区域常量，但漏改了实际采样数学。真正根因在 clamp（mode 8）重采样分支：

```hlsl
uv = (id.xy + 0.5) / float2(gWidth, gHeight);
raw = gSource.SampleLevel(gLinear, uv, 0);
```

`uv` 覆盖目标网格的 [0,1]，采样时却铺满**整个 gSource 纹理**。链内 raster 按 max(work, later) 分配，当上一 pass 答案只占左上角（如 model 50% + later 100%：答案 1280x800 位于 2560x1600 分配内），uv>0.5 的所有目标像素读到的都是未初始化区域——表现正是"左上角一个画面 + 全屏一个画面没叠上"。

修复（`dlssnr.hlsl` mode 8）：重采样时若 `SourceContentWidth/Height != GetDimensions`，把 srcUv 乘以内容/分配比例，只从内容区域采样。`gOriginal`（编码后游戏帧，独立全尺寸缓冲、无 padding）仍用未缩放的满帧 uv。shader 已用 dxc cs_6_0 / fxc cs_5_0 重编译（DlssNr_Shader.h 18444B、dlssnr_Shader_Dx11.h 32016B）。

**遗留说明**：superDown（仅 work>native 超采样探测路径）与 EnlargeMatchedResidual（仅 Transfer=2）也按整纹理读 answer，在"独立 later + 这两种路径叠加"的角落组合下理论上仍有同样问题；默认配置（Transfer=1、model≤100%）不触发，未处理。

**问题 2：中文主窗口打开后持续拉宽直到顶满屏幕宽度，改窗口缩放后再次拉宽**

仅中文界面出现。机理：`2c81f833` 引入的 `SetNextWindowSizeConstraints(min=46*fontSize*menuResScale, max=FLT_MAX)` 只设下限，中文标签短、内容天然窄于下限，min 生效后 stretch-same 的 main/plots 表格不断填满多出来的水平空间，auto-resize 每帧把内容宽度回写窗口尺寸，正反馈增长直至碰到屏幕边缘；改 MenuScale 触发 `SetNextWindowSize({1,1})` 后回路重新跑一遍，所以"再次拉宽"。

修复（`menu_common.cpp` RenderMainMenuWindow）：中文模式下把宽度**钉住**——`min.x == max.x == min(46*fontSize*menuResScale, DisplaySize.x*0.85)`，X 轴不再随内容变化（任何增长回路在 X 轴都不可能发生），Y 轴仍保持内容自适应。英文界面路径不变。

**未改动**：9.18 闪烁修复、9.19 窗口状态保留、HSR 6 倍 DLSSG quirk 隔离、ZZZ native 路径均保持。

部署：根目录与内层 `OptiScaler.dll` 均为 2026-09-28 23:24:07，26,493,952 字节。

待验证：
1. ZZZ Later pass 设 later > model（如 model 50% / later 100%）与 later < model 两种方向：画面单一、为 NR 版，无左上角小图。
2. 中文菜单反复开关、改 MenuScale：主窗口宽度稳定不再增长。


### 9.22 Later pass 常量布局错位修复（2026-09-29）

用户在崩铁 DX11 bridge 复现 model=25%、later=50% 的双画面；Model output (raw) 也显示左上角小图。
日志显示 clamp CPU 参数正确（sourceContent=640x400、sourceTex=1280x800、target=1280x800），但这不能证明 shader 读取正确。

本轮确认的代码缺陷：C++ DlssNrConstants 在 EnvironmentColour 与 ClampMerge 之间有四个残差字段（116..128 字节），主 dlssnr.hlsl 却遗漏了它们，导致 ClampMerge/SourceContent/ModelContent 提前 16 字节读取。此前 9.20/9.21 的尺寸/UV 修正因 ABI 错位无法按预期生效；9.21 中“真正根因/已修复”的结论过早，游戏效果当时并未通过。

修复：
- 主 shader 补齐四个保留字段，不改变 CPU 结构或旧字段偏移。
- 增加 C++ offsetof 静态断言；新增 tests/nr_shader_constants_layout_test.py 比较主/残差 shader 与 CPU 的字段顺序及类型。
- DX12/DX11 shader 重新编译并生成头文件。DXC 反射确认 ClampMerge=132、SourceContent=136/140、ModelContent=144/148。
- 删除 00:14 诊断版刷白 nr.output 的代码及 clamp 每帧日志，保留尺寸变化时的 chain 日志。
- 未修改 FinishedPicture、DX11 bridge、DLSSG/Streamline 或窗口相关逻辑；未重新生成 Vulkan blob，本轮运行验证范围为 DirectX。

刷白诊断版的“大画面黑白、小画面有色”不足以证明 FG 绑定错误：该实验修改活跃模型资源且没有隔离所有 GPU 读取依赖。此前据此推断 NGX 只写 work 区域/FG 拿错资源均不是确证。

验证：布局回归测试通过；DXC 反射偏移与静态断言一致；Release x64 构建成功。2026-09-29 00:26:19 DLL（26,497,536 字节）已同步到根目录/内层，两处 SHA256 均与构建产物一致：CC87DA1A76BB5B3955AC9A9B4E77547FF5B5A9B1FFEA7EE508BC2B6EAA1E495B。游戏视觉效果尚待重启后验证：NR 开启、model 25%、later 50%，先 Debug Off 看是否单一全屏彩色画面，再 Model output (raw) 看是否完整铺满。后续检查 later=25% 与 FG 6x，不能用编译成功代替游戏回归验证。


### 9.23 性能排查：明确关闭 FG 时结束软暂停运行态（2026-09-29 00:44 构建）

用户已确认 9.22 双画面修复有效；本次反馈为两层 NR、primary/later 均 50%，勾选独立 later 后约 40→20 FPS。不能拿较早 later=70% 的片段解释最后这段，也不能把 CPU 提交耗时当成 GPU 成本。

日志证据（hsr_optiscaler.log，00:33:01 结束的会话）：
- 00:31:58.152：native 2560x1600，work/later 均 1280x800，effective/completed 均 2。
- 00:32:05.784：NR GPU elapsed 17.09ms，model 16.57ms，周边 0.53ms；计时间隔可能包含其他 GPU 工作，不等于纯模型独占耗时。
- 00:31:52.841：用户关 FG 后 Deactivate 仍走 Soft pause，没有给 Streamline 发 eOff。
- 00:32:31/45：bridge fgPres 42.44/42.55ms。00:32:59.866508 原生 Present 已返回，00:32:59.907910 外层 FGPresent 才返回，约 41.4ms 在嵌套呈现调用里；当前 FGEnabled=false。
- 慢呈现在此前 FG 开启时也出现过，因此不能声称本次修改已经解释/修复全部 40→20，或证明独立 later 开关就是根因。

代码检查：独立 later 的有无只解析有效尺寸；50%/50% 和不勾选时两层 50% 进入相同资源及模型路径，不会因勾选额外增加模型 pass。暂时保留功能。

修复：
- SoftPause 仅在 FGEnabled=true 时保留运行时；用户明确关闭 FG 时发送 eOff，同时清零已接受的插帧计数/Reflex pacing。
- 记录软暂停遗留状态，覆盖“先临时暂停、再关 FG”的情况；eOff 失败保留待关闭状态以便重试，成功后不逐帧重复关闭。
- FG 开启时的临时暂停、2x/6x 倍率、Dispatch、DX11 资源交接、NR shader 和已修复的防闪烁逻辑均未改动。
- NR 菜单仅在提交滑条值/点击独立 later 开关时记录准确时间和两层比例，避免下次日志无法对齐 A/B 切换。

验证：Release x64 构建成功（已有继承/链接等警告及打包脚本的一条缺失路径提示；目标 DLL 已产出）；新增真实暂停策略的 16 组合及状态转换 C++ 测试通过；既有 Evaluate/External FG 模拟单测通过；NR shader 布局测试通过。这些不是游戏运行验证，6x 画面/帧率仍需实测。

部署：x64/Release/a/OptiScaler.dll，2026-09-29 00:44:29，26,502,656 字节，已同步 0.1.5 根目录和内层 OptiScaler/OptiScaler.dll；三处 SHA256：2693B9EA7C96B3B8665BF346069AB26984245808D59E63F0AA8210243369322F。两处旧 DLL 均备份为同路径加 .bak-before-perf-20260929-0044，未修改用户 ini。

待用户重启崩铁实测：固定场景、NR 两层且两个比例都是 50%、调试视图 Off，FG 关闭下切换独立 later，各保持约 15 秒；随后打开 2x/6x 检查恢复。真正关闭运行时后再开 FG 可能有一次模型重建停顿，这是与短暂软暂停不同的显式开关代价。尚不能承诺帧率已恢复。


### 9.24 FG 关闭时 NR 输入交接与帧时间优化（2026-09-29 01:05 构建）

用户反馈：FG 关闭时帧时间上下跳动，两层 50% NR 只有三十多帧，期望约 40。继续修复，不删除 later-pass。

最新会话证据（hsr_optiscaler.log，00:52:52 结束）：
- 00:51:44.890 已确认运行时 eOff；9.23 所见约 41ms 的关闭 FG 后呈现等待在抽样帧中不再出现。
- 两层均为 1280x800。00:51:48–54 later 关闭平均 38.27 FPS，00:51:55–57 开启 38.61，00:51:59–00:52:01 关闭 37.97，00:52:10–15 开启 36.97。不是固定场景受控基准，不能据此保证性能相等，但不能再把勾选 later 本身判定为 40→20 根因。
- 呈现间隔仍周期性出现约 30.643 / 29.791 / 11.203 / 30.234 / 28.356ms；约每五次呈现出现一次无完成输入和一次输入槽忙（不同帧）。旧防闪烁逻辑会重播 NR 快照，因此呈现 FPS 不等于新 NR 画面更新率。
- NR elapsed 约 22.5–23.8ms，区间可能包含并发 GPU 工作，不应当作纯模型独占成本，也不能从此保证稳定 40 FPS。

本次修复：
- 利用已有 GPU 顺序：NR producer → D3D12 Signal / DX11 Wait → 游戏输出与桥接复制 → present queue 等待复制 → NR compose。顺序已保证时，不再仅因 CPU fence 查询尚未完成而拒绝输入。
- 两阶段证明：在 Signal 前快照同 producer queue 已提交的槽，在 DX11 Wait 成功后才提交证明；与槽 ready 值绑定，Arm 时重置，失败同步、未来提交、复用后的槽不能继承旧证明。
- 仅在显式 ApplyFinishedBridge 入口、DX11wDx12、FG 关闭、无 external FG、非 residual 路径启用。其他入口、原生 DX12、FG 2x/6x 开启时继续原有 readiness 策略；未新增 CPU/GPU 等待。
- 保留原有 allocator 完成检查、防闪烁快照和 shader/model，不跳过 DLAA，不改 ini。
- 新增每约 300 次调用的 DLSS-NR schedule 日志，记录 fresh / replay / input-ring-full / GPU-ordered selections，供下轮判断真实更新率与槽饥饿是否改善。首个窗口结果计数覆盖前 299 次，后续为 300 次。

验证：最终 Release x64 编译退出码 0；桥接 handoff 单测（快照/提交、失败、复用、未来提交、设备移除、32 种开关组合）通过；真实 D3D12 WARP 队列测试通过（保留 native FG 依赖测试，新增 producer→interop→present 的受控延迟顺序测试）；DLSSG 暂停策略和 Evaluate 模拟单测通过；NR shader 常量布局测试通过；本轮代码 diff --check 通过。WARP 测试不等于真实 DX11 游戏验证，2x/6x 游戏回归仍待用户复测。

部署：确认 StarRail 未运行后，构建产物同步到 0.1.5 根目录与内层 OptiScaler/OptiScaler.dll。DLL 时间 2026-09-29 01:05:49，26,505,216 字节；三处 SHA256 一致：6CD62B8A2B7177FD8E3F09426A9853F69005BE6220FC186EC8AD0D793A240069。两处部署前 DLL 均保留，备份为各自完整路径加 .bak-before-nr-handoff-20260929-0110。

待验证：重启崩铁，固定场景、NR 两层且 primary/later 均 50%、调试 Off，FG 关闭保持约 20 秒，然后 2x、6x 各约 15 秒。对比帧间隔、fresh/replay、槽忙与视觉稳定性；不能只看 FPS，更不能以曲线平滑代替实际修复。增加新 NR 画面的处理比例可能增加 GPU 工作量，是否恢复 40 FPS 尚未实测。

### 9.25 模型页逐层开关、分辨率与输入融合（2026-09-29）

用户需求：把分辨率控制移到模型页，顶部开关独立 later-pass；每层独立启用/关闭，保留原逻辑层号（如三层中关闭第二层）；每层模型结果与自己的输入混合。用户另提供本地 renodx-dlss5.addon64 作为设计参考。

原链路：仅相邻层分辨率不同时，clamp 按全局 PassMerge 混入原始编码游戏帧，再作为下一层输入；同为 50% 时该控件不生效。不是逐层输出权重，也不是每层混入上一层的混合结果。

新链路（DX12，包括崩铁 DX11→DX12 桥接）：
- 模型页顶部“启用独立 Later-pass 分辨率”。关闭时每层跟随 Pass 1，仅 Pass 1 显示分辨率编辑；开启后各层显示独立分辨率。关闭总开关不清除各层保存值。
- 每层有“启用此 Pass”“本层分辨率”“本层模型融合比例”，支持现有界面的 1–5 层及高级 INI 的最多 30 层。
- 逐层公式：O_i = blend_i * NR_i(I_i) + (1-blend_i) * I_i。第一启用层输入为游戏编码代理；下一启用层接上一层混合结果（按本层尺寸重采样）。该混合发生在 NR 编码工作域，最终 HDR/强度/残差回写设置仍然生效，不应解释为最终屏幕上的线性亮度权重。
- 默认各层模型融合 100%，两层同为 50% 的原有全模型链路保持；0% 保留本层输入但仍计算模型，需要省模型耗时应关闭该层。
- 旧全局 PassMerge 保留在配置兼容字段中，但不再驱动新的逐层链路。分辨率不同的旧配置升级后不会再自动混入原始游戏帧；这是本次按用户要求替换的融合语义，不宣称自动视觉等价迁移。
- 三层关闭第二层：逻辑 Pass 1 → Pass 3；第三层保留自己的风格、强度、分辨率、融合比例。物理模型槽重新匹配逻辑层号，拓扑/尺寸改变时安全退役旧资源和历史，不把第二层历史当第三层使用。切换时可能短暂重建。
- 全部层关闭时 finished-picture 路径直接保持游戏画面，禁用旧 NR 快照回放；其他 Run 入口执行无模型的恒等路径。
- 100% 不新增混合 dispatch；单层且 100% 不新建混合 scratch。部分融合使用独立读写纹理，避免上一层输入 passClamp 的读写别名。
- 中间层最大尺寸也参与资源分配；末层回写使用实际最后完成层的有效尺寸。采样限制在有效内容边界，避免纹理填充区重新导致小图或脏边。
- 分辨率/融合滑条在释放时提交（整数百分比）；新增中文标签/提示及链路变更日志。原输入页不再重复放分辨率滑条。
- Vulkan 保留旧链路，新控件在该后端禁用并说明范围，未把 DX12 的实现冒充为 Vulkan 已支持。

持久化：Pass 1 沿用 WorkingScale；IndependentPassResolution 控制总开关；PassNEnabled、PassNBlend、PassNResolution（N>=2）保存逐层设置。未设置独立总开关时继承旧 LaterPassScale 是否存在，未设置某层尺寸时继承旧 LaterPassScale/WorkingScale。未直接改用户 ini。

参考插件：只读检查 D:/APPS/HoYoShadeHub/HoYoShade/reshade-shaders/Addons/renodx-dlss5.addon64（3,141,632 字节）的可读字符串，可确认“每层独立时序历史”“每增加一层执行一次额外 NR evaluate”的说明，含构建 PDB 路径 F:/renodx-worktrees/v85-rc2/build/Release/renodx-dlss5.pdb。没有获得对应源码，未加载执行、未改写插件、未声称从二进制确认内部融合公式；本次逐层公式是按用户需求独立实现。

性能背景：上次会话 01:14:16–25 约 33.3 FPS，连续窗口 fresh=300/replay=0/input-ring-full=0，已没有该片段的输入饥饿/旧画面重播；NR 计时间隔约 22–23ms（可能包含并发工作）。本次提供真实跳层及分层分辨率控制，不代表两层 50% 的模型成本已降到稳定 40 FPS。

验证：纯 C++ 链路规划测试通过（跳第二层、仅第三层、全关闭、尺寸极值、非法值、融合修改不改变资源布局）；生产 SM5 shader 的 12 组 D3D11 WARP GPU 测试通过（同分辨率 0/25/50/100% 混合、上下采样、带污染填充区的有效边界）；DX12/SM5 shader 均编译，常量布局回归通过；既有桥接 handoff、真实 D3D12 队列顺序及 DLSSG 暂停策略测试通过。本轮不改 DLSSG 倍率、Dispatch 或已修复的 FG-off GPU handoff 策略。尚未进行游戏画面与 6x 回归，不把软件 GPU 测试当作游戏验证。

构建/部署：Release x64 最终构建成功，DLL 时间 2026-09-29 01:41:17，26,514,944 字节。01:45:21 确认 StarRail 未运行后同步至 0.1.5 根目录及内层 OptiScaler/OptiScaler.dll，三处 SHA256 一致：0B70B1EE4FAE805A568934332B6F95C0499DAD7333AC2FB7A1BB57177BE03EBE。两处原 DLL 均备份为同路径加 .bak-before-per-pass-20260929-014521，未修改用户 ini。最终再次运行链路规划、12 组生产 shader WARP、bridge handoff、D3D12 队列顺序、DLSSG 暂停策略及常量布局测试，全部通过。生成的 shader 头文件保留生成器的行尾空格；排除此项后的 diff --check 通过。

待游戏复测：重启崩铁，在模型页设三层，分别配置 50/25/50%，关闭第二层应保留第一和第三层；测试同分辨率下每层融合 0/50/100%、第一层关闭、全部关闭；最后恢复两层 50%、100% 融合，对比 FG off/2x/6x 的帧时间、真实更新计数和视觉稳定性。

### 9.26 DLAA 的 SR-only 直通与 Esc 停止输入后的实时画面恢复（2026-09-29）

用户对照：FG 关闭、两层 50%，DLSS Performance 约 37 FPS，DLAA 约 32–33 FPS；Esc 菜单停用 DLSS 后 NR 仍覆盖旧画面。检查 hsr_optiscaler.log 02:02–02:04 会话：DLAA ratio diag 为 passthrough 0 / type 9 / sharpen 1 / render=target=2560x1600，实际 Sharpness=0。旧 DX11 桥外层直通被 sharpening 创建标志阻止；且该外层直通跳过整个 IFeature_Dx12::Evaluate，不能直接放宽条件，否则丢失 NR guide capture。02:03:10/12/15/17 的 fresh=0、replay=300、input-ring-full=0 与菜单停止 SR 生产后无限回放缓存吻合。

本次修改：
- NR 启用时不走旧桥外层直通，始终进入完整 DX12 管线。新增内部 SR-only 拷贝：仅 DX11→DX12 DLSS、finished-picture NR、非 deferred/非 RR/非 hold、有效输入与目标尺寸 1:1、锐化为零或可由 RCAS 接管时适用；保留深度/运动矢量采集、NR、RCAS 和输出管线。纹理格式/尺寸/子区域不支持安全拷贝则仍运行 NGX。旧非 NR 行为不变。
- 拷贝限定有效区域，恢复共享纹理 COMMON / 中间纹理 UAV 状态。退出直通恢复 DLSS 时置一次 Reset，避免沿用未更新的 SR 历史。切换日志为 NR DLAA SR-only bypass: true/false。
- 新增 producer activity 策略：真实输入尝试续期，包括 ring busy；不以 GPU 完成速度判定生产停止。DX11 真实游戏交接允许一个缺输入帧，连续第二个缺输入帧停止旧图回放；其他路径使用 500ms 超时。生成帧不累计缺输入帧数，新输入即使慢于 500ms 也先作为有效活动处理。
- 输入停止时取消待呈现旧结果、失效缓存但保留 GPU fence/resource 安全约束，放行实时游戏画面；恢复输入重置 NR 历史。明确的残差 hold 不误当作停产。新增中文暂停状态。未修改 DLSSG 倍率、Dispatch 或 FG-off 的 GPU 交接顺序。

性能/画质边界：跳过 DLAA/SR 模型并不降低游戏原生渲染分辨率；原生 2560x1600 仍比 Performance 的 1280x800 有更高的游戏渲染工作量。直通输入也不再获得 DLAA 模型的抗锯齿处理。不能据此承诺达到 40 FPS 或必然快于 Performance；需要游戏日志确认 bypass=true 与实测。

验证：新增 activity 测试覆盖 Esc、恢复、单帧间断、busy ring、每游戏帧六个生成回调、native 超时和 cancel；新增直通测试覆盖 128 组范围条件及 1:1/锐化/格式/子区域约束，均通过。7 组真实 D3D12 WARP 有效区域拷贝测试（含 COMMON 来源，debug layer on）通过；12 组生产混合 shader WARP、pipeline setup、bridge handoff、finished queue、DLSSG pause/evaluate、pass chain 与 shader constants layout 回归通过。排除已有生成 shader 行尾空格后的 diff --check 通过。软件 GPU/策略测试不等于崩铁实机或 6x 画面验证。

构建/部署：Release x64 成功，DLL 2026-09-29 02:21:16，26,519,040 字节。02:22:58 确认 StarRail 未运行后部署至 D:/APPS/HoYoShadeHub/OptiScaler/mfg-ada/mfg-ada-0.1.5/OptiScaler.dll 及其内层 OptiScaler/OptiScaler.dll；两处原文件备份后缀 .bak-before-dlaa-menu-20260929-022258。构建源与两处部署 SHA256 均为 731F59C0251181930F743C6862109598FB9DFC440407C9EFABE7ED5A7829C171。未改用户 ini。

待复测：重启崩铁，FG off + 两层 50% + DLAA，确认日志 bypass=true、比较帧率与边缘画质；打开 Esc 应回到可动菜单，退出后 NR 恢复；切回 DLSS Performance，再检查 2x/6x，无冻结/黑帧/周期闪烁。


### 9.27 原生 DLSS + FG companion 独立构建及部署（2026-09-29 03:33）

用户授权：仅帧生成版直接部署到现有 0.1.5；不再需要 W12 超分，专注 DLSS，并兼容绝区零 DX12。此版保留原生 DLSS SR 和帧生成输入接入，不是完全不拦截 SR 的代理。

实现：
- 新增 /p:OptiScalerFgOnly=true 构建属性（OPTISCALER_FG_ONLY）。正常构建默认不启用该策略；旧 NR/皮肤代码仍留在工作区，本变体不启用，并非已物理移除所有相关对象的最小 DLL。
- 强制原生 DX11/DX12 DLSS，不选择 W12/FSR/XeSS 后备。崩铁仍保留 DX11→DX12 的 FG 桥，绝区零走原生 DX12 DLSS/FG。未调整原有 FG 倍率与时序。
- 运行时 volatile 覆盖禁用 Opt NR、finished-picture、scan meter、额外 RCAS/对比度/运动锐化/放大镜及分辨率覆盖；保留游戏原生 DLSS 锐化。部署不改 ini，策略不持久写回旧版配置。
- 禁用 Opt NR 的 Streamline Present 包装和 NGX 诊断回调安装，减少外部 renodx-dlss5 插件接入干扰；隐藏 NR 菜单/快捷键与非本版后端选项。
- 菜单提供“重建 DLSS 输入（尝试恢复外部 NR）”，通过现有 render-thread 重建入口执行。只作为手动恢复尝试；可能短暂停顿，尚未证实能解决切出切回失效。

编译：VS2022BuildTools MSBuild，OptiScaler.sln /p:Configuration=Release /p:Platform=x64 /p:OptiScalerFgOnly=true /t:OptiScaler /m:1 /nodeReuse:false。最终构建退出码 0；仍有既有 C4819/C4250/C4744/LNK4098 警告，不能视为无警告构建。

验证：FG-only/正常版策略两种编译测试、DLSSG pause 策略、Evaluate 参数模拟、NR bridge handoff、真实 D3D12 WARP finished queue、swapchain present 重入测试通过；实现路径 diff --check 通过。配置测试验证 volatile 写入调用和 mock 中原值保留，不等于真实 ini 序列化集成测试。已检查产物包含 FG-only companion v1 标识。上述不代替游戏实测。

部署：确认 StarRail/ZenlessZoneZero/GenshinImpact 未运行后，于 03:33:10 将 x64/Release/a/OptiScaler.dll 同步到 D:/APPS/HoYoShadeHub/OptiScaler/mfg-ada/mfg-ada-0.1.5/OptiScaler.dll 及其内层 OptiScaler/OptiScaler.dll。产物时间 03:30:33，26,217,984 字节；源及两个目标 SHA256 均为 1970F4FEA0C5095995052C1B729E2A383E943BBDC51356633CB921A73D6C16F1。两处旧 DLL 均保留在各自完整路径加 .bak-before-fg-only-20260929-033310。未修改部署配置、依赖或外部 addon。回退需退出游戏，再将各自备份复制回对应 DLL；正常完整版重新编译应使用 false 并重建，避免误用本次产物。

待实测：崩铁 DLSS Performance + 外部 NR，FG off/2x/6x、Esc、切出切回；绝区零 DX12 同样检查 DLSS/外部 NR/FG、切画质和切出切回。旧日志显示外部 NR 仍推理但 pre-FG 计数停止，具体接入断点尚未最终确认。本轮减少干扰并提供恢复入口，不能保证已解决 alt-tab 或已达到完美兼容。


### 9.28 FG-only 呈现模式 NR 捕获顺序与缺失 FG 菜单（2026-09-29 03:50）

用户反馈：崩铁外部 NR 持续丢失，Opt 重建 DLSS 无效；固定使用外部插件“呈现”模式，不允许修改。另指出 FG-only 缺帧生成选项，授权开始修改。

证据：03:35–03:39 会话中 ReShade 明确记录两个 live runtime、因此冻结 HUD/快捷键/停靠状态；overlay_frozen 不能直接解释为图像冻结。外部 NR 评估与桥接提交持续增长但 prefg=0。Opt DX11 桥原顺序为先复制 DX11 图像到 DX12，再调用隐藏 DX11 Present，可能错过外部插件在 Present 中写回的 NR 图像。具体游戏内像素写回仍待本版复测。

修改：
- 仅在 FG-only + DX11 桥创建时，查询 ReShade 6.8.0 的 IID_UnwrappedObject（7F2C9A11-3B4E-4D6A-812F-5E9CD37A1B42）并安装 native Present 边界捕获。核对官方 v6.8.0 source/dxgi/dxgi_swapchain.cpp 的 on_present→_orig->Present 顺序；GUID 参考官方 source/com_utils.hpp。
- 保留原图 shadow copy 作为失败回退，提前执行隐藏 DX11 的 ReShade Present；只在对应线程、对应 native swapchain、非 TEST 的实际原生 Present 入口补充复制。此处位于 addon 处理之后、系统 flip/discard 之前；不在 Present 返回后读取已轮转的缓冲区。随后沿用既有 DX11 Signal→DX12 copy queue Wait→FG present queue Wait。每帧隐藏 Present 只调用一次。
- thread-local 作用域支持嵌套恢复、同请求只捕获一次，避免递归或其他交换链误触发。找不到 unwrapped 接口、安装失败或挂钩未触发时仍保留原来的图像路径并记录诊断。新增一次 GPU 复制的成本需要实测，未引入 CPU fence 等待。
- 未修改 FG 倍率、DLSSG Dispatch/插值时序、原生 DX12 链路或外部插件设置。不会自动改变 NR 模式。两个 runtime 导致的 overlay_frozen 仍是独立未解决事项，未强行销毁任一 runtime。
- FG-only 主界面恢复 RenderAdaMfgUnlock 与 RenderFrameGenerationRuntimeSettings：RTX40 解锁/插值时序修正、FG Active/MFG 倍率及既有运行设置。控件原有硬件/运行状态可见性条件不变；无需打开已隐藏的 NR 中心。

验证：Release x64 /p:OptiScalerFgOnly=true 构建成功（既有编译/链接警告仍在）。新增 present_capture_unit 覆盖后处理颜色、翻转前复制、TEST/其他交换链过滤、一次性/递归、线程隔离、嵌套、失败与未进入边界回退及异常离开作用域。新增 present_capture_dx11_smoke 使用生产 ReShadePresentCapture 安装/挂钩实现，真实 D3D11 WARP flip-discard 隐藏交换链，模拟 ReShade proxy 在 native Present 前写绿色，连续四次 GPU staging 回读确认捕获绿色而非原红色；另测无 proxy、重复安装与 TEST。此测试模拟 addon 顺序，不加载实际 ReShade/NR 插件，不等于崩铁实测。既有 FG-only/full policy、DLSSG pause/evaluate、bridge handoff、D3D12 WARP finished queue、swapchain reentrancy 测试通过；本次修改 diff --check 通过。

部署：03:50:49 再次确认三个游戏进程均未运行后，替换 0.1.5 根目录与内层 DLL。产物 03:49:29、26,259,968 字节；构建源与两个部署目标 SHA256=AE3D69473F519EC741A4A5B076717243D34588B5CE396AE46C2D2994544AD72B。旧 DLL 各自备份后缀 .bak-before-present-capture-20260929-035049。部署未改 INI/外部 addon，其 SHA256 仍为 DCD93881E976AD033D83C2BB01F4BC3E4DDC59C15FE0DD4CA165BC5FC7D1AC68。

下一轮：保持外部 NR“呈现”，重启崩铁；先 FG off 看 NR，再 2x/6x、Esc 和切出切回。Opt 日志应有 FG companion capture: ReShade post-addon / pre-flip DX11 capture installed，抽样应为 attempted=true copied=true。捕获成功只能证明桥接复制执行，仍需视觉验证实际 NR 效果；若捕获成功但无效果，应继续定位插件真实写回面/队列，不能继续靠重建 DLSS。prefg 计数可能仍为 0（本修复是 Opt 捕获，不是 addon 的 prefg 计数路径），不能单独用它判失败。


### 9.31 FG 深度转换的 D3D11 输出绑定冲突（2026-09-29 04:44）

用户增强深度视图全屏蓝色。核对部署根INI和profiles/hkrpg_bilibili.ini，两者SHA256均6951977C0B346314CD225D31C925CCE354A08C3A39441E09157252A60470C5C4，FGInput=Upscaler、FGOutput=DLSSG、ResourceFlip=true，深度项auto，无显式禁用深度。最新04:38日志确认recorded=true，实际tagged资源R32_FLOAT(41)、1280x800、extent匹配、offset=0。全蓝说明预览采样值零，但尚不能确定源纹理自身为空。

检查发现原生DX11 DLSS Evaluate恢复游戏OM/DSV后，才执行UpscalerInputsDx11wDx12转换；DepthTransfer_Dx11直接将深度绑CS SRV，没有解除重叠DSV，也未保存恢复自己修改的CS绑定。新增DepthTransferBindings作用域：通过COM identity检查输入是否为当前DSV资源，若匹配临时解除DSV，保留RTV和OM UAV；结束恢复DSV、CS shader/class instances、SRV0/UAV0。绑定后检查实际SRV，若被运行时拒绝则报错并返回失败，不再提交零输入转换。命中DSV冲突时每240次日志一次，待游戏验证该路径是否命中。

新增真实D3D11 WARP测试fg_depth_binding_gpu：源R24G8深度清为0.25并绑定DSV，旧路径SRV变null且GPU回读全0；生产guard下GPU回读0.25。验证null/非null CS shader、UAV、DSV以及OM UAV slot1恢复，修复路径debug layer无warning/error。此测试复现并证明绑定缺陷，不冒充已经证明崩铁本次全零仅由此引起。原深度预览和翻转GPU测试重跑通过。Release FG-only编译退出0，既有警告保留。

04:44:40确认游戏退出后更新两处0.1.5 DLL；源/目标SHA256=A9AF2BF443491AA264D33AA3B94F2D97FCDA7405F6CAAB9F206F01C6A841CE96，产物04:44:13、26,272,768字节。备份后缀.bak-before-depth-binding-20260929-044440。两份INI及外部addon哈希保持不变；不改呈现NR、倍率、相机/运动向量参数、节奏或原生DX12路径。待重启崩铁复测增强深度并读取detached overlapping game DSV诊断。

### 9.30 FG 深度全黑的可辨识预览（2026-09-29 04:36）

用户实测深度调试全黑。04:27–04:28日志确认 DepthInverted=true，DepthTransfer/FG depth tagging在运行；成功tag不能证明像素有效，也不能凭全黑断言没有深度。桥接计时样本 allocatorWait约0、getBuffer约0.01–0.03ms，长耗时主要处于FG Present区间，不据此盲目增加CPU等待或改FG时序。

本轮仅改诊断：新增默认开启的“增强深度预览（对数显示）”，微小非零值增强成灰色，精确0蓝色、精确1黄色、越界红色、NaN/Inf紫色；分类在显示反转之前，图例中文。可关闭增强回到原线性预览。只读取实际FG所tag资源，不改送给FG的像素、反转标志、MV、相机或插值参数。预览开启时每120次记录命令录制结果、资源地址、格式、纹理/extent尺寸及显示设置；recorded不冒充GPU执行完成或内容有效。未新增CPU回读/同步等待。

测试：生产FgDepthDebug的D3D12 WARP四槽测试重编，线性及增强两种模式均通过；新增0、1e-8、1、负数、NaN与0.001像素验证，包含crop/gain/invert，debug layer=1无error/corruption。相关已有文件git diff --check通过。Release x64 FG-only编译退出0，保留既有编译/链接警告。

部署：确认游戏未运行后04:36:03更新0.1.5根目录与内层DLL。产物04:35:47、26,270,208字节，源及两目标SHA256=ED93F278129BF989A7E73B0BDDF068C2ED26B4418BBAF20422047622B9CDA226。两处备份后缀.bak-before-depth-enhanced-20260929-043603。未修改部署INI、外部addon或NR呈现设置；外部addon哈希仍为DCD93881E976AD033D83C2BB01F4BC3E4DDC59C15FE0DD4CA165BC5FC7D1AC68。

待游戏验证：默认增强模式下观察人物/场景是否有稳定灰度轮廓；整屏蓝色表示采样结果全0（不能单凭此区别源全零与错误绑定），整屏黑色则不是增强shader的正常深度输出，应检查命令是否执行/后续覆盖。未声称本轮已修复果冻或证明真实深度正确；当前没有实际游戏像素统计回读。

### 9.29 FG 输入翻转修复与实际深度调试视图（2026-09-29 04:24）

用户确认上一轮外部 NR 呈现效果恢复，但移动镜头时 FG 人物/物体闪烁、果冻、帧率偏低；本轮授权继续修复，并要求增加 FG 深度获取 debug 视图。保持外部插件“呈现”模式，不修改 addon/INI，不调整 2x/6x 倍率或 Reflex 节奏。

诊断：03:59–04:04 会话中 Depth/Velocity 按相同 FG 帧号 SetTagForFrame 返回 eOk，不能据此保证像素内容、方向、尺度和颜色时序正确。桥接 capture 抽样 attempted=true/copied=true；末段桥接 CPU 耗时约27–29ms，旧 waitInterop 标记实际覆盖 _CopyDx11SharedToDx12FGBackBuffer 内多个步骤，不能当作 NR GPU 纯耗时。新增 FG copy acquire 分项（allocatorWait/reset/indexWait/getBuffer）以区分下一轮瓶颈；本轮没有证实帧率提升。

修复：
- RF_Dx12 原本只有2份描述符、1份可改写常量，但FG有4个槽；改为 BUFFER_COUNT 描述符及常量，显式使用 FG frameSlot。依赖调用方 UI 槽的既有 fence 等待，避免独立短环提前覆盖。DLSSG 保存实际 fIndex，翻转使用该索引。
- 首次创建翻转器不再直接 return 跳过首帧；按 Depth/Velocity 类型传 velocity 标志，不再一律 true。
- flip shader 按资源有效区域派发，不再用当前 feature 的 MVLowRes 决定深度派发尺寸；补 X/Y 边界检查。输入 offset 按 sourceY=y+offset，输出Y=height-1-y，修复原有偏移下越界/缺行。预编译 HLSL、运行时字符串、DX12 CSO/header 同步更新。
- 输入状态按调用方声明切到 NON_PIXEL_SHADER_RESOURCE 后还原，输出增加 UAV barrier。未宣称已验证游戏实际深度数值、相机参数或原始共享缓存与 NR 颜色的全部时序。

深度 debug：
- 帧生成运行设置新增中文“FG 深度调试视图”“反转深度预览”“深度预览增益”；默认关闭，仅运行态，不写INI。
- DLSSG Present 使用当前 fIndex 的 Depth.GetResource()（含翻转/复制后的实际 tag 资源）、extent/state，在受UI fence保护的同槽命令列表绘制全屏灰度；无有效资源显示等待提示。调试会替换最终画面，正常游玩与帧率比较时应关闭。
- 独立 FgDepthDebug 绘制器：4槽 SRV/RTV、root constants（无共享可变 upload）、按目标格式缓存 PSO、还原深度及 backbuffer 状态；无效深度NaN/Inf显示品红。不将显示增益/反转写回真实FG输入。原生DX12 DLSSG同样可用，未做绝区零游戏实测。

验证：
- 新 tests/fg_resource_flip_gpu.cpp 使用 WARP 执行生产 HLSL，逐像素检查深度值、MV Y符号、偏移、非16倍数尺寸、未写区域保护，通过。测试为相同HLSL的SM5编译执行，不冒充完整RF_Dx12类集成测试；DX12部署CSO由dxc重新生成。
- 新 tests/fg_depth_gpu_smoke.cpp 使用生产 FgDepthDebug + D3D12 WARP，先记录4个列表再统一提交，验证各槽不同extent/crop/gain/invert的全屏回读，D3D12 debug layer=1，无error/corruption；无效槽/越界输入拒绝测试通过。
- 既有 fg_only/full policy、pause/evaluate、bridge、reentrancy、queue、present_capture unit/真实D3D11 WARP测试重新运行通过（既有测试二进制未因本轮未涉及代码而重编）。运行时/预编译flip源码逐字一致。本轮相关源码diff --check通过；工作区其他旧改动/生成文件存在既有空白警告，不做全仓清理。
- 最终 Release x64 /p:OptiScalerFgOnly=true 构建退出0，仍有既有 C4819/C4250/C4744/LNK4098 等警告。

部署：04:24:40确认StarRail/ZenlessZoneZero/GenshinImpact均未运行后，替换现有0.1.5根目录及内层OptiScaler/OptiScaler.dll。产物04:23:56、26,266,624字节；源及两目标SHA256=6639B3CE3FB3987F06DC43A92F34DE90097A957C77B0560E5B4EC2B5C0DDD8E5。两处各自备份后缀 .bak-before-fg-depth-20260929-042440。外部addon哈希仍为DCD93881E976AD033D83C2BB01F4BC3E4DDC59C15FE0DD4CA165BC5FC7D1AC68。

待游戏验证：重启崩铁、保持NR呈现，先正常画面比较2x/6x镜头移动，再开FG深度视图观察人物/场景轮廓是否稳定随镜头移动；若全黑/全白，调整增益和反转，不据此直接断言无深度。检查Esc/切出切回。不要用调试视图开启时的性能作为正常帧率；低帧率的同步细分需下一次日志判断。本轮修复的是可证明的输入准备缺陷，不能承诺果冻全部消失或已达到预期帧率。
