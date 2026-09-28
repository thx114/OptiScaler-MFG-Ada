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
