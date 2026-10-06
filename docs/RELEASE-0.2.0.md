# OptiScaler MFG Ada 0.2.0 — 2026-10-06

## Changes

- Integrate MFGAdaUnlock-RenoDx Local Stable geometry, V2 Compatibility inpaint, validated warp blend, intermediate scatter retention and boundary artifact protection. Exact provider/payload matching; `MavisQuality=false` by default, save/restart to enable. Unsupported payloads keep the existing timing fallback.
- DLSSG input quality Native / automatic optional HUD/UI guard / explicit verified UI recomposition; no rewriting of required color/depth/motion input. Optional depth-edge threshold and Reflex output FPS cap default off.
- Runtime selection preserves current policy by default, with optional local/OTA selection via documented Streamline flags (restart). Dynamic MFG rejection falls back to fixed mode without silently changing saved intent.
- Fix depth diagnostic preview covering the fallback Opt menu with `OverlayMenu=false`; preserve ReShade input compatibility and menu Y-orientation settings.
- Fix DLSS/DLSSD rebuild ownership: game/Bridge-borrowed NGX parameters are not freed by Opt; old feature retirement no longer shuts down its shared fallback menu. Failed initialization refuses null parameter/feature states.
- FPS now separates measured successful client-source Present submissions and DLSSG provider presentation counts over elapsed time. No requested-multiplier extrapolation. Startup/reset/stale samples show waiting. Reported presents can include generated/repeated frames and are not unique image/physical scanout measurements.
- Retain prior native Genshin guide-coordinate corrections, CPU FG input-reader retirement, menu and resolution rebuild safety, Chinese menu support and disabled built-in update checks.

## Verification / limits

Release x64 FG-only build. Ownership/FPS CPU regressions, MFG options/ABI regressions, upstream quality-policy suites, three production depth-renderer WARP GPU readbacks and real 310.9.1 DLL image patch/restore validation pass. GPU validation does not execute the actual game or NVIDIA frame interpolation.

Repeated model application, actual FG quality/pacing, HDR, mouse interaction and game startup compatibility remain subject to in-game validation. Do not interpret this release as a guarantee that XXMI, every Genshin/Star Rail/ZZZ setup, or every NVIDIA provider is compatible.

The independent addon's experimental CUDA/NVAPI confidence-history interception, legacy software-flip mutation, native-game Vulkan discovery and automatic latency-trial harness are not ported. FSR Bridge skin-mask experiments are not part of this release. Opt remains FG-only; external RenoDX owns NR.

## Package / upgrade

Asset: `optiscaler-mfg-ada-fg-only-0.2.0.zip`.

Contains OptiScaler.dll, minimal default INI, installation/runtime-fetch scripts, notices and documentation. No raw NVIDIA DLSS/NR/Streamline runtime DLLs, external RenoDX addon, Bridge or personal profiles/logs are bundled. The release DLL embeds locally generated exact-match quality kernel tables for the validated 310.9.1 provider; source-only builds without these tables safely report unavailable/fallback. Generation tool and upstream MIT notices are included in source.

Existing users: back up loaded Opt DLL(s) and INI, exit games, update the actual injected DLL and any manager-maintained sibling copy, retain your current INI and runtime libraries. New installations start with FG disabled; enable it after verifying a normal picture. Never overwrite game-native `nvngx_dlss*.dll` with OptiScaler.dll.

Recommended first check: apply DLSS model repeatedly; toggle depth preview with fallback menu; compare source/submission FPS and provider-present FPS. Enable the quality master separately and restart for a controlled A/B.
