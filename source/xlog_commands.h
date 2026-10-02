#ifndef XLOG_COMMANDS_H
#define XLOG_COMMANDS_H
#pragma once

// The pipe commands of xlog (documentation/Editors/DESIGN_logs.md in xLION, section 7). They register into any xundo::system and read the current hub:
// no Level, no UI, no editor, so the headless host answers them too.
// Every query drains the ring first (the ingestion barrier): anything pushed before the query is in its answer.
//
// A reply is  "<Command>: ok"  then key=value header lines, then (for lists) a blank line, a header row and tab-separated rows. Text is escaped
// (\n, \t). A reply is capped; a truncated one says so and gives the cursor to continue from.
#include "xlog_hub.h"
#include "xlog_build.h"
#include "xlog_pipeline.h"
#include "xlog_view.h"
#include "xlog_store.h"
#include "dependencies/xundo/source/xundo_system.h"

#include <charconv>
#include <set>
#include <functional>
#include <cstdlib>
#include "xlog_stdout.h"

namespace xlog::commands
{
    inline std::string Escape(std::string_view In) noexcept
    {
        std::string Out;
        Out.reserve(In.size());
        for (char c : In)
        {
            if (c == '\n') Out += "\\n";
            else if (c == '\t') Out += "\\t";
            else if (c != '\r') Out += c;
        }
        return Out;
    }

    constexpr std::size_t reply_cap_v = 32 * 1024;      // far under the pipe's 64 KB buffer, and a sane AI budget

    // The lines of a body as "| line" continuation lines: they can never be mistaken for a reply row. Stops before the reply would pass MaxSize and
    // says where to continue (Next = the index of the first line not written); returns false then. FirstLine skips what an earlier page already gave.
    inline bool AppendBody(std::string& Out, std::string_view Body, std::size_t MaxSize, std::size_t FirstLine, std::size_t& Next) noexcept
    {
        std::size_t Start = 0, Index = 0;
        while (Start <= Body.size())
        {
            const auto Eol = Body.find('\n', Start);
            const auto Line = Body.substr(Start, Eol == std::string_view::npos ? Body.size() - Start : Eol - Start);
            if (!(Eol == std::string_view::npos && Line.empty()))
            {
                if (Index >= FirstLine)
                {
                    if (Out.size() + Line.size() + 3 > MaxSize) { Next = Index; return false; }
                    Out += "| "; Out += Line; Out += '\n';
                }
                ++Index;
            }
            if (Eol == std::string_view::npos) break;
            Start = Eol + 1;
        }
        Next = Index;
        return true;
    }

    inline bool ParseNumber(std::string_view Text, std::uint64_t& Out) noexcept
    {
        std::string S(Text);
        if (S.empty()) return false;
        char* pEnd = nullptr;
        Out = std::strtoull(S.c_str(), &pEnd, 0);
        return *pEnd == 0;
    }

    inline std::string Ms(std::uint64_t Ns) noexcept { return std::format("{}.{:03}", Ns / 1000000, (Ns / 1000) % 1000); }

    struct log_query : xundo::query_command_base
    {
        log_query(xundo::system& System, const char* pName) noexcept : xundo::query_command_base(System, pName, nullptr) {}

        hub* Hub() noexcept { return hub::current(); }

        bool Arg(xcmdline::parser::handle Handle, std::string& Out) noexcept
        {
            auto A = m_Parser.getOptionArgAs<std::string>(Handle, 0);
            if (std::holds_alternative<xerr>(A)) return false;
            Out = std::get<std::string>(A);
            return true;
        }

        // The barrier: everything any thread has pushed so far is committed before the answer is built.
        void Settle(hub& H) noexcept { for (int i = 0; i < 64 && H.Drain(1u << 16) > 0; ++i) {} }

        std::string Header(std::string_view Name, hub& H) noexcept
        {
            return std::format("{}: ok\nSession={}  CommittedThrough={}\n", Name, Hex16(H.Session()), H.Committed());
        }
    };

    //==================================================================================================================
    struct status_cmd : log_query
    {
        status_cmd(xundo::system& System) noexcept : log_query(System, "LogStatus") {}
        const char* getCommandHelp() const noexcept override { return "The state of the Logs: counts by severity, problems, collector health, the last build. Usage: LogStatus"; }
        void RegisterArguments() noexcept override {}
        std::string Query() noexcept override
        {
            auto* pHub = Hub();
            if (!pHub) return "LogStatus: no host";
            hub& H = *pHub;
            Settle(H);
            const auto S = H.Status();
            std::uint64_t Dropped = 0;
            for (auto D : S.m_Dropped) Dropped += D;

            std::string Out = Header("LogStatus", H);
            { std::string O; for (const auto& Origin : H.Origins()) O += (O.empty() ? "" : ",") + std::string(OriginTypeName(Origin.m_Type)) + ":" + Origin.m_Name; Out += "Origins=" + O + "\n"; }
            Out += std::format("Started={}ms  Events={}  Problems={}  Operations={}  Capacity={}  Backlog={}\n", H.StartWallMs(), S.m_Events, S.m_Problems, S.m_Operations, S.m_Capacity, S.m_Backlog);
            Out += std::format("EventsBy: trace={} debug={} info={} warning={} error={} fatal={}\n", S.m_BySeverity[0], S.m_BySeverity[1], S.m_BySeverity[2], S.m_BySeverity[3], S.m_BySeverity[4], S.m_BySeverity[5]);
            Out += std::format("ProblemsBy: warning={} error={} fatal={}\n", S.m_ProblemsBySeverity[3], S.m_ProblemsBySeverity[4], S.m_ProblemsBySeverity[5]);
            const badge_counts B = CountBadge(H);
            Out += std::format("Badge: errors={} warnings={} new={} critical={}\n", B.m_Errors, B.m_Warnings, B.m_New, B.m_Critical);
            {
                std::string Excluded;
                for (int i = 0; i < 6; ++i) if (const auto N = H.ExcludedCount(static_cast<severity>(i))) Excluded += std::format("{}{}:{}", Excluded.empty() ? "" : ",", SeverityName(static_cast<severity>(i)), N);
                Out += std::format("Dropped={}  Expired={}  Excluded={}  PendingWrite={}  PersistenceFailed={}\n", Dropped, S.m_Expired, Excluded.empty() ? "none" : Excluded, H.SinkStatus().m_Pending, H.SinkStatus().m_bFailed ? "true" : "false");
                if (const std::string Remote = H.RemoteStatus(); !Remote.empty()) Out += "Remote: " + Remote + "\n";
                if (H.HasSink()) { const sink_status St = H.SinkStatus(); Out += std::format("Persistence: Directory={} Written={} PersistenceDropped={} Import={}{}\n", Escape(H.PersistenceDirectory()), St.m_Written, St.m_Dropped, H.ImportState().empty() ? "none" : H.ImportState(), St.m_bFailed ? " Reason=" + St.m_Reason : std::string()); }
                else Out += "Persistence: none (this launch is kept in memory only)\n";
                std::string Rules;
                for (const auto& R : H.CaptureRules()) Rules += std::format(" {}>={}{}", R.m_Prefix, SeverityName(R.m_Min), R.m_UntilNs ? std::format("(for {}s more)", (R.m_UntilNs - std::min(R.m_UntilNs, H.Now())) / 1000000000ull) : std::string());
                Out += std::format("Capture: default>={}{}\n", SeverityName(H.DefaultCapture()), Rules.empty() ? std::string() : "  rules:" + Rules);
            }
            for (auto It = H.OperationOrder().rbegin(); It != H.OperationOrder().rend(); ++It)
            {
                const auto* O = H.FindOperation(*It);
                if (O && O->m_Kind == "game.build")
                {
                    Out += std::format("LastBuild: id={} Outcome={} EvidenceReady={} Problems={}\n", O->m_Id, OutcomeName(O->m_Outcome), O->m_bEvidenceReady, O->m_Problems.size());
                    break;
                }
            }
            return Out;
        }
    };

    //==================================================================================================================
    struct operations_cmd : log_query
    {
        operations_cmd(xundo::system& System) noexcept : log_query(System, "LogOperations") { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override { return "Operations (builds, compiles, loads, tests) newest first, with outcome, EvidenceReady and coverage. Usage: LogOperations [-Kind k] [-Outcome Failed|Succeeded|Running|Cancelled|Abandoned] [-Id n] [-Limit n] [-After id]"; }
        void RegisterArguments() noexcept override
        {
            m_hKind = m_Parser.addOption("Kind", "Operation kind, e.g. game.build", false, 1);
            m_hOutcome = m_Parser.addOption("Outcome", "Only this outcome", false, 1);
            m_hId = m_Parser.addOption("Id", "One operation", false, 1);
            m_hLimit = m_Parser.addOption("Limit", "Rows, default 20", false, 1);
            m_hAfter = m_Parser.addOption("After", "Continue after this operation id (older ones)", false, 1);
        }
        std::string Query() noexcept override
        {
            auto* pHub = Hub();
            if (!pHub) return "LogOperations: no host";
            hub& H = *pHub;
            Settle(H);
            std::string Kind, Outcome, Text; std::uint64_t Id = 0, Limit = 20, After = 0;
            Arg(m_hKind, Kind); Arg(m_hOutcome, Outcome);
            if (Arg(m_hId, Text) && !ParseNumber(Text, Id)) return "LogOperations: -Id is not a number";
            if (Arg(m_hLimit, Text) && !ParseNumber(Text, Limit)) return "LogOperations: -Limit is not a number";
            if (Arg(m_hAfter, Text) && !ParseNumber(Text, After)) return "LogOperations: -After is not a number";

            std::string Rows = "Id\tKind\tOutcome\tEvidenceReady\tDurationMs\tCoverage\tUnits\tEvents\tErrors\tWarnings\tProblems\tOrigin\tSubject\tTitle\n";
            std::size_t Matched = 0, Returned = 0; std::uint64_t Last = 0; bool bTruncated = false;
            for (auto It = H.OperationOrder().rbegin(); It != H.OperationOrder().rend(); ++It)
            {
                const operation* O = H.FindOperation(*It);
                if (!O) continue;
                if (Id && O->m_Id != Id) continue;
                if (!Kind.empty() && O->m_Kind != Kind) continue;
                if (!Outcome.empty() && Outcome != OutcomeName(O->m_Outcome)) continue;
                ++Matched;
                if (After && O->m_Id >= After) continue;
                if (Returned >= Limit || Rows.size() > reply_cap_v) { bTruncated = true; continue; }
                const auto Duration = (O->m_Ended ? O->m_Ended : H.Now()) - O->m_Started;
                Rows += std::format("{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}:{}\t{}\t{}\n", O->m_Id, O->m_Kind, OutcomeName(O->m_Outcome), O->m_bEvidenceReady ? "true" : "false"
                    , Ms(Duration), CoverageName(O->m_Coverage), O->m_Units.size(), O->m_Events, O->m_Errors, O->m_Warnings, O->m_Problems.size()
                    , OriginTypeName(O->m_Origin.m_Type), Escape(O->m_Origin.m_Name), Escape(O->m_Subject.m_Path), Escape(O->m_Title));
                ++Returned; Last = O->m_Id;
            }
            std::string Out = Header("LogOperations", H);
            Out += std::format("Matched={} Returned={}{}\n", Matched, Returned, bTruncated ? std::format("  Truncated=true Cursor={}", Last) : "");
            if (Id)
                if (const operation* O = H.FindOperation(Id))
                {
                    Out += std::format("VerificationTarget={}\n", Escape(O->m_VerificationTarget));
                    for (const auto& U : O->m_Units) Out += std::format("Unit={}\n", Escape(U));
                }
            return Out + "\n" + Rows;
        }
        xcmdline::parser::handle m_hKind, m_hOutcome, m_hId, m_hLimit, m_hAfter;
    };

