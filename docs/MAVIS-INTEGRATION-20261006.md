# MFGAdaUnlock-RenoDx integration — 2026-10-06

Upstream mavismmg/MFGAdaUnlock-RenoDx commit: a2ca96f3eacebf0bec5e25f07f15b46fbf3a0aef. MIT attribution retained in mavis/UPSTREAM.md and LICENSE-MFGAdaUnlock-RenoDx.txt. Existing Opti source base plus local changes; no GitHub push/release.

## Integrated behavior

- Exact provider/payload validated Blackwell-to-Ada kernels, Local Stable geometry and V2 Compatibility inpaint. Never use confidence-history/CUDA/NVAPI temporal interception; this follows upstream release 1.4.1's selected path.
- Validated warp blend, intermediate scatter retention, boundary guard Off/Balanced/Aggressive. Unsupported provider/payload retains the existing midpoint path and reports why; no speculative patch.
- Native input (default), Automatic HUD/UI Guard, explicit verified UI recomposition. Required depth/motion/final color are not changed. On mode/depth-threshold changes reset FG history once. Optional depth-edge separation override defaults off.
- Existing fixed 2x–6x and Dynamic MFG retained, Dynamic rejection bounded to three attempts with fixed-MFG fallback, saved request preserved. Requested capability and runtime presentation count/status shown separately; GetState only at serialized Present, bounded to 250ms (counter means presents since query, not multiplier).
- Reflex output-FPS cap defaults off. Runtime selection default/current policy, local, OTA uses documented slInit flags, restart required; active provider paths logged.
- Provider references and replacement PTX/cubin allocations share process lifetime, including cached replay across remaps. No per-frame full-provider scan introduced.

## Depth debug/menu fix

Observed main configuration OverlayMenu=false embeds the menu in upscaler output. Later full-screen depth preview erased it. The old post-depth MenuOverlayDx call immediately returned in fallback mode.

Snapshot upscaler-menu normalized clip regions without accessing ImGui from another thread; depth shader discards those pixels. Preserve both Y orientations and bounded clips; large-menu fallback protects the left region; snapshot expires at 500ms. Native overlay mode renders after depth + SC list submission. Never change OverlayMenu, MenuFlipY, input HWND or ReShade click policy. Paused/absent-depth path no longer suppresses the ordinary menu just because debug is checked.

## Validation

Release x64 FG-only build succeeds. Eight CPU suites (upstream logic and Local/research build policies), three production depth renderer WARP GPU readbacks across four slots, mapped real 310.9.1 provider mutation/restore with Local V3 geometry + V2 inpaint + warp assertions and unsupported image fail-closed check succeed. GPU tests enable D3D12 debug layer and reject errors. They do not run the actual game, NVIDIA FG interpolation, HDR transfer validation or ReShade mouse interaction.

## Reproducible local payload generation

python tools/mavis_quality/generate.py --ptxas <ptxas.exe> --provider <local nvngx_dlssg.dll> [other local provider ...]

Generates ignored blackwell_cubins.generated.hpp and thin_geometry_cubins.generated.hpp from installed NVIDIA providers. Raw NVIDIA DLLs/payload tables are not source-controlled. Without tables the Blackwell quality path reports unavailable; existing midpoint correction remains and supported warp PTX may still apply. Tool/test source retains upstream license.

## Scope limits

This integrates the quality/control behavior into Opti's own Streamline output rather than loading the ReShade addon alongside it. The addon-only native-game Vulkan discovery, external game's SetOptions interception, optional legacy software-flip byte patch, PresentMon tooling and automatic latency-trial harness have not been duplicated. Do not describe this as every upstream diagnostic/experimental subsystem being ported. Native Opti pacing remains default; no binary metering mutation is enabled.

## Test instructions

Next game start loads the local DLL. First check that depth debug leaves Opt controls visible with existing OverlayMenu=false. New quality master is opt-in (RTX40 MFG area: Mavis Local Stable quality); Save Settings and restart the game to enable. Input quality and cap remain Native/off unless selected. Inspect MFG quality lines and actual presentation telemetry; API/test success is not proof of game compatibility or quality improvement.

DLL SHA256: c14c6407cca6b327964d9acd2573f82714c6ce176e292a5332bb294a60803f66
