#ifndef XLOG_TAB_H
#define XLOG_TAB_H
#pragma once

// The Logs window: Problems | Events, for any editor to embed (the host drawer's tab, a panel of its own, a section of a window). A view over the
// store, not a store of its own (documentation/Editors/DESIGN_logs.md, section 6):
//
//      xlog::RenderTab(*xlog::hub::current(), { .m_pState = &MyState });       // on the host thread, between ImGui::Begin and End
//
// Problems is the state: one row per identity, worst first, details inline under the row. Events is the evidence: one line per event (virtualized),
// the whole body of the selected one in the strip below. The query bar is the pipe's grammar, so what the window lists is what LogProblems/LogEvents return.
// Needs ImGui and nothing else of the editor; what only the host knows (opening a file at a line, running an undoable command) comes in as callbacks.
#include "dependencies/xlog/source/xlog_view.h"

#include "imgui.h"
// The editors' one search box (header-only, ImGui only): the Logs' query bar is not a second kind of box. xlog's tab is a light consumer of xeditor's widgets.
#include "dependencies/xeditor/include/xeditor/widgets.h"

#include <functional>

namespace xlog
{
    struct tab_options
    {
        std::string_view m_ChannelPrefix;              // only the problems/events whose channel starts with this ("game.", "asset.compile."); empty = every channel
        std::uint64_t    m_Window = 20000;             // how many of the most recent events are looked at (the view stays O(1) in the size of the store)
        bool             m_bShowCommands = false;      // the audit trail of commands is in the Commands tab; list it here only when asked
        view_state*      m_pState = nullptr;           // the owner's, so the selection and the query survive; null = one of the window's own
        std::function<void(const ref&)>          m_OnOpen;   // "Open source": the host knows how to open a file at a line, an asset, an entity
        std::function<void()>                    m_OnForward; // Forward: the host puts the drawer back on the Logs; null = only the window goes forward
        std::function<void()>                    m_OnBack;   // Back: the host restores what it had in front too (the drawer's tab); null = only the window goes back
        std::function<void(const std::string&)>  m_Run;      // runs a command line as an edit of the host's history (acknowledge and mute are undoable); null = applied directly
    };

    namespace details
    {
        inline ImU32 SeverityColor(severity S) noexcept
        {
            // the editor's theme is muted: colour only on the glyph, the edge and the counts
            if (S >= severity::Error)   return IM_COL32(212, 107, 105, 255);
            if (S == severity::Warning) return IM_COL32(201, 160, 74, 255);
            return IM_COL32(140, 140, 140, 255);
        }

        inline void DrawGlyph(ImDrawList* pList, ImVec2 C, severity S) noexcept
        {
            const ImU32 Col = SeverityColor(S);
            if (S >= severity::Error)
            {
                pList->AddCircleFilled(C, 4.0f, Col);
                if (S == severity::Fatal) pList->AddCircle(C, 6.5f, Col, 0, 1.5f);
            }
            else if (S == severity::Warning) pList->AddTriangleFilled(ImVec2(C.x, C.y - 4.5f), ImVec2(C.x + 4.5f, C.y + 3.5f), ImVec2(C.x - 4.5f, C.y + 3.5f), Col);
            else pList->AddCircle(C, 3.0f, Col, 0, 1.0f);
        }

        inline std::string Elapsed(std::uint64_t Ns) noexcept
        {
            const std::uint64_t S = Ns / 1000000000ull;
            if (S < 90) return std::format("{}s", S);
            if (S < 5400) return std::format("{}m", S / 60);
            return std::format("{}h", S / 3600);
        }

        inline std::string Clock(std::uint64_t Ns) noexcept
        {
            const std::uint64_t Ms = Ns / 1000000ull;
            return std::format("{:02}:{:02}.{:03}", Ms / 60000, (Ms / 1000) % 60, Ms % 1000);
        }

        inline std::string Leaf(std::string_view Path) noexcept
        {
            const auto Slash = Path.find_last_of("/\\");
            return std::string(Slash == std::string_view::npos ? Path : Path.substr(Slash + 1));
        }

        inline std::string Where(const ref& R) noexcept
        {
            if (!R.Valid()) return {};
            std::string Text = R.m_Type == ref::type::File ? Leaf(R.m_Path) : R.m_Path;
            if (R.m_Line > 0) Text += std::format(":{}", R.m_Line);
            return Text;
        }

        // Where a problem is: its site (file:line) first, else what it is about, else the thing that tells it apart.
        inline std::string Where(const problem& P) noexcept
        {
            if (P.m_Site.Valid()) return Where(P.m_Site);
            if (P.m_Subject.Valid()) return Where(P.m_Subject);
            return P.m_Discriminator;
        }

        // "Play > Load Level > Compile 37 shaders", innermost last
        inline std::string Breadcrumb(const hub& Hub, std::uint64_t OperationId) noexcept
        {
            std::vector<const operation*> Chain;
            for (std::uint64_t Id = OperationId; Id && Chain.size() < 6; )
            {
                const operation* O = Hub.FindOperation(Id);
                if (!O) break;
                Chain.push_back(O);
                Id = O->m_Parent;
            }
            std::string Text;
            for (auto It = Chain.rbegin(); It != Chain.rend(); ++It)
            {
                if (!Text.empty()) Text += " > ";
                Text += (*It)->m_Title.empty() ? (*It)->m_Kind : (*It)->m_Title;
            }
            if (!Chain.empty()) Text += std::format("  ({}, #{})", OutcomeName(Chain.front()->m_Outcome), Chain.front()->m_Id);
            return Text;
        }

