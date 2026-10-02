#ifndef XLOG_VIEW_H
#define XLOG_VIEW_H
#pragma once

// What a view of the Logs remembers (the query, the preset, the selection) and the lists it builds from the hub. No ImGui here: the state belongs to
// whoever owns the window (xeditor::host keeps one, so the drawer's tab survives the user moving between editors), and the lists are built by plain
// functions so they can be tested and reused (the badge counts with the same rules as the window).
#include "xlog_hub.h"

#include <unordered_set>

namespace xlog
{
    // "00:11.162": the time since the session started, as a clock
    inline std::string ClockText(std::uint64_t Ns) noexcept
    {
        const std::uint64_t Ms = Ns / 1000000ull;
        return std::format("{:02}:{:02}.{:03}", Ms / 60000, (Ms / 1000) % 60, Ms % 1000);
    }

    // Where a typed reference points, in words: "soccer_player_system.h:80:17", "asset Face (00000000D896E5)"
    inline std::string DescribeRef(const ref& R) noexcept
    {
        if (!R.Valid()) return {};
        static constexpr const char* Types[] = { "none", "asset", "entity", "file", "graph", "operation", "object" };
        std::string Text = std::format("{} {}", Types[static_cast<int>(R.m_Type)], R.m_Path);
        if (R.m_Line > 0) Text += std::format(":{}{}", R.m_Line, R.m_Column > 0 ? std::format(":{}", R.m_Column) : std::string());
        if (R.m_Id) Text += std::format(" (id {})", Hex16(R.m_Id));
        if (R.m_Revision) Text += std::format(" (revision {})", R.m_Revision);
        return Text;
    }

