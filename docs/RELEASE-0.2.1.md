# OptiScaler MFG Ada 0.2.1 — 2026-10-08

## Changes

- **Fix inverted menu in Genshin & Interop (FlipY default false)**: OptiScaler menu now renders right-side up by default. Previous heuristic inverted Y automatically on DX11-to-DX12 interop swapchains; this has been corrected to default to normal orientation, with manual toggle preserved in the bottom bar.
- **Pure FG-Only single-column layout**: Restored the clean single-column menu flow for FG-only companion builds while retaining all 0.2 frame generation features.
- **Full 0.2.0 Frame Generation features retained**:
  - Mavis Local Stable architecture (Blackwell kernels, validated warp blend, intermediate scatter retention, boundary artifact guard).
  - Measured source presentation & DLSSG provider present FPS telemetry.
  - Model rebuild safety and external NR recovery.
  - DLSSG runtime selection and input quality tags guard.
- **NVIDIA GPU startup stability guard**: Skip D3D12 probe device creation on NVIDIA GPUs to prevent race conditions during early game D3D11 device initialization.
- **Default configurations optimized**: `Fsr4ForceModel = auto` and `PreferFirstDedicatedGpu = true` set across presets to prevent unnecessary multi-adapter dummy device churn.

## Verification / limits

Release x64 FG-only build. All unit and smoke tests passed. Tested with Genshin Impact, Star Rail, and ZZZ.