        // Text for a person or an AI to paste: what failed, where, how often, and the last occurrence in full.
        inline std::string ContextPack(const hub& Hub, const problem& P) noexcept
        {
            std::string Out = std::format("{}{}\n", P.m_Code.empty() ? "" : P.m_Code + "  ", P.m_Title);
            Out += std::format("severity={} occurrences={} channel={} producer={}\n", SeverityName(P.m_Severity), P.m_Count, P.m_Channel, P.m_Producer);
            if (const std::string W = Where(P); !W.empty()) Out += std::format("where={}\n", W);
            if (P.m_LastOperation) Out += std::format("operation={}\n", Breadcrumb(Hub, P.m_LastOperation));
            if (const event* E = Hub.FindEvent(P.m_LastSeq); E && !E->m_Body.empty()) Out += E->m_Body + "\n";
            return Out;
        }

        // Runs an annotation as the host's undoable command when it gave a runner; otherwise sets it on the hub directly.
        inline void Annotate(hub& Hub, const tab_options& Options, const char* pCommand, std::uint64_t Id, bool bValue, void (hub::*Direct)(std::uint64_t, bool) noexcept) noexcept
        {
            if (Options.m_Run) Options.m_Run(std::format("{} -Id {} -Value {}", pCommand, Hex16(Id), bValue ? "true" : "false"));
            else (Hub.*Direct)(Id, bValue);
        }

        inline void Open(const tab_options& Options, const ref& R) noexcept
        {
            if (Options.m_OnOpen && R.Valid()) Options.m_OnOpen(R);
            else if (R.Valid()) ImGui::SetClipboardText(Where(R).c_str());       // nothing to open it with: at least the place is on the clipboard
        }

        // A block of lines (a compiler's notes, a stack): wrapped to the panel, bounded in height, scrollable.
        inline void Body(const char* pId, const std::string& Text, float MaxHeight) noexcept
        {
            if (Text.empty()) return;
            const int Lines = 1 + static_cast<int>(std::count(Text.begin(), Text.end(), '\n'));
            const float Height = std::min(MaxHeight, ImGui::GetTextLineHeightWithSpacing() * static_cast<float>(Lines) + ImGui::GetStyle().WindowPadding.y * 2.0f);
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
            if (ImGui::BeginChild(pId, ImVec2(0, Height), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoNavInputs))
            {
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextUnformatted(Text.c_str());
                ImGui::PopTextWrapPos();
            }
            ImGui::EndChild();
            ImGui::PopStyleColor();
        }