    //==================================================================================================================
    struct problems_cmd : log_query
    {
        problems_cmd(xundo::system& System) noexcept : log_query(System, "LogProblems") { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override { return "Problems: one row per diagnostic identity, in first-seen order. Usage: LogProblems [-Query q] [-State New|Active|All] [-IncludeMuted true] [-MinSeverity Warning|Error|Fatal] [-Operation id] [-Limit n] [-After cursor]. Without -State every problem is listed, muted ones too (Suppression says which); with it the rows are the window's list."; }
        void RegisterArguments() noexcept override
        {
            m_hQuery = m_Parser.addOption("Query", "sev>=error channel:game.* code:C2065 op:42 text ...", false, 1);
            m_hMin = m_Parser.addOption("MinSeverity", "Warning, Error or Fatal (Error includes Fatal)", false, 1);
            m_hOperation = m_Parser.addOption("Operation", "Only problems that occurred inside this operation", false, 1);
            m_hLimit = m_Parser.addOption("Limit", "Rows, default 50", false, 1);
            m_hAfter = m_Parser.addOption("After", "The cursor of the previous page", false, 1);
            m_hState = m_Parser.addOption("State", "The window's presets: New (first seen after the baseline), Active (not acknowledged), All; muted problems are left out", false, 1);
            m_hMuted = m_Parser.addOption("IncludeMuted", "true: list the muted problems too (with -State)", false, 1);
        }
        std::string Query() noexcept override
        {
            auto* pHub = Hub();
            if (!pHub) return "LogProblems: no host";
            hub& H = *pHub;
            Settle(H);
            std::string QueryString, Text; severity Min = severity::Trace; std::uint64_t Limit = 50, Operation = 0, After = 0;
            Arg(m_hQuery, QueryString);
            if (Arg(m_hMin, Text) && !ParseSeverity(Text, Min)) return std::format("LogProblems: unknown severity '{}'", Text);
            if (Arg(m_hOperation, Text) && !ParseNumber(Text, Operation)) return "LogProblems: -Operation is not a number";
            if (Arg(m_hLimit, Text) && !ParseNumber(Text, Limit)) return "LogProblems: -Limit is not a number";
            if (Arg(m_hAfter, Text))
            {
                const auto Colon = Text.find(':');
                if (!ParseNumber(std::string_view(Text).substr(Colon == std::string::npos ? 0 : Colon + 1), After)) return "LogProblems: -After is not a cursor";
            }
            problem_view View = problem_view::All; bool bState = false, bMuted = false;
            if (Arg(m_hState, Text)) { if (!ParseProblemView(Text, View)) return std::format("LogProblems: unknown state '{}' (New, Active, All)", Text); bState = true; }
            if (Arg(m_hMuted, Text)) bMuted = Text == "true";
            filter F = H.Parse(QueryString);
            if (!F.m_Error.empty()) return std::format("LogProblems: invalid query: {}", F.m_Error);
            if (Operation) F.m_Operation = Operation;

            std::string Rows = "Id\tSeverity\tCode\tOccurrences\tFirstSeq\tLastSeq\tSite\tSubject\tHeuristic\tUnit\tTitle\tTriage\tSuppression\tVerification\tPresence\tRegressions\tRecurring\n";
            std::size_t Matched = 0, Returned = 0, Occurrences = 0, Index = 0, LastIndex = 0; bool bTruncated = false;
            for (auto Id : H.ProblemOrder())
            {
                ++Index;
                const problem* P = H.FindProblem(Id);
                if (!P || !ProblemMatches(H, *P, F, Min) || (bState && !H.InView(*P, View, bMuted))) continue;
                ++Matched; Occurrences += P->m_Count;
                if (Index <= After) continue;
                if (Returned >= Limit || Rows.size() > reply_cap_v) { bTruncated = true; continue; }
                const std::string Site = P->m_Site.m_Type == ref::type::File ? (P->m_Site.m_Line > 0 ? std::format("{}:{}", P->m_Site.m_Path, P->m_Site.m_Line) : P->m_Site.m_Path) : std::string{};
                const annotation A = H.Annotation(P->m_Id);
                Rows += std::format("{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\n", Hex16(P->m_Id), SeverityName(P->m_Severity), Escape(P->m_Code), P->m_Count, P->m_FirstSeq, P->m_LastSeq
                    , Escape(Site), Escape(P->m_Subject.m_Path.empty() ? P->m_Discriminator : P->m_Subject.m_Path), P->m_bHeuristic ? "true" : "false", Escape(P->m_CheckUnit), Escape(P->m_Title)
                    , A.m_bAcknowledged ? "Acknowledged" : "Unreviewed", A.m_bMuted ? "Muted" : "None"
                    , VerificationName(P->m_Verification), PresenceName(P->m_Presence), P->m_Regressions, P->m_bRecurring ? "true" : "false");
                ++Returned; LastIndex = Index;
            }
            std::string Out = Header("LogProblems", H);
            Out += std::format("Query={}\n", Normalized(F));
            Out += std::format("Matched={} Returned={} Occurrences={}{}\n", Matched, Returned, Occurrences, bTruncated ? std::format("  Truncated=true Cursor={}:{}", H.Committed(), LastIndex) : "");
            Out += std::format("Evidence=full  Gaps=none  Excluded=none  Dropped=0\n");
            return Out + "\n" + Rows;
        }
        xcmdline::parser::handle m_hQuery, m_hMin, m_hOperation, m_hLimit, m_hAfter, m_hState, m_hMuted;
    };

    //==================================================================================================================
    struct problem_cmd : log_query
    {
        problem_cmd(xundo::system& System) noexcept : log_query(System, "LogProblem") { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override { return "One problem in full: identity, state, source, retained occurrences and the last occurrence's body. Usage: LogProblem -Id hex16"; }
        void RegisterArguments() noexcept override { m_hId = m_Parser.addOption("Id", "The problem id (16 hex digits) as LogProblems prints it", true, 1); }
        std::string Query() noexcept override
        {
            auto* pHub = Hub();
            if (!pHub) return "LogProblem: no host";
            hub& H = *pHub;
            Settle(H);
            std::string Text;
            if (!Arg(m_hId, Text)) return "LogProblem: -Id is required";
            std::uint64_t Id = std::strtoull(Text.c_str(), nullptr, 16);
            const problem* P = H.FindProblem(Id);
            if (!P) return std::format("LogProblem: no problem {}", Text);

            std::string Out = Header("LogProblem", H);
            Out += std::format("Id={}  Severity={}  Code={}  Producer={}  Channel={}\n", Hex16(P->m_Id), SeverityName(P->m_Severity), P->m_Code, P->m_Producer, P->m_Channel);
            Out += std::format("Occurrences={}  FirstSeq={}  LastSeq={}  Heuristic={}  Discriminator={}\n", P->m_Count, P->m_FirstSeq, P->m_LastSeq, P->m_bHeuristic ? "true" : "false", Escape(P->m_Discriminator));
            if (P->m_Site.m_Type == ref::type::File) Out += std::format("Site={}:{}:{}\n", P->m_Site.m_Path, P->m_Site.m_Line, P->m_Site.m_Column);
            Out += std::format("LastOperation={}  CheckUnit={}\n", P->m_LastOperation, Escape(P->m_CheckUnit));
            // The four dimensions of the lifecycle: triage and suppression are the person's (acknowledge, mute); verification arrives with P2.
            const annotation A = H.Annotation(P->m_Id);
            Out += std::format("Triage={}  Verification={}  Suppression={}  RunPresence={}\n", A.m_bAcknowledged ? "Acknowledged" : "Unreviewed", VerificationName(P->m_Verification), A.m_bMuted ? "Muted" : "None", PresenceName(P->m_Presence));
            Out += std::format("Target={}  VerifiedBy={}  Regressions={}  Recurring={}  PreviousSession={}\n", Escape(P->m_Target), P->m_VerifiedBy, P->m_Regressions, P->m_bRecurring ? "true" : "false", P->m_PreviousSession.empty() ? "-" : P->m_PreviousSession);
            const auto Retained = P->m_First.size() + P->m_Last.size();
            Out += std::format("Evidence={}  Observed={}  Retained={}\n", Retained < P->m_Count ? "summarized" : "full", P->m_Count, Retained);
            for (auto Seq : P->m_First) Out += std::format("Occurrence={}\n", Seq);
            for (auto Seq : P->m_Last)  Out += std::format("Occurrence={}\n", Seq);
            Out += std::format("Title={}\n", Escape(P->m_Title));
            if (const event* E = H.FindEvent(P->m_LastSeq); E && !E->m_Body.empty()) { std::size_t Next = 0; AppendBody(Out, E->m_Body, reply_cap_v, 0, Next); }
            return Out;
        }
        xcmdline::parser::handle m_hId;
    };

    //==================================================================================================================
    struct events_cmd : log_query
    {
        events_cmd(xundo::system& System) noexcept : log_query(System, "LogEvents") { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override { return "Events, oldest first, one row each (title only, Lines = body lines). Usage: LogEvents [-Query q] [-MinSeverity s] [-Operation id] [-Limit n] [-After cursor]"; }
        void RegisterArguments() noexcept override
        {
            m_hQuery = m_Parser.addOption("Query", "sev>=error channel:game.* code:C2065 op:42 origin:msbuild body:text text ...", false, 1);
            m_hMin = m_Parser.addOption("MinSeverity", "Trace, Debug, Info, Warning, Error or Fatal", false, 1);
            m_hOperation = m_Parser.addOption("Operation", "Only events inside this operation", false, 1);
            m_hLimit = m_Parser.addOption("Limit", "Rows, default 50", false, 1);
            m_hAfter = m_Parser.addOption("After", "The cursor of the previous page (Snapshot:Sequence)", false, 1);
        }
        std::string Query() noexcept override
        {
            auto* pHub = Hub();
            if (!pHub) return "LogEvents: no host";
            hub& H = *pHub;
            Settle(H);
            std::string QueryString, Text; severity Min = severity::Trace; std::uint64_t Limit = 50, Operation = 0, After = 0, UpTo = H.Committed();
            Arg(m_hQuery, QueryString);
            if (Arg(m_hMin, Text) && !ParseSeverity(Text, Min)) return std::format("LogEvents: unknown severity '{}'", Text);
            if (Arg(m_hOperation, Text) && !ParseNumber(Text, Operation)) return "LogEvents: -Operation is not a number";
            if (Arg(m_hLimit, Text) && !ParseNumber(Text, Limit)) return "LogEvents: -Limit is not a number";
            if (Arg(m_hAfter, Text))
            {
                const auto Colon = Text.find(':');
                std::uint64_t Snapshot = 0;
                if (Colon == std::string::npos || !ParseNumber(std::string_view(Text).substr(0, Colon), Snapshot) || !ParseNumber(std::string_view(Text).substr(Colon + 1), After)) return "LogEvents: -After is not a cursor";
                UpTo = Snapshot;        // the same snapshot as the previous page: nothing is skipped or repeated while events arrive
            }
            filter F = H.Parse(QueryString);
            if (!F.m_Error.empty()) return std::format("LogEvents: invalid query: {}", F.m_Error);
            if (Min > F.m_Min) F.m_Min = Min;
            if (Operation) F.m_Operation = Operation;

            std::string Rows = "Seq\tTimeMs\tSeverity\tChannel\tOrigin\tCode\tLines\tTitle\n";
            std::size_t Matched = 0, Returned = 0; std::uint64_t Last = After; bool bTruncated = false;
            H.ForEachEvent(0, UpTo, [&](const event& E)
            {
                if (!Matches(F, E)) return true;
                ++Matched;
                if (E.m_Key.m_Sequence <= After) return true;
                if (Returned >= Limit || Rows.size() > reply_cap_v) { bTruncated = true; return true; }
                Rows += std::format("{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\n", E.m_Key.m_Sequence, Ms(E.m_ObservedAt), SeverityName(E.m_Severity), Escape(E.m_Channel)
                    , Escape(E.m_Origin.m_Name), Escape(E.m_Code), E.m_BodyLines, Escape(E.m_Title));
                ++Returned; Last = E.m_Key.m_Sequence;
                return true;
            });
            std::string Out = Header("LogEvents", H);
            Out += std::format("Query={}\n", Normalized(F));
            Out += std::format("Snapshot={}  Matched={} Returned={}{}\n", UpTo, Matched, Returned, bTruncated ? std::format("  Truncated=true Cursor={}:{}", UpTo, Last) : "");
            const auto S = H.Status();
            Out += std::format("Evidence={}  Gaps={}  Excluded=none  Dropped=0  Expired={}\n", S.m_Expired ? "partial" : "full", S.m_Expired ? "expired-before-retained" : "none", S.m_Expired);
            return Out + "\n" + Rows;
        }
        xcmdline::parser::handle m_hQuery, m_hMin, m_hOperation, m_hLimit, m_hAfter;
    };