    // The context pack of a problem (design 7.2): what a person or an AI needs to understand it, deterministic (the same store gives the same text), stable ids, bounded by Budget
    // bytes, and honest about what is missing. Sections are in order of importance, so a small budget cuts the least important first.
    inline std::string BuildContextPack(const hub& Hub, const problem& P, std::size_t Budget = 8192) noexcept
    {
        std::string Out;
        bool bCut = false;
        auto Add = [&](const std::string& Text) { if (bCut) return; if (Out.size() + Text.size() > Budget) { bCut = true; return; } Out += Text; };
        const annotation A = Hub.Annotation(P.m_Id);

        Add(std::format("Problem={}  Severity={}  Code={}  Heuristic={}\nProducer={}  Channel={}  Origin={}\nTitle={}\n", Hex16(P.m_Id), SeverityName(P.m_Severity), P.m_Code.empty() ? "-" : P.m_Code
            , P.m_bHeuristic ? "true" : "false", P.m_Producer, P.m_Channel, P.m_OriginName, P.m_Title));
        Add(std::format("State: Triage={}  Verification={}  Suppression={}  RunPresence={}  Regressions={}  Recurring={}\n", A.m_bAcknowledged ? "Acknowledged" : "Unreviewed"
            , VerificationName(P.m_Verification), A.m_bMuted ? "Muted" : "None", PresenceName(P.m_Presence), P.m_Regressions, P.m_bRecurring ? "true" : "false"));
        if (P.m_Verification == verification::Verified) Add(std::format("VerifiedBy=operation {}\n", P.m_VerifiedBy));
        if (!P.m_PreviousSession.empty()) Add(std::format("Verified resolved in the earlier launch {}: this is a regression across launches\n", P.m_PreviousSession));
        if (P.m_Site.Valid())    Add("Source: " + DescribeRef(P.m_Site) + "\n");
        if (P.m_Subject.Valid()) Add("About: " + DescribeRef(P.m_Subject) + "\n");
        if (!P.m_CheckUnit.empty()) Add("CheckUnit=" + P.m_CheckUnit + "\n");

        const auto Retained = P.m_First.size() + P.m_Last.size();
        Add(std::format("Evidence: observed {}, retained {} ({}); first seen at event {}, last at event {}\n", P.m_Count, Retained, Retained < P.m_Count ? "summarized, the rest are counted" : "full", P.m_FirstSeq, P.m_LastSeq));

        // the operation chain, outermost first, with what each one says about its own evidence
        if (P.m_LastOperation)
        {
            std::vector<const operation*> Chain;
            for (std::uint64_t Id = P.m_LastOperation; Id && Chain.size() < 6; )
            {
                const operation* O = Hub.FindOperation(Id);
                if (!O) break;
                Chain.push_back(O);
                Id = O->m_Parent;
            }
            Add("Operations:\n");
            for (auto It = Chain.rbegin(); It != Chain.rend(); ++It)
            {
                const operation& O = **It;
                Add(std::format("  #{} {}  {}  EvidenceReady={}  Coverage={}  Units={}  Target={}  {}ms{}\n", O.m_Id, O.m_Kind, OutcomeName(O.m_Outcome), O.m_bEvidenceReady ? "true" : "false", CoverageName(O.m_Coverage)
                    , O.m_Units.size(), O.m_VerificationTarget.empty() ? "-" : O.m_VerificationTarget, ((O.m_Ended ? O.m_Ended : O.m_Started) - O.m_Started) / 1000000, O.m_bCollectorLoss ? "  CollectorLoss=true" : ""));
            }
        }

        // the occurrences that were kept: the first ones and the latest, with their whole bodies
        std::vector<std::uint64_t> Seqs = P.m_First;
        Seqs.insert(Seqs.end(), P.m_Last.begin(), P.m_Last.end());
        std::sort(Seqs.begin(), Seqs.end());
        Seqs.erase(std::unique(Seqs.begin(), Seqs.end()), Seqs.end());
        bool bExpired = false;
        Add("Occurrences kept:\n");
        for (auto Seq : Seqs)
        {
            const event* E = Hub.FindEvent(Seq);
            if (!E) { bExpired = true; continue; }
            Add(std::format("  event {}  {}  {}\n", Seq, ClockText(E->m_ObservedAt), E->m_Title));
            std::size_t Start = 0;
            while (!E->m_Body.empty() && Start <= E->m_Body.size())
            {
                const auto Eol = E->m_Body.find('\n', Start);
                Add(std::string("      ") + E->m_Body.substr(Start, Eol == std::string::npos ? std::string::npos : Eol - Start) + "\n");
                if (Eol == std::string::npos) break;
                Start = Eol + 1;
            }
        }

        // what was going on around the last occurrence
        if (P.m_LastSeq)
        {
            Add(std::format("Around the last occurrence (event {}):\n", P.m_LastSeq));
            const std::uint64_t From = P.m_LastSeq > 6 ? P.m_LastSeq - 6 : 0;
            Hub.ForEachEvent(From, P.m_LastSeq + 5, [&](const event& C) { Add(std::format("  {}{}  {}  {}  {}\n", C.m_Key.m_Sequence == P.m_LastSeq ? "> " : "  ", C.m_Key.m_Sequence, SeverityName(C.m_Severity), C.m_Channel, C.m_Title)); return !bCut; });
        }

        const status S = Hub.Status();
        std::uint64_t Dropped = 0;
        for (auto D : S.m_Dropped) Dropped += D;
        Add(std::format("Session={}  Events={}\n", Hex16(Hub.Session()), S.m_Events));
        std::string Missing;
        if (Retained < P.m_Count) Missing += std::format(" summarized({} occurrences counted, not kept)", P.m_Count - Retained);
        if (bExpired) Missing += " expired(an occurrence left the store)";
        if (Dropped) Missing += std::format(" dropped({} low-level events were not collected)", Dropped);
        if (S.m_Expired) Missing += std::format(" expired-events({})", S.m_Expired);
        Add("Missing:" + (Missing.empty() ? std::string(" nothing known") : Missing) + "\n");
        if (bCut) Out += std::format("... context cut at the budget of {} bytes\n", Budget);
        return Out;
    }