        inline void ProblemDetails(hub& Hub, view_state& State, const tab_options& Options, const problem& P) noexcept
        {
            const annotation A = Hub.Annotation(P.m_Id);
            ImGui::PushID(static_cast<int>(P.m_Id & 0x7FFFFFFF));
            ImGui::Indent(26.0f);
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(P.m_Title.c_str());
            ImGui::PopTextWrapPos();
            ImGui::TextDisabled("%s%s%s  ·  %s  ·  %s%s", P.m_Code.c_str(), P.m_Code.empty() ? "" : "  ·  ", P.m_Producer.c_str(), P.m_Channel.c_str()
                , A.m_bAcknowledged ? "Acknowledged" : "Unreviewed", A.m_bMuted ? "  ·  Muted" : "");
            if (P.m_bHeuristic) ImGui::TextDisabled("Heuristic grouping: the producer gave no stable code, so numbers and names in the text are not part of the identity.");

            // Where
            if (P.m_Site.Valid() || P.m_Subject.Valid())
            {
                if (P.m_Site.Valid())
                {
                    ImGui::TextDisabled("Source"); ImGui::SameLine();
                    ImGui::TextUnformatted(Where(P.m_Site).c_str());
                    if (ImGui::IsItemHovered() && !P.m_Site.m_Path.empty()) ImGui::SetTooltip("%s", P.m_Site.m_Path.c_str());
                    ImGui::SameLine();
                    if (ImGui::SmallButton(Options.m_OnOpen ? "Open source" : "Copy location")) Open(Options, P.m_Site);
                }
                if (P.m_Subject.Valid())
                {
                    ImGui::TextDisabled("About"); ImGui::SameLine();
                    ImGui::TextUnformatted(Where(P.m_Subject).c_str());
                    if (Options.m_OnOpen && P.m_Subject.m_Type != ref::type::File) { ImGui::SameLine(); if (ImGui::SmallButton("Open")) Open(Options, P.m_Subject); }
                }
            }
            if (P.m_LastOperation)
            {
                ImGui::TextDisabled("During"); ImGui::SameLine();
                ImGui::PushTextWrapPos(0.0f); ImGui::TextUnformatted(Breadcrumb(Hub, P.m_LastOperation).c_str()); ImGui::PopTextWrapPos();
            }

            // Evidence
            const auto Retained = P.m_First.size() + P.m_Last.size();
            ImGui::TextDisabled("Evidence"); ImGui::SameLine();
            ImGui::Text("%llu observed, %llu retained%s", static_cast<unsigned long long>(P.m_Count), static_cast<unsigned long long>(Retained), Retained < P.m_Count ? ", the rest summarized" : "");
            if (const event* E = Hub.FindEvent(P.m_LastSeq); E && !E->m_Body.empty()) Body("body", E->m_Body, 200.0f);
            else if (!Hub.FindEvent(P.m_LastSeq)) ImGui::TextDisabled("The last occurrence has expired from the store.");

            // Actions
            if (ImGui::SmallButton(A.m_bAcknowledged ? "Unacknowledge" : "Acknowledge")) Annotate(Hub, Options, "LogAcknowledge", P.m_Id, !A.m_bAcknowledged, &hub::SetAcknowledged);
            ImGui::SameLine();
            ImGui::BeginDisabled(P.m_Severity >= severity::Fatal && !A.m_bMuted);
            if (ImGui::SmallButton(A.m_bMuted ? "Unmute" : "Mute")) Annotate(Hub, Options, "LogMute", P.m_Id, !A.m_bMuted, &hub::SetMuted);
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip(P.m_Severity >= severity::Fatal ? "A fatal problem cannot be hidden." : "Hide it from the lists. Collection continues and the footer counts what is hidden.");
            ImGui::SameLine();
            if (ImGui::SmallButton("Copy")) ImGui::SetClipboardText(ContextPack(Hub, P).c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Copies what failed, where, how often and the last occurrence in full.");
            ImGui::SameLine();
            if (ImGui::SmallButton("Show in Events"))
            {
                State.PushBack();
                std::snprintf(State.m_Query, sizeof(State.m_Query), "%s", P.m_LastOperation ? std::format("op:{}", P.m_LastOperation).c_str() : P.m_Code.empty() ? "" : std::format("code:{}", P.m_Code).c_str());
                State.m_RequestPage = 1;
            }
            ImGui::Unindent(26.0f);
            ImGui::Dummy(ImVec2(0, 4));
            ImGui::PopID();
        }

        // The query bar every page shares. The text is the pipe's grammar; a mistake explains itself instead of listing nothing.
        inline void QueryBar(view_state& State, const tab_options& Options, float ReserveRight) noexcept
        {
            // Back / Forward: the editors' arrow pair (the asset browser's path bar). Back returns to the previous view of the window and to what the host had in front
            // when it sent the person here; Forward undoes a Back. The tooltips are built BEFORE the buttons: a click changes the lists, and nothing may read them after.
            std::string BackTip = "Nothing to go back to", ForwardTip = "Nothing to go forward to";
            if (!State.m_Back.empty())
            {
                const view_snapshot& T = State.m_Back.back();
                BackTip = std::format("Back to {}{}", T.m_Label, T.m_bHasReturn ? (T.m_bReturnDrawerOpen ? "  (and the drawer tab you left)" : "  (and the drawer closed again)") : "");
            }
            if (!State.m_Forward.empty()) ForwardTip = std::format("Forward to {}", State.m_Forward.back().m_Label);
            bool bBack = false, bForward = false;
            xeditor::RenderBackForwardButtons(!State.m_Back.empty(), !State.m_Forward.empty(), bBack, bForward, BackTip.c_str(), ForwardTip.c_str(), State.m_BackButton, State.m_ForwardButton);
            if (bBack)         { if (Options.m_OnBack)    Options.m_OnBack();    else { view_snapshot Dropped; State.PopBack(Dropped); } }
            else if (bForward) { if (Options.m_OnForward) Options.m_OnForward(); else { view_snapshot Dropped; State.PopForward(Dropped); } }
            static constexpr const char* Help = "sev>=error  channel:game.*  -channel:x  code:C2065  origin:x  op:3  body:text  \"a phrase\"  free text"
                                                "\nThe same words the pipe's LogProblems and LogEvents take.";
            const float Width = std::max(120.0f, ImGui::GetContentRegionAvail().x - ReserveRight);
            std::string Text(State.m_Query);
            if (xeditor::RenderTreeSearchBar(Text, Width, false, Help)) std::snprintf(State.m_Query, sizeof(State.m_Query), "%s", Text.c_str());
        }

        // The two lenses (documentation/Editors/DESIGN_logs.md, 6.5): who produced it (Source: the origins that have spoken, any subset) and what it concerns (About: anything, the
        // selected row's operation, the selected row's asset). They are not state of their own: each is a token of the query (origin:a,b, op:N, asset:X), which this rewrites, so
        // the chips, the typed query and the pipe's LogProblems / LogEvents are always the same filter.
        inline void SetQueryText(view_state& State, const std::string& Query) noexcept { std::snprintf(State.m_Query, sizeof(State.m_Query), "%s", Query.c_str()); }

        inline void LensBar(const hub& Hub, view_state& State) noexcept
        {
            // ---- Source
            std::vector<std::string> Chosen;
            {
                const std::string List = QueryValue(State.m_Query, "origin");
                for (std::size_t At = 0; At <= List.size() && !List.empty(); )
                {
                    const auto Comma = List.find(',', At);
                    Chosen.push_back(List.substr(At, Comma == std::string::npos ? std::string::npos : Comma - At));
                    if (Comma == std::string::npos) break;
                    At = Comma + 1;
                }
            }
            const std::string SourceLabel = Chosen.empty() ? "All" : Chosen.size() == 1 ? Chosen[0] : std::format("{} sources", Chosen.size());
            ImGui::TextDisabled("Source"); ImGui::SameLine();
            if (ImGui::SmallButton((SourceLabel + " v###sourcelens").c_str())) ImGui::OpenPopup("##sourcepopup");
            State.m_SourceChipAt[0] = ImGui::GetItemRectMin().x + ImGui::GetItemRectSize().x * 0.5f; State.m_SourceChipAt[1] = ImGui::GetItemRectMin().y + ImGui::GetItemRectSize().y * 0.5f;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Who produced it: any editor, system, script or tool that has said something.");
            if (ImGui::BeginPopup("##sourcepopup"))
            {
                if (ImGui::Selectable("All", Chosen.empty())) SetQueryText(State, WithQueryToken(State.m_Query, "origin", {}));
                const struct { origin::type m_Type; const char* m_Heading; } Groups[] = { { origin::type::Editor, "Editors" }, { origin::type::System, "Systems" }, { origin::type::Script, "Scripts" }, { origin::type::Tool, "Tools" } };
                for (const auto& G : Groups)
                {
                    bool bAny = false;
                    for (const auto& O : Hub.Origins())
                    {
                        if (O.m_Type != G.m_Type) continue;
                        if (!bAny) { ImGui::Separator(); ImGui::TextDisabled("%s", G.m_Heading); bAny = true; }
                        bool bOn = std::find(Chosen.begin(), Chosen.end(), O.m_Name) != Chosen.end();
                        if (ImGui::Checkbox((O.m_Name + "##src").c_str(), &bOn))
                        {
                            std::vector<std::string> Next = Chosen;
                            if (bOn) Next.push_back(O.m_Name); else Next.erase(std::remove(Next.begin(), Next.end(), O.m_Name), Next.end());
                            std::string Joined;
                            for (const auto& N : Next) Joined += (Joined.empty() ? "" : ",") + N;
                            SetQueryText(State, WithQueryToken(State.m_Query, "origin", Joined));
                        }
                    }
                }
                ImGui::EndPopup();
            }

            // ---- About
            ImGui::SameLine(0, 18.0f);
            const std::string Operation = QueryValue(State.m_Query, "op"), Asset = QueryValue(State.m_Query, "asset");
            const std::string AboutLabel = !Operation.empty() ? "operation " + Operation : !Asset.empty() ? "asset " + Asset : "Anything";
            ImGui::TextDisabled("About"); ImGui::SameLine();
            if (ImGui::SmallButton((AboutLabel + " v###aboutlens").c_str())) ImGui::OpenPopup("##aboutpopup");
            State.m_AboutChipAt[0] = ImGui::GetItemRectMin().x + ImGui::GetItemRectSize().x * 0.5f; State.m_AboutChipAt[1] = ImGui::GetItemRectMin().y + ImGui::GetItemRectSize().y * 0.5f;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("What it concerns: anything, or what the selected row is about.");
            if (ImGui::BeginPopup("##aboutpopup"))
            {
                // what the selected row is about: its operation and its first subject
                std::uint64_t SelOp = 0; ref SelSubject;
                if (State.m_Page == 0) { if (const problem* P = Hub.FindProblem(State.m_Selected)) { SelOp = P->m_LastOperation; SelSubject = P->m_Subject; } }
                else if (const event* E = Hub.FindEvent(State.m_SelectedEvent)) { SelOp = E->m_Operation; if (!E->m_Subjects.empty()) SelSubject = E->m_Subjects[0]; }

                if (ImGui::Selectable("Anything", Operation.empty() && Asset.empty())) SetQueryText(State, WithQueryToken(WithQueryToken(State.m_Query, "op", {}), "asset", {}));
                ImGui::BeginDisabled(SelOp == 0);
                if (ImGui::Selectable(SelOp ? std::format("This operation (#{})", SelOp).c_str() : "This operation", !Operation.empty()))
                    SetQueryText(State, WithQueryToken(WithQueryToken(State.m_Query, "asset", {}), "op", std::to_string(SelOp)));
                ImGui::EndDisabled();
                ImGui::BeginDisabled(!SelSubject.Valid());
                const std::string AssetName = SelSubject.m_Path.empty() ? Hex16(SelSubject.m_Id) : SelSubject.m_Path;
                if (ImGui::Selectable(SelSubject.Valid() ? std::format("This asset ({})", AssetName).c_str() : "This asset", !Asset.empty()))
                    SetQueryText(State, WithQueryToken(WithQueryToken(State.m_Query, "op", {}), "asset", SelSubject.m_Id ? Hex16(SelSubject.m_Id) : SelSubject.m_Path));
                ImGui::EndDisabled();
                if (SelOp == 0 && !SelSubject.Valid()) ImGui::TextDisabled("Select a row to be about what it is about.");
                ImGui::EndPopup();
            }
        }

        inline void RenderProblems(hub& Hub, view_state& State, const tab_options& Options) noexcept
        {
            BuildProblemRows(Hub, State, Options.m_ChannelPrefix);

            // header: query, presets with their counts, "mark seen"
            const char* Names[3] = { "New", "Active", "All" };
            float PresetsW = ImGui::GetStyle().ItemSpacing.x * 4;
            for (int i = 0; i < 3; ++i) PresetsW += ImGui::CalcTextSize(std::format("{} {}", Names[i], State.m_Counts[i]).c_str()).x + ImGui::GetStyle().FramePadding.x * 2;
            QueryBar(State, Options, PresetsW + ImGui::CalcTextSize("Mark seen").x + ImGui::GetStyle().FramePadding.x * 2);
            for (int i = 0; i < 3; ++i)
            {
                ImGui::SameLine();
                const bool bOn = static_cast<int>(State.m_View) == i;
                ImGui::PushStyleColor(ImGuiCol_Button, bOn ? ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive) : ImVec4(0, 0, 0, 0));
                if (ImGui::SmallButton(std::format("{} {}###view{}", Names[i], State.m_Counts[i], i).c_str())) { State.m_View = static_cast<problem_view>(i); State.m_Expanded.clear(); State.m_Selected = 0; }
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(i == 0 ? "First seen after the baseline (the start of this launch, until you mark them seen)" : i == 1 ? "Not acknowledged yet" : "Everything, acknowledged or not (muted ones are hidden)");
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Mark seen"))
            {
                if (Options.m_Run) Options.m_Run("LogMark -Kind baseline"); else Hub.SetBaseline(Hub.Committed());
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Everything so far stops being New.");
            if (!State.m_QueryError.empty()) { ImGui::PushStyleColor(ImGuiCol_Text, SeverityColor(severity::Error)); ImGui::TextUnformatted(State.m_QueryError.c_str()); ImGui::PopStyleColor(); }
            LensBar(Hub, State);

            const float FooterH = ImGui::GetTextLineHeightWithSpacing() + 4.0f;
            if (ImGui::BeginChild("problems", ImVec2(0, -FooterH), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar))
            {
                if (State.m_Pending)
                {
                    if (ImGui::SmallButton(std::format("{} new problem{}  (show)", State.m_Pending, State.m_Pending == 1 ? "" : "s").c_str())) BuildProblemRows(Hub, State, Options.m_ChannelPrefix, true);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The list does not move while you read it. Click to apply.");
                }
                ImDrawList* pList = ImGui::GetWindowDrawList();
                const float Width = ImGui::GetContentRegionAvail().x;
                const float RowH = ImGui::GetTextLineHeightWithSpacing() + 2.0f;
                // columns disappear right to left when they do not fit: last seen, count, channel, subject; the title is the last to go
                float Age = 44, Count = 52, Channel = 130, Subject = std::clamp(Width * 0.24f, 110.0f, 260.0f);
                const float Glyph = 24, MinTitle = 190;
                if (Width - Glyph - Subject - Channel - Count - Age < MinTitle) Age = 0;
                if (Width - Glyph - Subject - Channel - Count - Age < MinTitle) Count = 0;
                if (Width - Glyph - Subject - Channel - Count - Age < MinTitle) Channel = 0;
                if (Width - Glyph - Subject - Channel - Count - Age < MinTitle) Subject = std::max(0.0f, Width - Glyph - MinTitle);
                const float Title = Width - Glyph - Subject - Channel - Count - Age;
                const ImU32 TextCol = ImGui::GetColorU32(ImGuiCol_Text), Muted = ImGui::GetColorU32(ImGuiCol_TextDisabled);
                const std::uint64_t Now = Hub.Now();

                int Move = 0;      // the arrow keys, while this list has the focus
                const bool bFocused = ImGui::IsWindowFocused();
                if (bFocused && !ImGui::GetIO().WantTextInput)
                {
                    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) Move = 1;
                    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))   Move = -1;
                }

                if (State.m_Rows.empty() && !State.m_Pending)
                    ImGui::TextDisabled("%s", State.m_Counts[2] == 0 && State.m_HiddenByMutes == 0 && State.m_Query[0] == 0 ? "Nothing needs attention." : "No problem matches.");

                for (std::size_t i = 0; i < State.m_Rows.size(); ++i)
                {
                    const problem* P = Hub.FindProblem(State.m_Rows[i]);
                    if (!P) continue;
                    if (Move && State.m_Selected == P->m_Id)
                    {
                        const std::size_t To = Move > 0 ? i + 1 : (i > 0 ? i - 1 : i);
                        if (To < State.m_Rows.size()) { State.m_Selected = State.m_Rows[To]; ImGui::SetScrollHereY(Move > 0 ? 1.0f : 0.0f); }
                        Move = 0;
                    }
                    const bool bSelected = State.m_Selected == P->m_Id;
                    const bool bOpen = State.IsExpanded(P->m_Id);
                    const annotation A = Hub.Annotation(P->m_Id);
                    ImGui::PushID(static_cast<int>(P->m_Id & 0x7FFFFFFF));
                    if (bSelected && State.m_bScrollToSelected) { ImGui::SetScrollHereY(0.0f); State.m_bScrollToSelected = false; }
                    const ImVec2 Min = ImGui::GetCursorScreenPos();
                    if (ImGui::Selectable("##row", bSelected, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(Width, RowH)))
                    {
                        State.m_Selected = P->m_Id;
                        State.SetExpanded(P->m_Id, !bOpen);
                        State.m_bScrollToSelected = !bOpen;
                    }
                    const bool bHovered = ImGui::IsItemHovered();
                    if (bHovered && ImGui::IsMouseDoubleClicked(0)) Open(Options, P->m_Site);
                    if (bSelected && bFocused)
                    {
                        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) State.SetExpanded(P->m_Id, true);
                        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_Escape)) State.SetExpanded(P->m_Id, false);
                        if (ImGui::IsKeyPressed(ImGuiKey_Enter)) Open(Options, P->m_Site);
                    }
                    const float Y = Min.y + (RowH - ImGui::GetTextLineHeight()) * 0.5f;
                    pList->AddRectFilled(Min, ImVec2(Min.x + 2.0f, Min.y + RowH), SeverityColor(P->m_Severity));      // the thin left edge
                    DrawGlyph(pList, ImVec2(Min.x + 14.0f, Min.y + RowH * 0.5f), P->m_Severity);
                    float X = Min.x + Glyph;

