#include "pch.h"

#include "DlssNr.h"
#include "DlssNr_PipelineUi.h"
#include "DlssNr_Upscaler.h"
#include "DlssNr_MenuSections.h"
#include "DlssNr_Placement.h"
#include <Config.h>
#include <menu/menu_common.h>
#include <algorithm>
#include <cmath>
#include <Localization.h>

namespace DlssNr
{

CenterNode& CenterSelected()
{
    static CenterNode selected = CenterNode::NrModel;
    return selected;
}

// Master toggles shown at the top of the NR center window.
void RenderCenterToggles(Config* config, float menuResScale)
{
    using namespace MenuSections;
    ImGui::Spacing();
    const float toggleGap = ImGui::GetStyle().ItemSpacing.x;
    const float toggleWidth = (ImGui::GetContentRegionAvail().x - toggleGap) * 0.5f;
    const float toggleRight = ImGui::GetCursorPosX() + toggleWidth + toggleGap;
    bool enabled = config->DlssNrEnabled.value_or_default();
    if (PipelineUi::CheckboxWrapped(I18n::Tr("Enable Neural Rendering"), &enabled, toggleWidth))
    {
        config->DlssNrEnabled = enabled;
        if (enabled && !config->DlssNrRunBeforeSr.has_value())
            config->DlssNrRunBeforeSr = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(I18n::Tr("Enable NR processing."));

    bool applyModel = config->DlssNrApplyModel.value_or_default();
    if (PipelineUi::CheckboxWrapped(I18n::Tr("Apply model"), &applyModel, toggleWidth))
        config->DlssNrApplyModel = applyModel;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(I18n::Tr("Show or hide the NR effect. The model still runs when hidden.\nDisable Enable Neural ""Rendering to stop its GPU cost."));

    bool finished = config->DlssNrFinishedPicture.value_or_default();
    auto placement = ResolvePlacement(config->DlssNrRunBeforeSr.value_or_default(),
                                      config->DlssNrDeferredDlss.value_or_default(),
                                      config->DlssNrResidualAcrossRr.value_or_default(), finished);
    bool generateBefore = placement.beforeUpscale;
    ImGui::SameLine(toggleRight);
    ImGui::BeginDisabled(placement.deferred);
    if (PipelineUi::CheckboxWrapped(I18n::Tr("Generate model before upscale"), &generateBefore, toggleWidth))
    {
        config->DlssNrRunBeforeSr = generateBefore;
        config->DlssNrResidualAcrossRr = false;
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(placement.deferred ? I18n::Tr("The separate-edit path always generates before upscale.")
                                             : I18n::Tr("Run NR before the game's upscaler, including RR."));

    if (PipelineUi::CheckboxWrapped(I18n::Tr("Apply NR to the finished picture"), &finished, toggleWidth))
    {
        config->DlssNrFinishedPicture = finished;
        DlssNr::RetryAfterFailure();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(I18n::Tr("Apply NR after game effects and HUD. Early generation carries the edit through a separate upscaler."));

    placement = ResolvePlacement(config->DlssNrRunBeforeSr.value_or_default(),
                                 config->DlssNrDeferredDlss.value_or_default(),
                                 config->DlssNrResidualAcrossRr.value_or_default(), finished);
    ImGui::SameLine(toggleRight);
    bool deferred = placement.deferred;
    if (PipelineUi::CheckboxWrapped(I18n::Tr("Generate before upscale, apply after upscale"), &deferred, toggleWidth))
    {
        config->DlssNrDeferredDlss = deferred;
        config->DlssNrResidualAcrossRr = false; // Clear the legacy alias when the unified option changes.
        if (deferred || finished)
            config->DlssNrRunBeforeSr = deferred;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(I18n::Tr("Keep the game's SR/RR input clean and upscale only the NR edit with a separate non-RR backend.""\nApply after upscale, or at presentation when finished-picture mode is enabled."));
    ImGui::Spacing();

    placement = ResolvePlacement(config->DlssNrRunBeforeSr.value_or_default(),
                                 config->DlssNrDeferredDlss.value_or_default(),
                                 config->DlssNrResidualAcrossRr.value_or_default(), finished);
    const auto feature = State::Instance().currentFeature;
    const bool nativePrivateVk = feature && feature->Api() == API::Vulkan && !feature->IsWithDx12();
    if (placement.deferred && !nativePrivateVk)
    {
        int backend = (int) GetPrivateUpscaler(config->DlssNrPrivateUpscaler.value_or_default());
        if (ImGui::Combo(I18n::Tr("Private NR upscaler"), &backend, "DLSS\0FSR 2.2\0FSR (FidelityFX)\0XeSS\0"))
            config->DlssNrPrivateUpscaler = backend;
        HelpMarker("Upscales only the NR edit, with or without game RR. FSR (FidelityFX) and XeSS need their runtimes.");
    }
}

void RenderCenterSection(Config* config, float menuResScale, CenterNode node)
{
    using namespace MenuSections;
    ImGui::PushItemWidth(std::min(220.0f * menuResScale, ImGui::GetContentRegionAvail().x * 0.6f));
    switch (node)
    {
    case CenterNode::Upscale:
    case CenterNode::FrameGen:
        break; // Rendered by the host menu (needs full context).
    case CenterNode::NrInput:
        RenderInput(config, menuResScale);
        break;
    case CenterNode::NrModel:
        RenderModel(config, menuResScale);
        break;
    case CenterNode::NrBlend:
        RenderBlend(config, menuResScale);
        break;
    case CenterNode::NrPlacement:
        RenderPlacement(config, menuResScale);
        break;
    case CenterNode::Status:
        RenderStatus(config, menuResScale);
        if (ImGui::CollapsingHeader(I18n::Tr("Inspect NR")))
            RenderInspect(config, menuResScale);
        break;
    }
    ImGui::PopItemWidth();
}

} // namespace DlssNr