    // The operations as spans on the ruler (design 6.4): bars in lanes, an operation that overlaps another one a lane lower (a child is inside its parent, so it is below it).
    struct ruler_span { std::uint64_t m_Id = 0, m_Start = 0, m_End = 0; int m_Lane = 0; outcome m_Outcome = outcome::Running; std::string m_Kind, m_Title; };
    inline std::vector<ruler_span> ComputeSpans(const hub& Hub, std::size_t Max = 400) noexcept
    {
        std::vector<ruler_span> Spans;
        const auto& Order = Hub.OperationOrder();
        for (std::size_t i = Order.size() > Max ? Order.size() - Max : 0; i < Order.size(); ++i)
            if (const operation* O = Hub.FindOperation(Order[i]))
                Spans.push_back({ O->m_Id, O->m_Started, O->m_Ended ? O->m_Ended : Hub.Now(), 0, O->m_Outcome, O->m_Kind, O->m_Title.empty() ? O->m_Kind : O->m_Title });
        std::stable_sort(Spans.begin(), Spans.end(), [](const ruler_span& A, const ruler_span& B) { return A.m_Start < B.m_Start; });
        std::vector<std::uint64_t> LaneEnd;                                  // when each lane is free again
        for (auto& S : Spans)
        {
            std::size_t Lane = 0;
            while (Lane < LaneEnd.size() && LaneEnd[Lane] > S.m_Start) ++Lane;
            if (Lane == LaneEnd.size()) LaneEnd.push_back(0);
            LaneEnd[Lane] = S.m_End; S.m_Lane = static_cast<int>(Lane);
        }
        return Spans;
    }

    // Where the window was: what Back returns to. Navigation that moves the person somewhere else in the Logs (Feedback of an editor, "Show in Events") pushes one,
    // so the way back to what they were doing is one click.
    struct view_snapshot
    {
        std::string     m_Query;
        int             m_Page = 0;
        problem_view    m_View = problem_view::Active;
        bool            m_bShowMuted = false;
        std::uint64_t   m_Selected = 0, m_SelectedEvent = 0;
        std::string     m_Label;                            // "Events · op:4": what the Back button says it goes back to
        // What the HOST had in front when it sent the person here (the drawer's tab, or the drawer closed): Back puts that back too.
        bool            m_bHasReturn = false;
        int             m_ReturnTab = -1;
        bool            m_bReturnDrawerOpen = false;
    };

    // The query text as tokens, quotes kept as typed, so a token can be replaced without disturbing the rest
    inline std::vector<std::string> SplitQuery(std::string_view Query) noexcept
    {
        std::vector<std::string> Tokens;
        for (std::size_t i = 0; i < Query.size(); )
        {
            while (i < Query.size() && Query[i] == ' ') ++i;
            if (i >= Query.size()) break;
            std::string Token;
            bool bQuoted = false;
            for (; i < Query.size() && (bQuoted || Query[i] != ' '); ++i) { if (Query[i] == '"') bQuoted = !bQuoted; Token += Query[i]; }
            Tokens.push_back(std::move(Token));
        }
        return Tokens;
    }

    // The value of the first "Key:" token, or empty
    inline std::string QueryValue(std::string_view Query, std::string_view Key) noexcept
    {
        const std::string Prefix = std::string(Key) + ":";
        for (const auto& T : SplitQuery(Query)) if (T.rfind(Prefix, 0) == 0) return T.substr(Prefix.size());
        return {};
    }

    // The query with its "Key:" tokens replaced by one "Key:Value" (none when Value is empty): what picking a lens does to the text
    inline std::string WithQueryToken(std::string_view Query, std::string_view Key, std::string_view Value) noexcept
    {
        const std::string Prefix = std::string(Key) + ":";
        std::string Out;
        for (const auto& T : SplitQuery(Query))
            if (T.rfind(Prefix, 0) != 0) { if (!Out.empty()) Out += ' '; Out += T; }
        if (!Value.empty()) { if (!Out.empty()) Out += ' '; Out += Prefix; Out += Value; }
        return Out;
    }

    // What the closed drawer's badge says: distinct problems (not occurrences) that still need attention. An acknowledged or muted problem no longer does; a Fatal one is counted
    // whatever was done to it (it cannot be hidden). New = first seen after the baseline.
    struct badge_counts
    {
        std::size_t m_Errors = 0, m_Warnings = 0, m_Critical = 0, m_New = 0;
        bool operator==(const badge_counts&) const noexcept = default;
        bool Any() const noexcept { return m_Errors || m_Warnings || m_Critical; }
    };