    struct event_cmd : log_query
    {
        event_cmd(xundo::system& System) noexcept : log_query(System, "LogEvent") { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override { return "One event in full, the body as '| ' lines, and the events around it. Usage: LogEvent -Id seq [-Context n]"; }
        void RegisterArguments() noexcept override
        {
            m_hId = m_Parser.addOption("Id", "The event sequence number (or session.seq)", true, 1);
            m_hContext = m_Parser.addOption("Context", "Events before and after to list, default 0", false, 1);
            m_hOffset = m_Parser.addOption("Offset", "The first body line to give (to continue a truncated reply)", false, 1);
        }
        std::string Query() noexcept override
        {
            auto* pHub = Hub();
            if (!pHub) return "LogEvent: no host";
            hub& H = *pHub;
            Settle(H);
            std::string Text; std::uint64_t Seq = 0, Context = 0, Offset = 0;
            if (!Arg(m_hId, Text)) return "LogEvent: -Id is required";
            if (const auto Dot = Text.find('.'); Dot != std::string::npos) Text = Text.substr(Dot + 1);
            if (!ParseNumber(Text, Seq)) return "LogEvent: -Id is not a number";
            if (Arg(m_hContext, Text) && !ParseNumber(Text, Context)) return "LogEvent: -Context is not a number";
            if (Arg(m_hOffset, Text) && !ParseNumber(Text, Offset)) return "LogEvent: -Offset is not a number";
            const event* E = H.FindEvent(Seq);
            if (!E) return std::format("LogEvent: no event {} (never recorded, or expired from the store)", Seq);

            std::string Out = Header("LogEvent", H);
            Out += std::format("Key={}.{}  TimeMs={}  Severity={}  Kind={}  Producer={}  Channel={}\n", Hex16(E->m_Key.m_Session), E->m_Key.m_Sequence, Ms(E->m_ObservedAt), SeverityName(E->m_Severity), KindName(E->m_Kind), E->m_Producer, E->m_Channel);
            for (const auto& A : H.Attachments()) if (A.m_Event == Seq) Out += std::format("Attachment={}  Id={}  Bytes={}  Path={}\n", Escape(A.m_Name), A.m_Id, A.m_Bytes, Escape(A.m_Path));
            Out += std::format("Origin={}:{}  Operation={}  Code={}  Heuristic={}  Summarized={}  Lines={}\n", OriginTypeName(E->m_Origin.m_Type), E->m_Origin.m_Name, E->m_Operation, E->m_Code, E->m_bHeuristic ? "true" : "false", E->m_bSummarized ? "true" : "false", E->m_BodyLines);
            if (E->m_Source.m_Type == ref::type::File) Out += std::format("Source={}:{}:{}\n", E->m_Source.m_Path, E->m_Source.m_Line, E->m_Source.m_Column);
            if (!E->m_Discriminator.empty()) Out += std::format("Discriminator={}\n", Escape(E->m_Discriminator));
            for (const auto& A : E->m_Attributes)
            {
                std::string V;
                std::visit([&](const auto& X) { if constexpr (std::is_same_v<std::decay_t<decltype(X)>, std::string>) V = X; else if constexpr (std::is_same_v<std::decay_t<decltype(X)>, bool>) V = X ? "true" : "false"; else V = std::format("{}", X); }, A.m_Value);
                Out += std::format("Attr.{}={}\n", A.m_Name, Escape(V));
            }
            Out += std::format("Title={}\n", Escape(E->m_Title));
            bool bTruncated = false;
            if (!E->m_Body.empty())
            {
                // The reply keeps to its budget: a long body is given in pages, and the reply says where the next one starts.
                std::string BodyText; std::size_t Next = 0;
                bTruncated = !AppendBody(BodyText, E->m_Body, reply_cap_v - Out.size() - 128, static_cast<std::size_t>(Offset), Next);
                if (bTruncated) Out += std::format("Truncated=true  NextOffset={}\n", Next);
                Out += BodyText;
            }
            if (Context && !bTruncated)
            {
                Out += "Context:\n";
                const auto From = Seq > Context ? Seq - Context - 1 : 0;
                H.ForEachEvent(From, Seq + Context, [&](const event& C) { Out += std::format("{}{}\t{}\t{}\t{}\n", C.m_Key.m_Sequence == Seq ? "> " : "  ", C.m_Key.m_Sequence, SeverityName(C.m_Severity), Escape(C.m_Channel), Escape(C.m_Title)); return true; });
            }
            return Out;
        }
        xcmdline::parser::handle m_hId, m_hContext, m_hOffset;
    };

    //==================================================================================================================
    // What the Logs window is showing (its query, page and preset): a script that opened it for a person can read where it left them, and the
    // smoke tests can check what "Open in Logs" did. The window's state belongs to the host; it comes in as a getter.
    struct window_cmd : log_query
    {
        std::function<view_state*()> m_Get;
        window_cmd(xundo::system& System, std::function<view_state*()> Get) noexcept : log_query(System, "LogWindow"), m_Get(std::move(Get)) {}
        const char* getCommandHelp() const noexcept override { return "What the Logs window shows: Page (Problems|Events), Query, State (New|Active|All), ShowMuted, Selected. Usage: LogWindow"; }
        void RegisterArguments() noexcept override {}
        std::string Query() noexcept override
        {
            const view_state* pView = m_Get ? m_Get() : nullptr;
            if (!pView) return "LogWindow: no window";
            return std::format("LogWindow: ok\nPage={}  State={}  ShowMuted={}  Follow={}\nSelected={}  SelectedEvent={}\nBack={}\nBackDepth={}  ForwardDepth={}  BackAt={:.0f},{:.0f}  ForwardAt={:.0f},{:.0f}  MouseAt={:.0f},{:.0f}\nRulerAt={:.0f},{:.0f},{:.0f},{:.0f}  BadgeAt={:.0f},{:.0f}  SourceChipAt={:.0f},{:.0f}  AboutChipAt={:.0f},{:.0f}  ViewsChipAt={:.0f},{:.0f}  EventsOpen={}  SelectedRange={}..{}  EventArrowAt={:.0f},{:.0f}  EventRowAt={:.0f},{:.0f}  EventStride={:.0f}\nQuery={}\n"
                , (pView->m_RequestPage >= 0 ? pView->m_RequestPage : pView->m_Page) == 0 ? "Problems" : "Events", ProblemViewName(pView->m_View)       // where it is, or where it is about to be
                , pView->m_bShowMuted, pView->m_bFollow, pView->m_Selected ? Hex16(pView->m_Selected) : std::string("none"), pView->m_SelectedEvent
                , pView->m_Back.empty() ? std::string("none") : pView->m_Back.back().m_Label, pView->m_Back.size(), pView->m_Forward.size()
                , pView->m_BackButton[0], pView->m_BackButton[1], pView->m_ForwardButton[0], pView->m_ForwardButton[1], pView->m_MouseAt[0], pView->m_MouseAt[1]
                , pView->m_RulerAt[0], pView->m_RulerAt[1], pView->m_RulerAt[2], pView->m_RulerAt[3], pView->m_BadgeAt[0], pView->m_BadgeAt[1], pView->m_SourceChipAt[0], pView->m_SourceChipAt[1], pView->m_AboutChipAt[0], pView->m_AboutChipAt[1], pView->m_ViewsChipAt[0], pView->m_ViewsChipAt[1], pView->m_ExpandedEvents.size(), pView->m_SelFrom, pView->m_SelTo, pView->m_EventArrowAt[0], pView->m_EventArrowAt[1], pView->m_EventRowAt[0], pView->m_EventRowAt[1], pView->m_EventStride, pView->m_Query);       // the buttons' centres (-1 = not drawn): a test clicks there
        }
    };

    // The text the window's Copy puts on the clipboard for the events of a range of sequences (LogEvents gives the sequences): the same function, so a script sees what a person pastes.
    struct copy_cmd : log_query
    {
        copy_cmd(xundo::system& System) noexcept : log_query(System, "LogCopy") { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override { return "The events of a range of sequences as the plain text the window's Copy puts on the clipboard. Usage: LogCopy -From seq [-To seq]"; }
        void RegisterArguments() noexcept override
        {
            m_hFrom = m_Parser.addOption("From", "First sequence", true, 1);
            m_hTo = m_Parser.addOption("To", "Last sequence, default From", false, 1);
        }
        std::string Query() noexcept override
        {
            auto* pHub = Hub();
            if (!pHub) return "LogCopy: no host";
            Settle(*pHub);
            std::string Text; std::uint64_t From = 0, To = 0;
            if (!Arg(m_hFrom, Text) || !ParseNumber(Text, From)) return "LogCopy: -From is not a sequence";
            To = From;
            if (Arg(m_hTo, Text) && !ParseNumber(Text, To)) return "LogCopy: -To is not a sequence";
            std::vector<std::uint64_t> Seqs;
            pHub->ForEachEvent(From > 0 ? From - 1 : 0, To, [&](const event& E) { Seqs.push_back(E.m_Key.m_Sequence); return Seqs.size() < 2000; });
            std::string Out = FormatEventsForCopy(*pHub, Seqs);
            if (Out.size() > reply_cap_v) Out.resize(reply_cap_v);
            return "LogCopy: ok\n" + Out;
        }
        xcmdline::parser::handle m_hFrom, m_hTo;
    };

