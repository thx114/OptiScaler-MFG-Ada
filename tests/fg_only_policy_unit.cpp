#include "../OptiScaler/FgOnlyPolicy.h"
#include <cassert>
#include <cstdio>
#include <initializer_list>

// Policy test double records writes; serialization itself remains CustomOptional-owned.
template<class T> struct Option
{
    T runtime, persisted;
    int writes = 0;
    bool configured = true;
    bool has_value() const { return configured; }
    void set_volatile_value(T value) { runtime = value; configured = true; ++writes; }
};
enum class Backend { W12, XeSS, FSR31, DLSS };
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
    for (auto backend : {Backend::W12, Backend::XeSS, Backend::FSR31, Backend::DLSS})
    {
        Config cfg;
        cfg.Dx11Upscaler.runtime = cfg.Dx11Upscaler.persisted = backend;
        cfg.Dx12Upscaler.runtime = cfg.Dx12Upscaler.persisted = backend;
        cfg.fgMultiplier = multiplier;
        FgOnly::Apply(cfg, Backend::DLSS);
        FgOnly::Apply(cfg, Backend::DLSS); // repeat reload is idempotent
        assert(cfg.Dx11Upscaler.runtime == backend);
        assert(cfg.Dx11Upscaler.persisted == backend);
        assert(cfg.Dx11Upscaler.writes == 0);
        assert(cfg.Dx12Upscaler.runtime == backend);
        assert(cfg.Dx12Upscaler.persisted == backend);
        assert(cfg.Dx12Upscaler.writes == 0);
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
    // Auto/unset falls back to DLSS only once, without changing persisted values.
    Config automatic;
    automatic.Dx11Upscaler.configured = automatic.Dx12Upscaler.configured = false;
    FgOnly::Apply(automatic, Backend::DLSS);
    FgOnly::Apply(automatic, Backend::DLSS);
    assert(automatic.Dx11Upscaler.runtime == (FgOnly::Enabled ? Backend::DLSS : Backend::W12));
    assert(automatic.Dx12Upscaler.runtime == (FgOnly::Enabled ? Backend::DLSS : Backend::W12));
    assert(automatic.Dx11Upscaler.writes == (FgOnly::Enabled ? 1 : 0));
    assert(automatic.Dx12Upscaler.writes == (FgOnly::Enabled ? 1 : 0));
    assert(automatic.Dx11Upscaler.persisted == Backend::W12);
    assert(automatic.Dx12Upscaler.persisted == Backend::W12);
    // A user-selected FSR backend must survive the next policy application.
    automatic.Dx11Upscaler.runtime = automatic.Dx11Upscaler.persisted = Backend::FSR31;
    automatic.Dx11Upscaler.configured = true;
    FgOnly::Apply(automatic, Backend::DLSS);
    assert(automatic.Dx11Upscaler.runtime == Backend::FSR31);
    assert(automatic.Dx11Upscaler.persisted == Backend::FSR31);
    std::puts(FgOnly::Enabled ? "FG-only policy passed: selectable DX11/DX12, auto DLSS fallback, no NR/post-FX, FG unchanged" : "Full build policy passed: no overrides");
}

