#pragma once

#include <imgui/imgui.h>

class Config;

namespace DlssNr::MenuSections
{
// Compact contextual help, matching the rest of the menu.
inline void HelpMarker(const char* tip)
{
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");

    if (ImGui::IsItemHovered())
    {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 40.0f);
        ImGui::TextUnformatted(tip);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}


void RenderPlacement(Config* config, float menuResScale);
void RenderStatus(Config* config, float menuResScale);
void RenderInput(Config* config, float menuResScale);
void RenderModel(Config* config, float menuResScale);
void RenderBlend(Config* config, float menuResScale);
void RenderInspect(Config* config, float menuResScale);
} // namespace DlssNr::MenuSections

namespace DlssNr
{
// Nodes of the standalone NR center window. The left rail shows these as short
// vertical buttons; the right side renders the matching configuration panel.
enum class CenterNode
{
    NrInput,
    Upscale,
    NrModel,
    NrBlend,
    NrPlacement,
    FrameGen,
    Status
};

CenterNode& CenterSelected();
void RenderCenterToggles(Config* config, float menuResScale);
void RenderCenterSection(Config* config, float menuResScale, CenterNode node);
} // namespace DlssNr