    // What the right-click menu of the Events list does, on the selected events: Copy (the text, as the clipboard gets it), Open and Close (every selected event in place).
    struct events_action_cmd : log_query
    {
        std::function<view_state*()> m_Get;
        events_action_cmd(xundo::system& System, std::function<view_state*()> Get) noexcept : log_query(System, "LogEventsAction"), m_Get(std::move(Get)) { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override { return "The Events list's right-click menu on the selected events: copy (replies with the text the clipboard gets), open or close (every selected event in place). Usage: LogEventsAction -Action copy|open|close"; }
        void RegisterArguments() noexcept override { m_hAction = m_Parser.addOption("Action", "copy, open or close", true, 1); }
        std::string Query() noexcept override
        {
            view_state* pView = m_Get ? m_Get() : nullptr;
            auto* pHub = Hub();
            if (!pView || !pHub) return "LogEventsAction: no window";
            Settle(*pHub);
            std::string Action;
            Arg(m_hAction, Action);
            const auto Selected = pView->SelectedEvents();
            if (Action == "open" || Action == "close") { pView->SetSelectedEventsOpen(Action == "open"); return std::format("LogEventsAction: {} {} event(s)", Action, Selected.size()); }
            if (Action == "copy")
            {
                std::string Out = FormatEventsForCopy(*pHub, Selected);
                if (Out.size() > reply_cap_v) Out.resize(reply_cap_v);
                return std::format("LogEventsAction: copied {} event(s)\n", Selected.size()) + Out;
            }
            return "LogEventsAction: -Action must be copy, open or close";
        }
        xcmdline::parser::handle m_hAction;
    };

    // The Source and About lenses as a command: the same edit of the query's tokens the two chips make. -Origin a,b (or all), -About anything | op:N | asset:X.
    struct lens_cmd : log_query
    {
        std::function<view_state*()> m_Get;
        lens_cmd(xundo::system& System, std::function<view_state*()> Get) noexcept : log_query(System, "LogLens"), m_Get(std::move(Get)) { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override { return "Sets the Logs window's Source and About lenses (they are tokens of its query). Usage: LogLens [-Origin names|all] [-About anything|op:N|asset:X|deps:X]"; }
        void RegisterArguments() noexcept override
        {
            m_hOrigin = m_Parser.addOption("Origin", "Origin names, comma separated, or all", false, 1);
            m_hAbout = m_Parser.addOption("About", "anything, op:N, asset:X (an id or part of a name) or deps:X (that asset and what it depends on)", false, 1);
        }
        std::string Query() noexcept override
        {
            view_state* pView = m_Get ? m_Get() : nullptr;
            if (!pView) return "LogLens: no window";
            std::string Text, Query = pView->m_Query;
            if (Arg(m_hOrigin, Text)) Query = WithQueryToken(Query, "origin", Text == "all" ? std::string_view{} : std::string_view(Text));
            if (Arg(m_hAbout, Text))
            {
                if (Text == "anything") Query = WithQueryToken(WithQueryToken(WithQueryToken(Query, "asset", {}), "op", {}), "deps", {});
                else if (Text.rfind("deps:", 0) == 0)  Query = WithQueryToken(WithQueryToken(WithQueryToken(Query, "op", {}), "asset", Text.substr(5)), "deps", "yes");
                else if (Text.rfind("op:", 0) == 0)    Query = WithQueryToken(WithQueryToken(WithQueryToken(Query, "asset", {}), "deps", {}), "op", Text.substr(3));
                else if (Text.rfind("asset:", 0) == 0) Query = WithQueryToken(WithQueryToken(WithQueryToken(Query, "op", {}), "deps", {}), "asset", Text.substr(6));
                else return "LogLens: -About is anything, op:N, asset:X or deps:X";
            }
            if (const filter F = ParseQuery(Query); !F.m_Error.empty()) return std::format("LogLens: invalid query: {}", F.m_Error);
            std::snprintf(pView->m_Query, sizeof(pView->m_Query), "%s", Query.c_str());
            return std::format("LogLens: Query={}", Query);
        }
        xcmdline::parser::handle m_hOrigin, m_hAbout;
    };

    // The context pack of a problem: everything needed to understand it, deterministic and bounded (the window's Copy gives the same text).
    struct context_cmd : log_query
    {
        context_cmd(xundo::system& System) noexcept : log_query(System, "LogContext") { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override { return "The context pack of a problem: its state, where it is, the operation chain, the occurrences kept (whole bodies), the events around the last one, and what is missing, summarized or expired. Deterministic and bounded. Usage: LogContext -Id hex16 [-Budget bytes]"; }
        void RegisterArguments() noexcept override
        {
            m_hId = m_Parser.addOption("Id", "The problem id as LogProblems prints it", false, 1);
            m_hProblem = m_Parser.addOption("Problem", "The same (the design's name for it)", false, 1);
            m_hBudget = m_Parser.addOption("Budget", "Bytes, default 8192 (at most the reply cap)", false, 1);
        }
        std::string Query() noexcept override
        {
            auto* pHub = Hub();
            if (!pHub) return "LogContext: no host";
            Settle(*pHub);
            std::string Text; std::uint64_t Budget = 8192;
            if (!Arg(m_hId, Text) && !Arg(m_hProblem, Text)) return "LogContext: -Id is required";
            const problem* P = pHub->FindProblem(std::strtoull(Text.c_str(), nullptr, 16));
            if (!P) return std::format("LogContext: no problem {}", Text);
            if (Arg(m_hBudget, Text) && !ParseNumber(Text, Budget)) return "LogContext: -Budget is not a number";
            Budget = std::min<std::uint64_t>(Budget, reply_cap_v - 256);
            return Header("LogContext", *pHub) + BuildContextPack(*pHub, *P, static_cast<std::size_t>(Budget));
        }
        xcmdline::parser::handle m_hId, m_hProblem, m_hBudget;
    };

    // Raises the capture level of a channel for a while: "Focus" on what is being investigated. Undo puts the policy back; it does NOT recover what was not collected meanwhile.
    struct focus_cmd : xundo::command_base
    {
        focus_cmd(xundo::system& System) noexcept : xundo::command_base(System, "LogFocus", nullptr) { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override { return "Collects a channel at Trace level for a while (undoable: Undo restores the policy, it cannot recover events that were not collected meanwhile). Usage: LogFocus -Channel prefix -Minutes n | LogFocus -Channel prefix -Off true"; }
        void RegisterArguments() noexcept override
        {
            m_hChannel = m_Parser.addOption("Channel", "A channel or a prefix of channels, e.g. game. or physics", true, 1);
            m_hMinutes = m_Parser.addOption("Minutes", "How long (may be a fraction); default 5", false, 1);
            m_hOff = m_Parser.addOption("Off", "true: put the channel's capture back to the default", false, 1);
        }
        bool Arg(xcmdline::parser::handle Handle, std::string& Out) noexcept
        {
            auto A = m_Parser.getOptionArgAs<std::string>(Handle, 0);
            if (std::holds_alternative<xerr>(A)) return false;
            Out = std::get<std::string>(A);
            return true;
        }
        std::string Redo() noexcept override
        {
            auto* pHub = hub::current();
            if (!pHub) return "LogFocus: no host";
            std::string Channel, Text;
            if (!Arg(m_hChannel, Channel) || Channel.empty()) return "LogFocus: -Channel is required";
            if (Arg(m_hOff, Text) && Text == "true") { pHub->ClearCapture(Channel); return {}; }
            double Minutes = 5.0;
            if (Arg(m_hMinutes, Text)) { char* pEnd = nullptr; Minutes = std::strtod(Text.c_str(), &pEnd); if (Text.empty() || *pEnd != 0 || Minutes <= 0.0 || Minutes > 24 * 60) return "LogFocus: -Minutes is a number of minutes, more than 0"; }
            pHub->SetCapture(Channel, severity::Trace, static_cast<std::uint64_t>(Minutes * 60.0 * 1.0e9));
            return {};
        }
        void BackupCurrenState(xundo::undo_file& File) noexcept override
        {
            std::string Channel; Arg(m_hChannel, Channel);
            hub::capture_rule R; const bool bHad = hub::current() && hub::current()->GetCapture(Channel, R);
            const std::uint8_t Had = bHad ? 1 : 0, Min = static_cast<std::uint8_t>(R.m_Min);
            const std::uint64_t Left = bHad && R.m_UntilNs ? R.m_UntilNs - std::min(R.m_UntilNs, hub::current()->Now()) : 0;
            File.Write(Had); File.Write(Min); File.Write(Left);
            const std::uint32_t Size = static_cast<std::uint32_t>(Channel.size()); File.Write(Size);
            for (char c : Channel) File.Write(c);
        }
        void Undo(xundo::undo_file& File) noexcept override
        {
            std::uint8_t Had = 0, Min = 0; std::uint64_t Left = 0; std::uint32_t Size = 0;
            File.Read(Had); File.Read(Min); File.Read(Left); File.Read(Size);
            std::string Channel(Size, ' ');
            for (auto& c : Channel) File.Read(c);
            if (auto* pHub = hub::current()) { if (Had) pHub->SetCapture(Channel, static_cast<severity>(Min), Left); else pHub->ClearCapture(Channel); }
        }
        xcmdline::parser::handle m_hChannel, m_hMinutes, m_hOff;
    };

    // The views the person (or the team) keeps. LogViews lists them, and with -Load puts one on the window (Back returns to what was there).
    struct views_cmd : log_query
    {
        std::function<view_state*()> m_Get;
        views_cmd(xundo::system& System, std::function<view_state*()> Get) noexcept : log_query(System, "LogViews"), m_Get(std::move(Get)) { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override { return "The saved views of the Logs (yours and the team's); -Load puts one on the window. Usage: LogViews [-Load name]"; }
        void RegisterArguments() noexcept override { m_hLoad = m_Parser.addOption("Load", "The name of a saved view to show", false, 1); }
        std::string Query() noexcept override
        {
            auto* pHub = Hub();
            if (!pHub) return "LogViews: no host";
            std::string Name;
            if (Arg(m_hLoad, Name))
            {
                const saved_view* pView = pHub->FindView(Name);
                if (!pView) return std::format("LogViews: no saved view named '{}'", Name);
                view_state* pState = m_Get ? m_Get() : nullptr;
                if (!pState) return "LogViews: no window";
                pState->Apply(*pView);
                return std::format("LogViews: loaded '{}'  Query={}", Name, pView->m_Query);
            }
            std::string Rows = "Name\tScope\tPage\tState\tShowMuted\tQuery\n";
            for (const auto& V : pHub->SavedViews())
                Rows += std::format("{}\t{}\t{}\t{}\t{}\t{}\n", Escape(V.m_Name), V.m_bTeam ? "team" : "mine", V.m_Page == 0 ? "Problems" : "Events", ProblemViewName(static_cast<problem_view>(std::min<int>(V.m_State, 2))), V.m_bShowMuted, Escape(V.m_Query));
            return Header("LogViews", *pHub) + std::format("Views={}\n\n", pHub->SavedViews().size()) + Rows;
        }
        xcmdline::parser::handle m_hLoad;
    };