    inline badge_counts CountBadge(const hub& Hub) noexcept
    {
        badge_counts C;
        for (auto Id : Hub.ProblemOrder())
        {
            const problem* P = Hub.FindProblem(Id);
            if (!P) continue;
            if (P->m_Severity >= severity::Fatal) ++C.m_Critical;
            const annotation A = Hub.Annotation(P->m_Id);
            if (A.m_bMuted || A.m_bAcknowledged) continue;
            if (P->m_Severity >= severity::Error) ++C.m_Errors; else ++C.m_Warnings;
            if (P->m_FirstSeq > Hub.Baseline()) ++C.m_New;
        }
        return C;
    }

    // The events as plain text, to paste into any application: one header line each (time, severity, channel, title, code) and the body under it, indented. The window's
    // Copy and the pipe's LogCopy are this one function, so what is on the clipboard is what a script reads.
    inline std::string FormatEventsForCopy(const hub& Hub, const std::vector<std::uint64_t>& Sequences) noexcept
    {
        std::string Out;
        for (const auto Seq : Sequences)
        {
            const event* E = Hub.FindEvent(Seq);
            if (!E) continue;
            Out += std::format("{}  {:<7}  {}  {}{}\n", ClockText(E->m_ObservedAt), SeverityName(E->m_Severity), E->m_Channel, E->m_Title, E->m_Code.empty() ? std::string() : "  [" + E->m_Code + "]");
            std::size_t Start = 0;
            while (!E->m_Body.empty() && Start <= E->m_Body.size())
            {
                const auto Eol = E->m_Body.find('\n', Start);
                Out += "    "; Out.append(E->m_Body, Start, Eol == std::string::npos ? std::string::npos : Eol - Start); Out += '\n';
                if (Eol == std::string::npos) break;
                Start = Eol + 1;
            }
        }
        return Out;
    }

    struct view_state
    {
        // ---- what the person chose
        char            m_Query[256] = {};                  // the query bar, compiled with ParseQuery: the same grammar as the pipe's commands
        problem_view    m_View       = problem_view::Active;
        bool            m_bShowMuted = false;
        int             m_Page       = 0;                   // 0 Problems, 1 Events
        int             m_RequestPage = -1;                 // set by a button that wants the other page; consumed by the window
        bool            m_bFollow    = true;                // Events: stick to the newest
        std::uint64_t   m_ViewFrom   = 0;                   // Events: "Clear view" hides what is older (nothing is deleted)
        std::uint64_t   m_Selected   = 0;                   // a problem id
        std::uint64_t   m_SelectedEvent = 0;                // an event sequence
        std::vector<view_snapshot> m_Back;                  // newest last; bounded
        std::vector<view_snapshot> m_Forward;               // what Back left: Forward returns to it; any new navigation empties it (as in a browser)
        float           m_BackButton[2]    = { -1.0f, -1.0f };  // where the window last drew the Back / Forward buttons (screen coordinates of their centres; -1 = not drawn):
        float           m_ForwardButton[2] = { -1.0f, -1.0f };  // so a test can click them
        badge_counts    m_Badge;                            // cached by the hub's revision
        std::uint64_t   m_BadgeRevision = 0;
        float           m_BadgeAt[2]       = { -1.0f, -1.0f };  // where the badge was last drawn (a test clicks it); -1 = not drawn
        float           m_SourceChipAt[2]  = { -1.0f, -1.0f };  // where the two lens chips were last drawn (a test clicks them)
        float           m_AboutChipAt[2]   = { -1.0f, -1.0f };
        float           m_ViewsChipAt[2]   = { -1.0f, -1.0f };
        float           m_RulerAt[4]       = { -1.0f, -1.0f, -1.0f, -1.0f };  // the ruler's strip last drawn (x0, y0, x1, y1, screen): a test drags across it
        std::uint64_t   m_RulerFrom = 0, m_RulerTo = 0;                      // what part of the launch the ruler shows (collector clock ns); 0..0 = all of it
        float           m_MouseAt[2]       = { -1.0f, -1.0f };  // where the window last saw the pointer (ImGui's idea of it): a test aims its clicks by the difference
        bool            m_bScrollToSelected = false;        // the selected problem was just opened: bring it to the top so its details are in view
        std::vector<std::uint64_t> m_Expanded;              // problems whose details are open inline
        std::vector<std::uint64_t> m_ExpandedEvents;        // events open in place (each one alone, by its arrow)
        std::uint64_t   m_SelAnchor = 0, m_SelFrom = 0, m_SelTo = 0;   // the events selected: a range of sequences; a click sets the anchor, Shift+click extends
        float           m_EventRowAt[2]   = { -1.0f, -1.0f };  // where the first drawn event's row and arrow are (screen coordinates), and the step between rows: a test aims by them
        float           m_EventArrowAt[2] = { -1.0f, -1.0f };
        float           m_EventStride     = 0.0f;

