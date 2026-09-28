#pragma once
#include <optional>
#include <array>
// Narrow test double; the production PassProfiles.h is compiled unchanged.
template<class T> struct SkinTestOption
{
    std::optional<T> stored;
    T fallback{};
    SkinTestOption() = default;
    SkinTestOption(T def) : fallback(def) {}
    bool has_value() const { return stored.has_value(); }
    T value() const { return stored.value(); }
    T value_or_default() const { return stored.value_or(fallback); }
    SkinTestOption& operator=(T val) { stored=val; return *this; }
};
struct Config
{
    SkinTestOption<unsigned> DlssNrPreset{0}, DlssNrStyle{0};
    SkinTestOption<unsigned> DlssNrPass2Preset, DlssNrPass3Preset, DlssNrPass2Style, DlssNrPass3Style;
    SkinTestOption<float> DlssNrIntensity{1}, DlssNrLocalStructure{1}, DlssNrLocalTone{1}, DlssNrSkinStructure{-1};
    SkinTestOption<bool> DlssNrAutoMask{true}, DlssNrSkinIndependent{false};
    SkinTestOption<float> DlssNrPass2Intensity, DlssNrPass2LocalStructure, DlssNrPass2LocalTone, DlssNrPass2SkinStructure;
    SkinTestOption<float> DlssNrPass3Intensity, DlssNrPass3LocalStructure, DlssNrPass3LocalTone, DlssNrPass3SkinStructure;
    SkinTestOption<bool> DlssNrPass2AutoMask, DlssNrPass3AutoMask;
    struct Extra {
        SkinTestOption<unsigned> style;
        SkinTestOption<float> intensity, structure, tone, skin;
        SkinTestOption<bool> autoMask;
    };
    std::array<Extra,27> DlssNrExtraPasses;
};