    // Keeps a view / forgets one (both undoable). Saving without -Query keeps what the window shows now.
    struct view_edit_cmd : xundo::command_base
    {
        std::function<view_state*()> m_Get;
        bool                         m_bDelete;
        view_edit_cmd(xundo::system& System, std::function<view_state*()> Get, bool bDelete) noexcept : xundo::command_base(System, bDelete ? "LogViewDelete" : "LogViewSave", nullptr), m_Get(std::move(Get)), m_bDelete(bDelete) { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override
        {
            return m_bDelete ? "Forgets a saved view (undoable). Usage: LogViewDelete -Name n"
                             : "Keeps a view of the Logs under a name (undoable); without -Query it keeps what the window shows. -Team true shares it through the project's files. Usage: LogViewSave -Name n [-Query q] [-Page Problems|Events] [-State New|Active|All] [-ShowMuted true] [-Team true]";
        }
        void RegisterArguments() noexcept override
        {
            m_hName = m_Parser.addOption("Name", "The view's name", true, 1);
            if (m_bDelete) return;
            m_hQuery = m_Parser.addOption("Query", "The query bar's text", false, 1);
            m_hPage = m_Parser.addOption("Page", "Problems or Events", false, 1);
            m_hState = m_Parser.addOption("State", "New, Active or All", false, 1);
            m_hMuted = m_Parser.addOption("ShowMuted", "true to list muted problems", false, 1);
            m_hTeam = m_Parser.addOption("Team", "true to share it with the team", false, 1);
        }
        bool Arg(xcmdline::parser::handle Handle, std::string& Out) noexcept
        {
            auto A = m_Parser.getOptionArgAs<std::string>(Handle, 0);
            if (std::holds_alternative<xerr>(A)) return false;
            Out = std::get<std::string>(A);
            return true;
        }
        std::string Name() noexcept { std::string Text; Arg(m_hName, Text); return Text; }
        std::string Redo() noexcept override
        {
            auto* pHub = hub::current();
            if (!pHub) return std::format("{}: no host", m_pCommandName);
            const std::string Name = this->Name();
            if (Name.empty() || Name.size() > 48 || Name.find_first_of("\t\n\r\"") != std::string::npos) return std::format("{}: -Name is 1 to 48 characters without tabs, quotes or line breaks", m_pCommandName);
            if (m_bDelete) return pHub->DeleteView(Name) ? std::string() : std::format("LogViewDelete: no saved view named '{}'", Name);
            std::string Text;
            view_state* pState = m_Get ? m_Get() : nullptr;
            saved_view V = pState ? pState->ToSaved(Name, false) : saved_view{ Name };
            if (Arg(m_hQuery, Text)) V.m_Query = Text;
            if (Arg(m_hPage, Text)) { if (Text != "Problems" && Text != "Events") return "LogViewSave: -Page is Problems or Events"; V.m_Page = Text == "Events"; }
            if (Arg(m_hState, Text)) { problem_view PV; if (!ParseProblemView(Text, PV)) return "LogViewSave: -State is New, Active or All"; V.m_State = static_cast<std::uint8_t>(PV); }
            if (Arg(m_hMuted, Text)) V.m_bShowMuted = Text == "true";
            if (Arg(m_hTeam, Text)) V.m_bTeam = Text == "true";
            if (const filter F = ParseQuery(V.m_Query); !F.m_Error.empty()) return std::format("LogViewSave: invalid query: {}", F.m_Error);
            pHub->SaveView(std::move(V));
            return {};
        }
        static void WriteText(xundo::undo_file& File, const std::string& S) noexcept { const std::uint32_t N = static_cast<std::uint32_t>(S.size()); File.Write(N); for (char c : S) File.Write(c); }
        static std::string ReadText(xundo::undo_file& File) noexcept { std::uint32_t N = 0; File.Read(N); std::string S(N, ' '); for (auto& c : S) File.Read(c); return S; }
        void BackupCurrenState(xundo::undo_file& File) noexcept override
        {
            const std::string Name = this->Name();
            const saved_view* pOld = hub::current() ? hub::current()->FindView(Name) : nullptr;
            const std::uint8_t Had = pOld ? 1 : 0;
            File.Write(Had);
            WriteText(File, Name);
            if (!pOld) return;
            WriteText(File, pOld->m_Query);
            const std::int32_t Page = pOld->m_Page; const std::uint8_t State = pOld->m_State, Muted = pOld->m_bShowMuted, Team = pOld->m_bTeam;
            File.Write(Page); File.Write(State); File.Write(Muted); File.Write(Team);
        }
        void Undo(xundo::undo_file& File) noexcept override
        {
            std::uint8_t Had = 0; File.Read(Had);
            saved_view V; V.m_Name = ReadText(File);
            if (Had)
            {
                V.m_Query = ReadText(File);
                std::int32_t Page = 0; std::uint8_t State = 0, Muted = 0, Team = 0;
                File.Read(Page); File.Read(State); File.Read(Muted); File.Read(Team);
                V.m_Page = Page; V.m_State = State; V.m_bShowMuted = Muted != 0; V.m_bTeam = Team != 0;
            }
            if (auto* pHub = hub::current()) { if (Had) pHub->SaveView(std::move(V)); else pHub->DeleteView(V.m_Name); }
        }
        xcmdline::parser::handle m_hName, m_hQuery, m_hPage, m_hState, m_hMuted, m_hTeam;
    };

    // What an asset depends on, as the editor's provider answers (the About lens "and what it depends on" selects events and problems about any of these too).
    struct dependencies_cmd : log_query
    {
        dependencies_cmd(xundo::system& System) noexcept : log_query(System, "LogDependencies") { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override { return "The assets an asset depends on, as the editor knows them (what the About lens's deps:X adds). Usage: LogDependencies -Asset id|name"; }
        void RegisterArguments() noexcept override { m_hAsset = m_Parser.addOption("Asset", "The asset's id (hex) or a part of its name", true, 1); }
        std::string Query() noexcept override
        {
            auto* pHub = Hub();
            if (!pHub) return "LogDependencies: no host";
            std::string Asset;
            if (!Arg(m_hAsset, Asset) || Asset.empty()) return "LogDependencies: -Asset is required";
            const auto Deps = pHub->Dependencies(Asset);
            std::string Rows = "Id\tName\n";
            for (const auto& D : Deps) Rows += std::format("{}\t{}\n", D.m_Id ? Hex16(D.m_Id) : std::string("-"), Escape(D.m_Path));
            return Header("LogDependencies", *pHub) + std::format("Asset={}  Dependencies={}\n\n", Asset, Deps.size()) + Rows;
        }
        xcmdline::parser::handle m_hAsset;
    };

    // The provider's stand-in for a test: this asset depends on these names.
    struct simulate_dependencies_cmd : log_query
    {
        simulate_dependencies_cmd(xundo::system& System) noexcept : log_query(System, "LogSimulateDependencies") { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override { return "Makes the hub believe an asset depends on some others (for tests). Usage: LogSimulateDependencies -Asset X -On name,name"; }
        void RegisterArguments() noexcept override
        {
            m_hAsset = m_Parser.addOption("Asset", "The asset (as the query's asset: names it)", true, 1);
            m_hOn = m_Parser.addOption("On", "The names of the assets it depends on, comma separated", true, 1);
        }
        std::string Query() noexcept override
        {
            auto* pHub = Hub();
            if (!pHub) return "LogSimulateDependencies: no host";
            std::string Asset, On;
            if (!Arg(m_hAsset, Asset) || !Arg(m_hOn, On)) return "LogSimulateDependencies: -Asset and -On are required";
            std::vector<ref> Deps;
            for (std::size_t At = 0; At <= On.size(); )
            {
                const auto Comma = On.find(',', At);
                const std::string Name = On.substr(At, Comma == std::string::npos ? std::string::npos : Comma - At);
                if (!Name.empty()) { ref R; R.m_Type = ref::type::Asset; R.m_Path = Name; Deps.push_back(std::move(R)); }
                if (Comma == std::string::npos) break;
                At = Comma + 1;
            }
            pHub->SimulateDependencies(Asset, std::move(Deps));
            return "LogSimulateDependencies: ok";
        }
        xcmdline::parser::handle m_hAsset, m_hOn;
    };

    // Keeps a file with an event or an operation.
    struct attach_cmd : log_query
    {
        attach_cmd(xundo::system& System) noexcept : log_query(System, "LogAttach") { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override { return "Copies a file into the launch's Logs folder and attaches it to an event or an operation (max 8 MB each, 64 MB a launch). Usage: LogAttach (-Path file | -Path64 text-of-file) (-Event seq | -Operation id) [-Name n]"; }
        void RegisterArguments() noexcept override
        {
            m_hPath = m_Parser.addOption("Path", "The file to keep", true, 1);
            m_hEvent = m_Parser.addOption("Event", "The event sequence it belongs to", false, 1);
            m_hOperation = m_Parser.addOption("Operation", "The operation id it belongs to", false, 1);
            m_hName = m_Parser.addOption("Name", "What to call it (default: the file's name)", false, 1);
        }
        std::string Query() noexcept override
        {
            auto* pHub = Hub();
            if (!pHub) return "LogAttach: no host";
            Settle(*pHub);
            std::string Path, Text, Name; std::uint64_t Event = 0, Operation = 0;
            if (!Arg(m_hPath, Path)) return "LogAttach: -Path is required";
            if (Arg(m_hEvent, Text) && !ParseNumber(Text, Event)) return "LogAttach: -Event is not a sequence";
            if (Arg(m_hOperation, Text) && !ParseNumber(Text, Operation)) return "LogAttach: -Operation is not an id";
            Arg(m_hName, Name);
            std::string Why;
            if (!store::Attach(*pHub, Path, Event, Operation, Name, Why)) return "LogAttach: not attached: " + Why;
            const auto& A = pHub->Attachments().back();
            return std::format("LogAttach: attached {}  Id={}  Bytes={}  Path={}", A.m_Name, A.m_Id, A.m_Bytes, A.m_Path);
        }
        xcmdline::parser::handle m_hPath, m_hEvent, m_hOperation, m_hName;
    };

    struct attachments_cmd : log_query
    {
        attachments_cmd(xundo::system& System) noexcept : log_query(System, "LogAttachments") { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override { return "The files kept with this launch's events and operations. Usage: LogAttachments [-Event seq] [-Operation id]"; }
        void RegisterArguments() noexcept override
        {
            m_hEvent = m_Parser.addOption("Event", "Only the ones of this event", false, 1);
            m_hOperation = m_Parser.addOption("Operation", "Only the ones of this operation", false, 1);
        }
        std::string Query() noexcept override
        {
            auto* pHub = Hub();
            if (!pHub) return "LogAttachments: no host";
            std::string Text; std::uint64_t Event = 0, Operation = 0;
            if (Arg(m_hEvent, Text) && !ParseNumber(Text, Event)) return "LogAttachments: -Event is not a sequence";
            if (Arg(m_hOperation, Text) && !ParseNumber(Text, Operation)) return "LogAttachments: -Operation is not an id";
            std::string Rows = "Id\tEvent\tOperation\tBytes\tName\tPath\n";
            std::size_t N = 0;
            for (const auto& A : pHub->Attachments())
            {
                if ((Event && A.m_Event != Event) || (Operation && A.m_Operation != Operation)) continue;
                Rows += std::format("{}\t{}\t{}\t{}\t{}\t{}\n", A.m_Id, A.m_Event, A.m_Operation, A.m_Bytes, Escape(A.m_Name), Escape(A.m_Path)); ++N;
            }
            return Header("LogAttachments", *pHub) + std::format("Attachments={}  Bytes={}\n\n", N, pHub->AttachmentBytes()) + Rows;
        }
        xcmdline::parser::handle m_hEvent, m_hOperation;
    };

