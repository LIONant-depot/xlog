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
#include "dependencies/xundo/source/xundo_system.h"

#include <charconv>
#include <functional>
#include <cstdlib>

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

        // The query text: -Query as typed, or -Query64 in base64 (a phrase in quotes cannot go through the command line itself).
        bool QueryText(xcmdline::parser::handle Plain, xcmdline::parser::handle Encoded, std::string& Out) noexcept
        {
            std::string Text;
            if (Arg(Encoded, Text)) { Out = Base64Decode(Text); return true; }
            return Arg(Plain, Out);
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
            Out += std::format("Dropped={}  Expired={}  Excluded=none  PendingWrite=0  PersistenceFailed=false\n", Dropped, S.m_Expired);
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
            m_hQuery64 = m_Parser.addOption("Query64", "The same, in base64 (for text with quotes or spaces)", false, 1);
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
            QueryText(m_hQuery, m_hQuery64, QueryString);
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
            filter F = ParseQuery(QueryString);
            if (!F.m_Error.empty()) return std::format("LogProblems: invalid query: {}", F.m_Error);
            if (Operation) F.m_Operation = Operation;

            std::string Rows = "Id\tSeverity\tCode\tOccurrences\tFirstSeq\tLastSeq\tSite\tSubject\tHeuristic\tUnit\tTitle\tTriage\tSuppression\n";
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
                Rows += std::format("{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\n", Hex16(P->m_Id), SeverityName(P->m_Severity), Escape(P->m_Code), P->m_Count, P->m_FirstSeq, P->m_LastSeq
                    , Escape(Site), Escape(P->m_Subject.m_Path.empty() ? P->m_Discriminator : P->m_Subject.m_Path), P->m_bHeuristic ? "true" : "false", Escape(P->m_CheckUnit), Escape(P->m_Title)
                    , A.m_bAcknowledged ? "Acknowledged" : "Unreviewed", A.m_bMuted ? "Muted" : "None");
                ++Returned; LastIndex = Index;
            }
            std::string Out = Header("LogProblems", H);
            Out += std::format("Query={}\n", Normalized(F));
            Out += std::format("Matched={} Returned={} Occurrences={}{}\n", Matched, Returned, Occurrences, bTruncated ? std::format("  Truncated=true Cursor={}:{}", H.Committed(), LastIndex) : "");
            Out += std::format("Evidence=full  Gaps=none  Excluded=none  Dropped=0\n");
            return Out + "\n" + Rows;
        }
        xcmdline::parser::handle m_hQuery, m_hQuery64, m_hMin, m_hOperation, m_hLimit, m_hAfter, m_hState, m_hMuted;
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
            Out += std::format("Triage={}  Verification=Unverified  Suppression={}  RunPresence=Observed\n", A.m_bAcknowledged ? "Acknowledged" : "Unreviewed", A.m_bMuted ? "Muted" : "None");
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
            m_hQuery64 = m_Parser.addOption("Query64", "The same, in base64 (for text with quotes or spaces)", false, 1);
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
            QueryText(m_hQuery, m_hQuery64, QueryString);
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
            filter F = ParseQuery(QueryString);
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
        xcmdline::parser::handle m_hQuery, m_hQuery64, m_hMin, m_hOperation, m_hLimit, m_hAfter;
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
            return std::format("LogWindow: ok\nPage={}  State={}  ShowMuted={}  Follow={}\nSelected={}  SelectedEvent={}\nBack={}\nBackDepth={}  ForwardDepth={}  BackAt={:.0f},{:.0f}  ForwardAt={:.0f},{:.0f}  MouseAt={:.0f},{:.0f}\nBadgeAt={:.0f},{:.0f}  SourceChipAt={:.0f},{:.0f}  AboutChipAt={:.0f},{:.0f}  EventsOpen={}  SelectedRange={}..{}  EventArrowAt={:.0f},{:.0f}  EventRowAt={:.0f},{:.0f}  EventStride={:.0f}\nQuery={}\n"
                , (pView->m_RequestPage >= 0 ? pView->m_RequestPage : pView->m_Page) == 0 ? "Problems" : "Events", ProblemViewName(pView->m_View)       // where it is, or where it is about to be
                , pView->m_bShowMuted, pView->m_bFollow, pView->m_Selected ? Hex16(pView->m_Selected) : std::string("none"), pView->m_SelectedEvent
                , pView->m_Back.empty() ? std::string("none") : pView->m_Back.back().m_Label, pView->m_Back.size(), pView->m_Forward.size()
                , pView->m_BackButton[0], pView->m_BackButton[1], pView->m_ForwardButton[0], pView->m_ForwardButton[1], pView->m_MouseAt[0], pView->m_MouseAt[1]
                , pView->m_BadgeAt[0], pView->m_BadgeAt[1], pView->m_SourceChipAt[0], pView->m_SourceChipAt[1], pView->m_AboutChipAt[0], pView->m_AboutChipAt[1], pView->m_ExpandedEvents.size(), pView->m_SelFrom, pView->m_SelTo, pView->m_EventArrowAt[0], pView->m_EventArrowAt[1], pView->m_EventRowAt[0], pView->m_EventRowAt[1], pView->m_EventStride, pView->m_Query);       // the buttons' centres (-1 = not drawn): a test clicks there
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
        const char* getCommandHelp() const noexcept override { return "Sets the Logs window's Source and About lenses (they are tokens of its query). Usage: LogLens [-Origin names|all] [-About anything|op:N|asset:X]"; }
        void RegisterArguments() noexcept override
        {
            m_hOrigin = m_Parser.addOption("Origin", "Origin names, comma separated, or all", false, 1);
            m_hAbout = m_Parser.addOption("About", "anything, op:N or asset:X (an id or part of a name)", false, 1);
        }
        std::string Query() noexcept override
        {
            view_state* pView = m_Get ? m_Get() : nullptr;
            if (!pView) return "LogLens: no window";
            std::string Text, Query = pView->m_Query;
            if (Arg(m_hOrigin, Text)) Query = WithQueryToken(Query, "origin", Text == "all" ? std::string_view{} : std::string_view(Text));
            if (Arg(m_hAbout, Text))
            {
                if (Text == "anything") Query = WithQueryToken(WithQueryToken(Query, "asset", {}), "op", {});
                else if (Text.rfind("op:", 0) == 0)    Query = WithQueryToken(WithQueryToken(Query, "asset", {}), "op", Text.substr(3));
                else if (Text.rfind("asset:", 0) == 0) Query = WithQueryToken(WithQueryToken(Query, "op", {}), "asset", Text.substr(6));
                else return "LogLens: -About is anything, op:N or asset:X";
            }
            if (const filter F = ParseQuery(Query); !F.m_Error.empty()) return std::format("LogLens: invalid query: {}", F.m_Error);
            std::snprintf(pView->m_Query, sizeof(pView->m_Query), "%s", Query.c_str());
            return std::format("LogLens: Query={}", Query);
        }
        xcmdline::parser::handle m_hOrigin, m_hAbout;
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
        const char* getCommandHelp() const noexcept override { return "Diagnostic: runs compiler output through the build adapter as a game.build operation. Usage: LogSimulateBuild -Text base64 [-Exit n] [-Subject name]"; }
        void RegisterArguments() noexcept override
        {
            m_hText = m_Parser.addOption("Text", "The output, base64", true, 1);
            m_hExit = m_Parser.addOption("Exit", "The exit code, default 0", false, 1);
            m_hSubject = m_Parser.addOption("Subject", "What was built, default Game.dll (simulated)", false, 1);
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

            const std::string Output = Base64Decode(Text);
            auto Op = pHub->Begin("game.build", { origin::type::Tool, "msbuild", 0 }, { ref::type::File, Subject, 0, 0, 0, 0 }, "Build Game.dll", "Game.dll|simulated");
            const auto Id = Op.Id();
            {
                build_output_adapter Adapter(*pHub, Op);
                for (std::size_t Start = 0; Start <= Output.size(); )
                {
                    const auto Eol = Output.find('\n', Start);
                    Adapter.Feed(std::string_view(Output).substr(Start, Eol == std::string::npos ? Output.size() - Start : Eol - Start));
                    if (Eol == std::string::npos) break;
                    Start = Eol + 1;
                }
                Adapter.Finish();
            }
            Op.SetCoverage(coverage_kind::Subjects);
            Exit == 0 ? Op.Succeed() : Op.Fail();
            return std::format("LogSimulateBuild: operation {}", Id);
        }
        xcmdline::parser::handle m_hText, m_hExit, m_hSubject;
    };

    struct simulate_compile_cmd : log_query
    {
        simulate_compile_cmd(xundo::system& System) noexcept : log_query(System, "LogSimulateCompile") { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override { return "Diagnostic: runs a resource compiler's output through the pipeline adapter as an asset.compile operation about one asset. Usage: LogSimulateCompile -Text base64 -Asset id [-Exit n] [-Name n] [-Type texture]"; }
        void RegisterArguments() noexcept override
        {
            m_hText = m_Parser.addOption("Text", "The compiler's output, base64", true, 1);
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
                FeedPipelineOutput(Adapter, Base64Decode(Text));
            }
            Exit == 0 ? Op.Succeed() : Op.Fail();
            return std::format("LogSimulateCompile: operation {}", Id);
        }
        xcmdline::parser::handle m_hText, m_hAsset, m_hExit, m_hName, m_hType;
    };

    struct emit_cmd : log_query
    {
        emit_cmd(xundo::system& System) noexcept : log_query(System, "LogEmit") { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override { return "Diagnostic: records an event (origin tool:pipe). Usage: LogEmit -Text base64 [-Severity s] [-Channel c] [-Code x] [-Kind log|diagnostic] [-Count n]"; }
        void RegisterArguments() noexcept override
        {
            m_hText = m_Parser.addOption("Text", "First line is the title, the rest the body; base64", true, 1);
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
            const std::string Decoded = Base64Decode(Text);
            for (std::uint64_t i = 0; i < Count; ++i)
            {
                event E;
                E.m_Producer = "xlion.pipe"; E.m_Origin = { origin::type::Tool, "pipe", 0 }; E.m_Severity = Sev; E.m_Kind = bDiagnostic ? kind::Diagnostic : kind::Log;
                E.m_Channel = Channel; E.m_Code = Code;
                SetMessage(E, Decoded);
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
            , m_Acknowledge(System, "LogAcknowledge", annotate_cmd::what::Acknowledge), m_Mute(System, "LogMute", annotate_cmd::what::Mute), m_Mark(System), m_Window(System, Window), m_BackCmd(System, std::move(Back), false), m_ForwardCmd(System, std::move(Forward), true), m_Show(System, std::move(Show)), m_Copy(System), m_Lens(System, Window), m_EventsAction(System, std::move(Window)) {}
        status_cmd m_Status; operations_cmd m_Operations; problems_cmd m_Problems; problem_cmd m_Problem; events_cmd m_Events; event_cmd m_Event;
        simulate_build_cmd m_SimulateBuild; simulate_compile_cmd m_SimulateCompile; emit_cmd m_Emit;
        annotate_cmd m_Acknowledge, m_Mute; mark_cmd m_Mark; window_cmd m_Window; back_cmd m_BackCmd, m_ForwardCmd; show_cmd m_Show; copy_cmd m_Copy; lens_cmd m_Lens; events_action_cmd m_EventsAction;
    };
}

#endif // XLOG_COMMANDS_H
