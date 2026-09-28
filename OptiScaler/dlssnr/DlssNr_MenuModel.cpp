#include "pch.h"

#include "DlssNr_MenuSections.h"
#include <Config.h>
#include <dlssnr/DlssNr_Status.h>
#include <dlssnr/PassProfiles.h>
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <Localization.h>

namespace DlssNr::MenuSections
{

// Model tuning rebuilds the feature; commit slider changes only on release.
template <typename Option>
static bool DeferredSlider(const char* label, Option* opt, float mn, float mx, float def, const char* fmt = "%.2f",
                           bool inheritReset = false, float displayScale = 1.0f)
{
    static std::unordered_map<ImGuiID, float> pending;
    const char* rawLabel = label;
    label = I18n::Tr(label);
    const ImGuiID id = ImGui::GetID(label);

    auto it = pending.find(id);
    float value = it != pending.end() ? it->second : (opt->has_value() ? opt->value() : def) * displayScale;
    bool changed = false;

    if (ImGui::SliderFloat(label, &value, mn * displayScale, mx * displayScale, fmt))
        pending[id] = value;

    if (ImGui::IsItemDeactivatedAfterEdit())
    {
        auto committed = pending.find(id);

        if (committed != pending.end())
        {
            const float displayValue = displayScale == 100.0f ? std::round(committed->second) : committed->second;
            *opt = std::clamp(displayValue / displayScale, mn, mx);
            pending.erase(committed);
            changed = true;
        }
    }

    ImGui::SameLine();

    const std::string resetId = std::string(I18n::Tr("Reset")) + "##" + rawLabel;
    if (ImGui::SmallButton(resetId.c_str()))
    {
        if (inheritReset)
            *opt = std::optional<float> {};
        else
            *opt = def;
        pending.erase(id);
        changed = true;
    }

    if (std::strcmp(rawLabel, "Intensity") == 0)
        HelpMarker("Enhancement strength. 1 = default.");
    else if (std::strcmp(rawLabel, "Local structure") == 0)
        HelpMarker("Fine detail and local contrast. 1 = default.");
    else if (std::strcmp(rawLabel, "Local tone") == 0)
        HelpMarker("Broad lighting changes. Later passes default to 0.");
    else if (std::strcmp(rawLabel, "Skin structure") == 0)
        HelpMarker("Skin detail. -1 follows Local structure.");
    return changed;
}

// An absent later-pass setting inherits pass 1. The first combo item represents that absence; the
// remaining items map directly to the model's zero-based profile values.
static bool InheritedProfileCombo(const char* label, CustomOptional<uint32_t, NoDefault>* opt, const char* const* names,
                                  int nameCount)
{
    label = I18n::Tr(label);
    int selected = 0;

    if (opt->has_value())
        selected = std::clamp((int) opt->value(), 0, nameCount - 2) + 1;

    if (!ImGui::Combo(label, &selected, names, nameCount))
        return false;

    if (selected == 0)
        *opt = std::optional<uint32_t> {};
    else
        *opt = (uint32_t) (selected - 1);

    return true;
}

static void SkinStatus(Config* config, unsigned pass)
{
    const auto effective = DlssNr::Profiles::PassTuning(*config, pass);
    if (!effective.autoMask)
        ImGui::TextWrapped("%s", I18n::Tr("Character mask is off: the model skin control may have no effect."));
    else if (config->DlssNrSkinIndependent.value_or_default())
    {
        ImGui::Text("%s: %.2f", I18n::Tr("Structure sent to model"), effective.structure);
        if (effective.skin < 0.0f)
            ImGui::TextWrapped("%s", I18n::Tr("Skin follows Structure (-1). Choose an explicit skin value for independent tuning."));
    }
}

static void PassChainControls(Config* config, unsigned pass)
{
    auto& options = config->DlssNrPassChain[pass];
    const bool legacyVk = DlssNr::IsRunningVk();
    ImGui::BeginDisabled(legacyVk);
    bool enabled = options.enabled.value_or(true);
    if (ImGui::Checkbox(I18n::Tr("Enable this pass"), &enabled))
    {
        options.enabled = enabled;
        LOG_INFO("DLSS-NR pass UI: pass {}, enabled {}", pass + 1, enabled);
    }
    ImGui::EndDisabled();
    const bool independent = !legacyVk && config->DlssNrIndependentPassResolution.value_or(config->DlssNrLaterPassScale.has_value());
    if (pass == 0 || independent)
    {
        const float inherited = config->DlssNrLaterPassScale.value_or(config->DlssNrWorkingScale.value_or_default());
        // Uses the same deferred commit as model tuning to avoid rebuilding while dragging.
        bool changed = pass == 0
            ? DeferredSlider("Pass resolution", &config->DlssNrWorkingScale, 0.25f, 2.0f, 1.0f, "%.0f%%", false, 100.0f)
            : DeferredSlider("Pass resolution", &options.scale, 0.25f, 2.0f, inherited, "%.0f%%", true, 100.0f);
        if (changed)
            LOG_INFO("DLSS-NR pass UI: pass {}, scale {}", pass + 1,
                     pass == 0 ? config->DlssNrWorkingScale.value_or_default() : options.scale.value_or(inherited));
        HelpMarker(I18n::Tr("50% halves width and height. 100% uses the full input size."));
    }
    else
        ImGui::TextDisabled(I18n::Tr("Resolution follows Pass 1"));
    ImGui::BeginDisabled(legacyVk);
    if (DeferredSlider("Model contribution", &options.blend, 0.0f, 1.0f, 1.0f, "%.0f%%", false, 100.0f))
        LOG_INFO("DLSS-NR pass UI: pass {}, blend {}", pass + 1, options.blend.value_or(1.0f));
    HelpMarker(I18n::Tr("100% uses this model result; 0% keeps its input. Pass 1 mixes with the game; later passes mix with the previous enabled pass. Disable a pass to skip its model cost."));
    ImGui::EndDisabled();
}

void RenderModel(Config* config, float menuResScale)
{
    if (DlssNr::IsRunningVk())
        ImGui::TextDisabled(I18n::Tr("Per-pass chain controls currently require DirectX 12 (including the DX11 bridge)."));
    ImGui::BeginDisabled(DlssNr::IsRunningVk());
    bool independent = config->DlssNrIndependentPassResolution.value_or(config->DlssNrLaterPassScale.has_value());
    if (ImGui::Checkbox(I18n::Tr("Enable independent later-pass resolution"), &independent))
    {
        config->DlssNrIndependentPassResolution = independent;
        LOG_INFO("DLSS-NR pass UI: independent resolution {}", independent);
    }
    HelpMarker(I18n::Tr("Off: every enabled pass follows Pass 1 resolution. On: each pass has its own resolution. Saved per-pass values are retained when off."));
    ImGui::EndDisabled();
    // Keep the UI simple; advanced INI pass settings remain available.
    constexpr int menuPassLimit = 5;
    {
        int passes = (int) std::clamp(config->DlssNrPasses.value_or_default(), 1u, (unsigned int) menuPassLimit);
        const auto text = ImGui::GetStyleColorVec4(ImGuiCol_Text);
        const float brightness = std::max({ text.x, text.y, text.z });
        const auto passColour = [&](int count)
        {
            return count == 1 ? ImVec4(brightness * 0.35f, brightness * 0.75f, brightness * 0.45f, text.w)
                              : ImVec4(brightness * 0.80f, brightness * 0.35f, brightness * 0.32f, text.w);
        };
        char passLabel[8];
        snprintf(passLabel, sizeof(passLabel), "%d", passes);
        ImGui::PushStyleColor(ImGuiCol_Text, passColour(passes));
        const bool open = ImGui::BeginCombo("##Model passes", passLabel);
        ImGui::PopStyleColor();
        if (open)
        {
            for (int count = 1; count <= menuPassLimit; ++count)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, passColour(count));
                snprintf(passLabel, sizeof(passLabel), "%d", count);
                if (ImGui::Selectable(passLabel, passes == count))
                    config->DlssNrPasses = (uint32_t) count;
                ImGui::PopStyleColor();
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(I18n::Tr("Model passes"));
    }

    bool independentSkin = config->DlssNrSkinIndependent.value_or_default();
    if (ImGui::Checkbox(I18n::Tr("Independent skin (all passes)"), &independentSkin))
        config->DlssNrSkinIndependent = independentSkin;
HelpMarker(I18n::Tr("With each pass's Character mask enabled, cap Structure sent to the model at 1. Skin keeps its own value. Structure below 1 can still affect faces. Off preserves the original parameters."));

    static const char* styleKeys[] = { "Standard", "Natural", "Cinematic" };
    static const char* inheritedKeys[] = { "Auto", "Standard", "Natural", "Cinematic" };
    const char* styles[IM_ARRAYSIZE(styleKeys)];
    const char* inheritedStyles[IM_ARRAYSIZE(inheritedKeys)];
    for (int i = 0; i < IM_ARRAYSIZE(styleKeys); ++i)
        styles[i] = I18n::Tr(styleKeys[i]);
    for (int i = 0; i < IM_ARRAYSIZE(inheritedKeys); ++i)
        inheritedStyles[i] = I18n::Tr(inheritedKeys[i]);

    if (ImGui::TreeNodeEx(I18n::Tr("Pass 1"), ImGuiTreeNodeFlags_DefaultOpen))
    {
        PassChainControls(config, 0);
        int style = (int) std::min(config->DlssNrStyle.value_or_default(), 2u);
        if (ImGui::Combo(I18n::Tr("Style"), &style, styles, IM_ARRAYSIZE(styles)))
            config->DlssNrStyle = (uint32_t) style;

        DeferredSlider("Intensity", &config->DlssNrIntensity, 0.0f, 2.0f, 1.0f);
        DeferredSlider("Local structure", &config->DlssNrLocalStructure, 0.0f, 2.0f, 1.0f);
        DeferredSlider("Local tone", &config->DlssNrLocalTone, 0.0f, 2.0f, 1.0f);
        DeferredSlider("Skin structure", &config->DlssNrSkinStructure, -1.0f, 2.0f, -1.0f);
        bool mask = config->DlssNrAutoMask.value_or_default();
        if (ImGui::Checkbox(I18n::Tr("Character mask (model)"), &mask))
            config->DlssNrAutoMask = mask;
HelpMarker(I18n::Tr("Model character selection, not the colour-based final-edit preview."));
        SkinStatus(config, 0);
        ImGui::TreePop();
    }

    if (config->DlssNrPasses.value_or_default() >= 2 && ImGui::TreeNodeEx(I18n::Tr("Pass 2"), ImGuiTreeNodeFlags_DefaultOpen))
    {
        PassChainControls(config, 1);
        InheritedProfileCombo("Style", &config->DlssNrPass2Style, inheritedStyles, IM_ARRAYSIZE(inheritedStyles));
        DeferredSlider("Intensity", &config->DlssNrPass2Intensity, 0.0f, 2.0f,
                       config->DlssNrIntensity.value_or_default(), "%.2f", true);
        DeferredSlider("Local structure", &config->DlssNrPass2LocalStructure, 0.0f, 2.0f,
                       config->DlssNrLocalStructure.value_or_default(), "%.2f", true);
        DeferredSlider("Local tone", &config->DlssNrPass2LocalTone, 0.0f, 2.0f, 0.0f, "%.2f", true);
        DeferredSlider("Skin structure", &config->DlssNrPass2SkinStructure, -1.0f, 2.0f,
                       config->DlssNrSkinStructure.value_or_default(), "%.2f", true);
        bool mask = config->DlssNrPass2AutoMask.has_value() ? config->DlssNrPass2AutoMask.value()
                                                            : config->DlssNrAutoMask.value_or_default();
        if (ImGui::Checkbox(I18n::Tr("Character mask (model)"), &mask))
            config->DlssNrPass2AutoMask = mask;
        ImGui::SameLine();
        if (ImGui::SmallButton(I18n::Tr("Reset##mask")))
            config->DlssNrPass2AutoMask = std::optional<bool> {};
        SkinStatus(config, 1);
        ImGui::TreePop();
    }

    // Pass 3 keeps its legacy per-key configuration; passes 4+ draw from the ExtraPasses table
    // (index 0 is pass 4, index 1 is pass 5).
    if (config->DlssNrPasses.value_or_default() >= 3 && ImGui::TreeNodeEx(I18n::Tr("Pass 3"), ImGuiTreeNodeFlags_DefaultOpen))
    {
        PassChainControls(config, 2);
        InheritedProfileCombo("Style", &config->DlssNrPass3Style, inheritedStyles, IM_ARRAYSIZE(inheritedStyles));
        DeferredSlider("Intensity", &config->DlssNrPass3Intensity, 0.0f, 2.0f,
                       config->DlssNrIntensity.value_or_default(), "%.2f", true);
        DeferredSlider("Local structure", &config->DlssNrPass3LocalStructure, 0.0f, 2.0f,
                       config->DlssNrLocalStructure.value_or_default(), "%.2f", true);
        DeferredSlider("Local tone", &config->DlssNrPass3LocalTone, 0.0f, 2.0f, 0.0f, "%.2f", true);
        DeferredSlider("Skin structure", &config->DlssNrPass3SkinStructure, -1.0f, 2.0f,
                       config->DlssNrSkinStructure.value_or_default(), "%.2f", true);
        bool mask = config->DlssNrPass3AutoMask.has_value() ? config->DlssNrPass3AutoMask.value()
                                                            : config->DlssNrAutoMask.value_or_default();
        if (ImGui::Checkbox(I18n::Tr("Character mask (model)"), &mask))
            config->DlssNrPass3AutoMask = mask;
        ImGui::SameLine();
        if (ImGui::SmallButton(I18n::Tr("Reset##mask")))
            config->DlssNrPass3AutoMask = std::optional<bool> {};
        SkinStatus(config, 2);
        ImGui::TreePop();
    }

    for (int extra = 0; extra < 2; ++extra)
    {
        const int passNumber = 4 + extra;
        if (config->DlssNrPasses.value_or_default() < (unsigned int) passNumber)
            continue;
        char nodeLabel[16];
        snprintf(nodeLabel, sizeof(nodeLabel), "Pass %d", passNumber);
        if (!ImGui::TreeNodeEx(I18n::Tr(nodeLabel), ImGuiTreeNodeFlags_DefaultOpen))
            continue;

        PassChainControls(config, passNumber - 1);
        auto& extraPass = config->DlssNrExtraPasses[extra];
        InheritedProfileCombo("Style", &extraPass.style, inheritedStyles, IM_ARRAYSIZE(inheritedStyles));
        DeferredSlider("Intensity", &extraPass.intensity, 0.0f, 2.0f,
                       config->DlssNrIntensity.value_or_default(), "%.2f", true);
        DeferredSlider("Local structure", &extraPass.structure, 0.0f, 2.0f,
                       config->DlssNrLocalStructure.value_or_default(), "%.2f", true);
        DeferredSlider("Local tone", &extraPass.tone, 0.0f, 2.0f, 0.0f, "%.2f", true);
        DeferredSlider("Skin structure", &extraPass.skin, -1.0f, 2.0f,
                       config->DlssNrSkinStructure.value_or_default(), "%.2f", true);
        bool mask = extraPass.autoMask.has_value() ? extraPass.autoMask.value()
                                                   : config->DlssNrAutoMask.value_or_default();
        if (ImGui::Checkbox(I18n::Tr("Character mask (model)"), &mask))
            extraPass.autoMask = mask;
        ImGui::SameLine();
        ImGui::PushID(extra);
        if (ImGui::SmallButton(I18n::Tr("Reset##mask")))
            extraPass.autoMask = std::optional<bool> {};
        ImGui::PopID();
        SkinStatus(config, passNumber - 1);
        ImGui::TreePop();
    }

}

} // namespace DlssNr::MenuSections
