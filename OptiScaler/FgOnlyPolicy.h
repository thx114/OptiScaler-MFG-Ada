#pragma once

// Separate build flavour. The normal OptiScaler build keeps all existing behaviour.
namespace FgOnly
{
#ifdef OPTISCALER_FG_ONLY
inline constexpr bool Enabled = true;
#else
inline constexpr bool Enabled = false;
#endif

// Volatile overrides preserve the full-build profile when switching DLLs back.
template <class Config, class Backend> void Apply(Config& cfg, Backend nativeDlss)
{
    if constexpr (!Enabled)
        return;
    cfg.Dx11Upscaler.set_volatile_value(nativeDlss);
    cfg.Dx12Upscaler.set_volatile_value(nativeDlss);
    cfg.DLSSEnabled.set_volatile_value(true);
    cfg.DlssNrEnabled.set_volatile_value(false);
    cfg.DlssNrFinishedPicture.set_volatile_value(false);
    cfg.DlssNrScanMeter.set_volatile_value(false);
    cfg.RcasEnabled.set_volatile_value(false);
    cfg.ContrastEnabled.set_volatile_value(false);
    cfg.MotionSharpnessEnabled.set_volatile_value(false);
    cfg.OverrideSharpness.set_volatile_value(false);
    cfg.MagnifierEnabled.set_volatile_value(false);
    cfg.OutputScalingEnabled.set_volatile_value(false);
    cfg.ExtendedLimits.set_volatile_value(false);
    cfg.UpscaleRatioOverrideEnabled.set_volatile_value(false);
    cfg.QualityRatioOverrideEnabled.set_volatile_value(false);
    cfg.DrsMinOverrideEnabled.set_volatile_value(false);
    cfg.DrsMaxOverrideEnabled.set_volatile_value(false);
}
}