    // The stdout tap: what printf and fprintf(stderr) print becomes events (and still goes where it was going).
    struct stdout_cmd : log_query
    {
        stdout_tap m_Tap;
        stdout_cmd(xundo::system& System) noexcept : log_query(System, "LogStdout") { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override { return "Turns the stdout tap on or off: what the process prints becomes events of channel process.stdout / process.stderr (severity is a guess; it also still goes to the console). Usage: LogStdout [-On true|false]"; }
        void RegisterArguments() noexcept override { m_hOn = m_Parser.addOption("On", "true or false; without it the state is reported", false, 1); }
        std::string Query() noexcept override
        {
            auto* pHub = Hub();
            if (!pHub) return "LogStdout: no host";
            std::string Text;
            if (Arg(m_hOn, Text))
            {
                if (Text != "true" && Text != "false") return "LogStdout: -On is true or false";
                if (Text == "true") { std::string Why; if (!m_Tap.Start(*pHub, Why)) return "LogStdout: not started: " + Why; }
                else m_Tap.Stop();
            }
            return std::format("LogStdout: {}  Lines={}", m_Tap.Running() ? "on" : "off", m_Tap.Lines());
        }
        xcmdline::parser::handle m_hOn;
    };

    // A test's printf: writes a line to the process' stdout (or stderr), through the real streams.
    struct simulate_stdout_cmd : log_query
    {
        simulate_stdout_cmd(xundo::system& System) noexcept : log_query(System, "LogSimulateStdout") { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override { return "Prints a line to the process' stdout or stderr, as legacy code would (for tests). Usage: LogSimulateStdout -Text text [-Stderr true]"; }
        void RegisterArguments() noexcept override
        {
            m_hText = m_Parser.addOption("Text", "The line", true, 1);
            m_hStderr = m_Parser.addOption("Stderr", "true: to stderr", false, 1);
        }
        std::string Query() noexcept override
        {
            std::string Text, Flag;
            if (!Arg(m_hText, Text)) return "LogSimulateStdout: -Text is required";
            if (Arg(m_hStderr, Flag) && Flag == "true") std::fprintf(stderr, "%s\n", Text.c_str()); else std::printf("%s\n", Text.c_str());
            std::fflush(stdout); std::fflush(stderr);
            return "LogSimulateStdout: printed";
        }
        xcmdline::parser::handle m_hText, m_hStderr;
    };

    // The runtimes that speak into this editor's Logs over the pipe (the host runs the listener; the line is the host's account).
    struct remote_cmd : log_query
    {
        remote_cmd(xundo::system& System) noexcept : log_query(System, "LogRemote") {}
        const char* getCommandHelp() const noexcept override { return "The remote runtimes: the pipe the editor listens on and how many runtimes are speaking. Usage: LogRemote"; }
        void RegisterArguments() noexcept override {}
        std::string Query() noexcept override
        {
            auto* pHub = Hub();
            if (!pHub) return "LogRemote: no host";
            const std::string Status = pHub->RemoteStatus();
            return Header("LogRemote", *pHub) + (Status.empty() ? std::string("Listening=false\n") : Status + "\n");
        }
    };

    // What the ruler draws, as data: per second the number of events and the worst severity, the operations as spans in lanes, and the markers. -From / -To (seconds) select a range.
    struct ruler_cmd : log_query
    {
        ruler_cmd(xundo::system& System) noexcept : log_query(System, "LogRuler") { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override { return "The ruler's data: events per second with the worst severity, operation spans in lanes, the baseline. Usage: LogRuler [-From s] [-To s]"; }
        void RegisterArguments() noexcept override
        {
            m_hFrom = m_Parser.addOption("From", "Seconds since the launch started", false, 1);
            m_hTo = m_Parser.addOption("To", "Seconds since the launch started", false, 1);
        }
        std::string Query() noexcept override
        {
            auto* pHub = Hub();
            if (!pHub) return "LogRuler: no host";
            Settle(*pHub);
            std::string Text; double From = 0, To = 1.0e9;
            if (Arg(m_hFrom, Text)) { char* e = nullptr; From = std::strtod(Text.c_str(), &e); if (Text.empty() || *e) return "LogRuler: -From is not a number of seconds"; }
            if (Arg(m_hTo, Text)) { char* e = nullptr; To = std::strtod(Text.c_str(), &e); if (Text.empty() || *e) return "LogRuler: -To is not a number of seconds"; }
            const auto& D = pHub->Density();
            std::string Buckets = "Second\tEvents\tWorst\n";
            std::size_t Shown = 0; std::uint64_t Total = 0;
            for (std::size_t i = 0; i < D.size(); ++i)
                if (D[i].m_Count && static_cast<double>(i + 1) > From && static_cast<double>(i) <= To) { Buckets += std::format("{}\t{}\t{}\n", i, D[i].m_Count, SeverityName(D[i].m_Worst)); ++Shown; Total += D[i].m_Count; }
            std::string Spans = "Operation\tKind\tOutcome\tStartMs\tEndMs\tLane\n";
            const auto All = ComputeSpans(*pHub);
            int Lanes = 0;
            for (const auto& S : All) { Spans += std::format("{}\t{}\t{}\t{}\t{}\t{}\n", S.m_Id, S.m_Kind, OutcomeName(S.m_Outcome), S.m_Start / 1000000, S.m_End / 1000000, S.m_Lane); Lanes = std::max(Lanes, S.m_Lane + 1); }
            std::string Baseline = "none";
            if (pHub->Baseline()) if (const event* E = pHub->FindEvent(pHub->Baseline())) Baseline = std::to_string(E->m_ObservedAt / 1000000) + "ms";
            return Header("LogRuler", *pHub) + std::format("Seconds={} Events={} Operations={} Lanes={} Baseline={}\n\n", Shown, Total, All.size(), Lanes, Baseline) + Buckets + "\n" + Spans;
        }
        xcmdline::parser::handle m_hFrom, m_hTo;
    };

    // Asks the producer to look again (design 7.3): a normal operation the producer runs, refused with a reason when it has no recheck. NOT undoable: a completed check cannot
    // unhappen, and the evidence it produces decides the verification state, not this command.
    struct verify_cmd : log_query
    {
        verify_cmd(xundo::system& System) noexcept : log_query(System, "LogVerify") { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override { return "Asks the producer of a problem to recheck it, as a normal operation (a build, a compile); refused with the reason when it has no recheck. The evidence of that operation decides the verification, not this command. Usage: LogVerify -Id hex16"; }
        void RegisterArguments() noexcept override { m_hId = m_Parser.addOption("Id", "The problem id as LogProblems prints it", true, 1); }
        std::string Query() noexcept override
        {
            auto* pHub = Hub();
            if (!pHub) return "LogVerify: no host";
            Settle(*pHub);
            std::string Text;
            if (!Arg(m_hId, Text)) return "LogVerify: -Id is required";
            const problem* P = pHub->FindProblem(std::strtoull(Text.c_str(), nullptr, 16));
            if (!P) return std::format("LogVerify: no problem {}", Text);
            const std::string Why = pHub->Recheck(*P);
            return Why.empty() ? std::format("LogVerify: recheck requested for {} ({})", Hex16(P->m_Id), P->m_Channel) : "LogVerify: refused: " + Why;
        }
        xcmdline::parser::handle m_hId;
    };

    // The launches on disk (and this one), with how each ended
    struct sessions_cmd : log_query
    {
        sessions_cmd(xundo::system& System) noexcept : log_query(System, "LogSessions") { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override { return "The launches the Logs know: id, when, how each ended (current, clean, confirmed-crash, interrupted, unknown: classified from evidence, never assumed), counts, whether its stream was cut at a half-written record. Usage: LogSessions [-Limit n]"; }
        void RegisterArguments() noexcept override { m_hLimit = m_Parser.addOption("Limit", "Rows, default 20", false, 1); }
        std::string Query() noexcept override
        {
            auto* pHub = Hub();
            if (!pHub) return "LogSessions: no host";
            Settle(*pHub);
            std::string Text; std::uint64_t Limit = 20;
            if (Arg(m_hLimit, Text) && !ParseNumber(Text, Limit)) return "LogSessions: -Limit is not a number";
            std::vector<store::session_info> Infos;
            const std::string Dir = pHub->PersistenceDirectory();
            for (const auto& D : store::SessionDirectories(Dir)) Infos.push_back(store::ReadInfo(D));
            std::sort(Infos.begin(), Infos.end(), [](const store::session_info& A, const store::session_info& B) { return A.m_Header.m_StartWallMs > B.m_Header.m_StartWallMs; });
            const std::string Current = Hex16(pHub->Session());
            std::string Rows = "Id\tStartMs\tEndMs\tTermination\tEvents\tErrors\tWarnings\tProblems\tOperations\tAbandoned\tTorn\tBytes\tPinned\tPid\n";
            std::size_t Returned = 0;
            auto Add = [&](const store::session_info& I, bool bCurrent)
            {
                if (Returned >= Limit) return;
                Rows += std::format("{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\n", I.m_Id, I.m_Header.m_StartWallMs, bCurrent ? 0 : I.m_EndWallMs, store::TerminationName(bCurrent ? store::termination::Current : I.m_Termination)
                    , I.m_Counts.m_Events, I.m_Counts.m_Errors, I.m_Counts.m_Warnings, I.m_Counts.m_Problems, I.m_Counts.m_Operations, I.m_Abandoned, I.m_bTorn ? "true" : "false", I.m_Bytes, I.m_bPinned ? "true" : "false", I.m_Header.m_Pid);
                ++Returned;
            };
            { // this launch, from the live store
                store::session_info Now; Now.m_Id = Current; Now.m_Header.m_StartWallMs = pHub->StartWallMs();
                const status S = pHub->Status();
                Now.m_Counts = { S.m_Events, S.m_BySeverity[4] + S.m_BySeverity[5], S.m_BySeverity[3], S.m_Operations, S.m_Problems };
                Now.m_Bytes = 0;
                Add(Now, true);
            }
            for (const auto& I : Infos) if (I.m_Id != Current) Add(I, false);
            return Header("LogSessions", *pHub) + std::format("Current={}  Directory={}  Import={}\n", Current, Escape(Dir), pHub->ImportState().empty() ? "none" : pHub->ImportState()) + "\n" + Rows;
        }
        xcmdline::parser::handle m_hLimit;
    };