        // ---- what was built from the hub (rebuilt only when the hub's revision or the inputs change: a frame costs nothing when nothing happened)
        std::vector<std::uint64_t> m_Rows;                  // the problems listed, in the order shown
        std::size_t     m_Pending       = 0;                // problems that matched but are not listed yet (the list does not move under the reader)
        std::size_t     m_Counts[3]     = {};               // New / Active / All, for the preset buttons
        std::size_t     m_HiddenByMutes = 0;
        std::string     m_RowsKey;
        std::uint64_t   m_RowsRevision  = 0;

        std::vector<std::uint64_t> m_EventRows;             // event sequences that match
        std::uint64_t   m_EventScanned  = 0;
        std::uint64_t   m_EventNew      = 0;                // matches that arrived while following was off
        std::string     m_EventKey;
        std::string     m_QueryError;

        // The window as it is now, as a Back entry. Label says what it is, in the words of the window.
        view_snapshot Snapshot() const noexcept
        {
            view_snapshot S;
            S.m_Query = m_Query; S.m_Page = m_Page; S.m_View = m_View; S.m_bShowMuted = m_bShowMuted; S.m_Selected = m_Selected; S.m_SelectedEvent = m_SelectedEvent;
            const char* pPage = m_Page == 0 ? "Problems" : "Events";
            S.m_Label = m_Query[0] ? std::format("{} · {}", pPage, m_Query) : (m_Page == 0 ? std::format("Problems · {}", ProblemViewName(m_View)) : std::string("Events"));
            return S;
        }

        // Remembers where the window is, before something moves it. Nothing is pushed when it would go back to the very same place.
        void PushBack(bool bHasReturn = false, int ReturnTab = -1, bool bReturnDrawerOpen = false) noexcept
        {
            view_snapshot S = Snapshot();
            S.m_bHasReturn = bHasReturn; S.m_ReturnTab = ReturnTab; S.m_bReturnDrawerOpen = bReturnDrawerOpen;
            if (!m_Back.empty() && !bHasReturn && m_Back.back().m_Query == S.m_Query && m_Back.back().m_Page == S.m_Page && m_Back.back().m_View == S.m_View) return;
            m_Back.push_back(std::move(S));
            if (m_Back.size() > 16) m_Back.erase(m_Back.begin());
            m_Forward.clear();                              // a new way somewhere: what was ahead of the old one is gone
        }

        // The window put back as the snapshot says.
        void Restore(const view_snapshot& S) noexcept
        {
            std::snprintf(m_Query, sizeof(m_Query), "%s", S.m_Query.c_str());
            m_View = S.m_View; m_bShowMuted = S.m_bShowMuted; m_Selected = S.m_Selected; m_SelectedEvent = S.m_SelectedEvent;
            m_RequestPage = S.m_Page;
            m_Expanded.clear();
        }

        // The window as a view the person keeps / a kept view put on the window (what was in front is a Back entry: a view is a place to go, not a loss).
        saved_view ToSaved(std::string Name, bool bTeam) const noexcept
        {
            saved_view V; V.m_Name = std::move(Name); V.m_Query = m_Query; V.m_Page = m_RequestPage >= 0 ? m_RequestPage : m_Page; V.m_State = static_cast<std::uint8_t>(m_View); V.m_bShowMuted = m_bShowMuted; V.m_bTeam = bTeam;
            return V;
        }
        void Apply(const saved_view& V) noexcept
        {
            PushBack();
            view_snapshot S; S.m_Query = V.m_Query; S.m_Page = V.m_Page; S.m_View = static_cast<problem_view>(std::min<int>(V.m_State, 2)); S.m_bShowMuted = V.m_bShowMuted;
            Restore(S);
        }

