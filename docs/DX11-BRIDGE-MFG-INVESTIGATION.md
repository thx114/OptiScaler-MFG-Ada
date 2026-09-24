# DX11 bridge MFG investigation — HSR (hkrpg)

## Symptom

OptiScaler-owned DLSS-G multi-frame generation (FGDLSSGInterpolationCount=5, i.e. 6x)
silently degrades to 2x on the D3D11-with-D3D12 bridge (Dx11wDx12SC). The identical
build and options reach 6x on a D3D12 game (ZZZ). Measured: 5257 presents / 2721 real
frames = 1.93x over an 80 s focused session (240 Hz display, ~36.5 FPS base).

## Established facts (measured, not inferred)

- `MFG unlock: nvngx_dlssg.dll patched for 5 generated frames` — both the static DLL
  (`OptiScaler/streamline/nvngx_dlssg.dll`, handle 0x7FFDF7470000) and the OTA snippet
  (`C:\ProgramData\NVIDIA\NGX\models\dlssg\versions\20316416\files\160_E658703.bin`)
  are patched; the active target switches back to the static module before swapchain creation.
- `Max supported interpolations: 5 status 0` — raw runtime value, sampled at swapchain creation.
- Per-dispatch: `requestedNum 5 runtimeMax 5 runtimeReportedMax 5 status 0` (2672 samples, all status 0).
- Streamline's own present log (dlfgPresent.cpp presentCommon) reports the active option as
  `mode=eOn, numFramesToGenerate=1` — the clamp is on the option, not on the capability.
- Streamline v2.14.1 (installed, matches redist manifest). Its guide documents MFG up to
  numFramesToGenerateMax and states support is governed by that value (>1 means supported).
- `cloneFakeBuffers: bufferCount=6` — the interposer cloned six fake buffers (FG swapchain
  resized to 6; the game's DX11 side is 2).
- DLSSGStatus bitmask is clear: no Reflex-missing, no GetCurrentBackBufferIndex, no
  resolution/HDR/constants failure.

## Eliminated hypotheses

| # | Hypothesis | Killed by |
|---|---|---|
| 1 | Patch ineffective / wrong module | Patch logs name both modules; active handle is the patched one |
| 2 | Runtime max is 1 | Raw and plugin-reported max are 5 |
| 3 | Request rejected or clamped at the API | eOk with requestedNum 5 echoed back |
| 4 | Runtime degraded state | status bitmask 0x0 across the whole session |
| 5 | SL 2.14.1 lacks MFG | v2.14.1 guide documents numFramesToGenerate up to numFramesToGenerateMax |
| 6 | Refresh-rate throttling | 240 Hz display; 6x would be ~219 FPS |
| 7 | Swapchain too shallow for MFG | cloneFakeBuffers bufferCount=6 |
| 8 | Resize thrash resetting DLSS-G | Only 2 ResizeBuffers calls, 1 frame-counter reset |
| 9 | NR bridge interfering with generation | NR fully disabled → still 2x |
| 10 | hkslDLSSGSetOptions forcing eOff | That branch yields eOff; observed state is eOn/1 |
| 11 | hkslGetFeatureFunction dummy swallowing the request | Detour not attached (activeFgInput=Upscaler, no Ampere) |
| 12 | Reflex mode insufficient | Same eLowLatency in both games; DX12 works |
| 13 | Extent/alignment tag warnings | Both are auto-corrected by SL; present in the working path's class of warnings |

## Conclusion

The clamp happens inside Streamline's DLSS-G present path (sl.interposer/sl.dlss_g) for the
Dx11wDx12SC swapchain arrangement, with every documented gate reporting healthy. Byte-level
scans confirm the 0x1B0 advertise/validate gate signatures exist ONLY in nvngx_dlssg.dll —
sl.dlss_g.dll and sl.interposer.dll carry no copy of that logic, so the internal decision that
caps generation cannot be located by the same signatures.

This matches docs/DLSS-FRAME-GENERATION.md:100-102 — OptiScaler-owned FG is validated on
D3D12; D3D11 bridges were explicitly left for separate validation.

## Related finding (separate work item)

NR finished-picture has no path at all during OptiScaler-owned FG on D3D12:
ApplyToFinishedPictureBridge is called only from dx11_with_dx12_sc.cpp:328 (DX11-only), while
the D3D12 present hook calls the gated ApplyToFinishedPicture, which skips when
activeFgOutput == DLSSG. Observed as NR idling ("waiting for ...") in ZZZ with 6x active.

## Next options (in cost order)

1. Swap the bridge's Streamline stack to 3.x and retest — the DLSS-G present path was
   substantially reworked for DMFG; a file-swap experiment, reversible via the redist manifest.
2. Binary-instrument sl.dlss_g.dll's present decision (dlfgPresent.cpp) — real RE effort.
3. Accept 2x on the bridge for now and implement the NR-on-D3D12 bridge, which is
   well-understood and independently valuable.

## RESOLUTION (2026-09-24, 21:02 session)

The clamp was **not** in OptiScaler, Streamline, or the NGX runtime — it was the **NVIDIA
driver profile (DRS) for StarRail.exe**:

- The DLSS-G runtime reads per-app DRS keys at init (`DLSSGDRSKeys::ReadValuesFromDRSImpl`):
  ids `104d6667`, `10e41df1`, `10308298`.
- HSR read `104d6667=1` (override: 1 generated frame = 2x); ZZZ read `0` (unset → the app's
  option flows through). Every other measured quantity was identical between the two games
  (FC feedback, cloneFakeBuffers bufferCount=6, warning set, status bitmask, requestedNum,
  runtimeMax).
- presentCommon applies the DRS-driven cap regardless of what slDLSSGSetOptions wrote, which
  is why the option accepted 5 with eOk while the effective value stayed 1.
- Fix: remove the override — set "DLSS-FG - Multi-Frame Generation Count" to N/A for
  StarRail.exe in NVIDIA Profile Inspector. Raw value encoding: count = generated frames
  (2x=1, 4x=3, 6x=5); 0/N/A = follow the app's option.
- Verified: DRS reads 0, presentCommon reports numFramesToGenerate=5, 6x works.

Dead ends that consumed effort, for the record: no foreign slDLSSGSetOptions caller exists
(a temporary intercepting hook never fired once across HSR and ZZZ sessions — reverted);
no public Streamline 3.x exists (v2.14.1 is the latest tag and the launcher's RHI manifest
tops out at 2.14.1.0); NR disabled → still 2x.

Follow-up work items:
1. Auto-clear the DRS override at DLSS-G init via nvAPI DRS (guarded by the Ada MFG unlock
   flag) so other users do not hit this.
2. NR finished-picture bridge for the D3D12 path (ApplyToFinishedPictureBridge is only wired
   for the D3D11 bridge today).
3. The status {:X} diagnostics (DLSSG_Dx12) stay — they enabled this diagnosis.
