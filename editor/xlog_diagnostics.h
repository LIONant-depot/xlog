#ifndef XLOG_DIAGNOSTICS_H
#define XLOG_DIAGNOSTICS_H
#pragma once

// The diagnostics view: what the last operation of a kind (an asset's compile) said about one subject, generic and easy to read, for any editor to embed
// (documentation/Editors/DESIGN_logs.md, section 6.10). No editor writes its own error UI: it names the asset it edits and reacts to the references
// the view hands back (open a file at a line); the compiler's messages arrive through the pipeline adapter as events of an "asset.compile" operation.
//
//      xlog::RenderDiagnostics(*xlog::hub::current(), { .m_Subject = AssetRef, .m_pLiveErrors = &Model.m_ValidationErrors });
//
// Reads only the store, on the host thread. Uses the same glyphs and rules as the Logs window so the two never disagree.
#include "xlog_tab.h"

namespace xlog
{
    struct diagnostics_options
    {
        ref                         m_Subject;                              // the asset (type Asset, its instance id as m_Id)
        std::string_view            m_OperationKind = "asset.compile";
        const std::vector<std::string>* m_pLiveErrors = nullptr;            // what the editor knows right now without a compile (descriptor validation): shown first
        std::function<void(const ref&)>             m_OnOpen;               // "Open source"
        std::function<void(std::uint64_t)>          m_OnOpenInLogs;         // "Open in Logs", given the operation id (0 = none yet)
    };

    // The newest operation of the kind that is about the subject; null when there has been none.
    inline const operation* FindLatestOperation(const hub& Hub, std::string_view Kind, const ref& Subject) noexcept
    {
        const auto& Order = Hub.OperationOrder();
        for (auto It = Order.rbegin(); It != Order.rend(); ++It)
        {
            const operation* O = Hub.FindOperation(*It);
            if (!O || O->m_Kind != Kind) continue;
            if (O->m_Subject.m_Type == Subject.m_Type && (Subject.m_Id ? O->m_Subject.m_Id == Subject.m_Id : O->m_Subject.m_Path == Subject.m_Path)) return O;
        }
        return nullptr;
    }

    // One line saying what happened: the colour of the Feedback button is this state, not a second source of truth.
    inline std::string DescribeOperation(const hub& Hub, const operation* pOp) noexcept
    {
        if (!pOp) return "Not compiled in this session";
        const double Seconds = static_cast<double>((pOp->m_Ended ? pOp->m_Ended : Hub.Now()) - pOp->m_Started) / 1.0e9;
        const std::uint64_t Age = pOp->m_Ended && Hub.Now() > pOp->m_Ended ? Hub.Now() - pOp->m_Ended : 0;
        auto Counts = [&] { return std::format("{} error{} · {} warning{}", pOp->m_Errors, pOp->m_Errors == 1 ? "" : "s", pOp->m_Warnings, pOp->m_Warnings == 1 ? "" : "s"); };
        switch (pOp->m_Outcome)
        {
        case outcome::Running:   return std::format("Compiling... {:.0f} s", Seconds);
        case outcome::Succeeded: return pOp->m_Warnings ? std::format("Built in {:.1f} s · {} · {} ago", Seconds, Counts(), details::Elapsed(Age)) : std::format("Built in {:.1f} s · {} ago", Seconds, details::Elapsed(Age));
        case outcome::Failed:    return std::format("Build failed · {} · {:.1f} s · {} ago", Counts(), Seconds, details::Elapsed(Age));
        case outcome::Cancelled: return "Build cancelled";
        default:                 return "Build abandoned (the editor stopped before it finished)";
        }
    }

