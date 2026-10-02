#ifndef XLOG_TAB_H
#define XLOG_TAB_H
#pragma once

// The Log tab: the events of a hub as a list, for any editor to embed (a drawer tab, a panel of its own, a section of a window). It is a view over the
// store, not a store of its own: nothing is kept here, and "Clear view" only hides what is listed; the events, problems and operations stay in the hub.
//
//      xlog::RenderTab(*xlog::hub::current(), { .m_ChannelPrefix = "game." });       // on the host thread, between ImGui::Begin and End
//
// Needs ImGui and nothing else of the editor. The hub's store is read on the host thread (the thread that drains it), which is where ImGui runs.
#include "xlog_hub.h"

#include "imgui.h"

namespace xlog
{
    struct tab_options
    {
        std::string_view m_ChannelPrefix;              // only the events whose channel starts with this ("game.", "asset.compile."); empty = every channel
        std::uint64_t    m_Window = 20000;             // how many of the most recent events are looked at (the view stays O(1) in the size of the store)
        bool             m_bShowCommands = false;      // the audit trail of commands is in the Commands tab; list it here only when asked
    };

    inline void RenderTab( hub& Hub, const tab_options& Options = {} ) noexcept
    {
        ImGui::PushID("xlog.tab");
        ImGuiStorage* pStorage = ImGui::GetStateStorage();
        const ImGuiID FromId = ImGui::GetID("from");

        if (ImGui::SmallButton("Clear view")) pStorage->SetInt(FromId, static_cast<int>(Hub.Committed()));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Hides what is listed here. The events stay in the Logs.");
        ImGui::Separator();

        // A child window of its own: build output has lines far wider than a panel.
        if (ImGui::BeginChild("rows", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar))
        {
            const std::uint64_t Committed = Hub.Committed();
            const std::uint64_t From = std::max<std::uint64_t>(static_cast<std::uint64_t>(pStorage->GetInt(FromId, 0)), Committed > Options.m_Window ? Committed - Options.m_Window : 0);
            const ImVec4 Text    = ImGui::GetStyleColorVec4(ImGuiCol_Text);
            const ImVec4 Muted   = ImVec4(0.55f, 0.55f, 0.55f, 1.0f);       // the editor's theme is muted: severity is a tint of the text, not a fill
            const ImVec4 Error   = ImVec4(0.83f, 0.42f, 0.41f, 1.0f);
            const ImVec4 Warning = ImVec4(0.79f, 0.63f, 0.29f, 1.0f);

            Hub.ForEachEvent(From, Committed, [&](const event& E)
            {
                if (!Options.m_ChannelPrefix.empty() && std::string_view(E.m_Channel).substr(0, Options.m_ChannelPrefix.size()) != Options.m_ChannelPrefix) return true;
                if (E.m_Kind == kind::Command && !Options.m_bShowCommands) return true;
                const bool bErr = E.m_Severity >= severity::Error, bWarn = E.m_Severity == severity::Warning;
                ImGui::PushStyleColor(ImGuiCol_Text, bErr ? Error : bWarn ? Warning : (E.m_Kind == kind::Log ? Text : Muted));
                ImGui::TextUnformatted(E.m_Title.c_str());
                if (!E.m_Body.empty())              // the whole body under its title: one event, many lines
                {
                    ImGui::Indent(16.0f);
                    ImGui::TextUnformatted(E.m_Body.c_str());
                    ImGui::Unindent(16.0f);
                }
                ImGui::PopStyleColor();
                return true;
            });
        }
        ImGui::EndChild();
        ImGui::PopID();
    }
}

#endif // XLOG_TAB_H