    // Two launches side by side: which problems are new, which came back, which persist, which were not seen (not "resolved": that is a claim of verification), and how the operations went
    struct compare_cmd : log_query
    {
        compare_cmd(xundo::system& System) noexcept : log_query(System, "LogCompare") { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override { return "Compares two launches (ids from LogSessions, or current / previous). -What problems (default): new, resolved (verified in B), regressed (verified in A, back in B), persisting, not-seen. -What operations: the last operation of each kind in each. Usage: LogCompare -A id -B id [-What problems|operations]"; }
        void RegisterArguments() noexcept override
        {
            m_hA = m_Parser.addOption("A", "The earlier launch: a session id, previous or current", true, 1);
            m_hB = m_Parser.addOption("B", "The later launch: a session id, previous or current", true, 1);
            m_hWhat = m_Parser.addOption("What", "problems (default) or operations", false, 1);
        }
        struct side { hub* m_pHub = nullptr; std::unique_ptr<hub> m_pOwned; std::string m_Id; };
        bool Resolve(hub& Live, const std::string& Name, side& Out, std::string& Error) noexcept
        {
            const std::string Current = Hex16(Live.Session());
            std::string Wanted = Name;
            std::vector<store::session_info> Infos;
            for (const auto& D : store::SessionDirectories(Live.PersistenceDirectory())) { auto I = store::ReadInfo(D); if (I.m_Id != Current) Infos.push_back(std::move(I)); }
            std::sort(Infos.begin(), Infos.end(), [](const store::session_info& A, const store::session_info& B) { return A.m_Header.m_StartWallMs > B.m_Header.m_StartWallMs; });
            if (Name == "previous") { if (Infos.empty()) { Error = "there is no previous launch"; return false; } Wanted = Infos.front().m_Id; }
            if (Name == "current" || Wanted == Current) { Out.m_pHub = &Live; Out.m_Id = Current; return true; }
            for (const auto& I : Infos) if (I.m_Id == Wanted) { Out.m_pOwned = store::LoadSession(I.m_Dir); Out.m_pHub = Out.m_pOwned.get(); Out.m_Id = I.m_Id; return true; }
            Error = std::format("no launch {}", Name);
            return false;
        }
        std::string Query() noexcept override
        {
            auto* pHub = Hub();
            if (!pHub) return "LogCompare: no host";
            Settle(*pHub);
            std::string NameA, NameB, What = "problems", Error;
            if (!Arg(m_hA, NameA) || !Arg(m_hB, NameB)) return "LogCompare: -A and -B are required";
            Arg(m_hWhat, What);
            if (What != "problems" && What != "operations") return "LogCompare: -What is problems or operations";
            side A, B;
            if (!Resolve(*pHub, NameA, A, Error) || !Resolve(*pHub, NameB, B, Error)) return "LogCompare: " + Error;

            if (What == "operations")
            {
                auto Last = [](const hub& H) { std::unordered_map<std::string, const operation*> M; for (auto Id : H.OperationOrder()) if (const operation* O = H.FindOperation(Id)) M[O->m_Kind] = O; return M; };
                const auto MA = Last(*A.m_pHub), MB = Last(*B.m_pHub);
                std::set<std::string> Kinds;
                for (const auto& [K, O] : MA) Kinds.insert(K);
                for (const auto& [K, O] : MB) Kinds.insert(K);
                auto Ms = [](const operation* O, const hub& H) -> std::string { return O ? std::to_string(((O->m_Ended ? O->m_Ended : H.Now()) - O->m_Started) / 1000000) : "-"; };
                std::string Rows = "Kind\tOutcomeA\tOutcomeB\tDurationMsA\tDurationMsB\tChanged\n";
                for (const auto& K : Kinds)
                {
                    const operation* OA = MA.contains(K) ? MA.at(K) : nullptr; const operation* OB = MB.contains(K) ? MB.at(K) : nullptr;
                    const std::string Oa = OA ? OutcomeName(OA->m_Outcome) : "-", Ob = OB ? OutcomeName(OB->m_Outcome) : "-";
                    Rows += std::format("{}\t{}\t{}\t{}\t{}\t{}\n", K, Oa, Ob, Ms(OA, *A.m_pHub), Ms(OB, *B.m_pHub), Oa != Ob ? "outcome" : "-");
                }
                return Header("LogCompare", *pHub) + std::format("A={} B={} What=operations Kinds={}\n\n", A.m_Id, B.m_Id, Kinds.size()) + Rows;
            }

            std::string Rows = "Change\tId\tCode\tOccurrencesA\tOccurrencesB\tVerificationA\tVerificationB\tTitle\n";
            std::size_t New = 0, Resolved = 0, Regressed = 0, Persisting = 0, NotSeen = 0;
            for (auto Id : B.m_pHub->ProblemOrder())
            {
                const problem* PB = B.m_pHub->FindProblem(Id);
                const problem* PA = A.m_pHub->FindProblem(Id);
                if (!PB) continue;
                const char* Change = "persisting";
                if (!PA) { Change = PB->m_Verification == verification::Verified ? "resolved" : "new"; }
                else if (PA->m_Verification == verification::Verified || PB->m_Regressions > 0) Change = PB->m_Verification == verification::Verified ? "resolved" : "regressed";
                else if (PB->m_Verification == verification::Verified) Change = "resolved";
                (Change[0] == 'n' ? New : Change[0] == 'r' && Change[2] == 's' ? Resolved : Change[0] == 'r' ? Regressed : Persisting)++;
                Rows += std::format("{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\n", Change, Hex16(Id), Escape(PB->m_Code), PA ? PA->m_Count : 0, PB->m_Count, PA ? VerificationName(PA->m_Verification) : "-", VerificationName(PB->m_Verification), Escape(PB->m_Title));
            }
            for (auto Id : A.m_pHub->ProblemOrder())
                if (!B.m_pHub->FindProblem(Id))
                    if (const problem* PA = A.m_pHub->FindProblem(Id)) { ++NotSeen; Rows += std::format("not-seen\t{}\t{}\t{}\t0\t{}\t-\t{}\n", Hex16(Id), Escape(PA->m_Code), PA->m_Count, VerificationName(PA->m_Verification), Escape(PA->m_Title)); }
            return Header("LogCompare", *pHub) + std::format("A={} B={} What=problems New={} Resolved={} Regressed={} Persisting={} NotSeen={}\n\n", A.m_Id, B.m_Id, New, Resolved, Regressed, Persisting, NotSeen) + Rows;
        }
        xcmdline::parser::handle m_hA, m_hB, m_hWhat;
    };

    // The Back and Forward buttons as commands: the window (and what the host had in front) returns to where it was, or goes ahead again.
    struct back_cmd : log_query
    {
        std::function<bool()> m_Go;
        bool                  m_bForward;
        back_cmd(xundo::system& System, std::function<bool()> Go, bool bForward) noexcept : log_query(System, bForward ? "LogForward" : "LogBack"), m_Go(std::move(Go)), m_bForward(bForward) {}
        const char* getCommandHelp() const noexcept override
        {
            return m_bForward ? "The Logs window's Forward: goes ahead again after a Back. Usage: LogForward"
                              : "The Logs window's Back: returns to the view it was in before something (an editor's Feedback, Show in Events) moved it. Usage: LogBack";
        }
        void RegisterArguments() noexcept override {}
        std::string Query() noexcept override
        {
            if (m_Go && m_Go()) return m_bForward ? "LogForward: forward" : "LogBack: back";
            return m_bForward ? "LogForward: there is nothing to go forward to" : "LogBack: there is nothing to go back to";
        }
    };

    // Opens the Logs for the person: the drawer on the Logs tab with a query in the bar (what an editor's Feedback does). Back returns them to what they had.
    struct show_cmd : log_query
    {
        std::function<void(const std::string&)> m_Show;
        show_cmd(xundo::system& System, std::function<void(const std::string&)> Show) noexcept : log_query(System, "LogShow"), m_Show(std::move(Show)) { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override { return "Opens the Logs window on a query, as an editor's Feedback does (Back returns to what was in front). Usage: LogShow [-Query q]"; }
        void RegisterArguments() noexcept override { m_hQuery = m_Parser.addOption("Query", "The query bar's text, e.g. op:4", false, 1); }
        std::string Query() noexcept override
        {
            std::string Text;
            Arg(m_hQuery, Text);
            if (const filter F = ParseQuery(Text); !F.m_Error.empty()) return std::format("LogShow: invalid query: {}", F.m_Error);
            if (!m_Show) return "LogShow: no window";
            m_Show(Text);
            return "LogShow: shown";
        }
        xcmdline::parser::handle m_hQuery;
    };

    //==================================================================================================================
    // Diagnostic producers for the smoke tests (and for a script that wants to leave a note): they go through the real ring, adapters and store.
    //==================================================================================================================
    struct simulate_build_cmd : log_query
    {
        simulate_build_cmd(xundo::system& System) noexcept : log_query(System, "LogSimulateBuild") { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override { return "Diagnostic: runs compiler output through the build adapter as a game.build operation. Usage: LogSimulateBuild -Text text [-Exit n] [-Subject name] [-Target t] [-Coverage unknown|subjects|complete]"; }
        void RegisterArguments() noexcept override
        {
            m_hText = m_Parser.addOption("Text", "The output", true, 1);
            m_hExit = m_Parser.addOption("Exit", "The exit code, default 0", false, 1);
            m_hSubject = m_Parser.addOption("Subject", "What was built, default Game.dll (simulated)", false, 1);
            m_hTarget = m_Parser.addOption("Target", "The verification target, default Game.dll|simulated: only a success with the same target speaks of earlier problems", false, 1);
            m_hCoverage = m_Parser.addOption("Coverage", "What the build checked: unknown, subjects (the units it compiled; default) or complete", false, 1);
        }
        std::string Query() noexcept override
        {
            auto* pHub = Hub();
            if (!pHub) return "LogSimulateBuild: no host";
            std::string Text, ExitText, Subject = "Game.dll (simulated)";
            if (!Arg(m_hText, Text)) return "LogSimulateBuild: -Text is required";
            std::uint64_t Exit = 0;
            if (Arg(m_hExit, ExitText) && !ParseNumber(ExitText, Exit)) return "LogSimulateBuild: -Exit is not a number";
            Arg(m_hSubject, Subject);

            std::string Target = "Game.dll|simulated", Cover = "subjects";
            Arg(m_hTarget, Target); Arg(m_hCoverage, Cover);
            if (Cover != "unknown" && Cover != "subjects" && Cover != "complete") return "LogSimulateBuild: -Coverage is unknown, subjects or complete";
            auto Op = pHub->Begin("game.build", { origin::type::Tool, "msbuild", 0 }, { ref::type::File, Subject, 0, 0, 0, 0 }, "Build Game.dll", Target);
            const auto Id = Op.Id();
            {
                build_output_adapter Adapter(*pHub, Op);
                for (std::size_t Start = 0; Start <= Text.size(); )
                {
                    const auto Eol = Text.find('\n', Start);
                    Adapter.Feed(std::string_view(Text).substr(Start, Eol == std::string::npos ? Text.size() - Start : Eol - Start));
                    if (Eol == std::string::npos) break;
                    Start = Eol + 1;
                }
                Adapter.Finish();
            }
            Op.SetCoverage(Cover == "unknown" ? coverage_kind::Unknown : Cover == "complete" ? coverage_kind::Complete : coverage_kind::Subjects);
            Exit == 0 ? Op.Succeed() : Op.Fail();
            return std::format("LogSimulateBuild: operation {}", Id);
        }
        xcmdline::parser::handle m_hText, m_hExit, m_hSubject, m_hTarget, m_hCoverage;
    };