        // Puts the window back where it was and hands the entry over (the host restores what it had in front). False when there is nowhere to go back to.
        bool PopBack(view_snapshot& Out) noexcept
        {
            if (m_Back.empty()) return false;
            Out = std::move(m_Back.back());
            m_Back.pop_back();
            // Where the window is now is what Forward comes back to. It carries the same host return as the entry it left, so Forward can put the Back entry back as it was.
            view_snapshot Ahead = Snapshot();
            Ahead.m_bHasReturn = Out.m_bHasReturn; Ahead.m_ReturnTab = Out.m_ReturnTab; Ahead.m_bReturnDrawerOpen = Out.m_bReturnDrawerOpen;
            m_Forward.push_back(std::move(Ahead));
            if (m_Forward.size() > 16) m_Forward.erase(m_Forward.begin());
            Restore(Out);
            return true;
        }

        // The other way: back to where Back came from. The window as it is now becomes a Back entry again (with the host return the forward entry carries).
        bool PopForward(view_snapshot& Out) noexcept
        {
            if (m_Forward.empty()) return false;
            Out = std::move(m_Forward.back());
            m_Forward.pop_back();
            view_snapshot Behind = Snapshot();
            Behind.m_bHasReturn = Out.m_bHasReturn; Behind.m_ReturnTab = Out.m_ReturnTab; Behind.m_bReturnDrawerOpen = Out.m_bReturnDrawerOpen;
            m_Back.push_back(std::move(Behind));
            if (m_Back.size() > 16) m_Back.erase(m_Back.begin());
            Restore(Out);
            return true;
        }

        // The events of the list inside the selected range, in list order
        std::vector<std::uint64_t> SelectedEvents() const noexcept
        {
            std::vector<std::uint64_t> Out;
            if (m_SelFrom)
                for (auto Seq : m_EventRows) if (Seq >= m_SelFrom && Seq <= m_SelTo) Out.push_back(Seq);
            return Out;
        }

        // Opens or closes every selected event in place
        void SetSelectedEventsOpen(bool b) noexcept { for (auto Seq : SelectedEvents()) SetEventOpen(Seq, b); }

        bool IsEventOpen(std::uint64_t Seq) const noexcept { return std::find(m_ExpandedEvents.begin(), m_ExpandedEvents.end(), Seq) != m_ExpandedEvents.end(); }
        void SetEventOpen(std::uint64_t Seq, bool b) noexcept
        {
            auto It = std::find(m_ExpandedEvents.begin(), m_ExpandedEvents.end(), Seq);
            if (b && It == m_ExpandedEvents.end()) m_ExpandedEvents.push_back(Seq);
            else if (!b && It != m_ExpandedEvents.end()) m_ExpandedEvents.erase(It);
        }

        bool IsExpanded(std::uint64_t Id) const noexcept { return std::find(m_Expanded.begin(), m_Expanded.end(), Id) != m_Expanded.end(); }
        void SetExpanded(std::uint64_t Id, bool b) noexcept
        {
            auto It = std::find(m_Expanded.begin(), m_Expanded.end(), Id);
            if (b && It == m_Expanded.end()) m_Expanded.push_back(Id);
            else if (!b && It != m_Expanded.end()) m_Expanded.erase(It);
        }
    };

