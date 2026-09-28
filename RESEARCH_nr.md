# NR 优化研究笔记（续 CC_kimi2.8.md 9.10）

目标：为"多级 NR 级联 + 帧生成"找一切可借鉴/可移植的优化。
每节完成即写入，时间倒序。
================================================================
## 1. dlss5-feed / dlss5-bridge 插件对（HoYoShade Addons 目录，供 ReShade 用）

来源：dlss5-feed.cfg / dlss5-bridge.cfg 注释与 dlss5-feed.log / dlss5-bridge.log 运行日志。

### dlss5-bridge 1.4.13-pre7（DLSS 契约镜像器）
- 作用：hook 游戏自己的 NGX DLSS（Create/Evaluate），把完整契约（color/depth/MV/flags/subrect）镜像到私有 D3D12 设备上重建一个 DLSS feature，让别的组件（renodx-dlss5 的 NR）能复用游戏的输入跑自己的模型。`source=auto` 让游戏自己的 DLSS 优先，没有时可用 `synth=1` 走"合成契约"（ReShade depth + 驱动 optical flow (ofa_grid/ofa_perf) 造 MV），文案明说效果差（"RDR2 looks like it is melting"）。
- 跨 API：D3D11<->D3D12 共享纹理（shared heap），fmt=26(R11G11B10_FLOAT)/41(D32F)/34(R16G16F) 逐个探测 "D3D12->D3D11 不行就换方向"。
- **vk_sync=2 流水线模式**：第 N 帧的 evaluate 和游戏渲染第 N+1 帧并行，队列等的是一帧前开始的活，代价是结果显示晚一帧（~17ms@60fps）+ 多一张 Output 纹理。→ 我们的薄桥可以把 NR evaluate 也做成跨帧流水，用一帧延迟换吞吐。
- 会复制游戏的 create flags（IsHDR/MVLowRes/DepthInverted/DoSharpening）并做 depth format 视图转换（R32G8X24 → R32_FLOAT_X8X24）。

### dlss5-feed（NR 供给器，mode=2）
- 通过 ReShade effect DLSS5_Feed.fx 拿 ColorInput/Depth/MV/Mask，自己持有一份 NR 管线；cfg 关键项：
  - work_resolution=100（NR 工作分辨率百分比，可调 → 降分辨率省 GPU 的直接开关）
  - work_upscale=0 / work_sharpness=0.30（NR 输出放大回用的缩放器+锐化）
  - warmup_rebuild=180（预热帧数后重建 feature）、create_delay=60、gpu_timeout_ms=2000、settle_evals、hold_strength/hold_tolerance（帧保持检测）
  - buffer_home=1 / async_home=0 / sync_home=0（缓冲与异步提交的归属）
- 日志教训：MV provider 配错（LumeniteFX 没装）时 MV 全零 → "still images only"，NR 时序模型直接废。MV 契约是 NR 的第一生命线。