    struct simulate_compile_cmd : log_query
    {
        simulate_compile_cmd(xundo::system& System) noexcept : log_query(System, "LogSimulateCompile") { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override { return "Diagnostic: runs a resource compiler's output through the pipeline adapter as an asset.compile operation about one asset. Usage: LogSimulateCompile -Text text -Asset id [-Exit n] [-Name n] [-Type texture]"; }
        void RegisterArguments() noexcept override
        {
            m_hText = m_Parser.addOption("Text", "The compiler's output", true, 1);
            m_hAsset = m_Parser.addOption("Asset", "The asset's instance id (any number): the operation's subject", true, 1);
            m_hExit = m_Parser.addOption("Exit", "0 (default) = succeeded, otherwise failed", false, 1);
            m_hName = m_Parser.addOption("Name", "The asset's name", false, 1);
            m_hType = m_Parser.addOption("Type", "The resource type, default texture", false, 1);
        }
        std::string Query() noexcept override
        {
            auto* pHub = Hub();
            if (!pHub) return "LogSimulateCompile: no host";
            std::string Text, AssetText, ExitText, Name = "simulated asset", Type = "texture";
            if (!Arg(m_hText, Text) || !Arg(m_hAsset, AssetText)) return "LogSimulateCompile: -Text and -Asset are required";
            std::uint64_t Asset = 0, Exit = 0;
            if (!ParseNumber(AssetText, Asset)) return "LogSimulateCompile: -Asset is not a number";
            if (Arg(m_hExit, ExitText) && !ParseNumber(ExitText, Exit)) return "LogSimulateCompile: -Exit is not a number";
            Arg(m_hName, Name); Arg(m_hType, Type);

            ref Subject; Subject.m_Type = ref::type::Asset; Subject.m_Id = Asset; Subject.m_Path = Name;
            auto Op = pHub->Begin("asset.compile", { origin::type::System, "asset pipeline", 0 }, Subject, std::format("Compile {}", Name));
            const auto Id = Op.Id();
            {
                pipeline_output_adapter Adapter(*pHub, Op, Subject, "asset.compile." + Type);
                FeedPipelineOutput(Adapter, Text);
            }
            Exit == 0 ? Op.Succeed() : Op.Fail();
            return std::format("LogSimulateCompile: operation {}", Id);
        }
        xcmdline::parser::handle m_hText, m_hAsset, m_hExit, m_hName, m_hType;
    };

    struct emit_cmd : log_query
    {
        emit_cmd(xundo::system& System) noexcept : log_query(System, "LogEmit") { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override { return "Diagnostic: records an event (origin tool:pipe). Usage: LogEmit -Text text [-Severity s] [-Channel c] [-Code x] [-Kind log|diagnostic] [-Count n]"; }
        void RegisterArguments() noexcept override
        {
            m_hText = m_Parser.addOption("Text", "First line is the title, the rest the body", true, 1);
            m_hSeverity = m_Parser.addOption("Severity", "Default info", false, 1);
            m_hChannel = m_Parser.addOption("Channel", "Default editor.pipe", false, 1);
            m_hCode = m_Parser.addOption("Code", "A stable code", false, 1);
            m_hKind = m_Parser.addOption("Kind", "log (default) or diagnostic (becomes a problem from warning up)", false, 1);
            m_hCount = m_Parser.addOption("Count", "Emit it this many times, default 1", false, 1);
        }
        std::string Query() noexcept override
        {
            auto* pHub = Hub();
            if (!pHub) return "LogEmit: no host";
            std::string Text, S, Channel = "editor.pipe", Code, KindText, CountText; severity Sev = severity::Info; std::uint64_t Count = 1;
            if (!Arg(m_hText, Text)) return "LogEmit: -Text is required";
            if (Arg(m_hSeverity, S) && !ParseSeverity(S, Sev)) return std::format("LogEmit: unknown severity '{}'", S);
            Arg(m_hChannel, Channel); Arg(m_hCode, Code);
            const bool bDiagnostic = Arg(m_hKind, KindText) && KindText == "diagnostic";
            if (Arg(m_hCount, CountText) && !ParseNumber(CountText, Count)) return "LogEmit: -Count is not a number";
            for (std::uint64_t i = 0; i < Count; ++i)
            {
                event E;
                E.m_Producer = "xlion.pipe"; E.m_Origin = { origin::type::Tool, "pipe", 0 }; E.m_Severity = Sev; E.m_Kind = bDiagnostic ? kind::Diagnostic : kind::Log;
                E.m_Channel = Channel; E.m_Code = Code;
                SetMessage(E, Text);
                pHub->Emit(std::move(E));
            }
            return std::format("LogEmit: {} recorded", Count);
        }
        xcmdline::parser::handle m_hText, m_hSeverity, m_hChannel, m_hCode, m_hKind, m_hCount;
    };

    //==================================================================================================================
    // The person's decisions about problems. They are edits like any other: undoable, in the workspace's history. (LogVerify, in P2, will not be:
    // a verification is a fact the evidence supports, not a choice.)
    struct annotate_cmd : xundo::command_base
    {
        enum class what : std::uint8_t { Acknowledge, Mute };
        what m_What;
        annotate_cmd(xundo::system& System, const char* pName, what What) noexcept : xundo::command_base(System, pName, nullptr), m_What(What) { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override
        {
            return m_What == what::Acknowledge ? "Marks a problem as seen (undoable); it stays listed and keeps counting. Usage: LogAcknowledge -Id hex16 [-Value true|false]"
                                               : "Hides a problem from the lists (undoable): collection continues, the footer counts what is hidden, a Fatal one cannot be hidden. Usage: LogMute -Id hex16 [-Value true|false]";
        }
        void RegisterArguments() noexcept override
        {
            m_hId = m_Parser.addOption("Id", "The problem id (16 hex digits) as LogProblems prints it", true, 1);
            m_hValue = m_Parser.addOption("Value", "true (default) or false: undo it by hand", false, 1);
        }
        bool Arg(xcmdline::parser::handle Handle, std::string& Out) noexcept
        {
            auto A = m_Parser.getOptionArgAs<std::string>(Handle, 0);
            if (std::holds_alternative<xerr>(A)) return false;
            Out = std::get<std::string>(A);
            return true;
        }
        std::uint64_t ProblemId() noexcept { std::string Text; return Arg(m_hId, Text) ? std::strtoull(Text.c_str(), nullptr, 16) : 0; }
        bool Flag(const annotation& A) const noexcept { return m_What == what::Acknowledge ? A.m_bAcknowledged : A.m_bMuted; }
        void Set(hub& H, std::uint64_t Id, bool b) const noexcept { if (m_What == what::Acknowledge) H.SetAcknowledged(Id, b); else H.SetMuted(Id, b); }

        std::string Redo() noexcept override
        {
            auto* pHub = hub::current();
            if (!pHub) return std::format("{}: no host", m_pCommandName);
            for (int i = 0; i < 64 && pHub->Drain(1u << 16) > 0; ++i) {}
            const auto Id = ProblemId();
            if (!pHub->FindProblem(Id)) return std::format("{}: no problem {}", m_pCommandName, Hex16(Id));
            std::string Text;
            const bool bValue = !(Arg(m_hValue, Text) && Text == "false");
            if (bValue && m_What == what::Mute && pHub->FindProblem(Id)->m_Severity >= severity::Fatal) return std::format("{}: a Fatal problem cannot be hidden", m_pCommandName);
            Set(*pHub, Id, bValue);
            return {};
        }
        void BackupCurrenState(xundo::undo_file& File) noexcept override
        {
            const std::uint64_t Id = ProblemId();
            std::uint8_t Before = 0;
            if (auto* pHub = hub::current()) Before = Flag(pHub->Annotation(Id)) ? 1 : 0;
            File.Write(Id);
            File.Write(Before);
        }
        void Undo(xundo::undo_file& File) noexcept override
        {
            std::uint64_t Id = 0; std::uint8_t Before = 0;
            File.Read(Id); File.Read(Before);
            if (auto* pHub = hub::current()) Set(*pHub, Id, Before != 0);
        }
        xcmdline::parser::handle m_hId, m_hValue;
    };

    // "Everything up to now has been seen": moves the baseline the New preset counts from.
    struct mark_cmd : xundo::command_base
    {
        mark_cmd(xundo::system& System) noexcept : xundo::command_base(System, "LogMark", nullptr) { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override { return "Moves the baseline: problems first seen before now are no longer New (undoable). Usage: LogMark [-Kind baseline]"; }
        void RegisterArguments() noexcept override { m_hKind = m_Parser.addOption("Kind", "baseline (the only kind so far)", false, 1); }
        std::string Redo() noexcept override
        {
            auto* pHub = hub::current();
            if (!pHub) return "LogMark: no host";
            auto A = m_Parser.getOptionArgAs<std::string>(m_hKind, 0);
            if (!std::holds_alternative<xerr>(A) && std::get<std::string>(A) != "baseline") return "LogMark: -Kind must be baseline";
            for (int i = 0; i < 64 && pHub->Drain(1u << 16) > 0; ++i) {}
            pHub->SetBaseline(pHub->Committed());
            return {};
        }
        void BackupCurrenState(xundo::undo_file& File) noexcept override { File.Write(hub::current() ? hub::current()->Baseline() : std::uint64_t{ 0 }); }
        void Undo(xundo::undo_file& File) noexcept override { std::uint64_t Before = 0; File.Read(Before); if (auto* pHub = hub::current()) pHub->SetBaseline(Before); }
        xcmdline::parser::handle m_hKind;
    };

    // All the commands of the Logs, owned together: add one of these to whatever owns the workspace's undo system.
    struct command_set
    {
        explicit command_set(xundo::system& System, std::function<view_state*()> Window = {}, std::function<bool()> Back = {}, std::function<bool()> Forward = {}
            , std::function<void(const std::string&)> Show = {}) noexcept
            : m_Status(System), m_Operations(System), m_Problems(System), m_Problem(System), m_Events(System), m_Event(System), m_SimulateBuild(System), m_SimulateCompile(System), m_Emit(System)
            , m_Acknowledge(System, "LogAcknowledge", annotate_cmd::what::Acknowledge), m_Mute(System, "LogMute", annotate_cmd::what::Mute), m_Mark(System), m_Window(System, Window), m_BackCmd(System, std::move(Back), false), m_ForwardCmd(System, std::move(Forward), true), m_Show(System, std::move(Show)), m_Copy(System), m_Verify(System), m_Context(System), m_Focus(System), m_Ruler(System), m_Dependencies(System), m_SimulateDependencies(System), m_Attach(System), m_Attachments(System), m_Stdout(System), m_SimulateStdout(System), m_Remote(System), m_Views(System, Window), m_ViewSave(System, Window, false), m_ViewDelete(System, Window, true), m_Sessions(System), m_Compare(System), m_Lens(System, Window), m_EventsAction(System, std::move(Window)) {}
        status_cmd m_Status; operations_cmd m_Operations; problems_cmd m_Problems; problem_cmd m_Problem; events_cmd m_Events; event_cmd m_Event;
        simulate_build_cmd m_SimulateBuild; simulate_compile_cmd m_SimulateCompile; emit_cmd m_Emit;
        annotate_cmd m_Acknowledge, m_Mute; mark_cmd m_Mark; window_cmd m_Window; back_cmd m_BackCmd, m_ForwardCmd; show_cmd m_Show; copy_cmd m_Copy; verify_cmd m_Verify; context_cmd m_Context; focus_cmd m_Focus; ruler_cmd m_Ruler; dependencies_cmd m_Dependencies; simulate_dependencies_cmd m_SimulateDependencies; attach_cmd m_Attach; attachments_cmd m_Attachments; stdout_cmd m_Stdout; simulate_stdout_cmd m_SimulateStdout; remote_cmd m_Remote; views_cmd m_Views; view_edit_cmd m_ViewSave, m_ViewDelete; sessions_cmd m_Sessions; compare_cmd m_Compare; lens_cmd m_Lens; events_action_cmd m_EventsAction;
    };
}

#endif // XLOG_COMMANDS_H
