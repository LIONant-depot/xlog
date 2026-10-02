#ifndef XLOG_BADGE_H
#define XLOG_BADGE_H
#pragma once

// The Logs badge of the closed drawer (documentation/Editors/DESIGN_logs.md, 6.2): "Logs  2 errors  1 new", a quiet item at the bottom right of the window. It counts distinct
// problems, not occurrences, and what was acknowledged or muted no longer counts; a Fatal one is counted whatever was done to it. Clicking it opens the Logs.
#include "dependencies/xlog/source/xlog_view.h"

#include "imgui.h"

#include <functional>

namespace xlog
{
    // True when it was clicked. Draws nothing when nothing needs attention.
    inline bool RenderBadge(const hub& Hub, view_state& State, const std::function<void()>& OnOpen) noexcept
    {
        State.m_BadgeAt[0] = State.m_BadgeAt[1] = -1.0f;
        if (Hub.Revision() != State.m_BadgeRevision) { State.m_Badge = CountBadge(Hub); State.m_BadgeRevision = Hub.Revision(); }
        const badge_counts& C = State.m_Badge;
        if (!C.Any()) return false;

        std::string Text = "Logs";
        if (C.m_Critical) Text += std::format("  {} critical", C.m_Critical);
        if (C.m_Errors)   Text += std::format("  {} error{}", C.m_Errors, C.m_Errors == 1 ? "" : "s");
        if (C.m_Warnings) Text += std::format("  {} warning{}", C.m_Warnings, C.m_Warnings == 1 ? "" : "s");
        if (C.m_New)      Text += std::format("  {} new", C.m_New);

        ImGuiViewport* pVp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(pVp->Pos.x + pVp->Size.x - 12.0f, pVp->Pos.y + pVp->Size.y - 8.0f), ImGuiCond_Always, ImVec2(1.0f, 1.0f));
        ImGui::SetNextWindowBgAlpha(0.88f);
        const ImGuiWindowFlags Flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking
                                     | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoScrollbar;
        bool bClicked = false;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 4.0f));
        if (ImGui::Begin("##xlog.badge", nullptr, Flags))
        {
            // the colour only on the dot: severity is a glyph, not a fill
            const ImVec2 At = ImGui::GetCursorScreenPos();
            const ImU32 Dot = (C.m_Critical || C.m_Errors) ? IM_COL32(212, 107, 105, 255) : IM_COL32(201, 160, 74, 255);
            ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(At.x + 4.0f, At.y + ImGui::GetTextLineHeight() * 0.5f), 4.0f, Dot);
            ImGui::Dummy(ImVec2(14.0f, 1.0f));
            ImGui::SameLine(0, 0);
            ImGui::TextUnformatted(Text.c_str());
            if (ImGui::IsWindowHovered()) { ImGui::SetMouseCursor(ImGuiMouseCursor_Hand); ImGui::SetTooltip("Open the Logs"); }
            if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) bClicked = true;
            State.m_BadgeAt[0] = ImGui::GetWindowPos().x + ImGui::GetWindowSize().x * 0.5f;
            State.m_BadgeAt[1] = ImGui::GetWindowPos().y + ImGui::GetWindowSize().y * 0.5f;
        }
        ImGui::End();
        ImGui::PopStyleVar();
        if (bClicked && OnOpen) OnOpen();
        return bClicked;
    }
}

#endif // XLOG_BADGE_H
