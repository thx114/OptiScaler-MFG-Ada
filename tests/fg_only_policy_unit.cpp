#include "../OptiScaler/FgOnlyPolicy.h"
#include <cassert>
#include <cstdio>
#include <initializer_list>

// Policy test double records writes; serialization itself remains CustomOptional-owned.
template<class T> struct Option
{
    T runtime, persisted;
    int writes = 0;
    void set_volatile_value(T value) { runtime = value; ++writes; }
};
enum class Backend { W12, XeSS, DLSS };
struct Config
{
    Option<Backend> Dx11Upscaler { Backend::W12, Backend::W12 };
    Option<Backend> Dx12Upscaler { Backend::W12, Backend::W12 };
    Option<bool> DLSSEnabled { false, false };
    Option<bool> DlssNrEnabled { true, true };
    Option<bool> DlssNrFinishedPicture { true, true };
    Option<bool> DlssNrScanMeter { true, true };
    Option<bool> RcasEnabled { true, true };
    Option<bool> ContrastEnabled { true, true };
    Option<bool> MotionSharpnessEnabled { true, true };
    Option<bool> OverrideSharpness { true, true };
    Option<bool> MagnifierEnabled { true, true };
    Option<bool> OutputScalingEnabled { true, true };
    Option<bool> ExtendedLimits { true, true };
    Option<bool> UpscaleRatioOverrideEnabled { true, true };
    Option<bool> QualityRatioOverrideEnabled { true, true };
    Option<bool> DrsMinOverrideEnabled { true, true };
    Option<bool> DrsMaxOverrideEnabled { true, true };
    int fgMultiplier = 6;
    bool fgEnabled = true;
};
int main()
{
    for (int multiplier : {2, 6})
    {
        Config cfg;
        cfg.fgMultiplier = multiplier;
        FgOnly::Apply(cfg, Backend::DLSS);
        FgOnly::Apply(cfg, Backend::DLSS); // repeat reload is idempotent
        assert(cfg.Dx11Upscaler.runtime == (FgOnly::Enabled ? Backend::DLSS : Backend::W12));
        assert(cfg.Dx11Upscaler.persisted == Backend::W12);
        assert(cfg.Dx11Upscaler.writes == (FgOnly::Enabled ? 2 : 0));
        assert(cfg.Dx12Upscaler.runtime == (FgOnly::Enabled ? Backend::DLSS : Backend::W12));
        assert(cfg.Dx12Upscaler.persisted == Backend::W12);
        assert(cfg.Dx12Upscaler.writes == (FgOnly::Enabled ? 2 : 0));
        assert(cfg.DLSSEnabled.runtime == (FgOnly::Enabled ? true : false));
        assert(cfg.DLSSEnabled.persisted == false);
        assert(cfg.DLSSEnabled.writes == (FgOnly::Enabled ? 2 : 0));
        assert(cfg.DlssNrEnabled.runtime == (FgOnly::Enabled ? false : true));
        assert(cfg.DlssNrEnabled.persisted == true);
        assert(cfg.DlssNrEnabled.writes == (FgOnly::Enabled ? 2 : 0));
        assert(cfg.DlssNrFinishedPicture.runtime == (FgOnly::Enabled ? false : true));
        assert(cfg.DlssNrFinishedPicture.persisted == true);
        assert(cfg.DlssNrFinishedPicture.writes == (FgOnly::Enabled ? 2 : 0));
        assert(cfg.DlssNrScanMeter.runtime == (FgOnly::Enabled ? false : true));
        assert(cfg.DlssNrScanMeter.persisted == true);
        assert(cfg.DlssNrScanMeter.writes == (FgOnly::Enabled ? 2 : 0));
        assert(cfg.RcasEnabled.runtime == (FgOnly::Enabled ? false : true));
        assert(cfg.RcasEnabled.persisted == true);
        assert(cfg.RcasEnabled.writes == (FgOnly::Enabled ? 2 : 0));
        assert(cfg.ContrastEnabled.runtime == (FgOnly::Enabled ? false : true));
        assert(cfg.ContrastEnabled.persisted == true);
        assert(cfg.ContrastEnabled.writes == (FgOnly::Enabled ? 2 : 0));
        assert(cfg.MotionSharpnessEnabled.runtime == (FgOnly::Enabled ? false : true));
        assert(cfg.MotionSharpnessEnabled.persisted == true);
        assert(cfg.MotionSharpnessEnabled.writes == (FgOnly::Enabled ? 2 : 0));
        assert(cfg.OverrideSharpness.runtime == (FgOnly::Enabled ? false : true));
        assert(cfg.OverrideSharpness.persisted == true);
        assert(cfg.OverrideSharpness.writes == (FgOnly::Enabled ? 2 : 0));
        assert(cfg.MagnifierEnabled.runtime == (FgOnly::Enabled ? false : true));
        assert(cfg.MagnifierEnabled.persisted == true);
        assert(cfg.MagnifierEnabled.writes == (FgOnly::Enabled ? 2 : 0));
        assert(cfg.OutputScalingEnabled.runtime == (FgOnly::Enabled ? false : true));
        assert(cfg.OutputScalingEnabled.persisted == true);
        assert(cfg.OutputScalingEnabled.writes == (FgOnly::Enabled ? 2 : 0));
        assert(cfg.ExtendedLimits.runtime == (FgOnly::Enabled ? false : true));
        assert(cfg.ExtendedLimits.persisted == true);
        assert(cfg.ExtendedLimits.writes == (FgOnly::Enabled ? 2 : 0));
        assert(cfg.UpscaleRatioOverrideEnabled.runtime == (FgOnly::Enabled ? false : true));
        assert(cfg.UpscaleRatioOverrideEnabled.persisted == true);
        assert(cfg.UpscaleRatioOverrideEnabled.writes == (FgOnly::Enabled ? 2 : 0));
        assert(cfg.QualityRatioOverrideEnabled.runtime == (FgOnly::Enabled ? false : true));
        assert(cfg.QualityRatioOverrideEnabled.persisted == true);
        assert(cfg.QualityRatioOverrideEnabled.writes == (FgOnly::Enabled ? 2 : 0));
        assert(cfg.DrsMinOverrideEnabled.runtime == (FgOnly::Enabled ? false : true));
        assert(cfg.DrsMinOverrideEnabled.persisted == true);
        assert(cfg.DrsMinOverrideEnabled.writes == (FgOnly::Enabled ? 2 : 0));
        assert(cfg.DrsMaxOverrideEnabled.runtime == (FgOnly::Enabled ? false : true));
        assert(cfg.DrsMaxOverrideEnabled.persisted == true);
        assert(cfg.DrsMaxOverrideEnabled.writes == (FgOnly::Enabled ? 2 : 0));
        assert(cfg.fgMultiplier == multiplier && cfg.fgEnabled);
    }
    std::puts(FgOnly::Enabled ? "FG-only policy passed: native DX11/DX12, no NR/post-FX, FG unchanged" : "Full build policy passed: no overrides");
}