    // Rebuilds state.m_Rows when something it depends on changed. While the person has a selection or an open detail the list keeps its order and
    // only counts what is waiting (m_Pending): it is applied when they ask (Refresh), or when they have nothing selected.
    inline void BuildProblemRows(const hub& Hub, view_state& State, std::string_view ChannelPrefix, bool bRefresh = false) noexcept
    {
        const std::string Key = std::format("{}|{}|{}|{}", State.m_Query, static_cast<int>(State.m_View), State.m_bShowMuted, ChannelPrefix);
        const bool bSameInputs = Key == State.m_RowsKey;
        if (bSameInputs && Hub.Revision() == State.m_RowsRevision && !bRefresh) return;

        filter F = Hub.Parse(State.m_Query);
        State.m_QueryError = F.m_Error;
        State.m_RowsKey = Key;
        State.m_RowsRevision = Hub.Revision();
        State.m_Counts[0] = State.m_Counts[1] = State.m_Counts[2] = 0;
        State.m_HiddenByMutes = 0;

        std::vector<const problem*> Fresh;
        if (F.m_Error.empty())
            for (auto Id : Hub.ProblemOrder())
            {
                const problem* P = Hub.FindProblem(Id);
                if (!P || !ProblemMatches(Hub, *P, F)) continue;
                if (!ChannelPrefix.empty() && std::string_view(P->m_Channel).substr(0, ChannelPrefix.size()) != ChannelPrefix) continue;
                if (!Hub.InView(*P, problem_view::All, false)) { ++State.m_HiddenByMutes; if (!State.m_bShowMuted) continue; }
                ++State.m_Counts[2];
                if (Hub.InView(*P, problem_view::Active, true)) ++State.m_Counts[1];
                if (Hub.InView(*P, problem_view::New, true))    ++State.m_Counts[0];
                if (Hub.InView(*P, State.m_View, true)) Fresh.push_back(P);
            }

        // Worst first, then the most recent
        std::stable_sort(Fresh.begin(), Fresh.end(), [](const problem* A, const problem* B)
        {
            if (A->m_Severity != B->m_Severity) return A->m_Severity > B->m_Severity;
            return A->m_LastSeq > B->m_LastSeq;
        });

        const bool bReading = State.m_Selected != 0 || !State.m_Expanded.empty();
        if (bSameInputs && bReading && !bRefresh)
        {
            std::unordered_set<std::uint64_t> Listed(State.m_Rows.begin(), State.m_Rows.end());
            State.m_Pending = 0;
            for (const problem* P : Fresh) if (!Listed.contains(P->m_Id)) ++State.m_Pending;
            return;
        }
        State.m_Rows.clear();
        for (const problem* P : Fresh) State.m_Rows.push_back(P->m_Id);
        State.m_Pending = 0;
    }

    // Appends the events that match the query to state.m_EventRows. Incremental: only what arrived since the last call is looked at, so a frame is
    // O(new events) however large the store is; the list never holds more than Window rows.
    inline void ScanEvents(const hub& Hub, view_state& State, std::string_view ChannelPrefix, std::uint64_t Window, bool bShowCommands) noexcept
    {
        const std::string Key = std::format("{}|{}|{}|{}", State.m_Query, ChannelPrefix, bShowCommands, State.m_ViewFrom);
        bool bFresh = false;                                  // the first fill of a view: what it lists is not "new", it is what was asked for
        if (Key != State.m_EventKey)
        {
            bFresh = true;
            State.m_EventKey = Key;
            State.m_EventRows.clear();
            State.m_EventScanned = State.m_ViewFrom;
            State.m_EventNew = 0;
        }
        const std::uint64_t Committed = Hub.Committed();
        filter F = Hub.Parse(State.m_Query);
        State.m_QueryError = F.m_Error;
        if (!F.m_Error.empty()) { State.m_EventRows.clear(); State.m_EventScanned = Committed; return; }
        const std::uint64_t Oldest = Committed > Window ? Committed - Window : 0;
        std::uint64_t After = std::max(State.m_EventScanned, Oldest);
        Hub.ForEachEvent(After, Committed, [&](const event& E)
        {
            if (E.m_Kind == kind::Command && !bShowCommands) return true;
            if (!ChannelPrefix.empty() && std::string_view(E.m_Channel).substr(0, ChannelPrefix.size()) != ChannelPrefix) return true;
            if (!Matches(F, E)) return true;
            State.m_EventRows.push_back(E.m_Key.m_Sequence);
            if (!State.m_bFollow && !bFresh) ++State.m_EventNew;
            return true;
        });
        State.m_EventScanned = Committed;
        if (State.m_EventRows.size() > Window) State.m_EventRows.erase(State.m_EventRows.begin(), State.m_EventRows.begin() + (State.m_EventRows.size() - Window));
    }
}

#endif // XLOG_VIEW_H