                    auto Cell = [&](float W, const std::string& Text, ImU32 Col)
                    {
                        if (W <= 0) return;
                        pList->PushClipRect(ImVec2(X, Min.y), ImVec2(X + W - 6.0f, Min.y + RowH), true);
                        pList->AddText(ImVec2(X, Y), Col, Text.c_str());
                        pList->PopClipRect();
                        X += W;
                    };
                    const ImU32 Dim = (A.m_bAcknowledged || A.m_bMuted) ? Muted : TextCol;
                    Cell(Title, (P->m_Code.empty() ? std::string() : P->m_Code + "  ") + P->m_Title + (P->m_bHeuristic ? "  [heuristic]" : ""), Dim);
                    Cell(Subject, Where(*P), Muted);
                    Cell(Channel, P->m_Channel, Muted);
                    Cell(Count, std::format("x{}", P->m_Count), Dim);
                    const event* pLast = Hub.FindEvent(P->m_LastSeq);
                    Cell(Age, pLast && Now >= pLast->m_ObservedAt ? Elapsed(Now - pLast->m_ObservedAt) : std::string("?"), Muted);
                    if (bHovered && !bOpen) { ImGui::BeginTooltip(); ImGui::TextUnformatted(P->m_Title.c_str()); const std::string W = Where(*P); if (!W.empty()) ImGui::TextDisabled("%s", W.c_str()); ImGui::EndTooltip(); }
                    ImGui::PopID();

