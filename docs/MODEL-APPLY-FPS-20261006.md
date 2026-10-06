# Apply model rebuild / measured FPS — 2026-10-06

## Evidence and limits

User reports a crash on model application around 19:04. ReShade log reaches a new NGX DLSS feature creation at 19:04:07 and stops; Hub reports YuanShen PID49156 exited at 19:04:11. No new crash stack/module/dump captured for this session. Source defects below are verified; they must not be described as proof of the only crash cause. Internal NR Apply model is not exposed in FG-only builds, so the targeted Opti path is the DLSS/DLSSD preset Apply Changes/rebuild path, not a change to the external RenoDX NR checkbox.

## Fixes

1. DX11/DX12/Vulkan model rebuild used the OptiScaler allocator marker as permission to free createParams. Passthrough parameters are borrowed from game/FSR Bridge and may have the same marker. ContextData now ownsCreateParams explicitly only for temporary maps allocated for this rebuild. ReleaseRebuildParameters frees only owned tables, clears its borrow and is idempotent. Null create/feature states reject failed phase transitions instead of dereferencing them.
2. Static DX11/DX12 fallback ImGui was reset by old feature destructors running on delayed worker threads. Retirement of a model no longer shuts down the shared renderer. A retained menu-device reference scopes replacement to Init/render-thread device changes. OverlayMenu=false and ReShade input settings preserved.
3. DLSSG FPS no longer uses the per-callback spacing / requested multiplier to infer base frame rate. Count successful client Present calls independently of FG source IDs, query SL presented-frame delta at >=250ms interval, integrate counts against elapsed >=500ms windows. Separate source/submission FPS and provider-present FPS. Exclude startup unknown counter, invalid clocks, resets and stale >2s samples; show waiting rather than an estimated multiplier rate. Presented events can include generated/repeated images and do not prove unique images or physical monitor scanout.
4. Overlay "DLSSG xN" formerly used maximum supported count, not actual selected count. It now explicitly labels selected count as requested, not measurement.

## Validation

Release x64 FG-only build succeeds, existing compiler/link warnings remain. Production ReleaseRebuildParameters and FrameRateWindow CPU tests pass borrowed marked-table survival, owned destroy exactly once, 24-source/120-present timing, a requested 6x with only 24 actual presents (no inference), burst/duplicate counters, rebuild resets, stale samples and zeros. Existing MFG option/ABI test harness updated for currently supported modern 2.11+ Dynamic trials versus legacy unsupported wrappers: 68 checks 0 failures; added missing preset mock only, no Streamline hook behavior changed by this fixture update. diff --check passes. Actual game repeated application, mouse handling, display FPS and compatibility remain pending.

## Deployment

Test root: D:\APPS\test\TEST\HoYoShadeHub_Portable_1.4.3.1_x64_Full. Both root and component OptiScaler.dll deployed with backups, configs unchanged.
Main root: D:\APPS\HoYoShadeHub. User started StarRail while fixing; do not terminate it or replace its in-use DLL. Hidden Apply-Main-When-Idle.ps1 waits (max6h) for YuanShen/StarRail/ZenlessZoneZero exit, validates staged and previous target hashes, copies backups, applies both DLLs and verifies hash. A restarted game or unrelated DLL update aborts instead of overwriting. State in main-status.json. Rollback script refuses to run with games active.

DLL SHA256: 110dd9e5339605ac785071b503c99893cefe60884cf6d1cbafbeac1ec0d3188c

No GitHub push or release. Old models, depth/MV guide orientation, Mavis Local Stable quality integration and native retirement kept. Do not claim this measures display scanout FPS or that an old version resource string proves obsolete native code.