    // One problem of the operation: glyph, code + title (wrapped to the panel), subject; the rest of its event expands under it.
    inline void DiagnosticRow(hub& Hub, const diagnostics_options& Options, const problem& P) noexcept
    {
        ImGui::PushID(static_cast<int>(P.m_Id & 0x7FFFFFFF));
        ImGuiStorage* pStorage = ImGui::GetStateStorage();
        const ImGuiID OpenId = ImGui::GetID("open");
        const event* pLast = Hub.FindEvent(P.m_LastSeq);
        const bool bBody = pLast && (!pLast->m_Body.empty() || pLast->m_Source.Valid());
        bool bOpen = pStorage->GetBool(OpenId, false);

        if (bBody) { if (ImGui::ArrowButton("##open", bOpen ? ImGuiDir_Down : ImGuiDir_Right)) { bOpen = !bOpen; pStorage->SetBool(OpenId, bOpen); } }
        else ImGui::Dummy(ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()));
        const float Hang = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x + 16.0f;          // what the lines under the title are indented by
        ImGui::SameLine();
        const ImVec2 At = ImGui::GetCursorScreenPos();
        details::DrawGlyph(ImGui::GetWindowDrawList(), ImVec2(At.x + 6.0f, At.y + ImGui::GetTextLineHeight() * 0.5f + 2.0f), P.m_Severity);
        ImGui::Dummy(ImVec2(16.0f, 1.0f));
        ImGui::SameLine(0, 0);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(((P.m_Code.empty() ? std::string() : P.m_Code + "  ") + P.m_Title).c_str());
        ImGui::PopTextWrapPos();
        ImGui::Indent(Hang);
        if (P.m_Count > 1) { ImGui::SameLine(); ImGui::TextDisabled("x%llu", static_cast<unsigned long long>(P.m_Count)); }
        const std::string Where = details::Where(P);
        if (!Where.empty()) { ImGui::TextDisabled("%s", Where.c_str()); }
        if (bOpen && pLast)
        {
            if (pLast->m_Source.Valid())
            {
                ImGui::TextDisabled("Source"); ImGui::SameLine(); ImGui::TextUnformatted(details::Where(pLast->m_Source).c_str());
                ImGui::SameLine(); if (ImGui::SmallButton(Options.m_OnOpen ? "Open source" : "Copy location")) { if (Options.m_OnOpen) Options.m_OnOpen(pLast->m_Source); else ImGui::SetClipboardText(details::Where(pLast->m_Source).c_str()); }
            }
            if (!pLast->m_Body.empty()) details::Body("body", pLast->m_Body, 180.0f);
        }
        ImGui::Unindent(Hang);
        ImGui::PopID();
    }

    inline void RenderDiagnostics(hub& Hub, const diagnostics_options& Options) noexcept
    {
        ImGui::PushID("xlog.diagnostics");
        const operation* pOp = FindLatestOperation(Hub, Options.m_OperationKind, Options.m_Subject);

        // State first
        const std::string State = DescribeOperation(Hub, pOp);
        ImVec4 Tint = ImGui::GetStyleColorVec4(ImGuiCol_Text);
        if (pOp && pOp->m_Outcome == outcome::Failed) Tint = ImGui::ColorConvertU32ToFloat4(details::SeverityColor(severity::Error));
        else if (pOp && pOp->m_Outcome == outcome::Succeeded && pOp->m_Warnings) Tint = ImGui::ColorConvertU32ToFloat4(details::SeverityColor(severity::Warning));
        ImGui::PushStyleColor(ImGuiCol_Text, Tint);
        ImGui::TextUnformatted(State.c_str());
        ImGui::PopStyleColor();
        if (pOp && !pOp->m_bEvidenceReady && pOp->m_Outcome != outcome::Running) ImGui::TextDisabled("The output is still being collected.");

        ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::CalcTextSize("Open in Logs  Copy").x - ImGui::GetStyle().FramePadding.x * 4 - ImGui::GetStyle().ItemSpacing.x);
        if (Options.m_OnOpenInLogs && ImGui::SmallButton("Open in Logs")) Options.m_OnOpenInLogs(pOp ? pOp->m_Id : 0);
        ImGui::SameLine();
        std::string Pack = State + "\n";
        if (ImGui::SmallButton("Copy"))
        {
            if (Options.m_pLiveErrors) for (const auto& E : *Options.m_pLiveErrors) Pack += "error: " + E + "\n";
            if (pOp) for (auto Id : pOp->m_Problems) if (const problem* P = Hub.FindProblem(Id)) Pack += details::ContextPack(Hub, *P) + "\n";
            ImGui::SetClipboardText(Pack.c_str());
        }
        ImGui::Separator();

        // What the editor knows right now
        if (Options.m_pLiveErrors && !Options.m_pLiveErrors->empty())
        {
            ImGui::TextDisabled("Descriptor");
            for (const auto& E : *Options.m_pLiveErrors)
            {
                const ImVec2 At = ImGui::GetCursorScreenPos();
                details::DrawGlyph(ImGui::GetWindowDrawList(), ImVec2(At.x + 6.0f, At.y + ImGui::GetTextLineHeight() * 0.5f + 1.0f), severity::Error);
                ImGui::Indent(18.0f); ImGui::PushTextWrapPos(0.0f); ImGui::TextUnformatted(E.c_str()); ImGui::PopTextWrapPos(); ImGui::Unindent(18.0f);
            }
            ImGui::Separator();
        }

        // The operation's problems: errors first, warnings collapsed to a count until asked
        if (pOp)
        {
            std::vector<const problem*> Errors, Warnings;
            for (auto Id : pOp->m_Problems)
                if (const problem* P = Hub.FindProblem(Id)) (P->m_Severity >= severity::Error ? Errors : Warnings).push_back(P);
            for (const problem* P : Errors) DiagnosticRow(Hub, Options, *P);
            if (!Warnings.empty())
            {
                ImGui::PushID("warnings");
                ImGuiStorage* pStorage = ImGui::GetStateStorage();
                const ImGuiID ShowId = ImGui::GetID("show");
                bool bShow = pStorage->GetBool(ShowId, Errors.empty());          // with no errors the warnings are the news: shown
                if (ImGui::SmallButton(std::format("{} {} warning{}###warn", bShow ? "Hide" : "Show", Warnings.size(), Warnings.size() == 1 ? "" : "s").c_str())) { bShow = !bShow; pStorage->SetBool(ShowId, bShow); }
                if (bShow) for (const problem* P : Warnings) DiagnosticRow(Hub, Options, *P);
                ImGui::PopID();
            }
            if (Errors.empty() && Warnings.empty() && pOp->m_Outcome == outcome::Succeeded) ImGui::TextDisabled("Nothing to report.");
        }
        else if (!(Options.m_pLiveErrors && !Options.m_pLiveErrors->empty())) ImGui::TextDisabled("Compile the resource to see what the compiler says.");
        ImGui::PopID();
    }
}

#endif // XLOG_DIAGNOSTICS_H