                    if (bOpen) ProblemDetails(Hub, State, Options, *P);
                }
            }
            ImGui::EndChild();

            // footer: what the list means, honestly
            const auto S = Hub.Status();
            std::uint64_t Dropped = 0;
            for (auto D : S.m_Dropped) Dropped += D;
            std::uint64_t Occurrences = 0;
            std::size_t Errors = 0, Warnings = 0;
            for (auto Id : State.m_Rows) if (const problem* P = Hub.FindProblem(Id)) { Occurrences += P->m_Count; if (P->m_Severity >= severity::Error) ++Errors; else ++Warnings; }
            ImGui::TextDisabled("%zu error problem%s  ·  %zu warning problem%s  ·  %llu occurrences", Errors, Errors == 1 ? "" : "s", Warnings, Warnings == 1 ? "" : "s", static_cast<unsigned long long>(Occurrences));
            if (State.m_HiddenByMutes)
            {
                ImGui::SameLine(); ImGui::TextDisabled("·"); ImGui::SameLine();
                if (ImGui::SmallButton(std::format("{} {} by mutes###muted", State.m_HiddenByMutes, State.m_bShowMuted ? "shown, muted" : "hidden").c_str())) State.m_bShowMuted = !State.m_bShowMuted;
            }
            ImGui::SameLine();
            if (Dropped) { ImGui::PushStyleColor(ImGuiCol_Text, SeverityColor(severity::Warning)); ImGui::Text("·  capture lost %llu low-level events", static_cast<unsigned long long>(Dropped)); ImGui::PopStyleColor(); }
            else ImGui::TextDisabled("·  capture OK");
        }

        // ---- Events -------------------------------------------------------------------------------------------------------------------------------------------
        // One line per event, each with an arrow (the compiler tab's) that opens THAT event in place: where and when, the operation it was part of, its attributes, the whole
        // body, and its actions. Open events make the list's rows uneven, so the list is virtualized by hand: a row is one line, an open event is its line plus a block whose
        // height follows from what it shows, and only what is in view is drawn however many events there are. A click selects, Shift+click extends the selection to a range,
        // and Copy (or Ctrl+C) puts the selected events on the clipboard as plain text.
        inline constexpr int event_body_lines_shown_v = 80;

        // Lines in the block an open event shows (every one is a text line high).
        inline int OpenEventLines(const event& E) noexcept
        {
            int Lines = 1;                                                        // where and when
            if (E.m_Operation) ++Lines;                                           // the operation it belongs to
            Lines += static_cast<int>(E.m_Attributes.size());
            Lines += static_cast<int>(std::min<std::uint32_t>(E.m_BodyLines, event_body_lines_shown_v));
            if (E.m_BodyLines > event_body_lines_shown_v) ++Lines;                // "... N more lines"
            return Lines + (E.m_Source.Valid() ? 1 : 0);                          // the way to its source
        }

        inline void RenderEvents(hub& Hub, view_state& State, const tab_options& Options) noexcept
        {
            ScanEvents(Hub, State, Options.m_ChannelPrefix, Options.m_Window, Options.m_bShowCommands);

            QueryBar(State, Options, ImGui::CalcTextSize("Follow").x + ImGui::CalcTextSize("Clear view").x + ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.x * 4 + ImGui::GetStyle().ItemSpacing.x * 3);
            ImGui::SameLine();
            ImGui::Checkbox("Follow", &State.m_bFollow);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Stay on the newest event. Scrolling up turns it off; collection never stops.");
            ImGui::SameLine();
            if (ImGui::SmallButton("Clear view")) { State.m_ViewFrom = Hub.Committed(); State.m_SelectedEvent = State.m_SelFrom = State.m_SelTo = State.m_SelAnchor = 0; State.m_ExpandedEvents.clear(); }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Hides what is listed. The events stay in the Logs.");
            if (!State.m_QueryError.empty()) { ImGui::PushStyleColor(ImGuiCol_Text, SeverityColor(severity::Error)); ImGui::TextUnformatted(State.m_QueryError.c_str()); ImGui::PopStyleColor(); }

            LensBar(Hub, State);

            const float FooterH = ImGui::GetTextLineHeightWithSpacing() + 4.0f;
            const auto SelectedCount = [&]() -> std::size_t { return State.SelectedEvents().size(); };

            // Ctrl+C with the list focused copies the selection (not while a text box has the keyboard)
            bool bCopy = false;
            bool bMoved = false;           // the person opened or closed something: the list stays where it is (it does not chase the newest, and nothing above the click moves)
            if (ImGui::BeginChild("events", ImVec2(0, -FooterH), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar))
            {
                if (ImGui::IsWindowFocused() && !ImGui::GetIO().WantTextInput && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_C)) bCopy = true;

                const float Width = ImGui::GetContentRegionAvail().x;
                const float LH = ImGui::GetTextLineHeightWithSpacing();           // a line: its height and the gap after it
                const float TH = ImGui::GetTextLineHeight();
                const float Pad = 4.0f;
                ImDrawList* pList = ImGui::GetWindowDrawList();
                const ImU32 TextCol = ImGui::GetColorU32(ImGuiCol_Text), Muted = ImGui::GetColorU32(ImGuiCol_TextDisabled);
                const float Arrow = TH + 4.0f, Time = 84.0f, Glyph = 20.0f, Channel = Width > 520 ? 130.0f : 0.0f, Badge = 64.0f;
                const std::size_t Count = State.m_EventRows.size();

                const float OriginCursorY = ImGui::GetCursorPosY();
                if (Count == 0) ImGui::TextDisabled("%s", State.m_Query[0] ? "No event matches." : "No events yet.");

                // The open events among the rows (their index in the list and how much taller they make it), in list order
                struct open_block { std::size_t m_Index; float m_Height; };
                std::vector<open_block> OpenBlocks;
                for (auto Seq : State.m_ExpandedEvents)
                {
                    const auto It = std::lower_bound(State.m_EventRows.begin(), State.m_EventRows.end(), Seq);
                    if (It == State.m_EventRows.end() || *It != Seq) continue;
                    if (const event* E = Hub.FindEvent(Seq)) OpenBlocks.push_back({ static_cast<std::size_t>(It - State.m_EventRows.begin()), OpenEventLines(*E) * LH + Pad * 2.0f });
                }
                std::sort(OpenBlocks.begin(), OpenBlocks.end(), [](const open_block& A, const open_block& B) { return A.m_Index < B.m_Index; });
                float ExtraTotal = 0.0f;
                for (const auto& B : OpenBlocks) ExtraTotal += B.m_Height;
                const float ContentH = static_cast<float>(Count) * LH + ExtraTotal;

                // The first row in view, and where it is: runs of plain rows, each broken by an open event's block
                const float ScrollY = ImGui::GetScrollY(), ViewH = ImGui::GetWindowHeight();
                std::size_t First = 0;
                float FirstY = 0.0f;
                {
                    float RowTop = 0.0f;
                    std::size_t Index = 0, Next = 0;
                    for (;;)
                    {
                        const std::size_t BlockRow = Next < OpenBlocks.size() ? OpenBlocks[Next].m_Index : Count;
                        const float PlainEnd = RowTop + static_cast<float>(BlockRow - Index) * LH;
                        if (BlockRow >= Count || PlainEnd > ScrollY)
                        {
                            const std::size_t Skip = static_cast<std::size_t>(std::max(0.0f, std::floor((ScrollY - RowTop) / LH)));
                            First = std::min(Index + Skip, Count);
                            FirstY = RowTop + static_cast<float>(First - Index) * LH;
                            break;
                        }
                        if (PlainEnd + LH + OpenBlocks[Next].m_Height > ScrollY) { First = BlockRow; FirstY = PlainEnd; break; }     // the view starts inside this open event
                        RowTop = PlainEnd + LH + OpenBlocks[Next].m_Height;
                        Index = BlockRow + 1;
                        ++Next;
                    }
                }

                // Draw what is in view
                State.m_EventRowAt[0] = State.m_EventRowAt[1] = State.m_EventArrowAt[0] = State.m_EventArrowAt[1] = -1.0f;
                State.m_EventStride = LH;
                float Y = FirstY;
                std::size_t Drawn = 0;
                const std::uint64_t Lo = State.m_SelFrom, Hi = State.m_SelTo;
                const ImGuiIO& IO = ImGui::GetIO();
                for (std::size_t i = First; i < Count && Y < ScrollY + ViewH + LH; ++i)
                {
                    const std::uint64_t Seq = State.m_EventRows[i];
                    const event* E = Hub.FindEvent(Seq);
                    ImGui::PushID(static_cast<int>(Seq & 0x7FFFFFFF));
                    ImGui::SetCursorPos(ImVec2(0.0f, OriginCursorY + Y));
                    const ImVec2 Min = ImGui::GetCursorScreenPos();
                    if (!E) { ImGui::Dummy(ImVec2(Width, TH)); ImGui::PopID(); Y += LH; continue; }          // expired from the store

                    const bool bOpen = State.IsEventOpen(Seq);
                    // the arrow: THAT event, alone (the same arrow button the compiler tab uses)
                    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2.0f, 0.0f));
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                    if (ImGui::ArrowButton("##open", bOpen ? ImGuiDir_Down : ImGuiDir_Right)) { State.SetEventOpen(Seq, !bOpen); bMoved = true; }
                    ImGui::PopStyleColor();
                    ImGui::PopStyleVar();
                    if (Drawn == 0) { State.m_EventArrowAt[0] = ImGui::GetItemRectMin().x + ImGui::GetItemRectSize().x * 0.5f; State.m_EventArrowAt[1] = ImGui::GetItemRectMin().y + ImGui::GetItemRectSize().y * 0.5f; }
                    ImGui::SameLine(0.0f, 0.0f);

                    const bool bSelected = Seq >= Lo && Seq <= Hi && Lo != 0;
                    const ImVec2 RowMin = ImGui::GetCursorScreenPos();
                    if (ImGui::Selectable("##event", bSelected, 0, ImVec2(Width - ImGui::GetCursorPosX(), TH)))
                    {
                        // a click selects; Shift+click extends from where the last plain click was
                        if (IO.KeyShift && State.m_SelAnchor) { State.m_SelFrom = std::min(State.m_SelAnchor, Seq); State.m_SelTo = std::max(State.m_SelAnchor, Seq); }
                        else { State.m_SelAnchor = State.m_SelFrom = State.m_SelTo = Seq; }
                        State.m_SelectedEvent = Seq;
                    }
                    if (Drawn == 0) { State.m_EventRowAt[0] = RowMin.x + 200.0f; State.m_EventRowAt[1] = RowMin.y + TH * 0.5f; }
                    ++Drawn;

                    // Right click: the menu of the selection. A right click on an event that is not selected selects it first (as a file list does).
                    if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && !bSelected) { State.m_SelAnchor = State.m_SelFrom = State.m_SelTo = Seq; State.m_SelectedEvent = Seq; }
                    if (ImGui::BeginPopupContextItem("##eventmenu"))
                    {
                        const std::size_t N = SelectedCount();
                        if (ImGui::MenuItem(N > 1 ? std::format("Copy {} events", N).c_str() : "Copy", "Ctrl+C", false, N > 0)) bCopy = true;
                        ImGui::Separator();
                        if (ImGui::MenuItem(N > 1 ? std::format("Open {} events", N).c_str() : "Open", nullptr, false, N > 0)) { State.SetSelectedEventsOpen(true); bMoved = true; }
                        if (ImGui::MenuItem(N > 1 ? std::format("Close {} events", N).c_str() : "Close", nullptr, false, N > 0)) { State.SetSelectedEventsOpen(false); bMoved = true; }
                        ImGui::EndPopup();
                    }

                    float X = RowMin.x;
                    pList->AddText(ImVec2(X, Min.y), Muted, Clock(E->m_ObservedAt).c_str());
                    X += Time;
                    DrawGlyph(pList, ImVec2(X + 6.0f, Min.y + TH * 0.5f), E->m_Severity);
                    X += Glyph;
                    if (Channel > 0) { pList->PushClipRect(ImVec2(X, Min.y), ImVec2(X + Channel - 6.0f, Min.y + LH), true); pList->AddText(ImVec2(X, Min.y), Muted, E->m_Channel.c_str()); pList->PopClipRect(); X += Channel; }
                    const float TitleW = Width - (X - Min.x) - (E->m_BodyLines ? Badge : 0.0f);
                    pList->PushClipRect(ImVec2(X, Min.y), ImVec2(X + std::max(0.0f, TitleW), Min.y + LH), true);
                    pList->AddText(ImVec2(X, Min.y), E->m_Kind == kind::Log || E->m_Severity >= severity::Warning ? TextCol : Muted, E->m_Title.c_str());
                    pList->PopClipRect();
                    if (E->m_BodyLines) pList->AddText(ImVec2(Min.x + Width - Badge + 8.0f, Min.y), Muted, std::format("+{} lines", E->m_BodyLines).c_str());
                    Y += LH;

                    // the open block
                    if (bOpen)
                    {
                        const float BlockH = OpenEventLines(*E) * LH + Pad * 2.0f;
                        const float X0 = Min.x + Arrow + Time;
                        float LineY = Min.y + LH + Pad;
                        auto Line = [&](const std::string& Text, ImU32 Col)
                        {
                            pList->PushClipRect(ImVec2(X0, LineY), ImVec2(Min.x + Width - 4.0f, LineY + LH), true);
                            pList->AddText(ImVec2(X0, LineY), Col, Text.c_str());
                            pList->PopClipRect();
                            LineY += LH;
                        };
                        Line(std::format("{}  ·  {}  ·  {}:{}  ·  {}{}{}", Clock(E->m_ObservedAt), E->m_Channel, OriginTypeName(E->m_Origin.m_Type), E->m_Origin.m_Name, SeverityName(E->m_Severity)
                            , E->m_Code.empty() ? "" : "  ·  ", E->m_Code), Muted);
                        if (E->m_Operation) Line("During  " + Breadcrumb(Hub, E->m_Operation), Muted);
                        for (const auto& A : E->m_Attributes)
                        {
                            std::string Text;
                            std::visit([&](const auto& V) { if constexpr (std::is_same_v<std::decay_t<decltype(V)>, std::string>) Text = V; else if constexpr (std::is_same_v<std::decay_t<decltype(V)>, bool>) Text = V ? "true" : "false"; else Text = std::format("{}", V); }, A.m_Value);
                            Line(A.m_Name + "  " + Text, Muted);
                        }
                        {
                            std::size_t Start = 0; int Shown = 0;
                            while (Start <= E->m_Body.size() && !E->m_Body.empty() && Shown < event_body_lines_shown_v)
                            {
                                const auto Eol = E->m_Body.find('\n', Start);
                                Line(E->m_Body.substr(Start, Eol == std::string::npos ? std::string::npos : Eol - Start), TextCol);
                                ++Shown;
                                if (Eol == std::string::npos) break;
                                Start = Eol + 1;
                            }
                            if (E->m_BodyLines > event_body_lines_shown_v) Line(std::format("... {} more lines (LogEvent -Id {} -Offset {} reads them)", E->m_BodyLines - event_body_lines_shown_v, Seq, event_body_lines_shown_v), Muted);
                        }
                        if (E->m_Source.Valid())          // the one action of an event (Copy, Open and Close are the menu's, for the whole selection)
                        {
                            ImGui::SetCursorScreenPos(ImVec2(X0, LineY));
                            if (ImGui::SmallButton(Options.m_OnOpen ? "Open source" : "Copy location")) Open(Options, E->m_Source);
                        }
                        Y += BlockH;
                    }
                    ImGui::PopID();
                }
                // the list's full height, so the scrollbar is the size of the whole list
                ImGui::SetCursorPosY(OriginCursorY + ContentH);
                ImGui::Dummy(ImVec2(0.0f, 0.0f));

                // follow the newest, until the person scrolls away or opens or closes something (then what is above the click stays where it is and what is below moves)
                if ((ImGui::IsWindowHovered() && IO.MouseWheel > 0.0f) || bMoved) State.m_bFollow = false;
                if (State.m_bFollow) { ImGui::SetScrollHereY(1.0f); State.m_EventNew = 0; }
                else if (!bMoved && ImGui::GetScrollMaxY() > 0.0f && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f) { State.m_bFollow = true; State.m_EventNew = 0; }       // a list that fits has no "end" to be at
            }
            ImGui::EndChild();

            const auto S = Hub.Status();
            ImGui::TextDisabled("%zu matching event%s  ·  %zu retained%s", State.m_EventRows.size(), State.m_EventRows.size() == 1 ? "" : "s", S.m_Events
                , S.m_Expired ? std::format("  ·  {} expired", S.m_Expired).c_str() : "");
            if (const std::size_t N = SelectedCount(); N > 1)
            {
                ImGui::SameLine(); ImGui::TextDisabled("·  %zu selected", N);
            }
            if (bCopy && State.m_SelFrom) ImGui::SetClipboardText(FormatEventsForCopy(Hub, State.SelectedEvents()).c_str());
            if (!State.m_bFollow && State.m_EventNew)
            {
                ImGui::SameLine();
                if (ImGui::SmallButton(std::format("{} new events  v", State.m_EventNew).c_str())) { State.m_bFollow = true; State.m_EventNew = 0; }
            }
        }
    }

    inline void RenderTab( hub& Hub, const tab_options& Options = {} ) noexcept
    {
        static view_state s_OwnState;          // only for a window whose owner did not give one: the drawer's tab always does
        view_state& State = Options.m_pState ? *Options.m_pState : s_OwnState;

        ImGui::PushID("xlog.tab");
        State.m_MouseAt[0] = ImGui::GetIO().MousePos.x; State.m_MouseAt[1] = ImGui::GetIO().MousePos.y;
        if (ImGui::BeginTabBar("xlog.pages", ImGuiTabBarFlags_None))
        {
            const int Request = State.m_RequestPage;
            State.m_RequestPage = -1;
            if (ImGui::BeginTabItem("Problems", nullptr, Request == 0 ? ImGuiTabItemFlags_SetSelected : 0)) { State.m_Page = 0; details::RenderProblems(Hub, State, Options); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Events",   nullptr, Request == 1 ? ImGuiTabItemFlags_SetSelected : 0)) { State.m_Page = 1; details::RenderEvents(Hub, State, Options);   ImGui::EndTabItem(); }
            ImGui::EndTabBar();
        }
        ImGui::PopID();
    }
}

#endif // XLOG_TAB_H
