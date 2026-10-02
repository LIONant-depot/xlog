#ifndef XLOG_HUB_H
#define XLOG_HUB_H
#pragma once

// xlog: the one place where "something happened" is recorded (documentation/Editors/DESIGN_logs.md in xLION).
// A library of its own, with no knowledge of any editor: the editors, their plugins, the compilers' adapters and the tools depend on it, not the other way round.
//
//   any thread  --Emit()/Begin()-->  bounded ring  --Drain() on the HOST thread-->  store  -->  queries (the pipe commands, later the window)
//
// Events are immutable occurrences. Problems are derived: the diagnostics that share an identity. Operations are units of work with an outcome
// (a build, a compile, a load). A session is one editor launch. Nothing here knows about ImGui, so the headless host has the same service.
//
// This is phase P0: an in-memory, bounded store. The record is already the persistent one (session id + sequence keys, stable producer names,
// owned values only, title + body), so the disk writer of P1 does not change any of it.
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>
#include <functional>
#include <format>
#include <mutex>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace xlog
{
    //------------------------------------------------------------------------------------------------------------------
    // The vocabulary
    //------------------------------------------------------------------------------------------------------------------
    enum class severity : std::uint8_t { Trace, Debug, Info, Warning, Error, Fatal };
    enum class kind     : std::uint8_t { Log, Diagnostic, Command, Progress, State };
    enum class outcome  : std::uint8_t { Running, Succeeded, Failed, Cancelled, Abandoned };
    enum class coverage_kind : std::uint8_t { Unknown, Subjects, Complete };      // what a successful operation actually checked

    inline constexpr const char* SeverityName(severity S) noexcept
    {
        constexpr const char* Names[] = { "trace", "debug", "info", "warning", "error", "fatal" };
        return Names[static_cast<int>(S)];
    }
    inline constexpr const char* KindName(kind K) noexcept
    {
        constexpr const char* Names[] = { "log", "diagnostic", "command", "progress", "state" };
        return Names[static_cast<int>(K)];
    }
    inline constexpr const char* OutcomeName(outcome O) noexcept
    {
        constexpr const char* Names[] = { "Running", "Succeeded", "Failed", "Cancelled", "Abandoned" };
        return Names[static_cast<int>(O)];
    }
    inline constexpr const char* CoverageName(coverage_kind C) noexcept
    {
        constexpr const char* Names[] = { "Unknown", "Selected", "Complete" };
        return Names[static_cast<int>(C)];
    }
    inline bool ParseSeverity(std::string_view Text, severity& Out) noexcept
    {
        std::string L(Text);
        for (auto& c : L) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        for (int i = 0; i < 6; ++i) if (L == SeverityName(static_cast<severity>(i))) { Out = static_cast<severity>(i); return true; }
        if (L == "warn") { Out = severity::Warning; return true; }
        return false;
    }

    // Owned values only: an event can always be kept, written and read back after a plugin unloads. (P0 subset of the closed set in the design.)
    using value = std::variant<bool, std::int64_t, std::uint64_t, double, std::string>;
    struct attribute { std::string m_Name; value m_Value; };

    // A typed target, never a string to parse.
    struct ref
    {
        enum class type : std::uint8_t { None, Asset, Entity, File, Graph, Operation, Object };
        type           m_Type     = type::None;
        std::string    m_Path;                 // File (and a readable name for the others)
        std::uint64_t  m_Id       = 0;         // Entity (scene permanent id) / Operation / Object
        std::int32_t   m_Line     = 0;
        std::int32_t   m_Column   = 0;
        std::uint64_t  m_Revision = 0;         // what it pointed at when recorded; 0 = unknown
        bool Valid() const noexcept { return m_Type != type::None; }
    };

    // WHO produced it - a different question from what it is about.
    struct origin
    {
        enum class type : std::uint8_t { Editor, System, Script, Tool };
        type           m_Type     = type::System;
        std::string    m_Name;                 // stable: "level", "physics", "game.module:Soccer", "msbuild"
        std::uint64_t  m_Instance = 0;         // an open editor instance, 0 for singletons
    };
    inline constexpr const char* OriginTypeName(origin::type T) noexcept
    {
        constexpr const char* Names[] = { "editor", "system", "script", "tool" };
        return Names[static_cast<int>(T)];
    }

    struct event_key
    {
        std::uint64_t m_Session  = 0;          // random, written once per launch: unique across launches, copies and imports
        std::uint64_t m_Sequence = 0;
        bool Valid() const noexcept { return m_Sequence != 0; }
    };

    struct event
    {
        event_key       m_Key;
        std::uint64_t   m_ObservedAt = 0;      // collector clock: ns since the session started
        std::string     m_Producer;            // STABLE namespace: "msvc.compiler", "vulkan.validation", "xlion.notify"
        origin          m_Origin;
        severity        m_Severity   = severity::Info;
        kind            m_Kind       = kind::Log;
        std::string     m_Channel;             // hierarchical: "game.build", "asset.compile.material"
        std::string     m_Title;               // the FIRST line: what a row shows and what a problem is named by
        std::string     m_Body;                // the rest, raw, any number of lines; empty for a one-line event
        std::uint32_t   m_BodyLines  = 0;
        std::string     m_Code;                // "C2065", "VUID-vkCmdDrawIndexed-None-08600"
        std::vector<attribute> m_Attributes;
        ref             m_Source;              // where it was raised from
        std::vector<ref> m_Subjects;           // what it is about
        std::uint64_t   m_Operation  = 0;      // operation id in the same session, 0 = none
        event_key       m_Cause;               // explicit "caused by"
        std::string     m_Discriminator;       // what makes this occurrence a different problem (the offending identifier); empty = unknown
        bool            m_bHeuristic = false;  // derived by an adapter from text, not stated by the producer
        bool            m_bSummarized = false; // the body was cut at the cap
    };

    inline constexpr std::size_t body_cap_v = 64 * 1024;

    // Splits a message: the first line is the title, the rest the body (raw lines, kept whole up to the cap).
    inline void SetMessage(event& E, std::string_view Text) noexcept
    {
        while (!Text.empty() && (Text.back() == '\n' || Text.back() == '\r')) Text.remove_suffix(1);
        const auto Eol = Text.find('\n');
        std::string_view First = Eol == std::string_view::npos ? Text : Text.substr(0, Eol);
        if (!First.empty() && First.back() == '\r') First.remove_suffix(1);
        E.m_Title.assign(First);
        E.m_Body.clear();
        E.m_BodyLines = 0;
        if (Eol == std::string_view::npos) return;

        std::string_view Rest = Text.substr(Eol + 1);
        std::uint32_t Lines = 1;
        for (char c : Rest) if (c == '\n') ++Lines;
        if (Rest.size() > body_cap_v)
        {
            // keep whole lines up to the cap, then say how many were not kept
            std::size_t Cut = Rest.rfind('\n', body_cap_v);
            if (Cut == std::string_view::npos) Cut = body_cap_v;
            std::uint32_t Kept = 1;
            for (std::size_t i = 0; i < Cut; ++i) if (Rest[i] == '\n') ++Kept;
            E.m_Body.assign(Rest.substr(0, Cut));
            E.m_Body += std::format("\n... {} more lines not kept", Lines - Kept);
            E.m_BodyLines = Kept + 1;
            E.m_bSummarized = true;
            return;
        }
        E.m_Body.assign(Rest);
        E.m_BodyLines = Lines;
    }

    //------------------------------------------------------------------------------------------------------------------
    // Operations and problems
    //------------------------------------------------------------------------------------------------------------------
    struct operation
    {
        std::uint64_t   m_Id = 0, m_Parent = 0;
        std::string     m_Kind;                // "game.build", "asset.compile", "play.run", "test"
        origin          m_Origin;
        ref             m_Subject;
        std::string     m_Title;
        std::uint64_t   m_Started = 0, m_Ended = 0;       // collector clock (ns since the session started)
        outcome         m_Outcome = outcome::Running;
        // What a success may claim (design 5.2). P0 records it; verification itself is P2.
        std::string     m_VerificationTarget;  // "Game.dll|Debug|x64|Visual Studio 17 2022"; empty = nothing can be verified through it
        coverage_kind   m_Coverage = coverage_kind::Unknown;
        std::vector<std::string> m_Units;      // the units a build really compiled (for Subjects coverage)
        bool            m_bEvidenceReady = false;   // ended AND every record of it is in the store
        std::uint32_t   m_Events = 0, m_Errors = 0, m_Warnings = 0;
        std::uint64_t   m_FirstSeq = 0, m_LastSeq = 0;      // the span of sequences its events live in (other operations' events may be interleaved): a view reads them without scanning the store
        std::vector<std::uint64_t> m_Problems; // unique problem ids that occurred inside it
        std::uint64_t   m_DroppedAtBegin = 0;  // diagnostics the collector had dropped when it began: more at its end means its evidence may be incomplete
        bool            m_bCollectorLoss = false;
    };

    // Verification (design 5.2): a success is a fact; "this problem is gone" is a separate claim that needs evidence.
    //   Unverified  nothing has checked it again.      Reproduced  it occurred again inside a successful operation (the exit code did not make it go away).
    //   Verified    a successful operation with the same verification target re-checked what produced it (its check unit was compiled, or the coverage was complete),
    //               with complete evidence and no collector loss, and it did not recur.
    enum class verification : std::uint8_t { Unverified, Reproduced, Verified };
    // Run presence: was it there the last time its target was checked? Not observed is NOT "fixed": it may simply not have been looked at.
    enum class presence : std::uint8_t { Observed, NotObserved, Unknown };
    inline constexpr const char* VerificationName(verification V) noexcept { constexpr const char* N[] = { "Unverified", "Reproduced", "Verified" }; return N[static_cast<int>(V)]; }
    inline constexpr const char* PresenceName(presence P) noexcept { constexpr const char* N[] = { "Observed", "NotObserved", "Unknown" }; return N[static_cast<int>(P)]; }

    struct problem
    {
        std::uint64_t   m_Id = 0;              // the identity (hash)
        std::string     m_Producer, m_Code, m_Title, m_Channel;
        severity        m_Severity = severity::Warning;    // the worst seen
        std::uint64_t   m_FirstSeq = 0, m_LastSeq = 0, m_Count = 0;
        std::vector<std::uint64_t> m_First;    // the first retained occurrences (sequence numbers)
        std::vector<std::uint64_t> m_Last;     // the most recent ones
        ref             m_Site, m_Subject;
        std::string     m_Discriminator;
        bool            m_bHeuristic = false;
        std::uint64_t   m_LastOperation = 0;
        std::string     m_CheckUnit;           // what produced the last occurrence (the translation unit that was compiled)
        std::string     m_OriginName;          // who produced it (the origin of its first occurrence)
        std::string     m_Target;              // the verification target of the last operation it occurred in ("Game.dll|Debug|x64"): what a later success has to share to speak of it
        verification    m_Verification = verification::Unverified;
        presence        m_Presence     = presence::Observed;
        std::uint64_t   m_VerifiedBy   = 0;    // the operation whose success verified it
        std::uint32_t   m_Regressions  = 0;    // times it came back after being verified
        bool            m_bRecurring   = false; // it came back after the person acknowledged it
        std::uint64_t   m_FirstAt = 0, m_LastAt = 0;   // when (collector clock) its first and its latest occurrence were observed: the ruler marks the first, a time range selects by both
        std::string     m_PreviousSession;     // hex id of an earlier launch in which it had been verified resolved (a regression across launches)
    };

    inline constexpr std::size_t problem_retained_v = 3;

    // What the person decided about a problem (design 5.2: triage and suppression are independent of what the producers say). Presentation only:
    // collection continues and the original severity stays.
    struct annotation { bool m_bAcknowledged = false; bool m_bMuted = false; std::string m_Label; };   // m_Label: what the problem was called, so the person's file reads and an unmatched entry can be recognised

    // A file kept with an event or an operation (design 9.3): a crash dump, a screenshot, the compiler's response file, a capture. The Logs copy it into the launch's folder
    // (xlog_store.h Attach) and say what it is about; the record itself stays small.
    struct attachment { std::uint64_t m_Id = 0, m_Event = 0, m_Operation = 0, m_Bytes = 0; std::string m_Name, m_Path; };

    // A view the person (or the team) keeps: the query, the page and the preset. Names are unique; a team view is shared through the project's files.
    struct saved_view { std::string m_Name, m_Query; int m_Page = 0; std::uint8_t m_State = 1; bool m_bShowMuted = false, m_bTeam = false; };

    // The presets of the Problems list. New = first seen after the baseline; Active = not acknowledged; All = everything not muted.
    enum class problem_view : std::uint8_t { New, Active, All };
    inline constexpr const char* ProblemViewName(problem_view V) noexcept
    {
        constexpr const char* Names[] = { "New", "Active", "All" };
        return Names[static_cast<int>(V)];
    }
    inline bool ParseProblemView(std::string_view Text, problem_view& Out) noexcept
    {
        for (int i = 0; i < 3; ++i)
        {
            const std::string_view Name = ProblemViewName(static_cast<problem_view>(i));
            if (Text.size() == Name.size() && std::equal(Text.begin(), Text.end(), Name.begin(), [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); }))
            { Out = static_cast<problem_view>(i); return true; }
        }
        return false;
    }

    //------------------------------------------------------------------------------------------------------------------
    // Identity of a problem: producer namespace + code + site + subject + discriminator. Never a timestamp, an address, a session or the rendered text.
    //------------------------------------------------------------------------------------------------------------------
    inline std::uint64_t Fnv1a(std::uint64_t H, std::string_view S) noexcept
    {
        for (unsigned char c : S) { H ^= c; H *= 1099511628211ull; }
        H ^= 0xFF; H *= 1099511628211ull;      // a separator, so ("ab","c") and ("a","bc") differ
        return H;
    }

    inline std::string NormalizePath(std::string_view P) noexcept
    {
        std::string Out(P);
        for (auto& c : Out) { if (c == '\\') c = '/'; else c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
        return Out;
    }

    // For text without a stable code: numbers, hex values and quoted names are replaced, so the same failure with other values groups together
    // (and is labelled a heuristic).
    inline std::string TemplateOf(std::string_view Text) noexcept
    {
        std::string Out;
        for (std::size_t i = 0; i < Text.size(); ++i)
        {
            const unsigned char c = static_cast<unsigned char>(Text[i]);
            if (c == '0' && i + 1 < Text.size() && (Text[i + 1] == 'x' || Text[i + 1] == 'X'))
            {
                i += 2; while (i < Text.size() && std::isxdigit(static_cast<unsigned char>(Text[i]))) ++i; --i; Out += "0x#"; continue;
            }
            if (std::isdigit(c)) { while (i + 1 < Text.size() && std::isdigit(static_cast<unsigned char>(Text[i + 1]))) ++i; Out += '#'; continue; }
            Out += static_cast<char>(c);
        }
        return Out;
    }

    inline std::uint64_t ProblemId(const event& E) noexcept
    {
        std::uint64_t H = 1469598103934665603ull;
        H = Fnv1a(H, E.m_Producer);
        H = Fnv1a(H, E.m_Code);
        std::string Site;
        if (E.m_Source.m_Type == ref::type::File) Site = std::format("{}:{}", NormalizePath(E.m_Source.m_Path), E.m_Source.m_Line);
        H = Fnv1a(H, Site);
        std::string Subject;
        if (!E.m_Subjects.empty()) Subject = std::format("{}:{}:{}", static_cast<int>(E.m_Subjects[0].m_Type), NormalizePath(E.m_Subjects[0].m_Path), E.m_Subjects[0].m_Id);
        H = Fnv1a(H, Subject);
        H = Fnv1a(H, E.m_Discriminator);
        if (E.m_bHeuristic || E.m_Code.empty()) H = Fnv1a(H, TemplateOf(E.m_Title));
        return H;
    }

    inline std::string Hex16(std::uint64_t V) noexcept { return std::format("{:016X}", V); }

    inline std::string Base64Decode(const std::string& In) noexcept
    {
        std::string Out;
        std::uint32_t Acc = 0; int Bits = 0;
        for (unsigned char c : In)
        {
            int V;
            if (c >= 'A' && c <= 'Z') V = c - 'A'; else if (c >= 'a' && c <= 'z') V = c - 'a' + 26; else if (c >= '0' && c <= '9') V = c - '0' + 52;
            else if (c == '+') V = 62; else if (c == '/') V = 63; else continue;
            Acc = (Acc << 6) | static_cast<std::uint32_t>(V); Bits += 6;
            if (Bits >= 8) { Bits -= 8; Out += static_cast<char>((Acc >> Bits) & 0xFF); }
        }
        return Out;
    }

    //------------------------------------------------------------------------------------------------------------------
    // The pending records (what travels through the ring)
    //------------------------------------------------------------------------------------------------------------------
    struct op_begin    { std::uint64_t m_Id = 0, m_Parent = 0, m_At = 0; std::string m_Kind, m_Title, m_Target; origin m_Origin; ref m_Subject; };
    struct op_unit     { std::uint64_t m_Id = 0; std::string m_Unit; };
    struct op_coverage { std::uint64_t m_Id = 0; coverage_kind m_Coverage = coverage_kind::Unknown; };
    struct op_end      { std::uint64_t m_Id = 0, m_At = 0; outcome m_Outcome = outcome::Succeeded; };
    using record = std::variant<event, op_begin, op_unit, op_coverage, op_end>;

    // Where committed records go to be kept (the writer of xlog_store.h): the hub knows only this. Push is called on the host thread, in commit order, with the key already assigned;
    // an implementation copies what it needs and never blocks (a full queue drops and counts).
    struct sink_status { std::uint64_t m_Written = 0, m_Pending = 0, m_Dropped = 0; bool m_bFailed = false; std::string m_Reason, m_Path; };
    struct record_sink
    {
        virtual ~record_sink() = default;
        virtual void        Push(const record& R) noexcept = 0;
        virtual sink_status Status() const noexcept = 0;
    };

    class hub;

    // An operation in progress. Destroyed without an outcome it is recorded Abandoned, never Succeeded.
    class op_handle
    {
    public:
        op_handle() noexcept = default;
        op_handle(hub* pHub, std::uint64_t Id) noexcept : m_pHub(pHub), m_Id(Id) {}
        op_handle(const op_handle&) = delete;
        op_handle& operator=(const op_handle&) = delete;
        op_handle(op_handle&& O) noexcept : m_pHub(O.m_pHub), m_Id(O.m_Id), m_bEnded(O.m_bEnded) { O.m_pHub = nullptr; }
        op_handle& operator=(op_handle&& O) noexcept
        {
            if (this != &O) { if (m_pHub && !m_bEnded) End(outcome::Abandoned); m_pHub = O.m_pHub; m_Id = O.m_Id; m_bEnded = O.m_bEnded; O.m_pHub = nullptr; }
            return *this;
        }
        ~op_handle() { if (m_pHub && !m_bEnded) End(outcome::Abandoned); }

        std::uint64_t Id() const noexcept { return m_Id; }
        explicit operator bool() const noexcept { return m_pHub != nullptr; }
        inline void   Unit(std::string Name) noexcept;                 // a unit the operation really checked
        inline void   SetCoverage(coverage_kind C) noexcept;
        inline void   End(outcome O) noexcept;
        void          Succeed() noexcept { End(outcome::Succeeded); }
        void          Fail()    noexcept { End(outcome::Failed); }
        void          Cancel()  noexcept { End(outcome::Cancelled); }

    private:
        hub*          m_pHub   = nullptr;
        std::uint64_t m_Id     = 0;
        bool          m_bEnded = false;
    };

    //------------------------------------------------------------------------------------------------------------------
    // Queries: a small grammar, one normalized form
    //------------------------------------------------------------------------------------------------------------------
    struct filter
    {
        severity        m_Min        = severity::Trace;
        std::string     m_Channel;                 // prefix; "game.*" and "game." both mean the prefix
        std::string     m_Code;
        std::vector<std::string> m_Origins;        // origin names (origin:a,b): any of them
        std::uint64_t   m_TimeFrom = 0, m_TimeTo = ~0ull;   // time:A-B, seconds since the launch started (either end may be left out): what the ruler's range selects
        std::string     m_Producer;                // the producer's stable namespace ("vulkan.validation", "msvc.compiler"), exactly
        std::string     m_Asset;                   // what it is about: the asset's id (hex, 8+ digits) or a part of its name/path
        bool            m_bDeps = false;           // deps:yes - and about what that asset depends on too (the producer of the dependency knowledge fills m_AssetAlso; see hub::Parse)
        std::vector<ref> m_AssetAlso;              // the assets m_Asset depends on (never parsed: hub::Parse resolves them)
        std::uint64_t   m_Operation  = 0;
        std::vector<std::pair<std::string, bool>> m_Terms;     // every term must match (case-insensitive); true = in the body only. A quoted phrase is ONE term
        std::string     m_NotChannel;
        std::string     m_Error;                   // a parse error: the query did not run
    };

    namespace details
    {
        inline bool ContainsNoCase(std::string_view Hay, std::string_view Needle) noexcept
        {
            if (Needle.empty()) return true;
            auto It = std::search(Hay.begin(), Hay.end(), Needle.begin(), Needle.end(), [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); });
            return It != Hay.end();
        }
        // Is the reference one of the assets the wanted asset depends on?
        inline bool RefIsOneOf(const ref& R, const std::vector<ref>& Also) noexcept
        {
            if (!R.Valid()) return false;
            for (const auto& A : Also)
                if ((A.m_Id && R.m_Id == A.m_Id) || (!A.m_Path.empty() && ContainsNoCase(R.m_Path, A.m_Path) && ContainsNoCase(A.m_Path, R.m_Path))) return true;
            return false;
        }
        inline bool AboutAsset(const filter& F, const ref& R) noexcept;
        // Is the reference about this (asset:Value)? A long hex value is an id, anything else a part of the name or path.
        inline bool RefMatchesAsset(const ref& R, std::string_view Value) noexcept
        {
            if (!R.Valid() || Value.empty()) return false;
            if (Value.size() >= 8 && Value.size() <= 16 && std::all_of(Value.begin(), Value.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; }))
                if (R.m_Id == std::strtoull(std::string(Value).c_str(), nullptr, 16)) return true;
            return ContainsNoCase(R.m_Path, Value);
        }
        inline bool Prefix(std::string_view Name, std::string Pattern) noexcept
        {
            if (!Pattern.empty() && Pattern.back() == '*') Pattern.pop_back();
            return Name.substr(0, Pattern.size()) == Pattern;
        }
    }

    // "sev>=error channel:game.* code:C2065 op:42 origin:msbuild -channel:physics body:undeclared free text"
    inline filter ParseQuery(std::string_view Query) noexcept
    {
        filter F;
        std::vector<std::string> Tokens;
        for (std::size_t i = 0; i < Query.size(); )
        {
            while (i < Query.size() && Query[i] == ' ') ++i;
            if (i >= Query.size()) break;
            std::string Token;
            bool bQuoted = false;
            for (; i < Query.size() && (bQuoted || Query[i] != ' '); ++i)
            {
                if (Query[i] == '"') { bQuoted = !bQuoted; continue; }
                Token += Query[i];
            }
            Tokens.push_back(std::move(Token));
        }
        auto Starts = [](const std::string& S, std::string_view P) { return S.rfind(P, 0) == 0; };
        for (auto& T : Tokens)
        {
            // a key with nothing after it ("channel:", "op:") is a mistake, not "no filter": say so instead of returning everything
            if (!T.empty() && T.back() == ':' && (Starts(T, "channel:") || Starts(T, "-channel:") || Starts(T, "code:") || Starts(T, "origin:") || Starts(T, "op:") || Starts(T, "body:") || Starts(T, "state:") || Starts(T, "asset:") || Starts(T, "producer:") || Starts(T, "time:") || Starts(T, "deps:")))
            { F.m_Error = std::format("'{}' needs a value (channel:, code:, origin:, op:, sev>=, body:)", T); return F; }
            if (Starts(T, "sev>=") || Starts(T, "sev:"))
            {
                const auto Text = T.substr(T[3] == ':' ? 4 : 5);
                if (!ParseSeverity(Text, F.m_Min)) { F.m_Error = std::format("unknown severity '{}' (trace, debug, info, warning, error, fatal)", Text); return F; }
            }
            else if (Starts(T, "channel:")) F.m_Channel = T.substr(8);
            else if (Starts(T, "-channel:")) F.m_NotChannel = T.substr(9);
            else if (Starts(T, "code:"))    F.m_Code = T.substr(5);
            else if (Starts(T, "origin:"))
            {
                const std::string List = T.substr(7);
                for (std::size_t At = 0; At <= List.size(); )
                {
                    const auto Comma = List.find(',', At);
                    const std::string Name = List.substr(At, Comma == std::string::npos ? std::string::npos : Comma - At);
                    if (!Name.empty()) F.m_Origins.push_back(Name);
                    if (Comma == std::string::npos) break;
                    At = Comma + 1;
                }
            }
            else if (Starts(T, "asset:"))   F.m_Asset = T.substr(6);
            else if (Starts(T, "deps:"))    { const std::string V = T.substr(5); F.m_bDeps = V == "yes" || V == "1" || V == "true"; }
            else if (Starts(T, "producer:")) F.m_Producer = T.substr(9);
            else if (Starts(T, "time:"))
            {
                const std::string V = T.substr(5);
                const auto Dash = V.find('-');
                if (Dash == std::string::npos) { F.m_Error = "time:A-B takes seconds since the launch started, e.g. time:12.5-30 or time:-5"; return F; }
                const std::string A = V.substr(0, Dash), B = V.substr(Dash + 1);
                char* pEnd = nullptr;
                if (!A.empty()) { F.m_TimeFrom = static_cast<std::uint64_t>(std::strtod(A.c_str(), &pEnd) * 1.0e9); if (*pEnd) { F.m_Error = std::format("time:{} is not a number of seconds", A); return F; } }
                if (!B.empty()) { F.m_TimeTo = static_cast<std::uint64_t>(std::strtod(B.c_str(), &pEnd) * 1.0e9); if (*pEnd) { F.m_Error = std::format("time:{} is not a number of seconds", B); return F; } }
            }
            else if (Starts(T, "op:"))
            {
                const auto Text = T.substr(3);
                char* pEnd = nullptr;
                F.m_Operation = std::strtoull(Text.c_str(), &pEnd, 0);
                if (Text.empty() || *pEnd != 0) { F.m_Error = std::format("op:{} is not an operation id", Text); return F; }
            }
            else if (Starts(T, "body:"))    F.m_Terms.emplace_back(T.substr(5), true);
            else if (!T.empty() && T.back() == ':') { F.m_Error = std::format("'{}' needs a value (channel:, code:, origin:, op:, sev>=, body:)", T); return F; }
            else if (T.find(':') != std::string::npos && Starts(T, "state:")) { /* problem state presets are handled by the caller */ }
            else if (!T.empty()) F.m_Terms.emplace_back(T, false);
        }
        return F;
    }

    inline std::string Normalized(const filter& F) noexcept
    {
        std::string Out;
        auto Add = [&](std::string S) { if (!Out.empty()) Out += ' '; Out += S; };
        if (F.m_Min != severity::Trace) Add(std::format("sev>={}", SeverityName(F.m_Min)));
        if (!F.m_Channel.empty())    Add("channel:" + F.m_Channel);
        if (!F.m_NotChannel.empty()) Add("-channel:" + F.m_NotChannel);
        if (!F.m_Code.empty())       Add("code:" + F.m_Code);
        if (!F.m_Origins.empty())    { std::string List; for (const auto& O : F.m_Origins) List += (List.empty() ? "" : ",") + O; Add("origin:" + List); }
        if (!F.m_Asset.empty())      Add("asset:" + F.m_Asset);
        if (F.m_bDeps)               Add("deps:yes");
        if (!F.m_Producer.empty())   Add("producer:" + F.m_Producer);
        if (F.m_TimeFrom != 0 || F.m_TimeTo != ~0ull) Add(std::format("time:{}-{}", F.m_TimeFrom ? std::format("{:.3f}", static_cast<double>(F.m_TimeFrom) / 1.0e9) : std::string(), F.m_TimeTo != ~0ull ? std::format("{:.3f}", static_cast<double>(F.m_TimeTo) / 1.0e9) : std::string()));
        if (F.m_Operation)           Add(std::format("op:{}", F.m_Operation));
        for (const auto& [Text, bBody] : F.m_Terms) Add((bBody ? "body:\"" : "\"") + Text + "\"");
        return Out;
    }

    namespace details { inline bool AboutAsset(const filter& F, const ref& R) noexcept { return RefMatchesAsset(R, F.m_Asset) || (F.m_bDeps && RefIsOneOf(R, F.m_AssetAlso)); } }

    inline bool Matches(const filter& F, const event& E) noexcept
    {
        if (E.m_Severity < F.m_Min) return false;
        if (!F.m_Channel.empty() && !details::Prefix(E.m_Channel, F.m_Channel)) return false;
        if (!F.m_NotChannel.empty() && details::Prefix(E.m_Channel, F.m_NotChannel)) return false;
        if (!F.m_Code.empty() && E.m_Code != F.m_Code) return false;
        if (!F.m_Origins.empty() && std::find(F.m_Origins.begin(), F.m_Origins.end(), E.m_Origin.m_Name) == F.m_Origins.end()) return false;
        if (!F.m_Producer.empty() && E.m_Producer != F.m_Producer) return false;
        if (E.m_ObservedAt < F.m_TimeFrom || E.m_ObservedAt > F.m_TimeTo) return false;
        if (!F.m_Asset.empty() && !std::any_of(E.m_Subjects.begin(), E.m_Subjects.end(), [&](const ref& R) { return details::AboutAsset(F, R); })) return false;
        if (F.m_Operation && E.m_Operation != F.m_Operation) return false;
        for (const auto& [Text, bBodyOnly] : F.m_Terms)
        {
            const bool bBody = details::ContainsNoCase(E.m_Body, Text);
            if (!(bBody || (!bBodyOnly && details::ContainsNoCase(E.m_Title, Text)))) return false;
        }
        return true;
    }

    //------------------------------------------------------------------------------------------------------------------
    // The hub
    //------------------------------------------------------------------------------------------------------------------
    struct status
    {
        std::uint64_t m_Session = 0, m_Committed = 0, m_Backlog = 0;
        std::size_t   m_Events = 0, m_Problems = 0, m_Operations = 0;
        std::uint64_t m_BySeverity[6] = {};
        std::uint64_t m_ProblemsBySeverity[6] = {};
        std::uint64_t m_Dropped[6] = {};
        std::uint64_t m_Expired = 0;
        std::size_t   m_Capacity = 0;
    };

    class hub
    {
    public:
        hub() noexcept
        {
            std::random_device Rd;
            m_Session = (static_cast<std::uint64_t>(Rd()) << 32) ^ Rd() ^ static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
            if (m_Session == 0) m_Session = 1;
            m_Start = std::chrono::steady_clock::now();
            m_StartWall = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        }

        // ---- the process-wide one --------------------------------------------------------------------------------
        // The owner (xeditor::host in the editors) makes its hub current; producers that have no reference to it (a plugin, an adapter, a tool)
        // reach it here. Null when nobody owns one: a producer then simply does not record.
        static inline hub* s_pCurrent = nullptr;
        static hub* current() noexcept { return s_pCurrent; }
        void make_current() noexcept    { s_pCurrent = this; }
        void release_current() noexcept { if (s_pCurrent == this) s_pCurrent = nullptr; }
        ~hub() { release_current(); m_pSink.reset(); }          // the sink first: its writer finishes while the rest of the hub is still alive

        // ---- any thread -------------------------------------------------------------------------------------------
        // ---- the capture policy ---------------------------------------------------------------------------------------------------------
        // What is collected at all, by channel and level (design 4: "a disabled call is one load and one compare"). By default everything from Debug up; Trace is
        // not collected. A rule raises the level of a channel prefix for a while (Focus). What the policy leaves out is INTENTIONAL exclusion, counted separately from loss
        // (a ring that overflowed): it never makes a session look partial. Callable from any thread.
        struct capture_rule { std::string m_Prefix; severity m_Min = severity::Debug; std::uint64_t m_UntilNs = 0; };      // m_UntilNs 0 = until removed

        // Is an event of this channel and level collected? Producers ask BEFORE formatting anything.
        bool Enabled(std::string_view Channel, severity S) const noexcept
        {
            std::lock_guard Lock(m_PolicyMutex);
            const std::uint64_t Now_ = Now();
            severity Min = m_DefaultMin;
            std::size_t Best = 0; bool bFound = false;
            for (const auto& R : m_Rules)
            {
                if (R.m_UntilNs && Now_ >= R.m_UntilNs) continue;
                if (Channel.compare(0, R.m_Prefix.size(), R.m_Prefix) != 0) continue;
                if (!bFound || R.m_Prefix.size() >= Best) { Min = R.m_Min; Best = R.m_Prefix.size(); bFound = true; }       // the most specific rule wins
            }
            return S >= Min;
        }

        void SetCapture(std::string Prefix, severity Min, std::uint64_t ForNs = 0) noexcept
        {
            std::lock_guard Lock(m_PolicyMutex);
            const std::uint64_t Until = ForNs ? Now() + ForNs : 0;
            for (auto& R : m_Rules) if (R.m_Prefix == Prefix) { R.m_Min = Min; R.m_UntilNs = Until; return; }
            m_Rules.push_back({ std::move(Prefix), Min, Until });
        }
        // The rule of a prefix as it is now (to put it back later); false when there is none
        bool GetCapture(const std::string& Prefix, capture_rule& Out) const noexcept
        {
            std::lock_guard Lock(m_PolicyMutex);
            for (const auto& R : m_Rules) if (R.m_Prefix == Prefix && !(R.m_UntilNs && Now() >= R.m_UntilNs)) { Out = R; return true; }
            return false;
        }
        void ClearCapture(const std::string& Prefix) noexcept
        {
            std::lock_guard Lock(m_PolicyMutex);
            m_Rules.erase(std::remove_if(m_Rules.begin(), m_Rules.end(), [&](const capture_rule& R) { return R.m_Prefix == Prefix; }), m_Rules.end());
        }
        std::vector<capture_rule> CaptureRules() const noexcept
        {
            std::lock_guard Lock(m_PolicyMutex);
            std::vector<capture_rule> Out;
            for (const auto& R : m_Rules) if (!(R.m_UntilNs && Now() >= R.m_UntilNs)) Out.push_back(R);
            return Out;
        }
        severity DefaultCapture() const noexcept { return m_DefaultMin; }
        std::uint64_t ExcludedCount(severity S) const noexcept { return m_Excluded[static_cast<int>(S)].load(std::memory_order_relaxed); }

        void Emit(event&& E) noexcept
        {
            if (!Enabled(E.m_Channel, E.m_Severity)) { m_Excluded[static_cast<int>(E.m_Severity)].fetch_add(1, std::memory_order_relaxed); return; }
            E.m_ObservedAt = Now();
            const severity S = E.m_Severity;                 // read before the event is moved
            Push(record{ std::move(E) }, S);
        }

        op_handle Begin(std::string Kind, origin Origin, ref Subject, std::string Title = {}, std::string VerificationTarget = {}, std::uint64_t Parent = 0) noexcept
        {
            op_begin B;
            B.m_Id = m_NextOperation.fetch_add(1, std::memory_order_relaxed);
            B.m_Parent = Parent; B.m_At = Now();
            B.m_Kind = std::move(Kind); B.m_Title = std::move(Title); B.m_Target = std::move(VerificationTarget);
            B.m_Origin = std::move(Origin); B.m_Subject = std::move(Subject);
            const auto Id = B.m_Id;
            Push(record{ std::move(B) }, severity::Info, /*bBoundary=*/true);
            return op_handle(this, Id);
        }

        void PushOperationRecord(record&& R) noexcept { Push(std::move(R), severity::Info, true); }
        std::uint64_t Now() const noexcept { return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - m_Start).count()); }

        // ---- the HOST thread (the UI frame, or the headless loop iteration) ----------------------------------------
        // Commits up to MaxRecords pending records; returns how many are still waiting. A backlog is reported, never silent.
        std::size_t Drain(std::size_t MaxRecords = 8192) noexcept
        {
            std::vector<record> Batch;
            {
                std::lock_guard Lock(m_RingMutex);
                const std::size_t N = std::min(MaxRecords, m_Ring.size());
                Batch.reserve(N);
                for (std::size_t i = 0; i < N; ++i) { Batch.push_back(std::move(m_Ring.front())); m_Ring.pop_front(); }
            }
            for (auto& R : Batch) Commit(std::move(R));
            if (m_pSink && !m_bPersistenceFailureReported)
                if (const sink_status St = m_pSink->Status(); St.m_bFailed)
                {
                    m_bPersistenceFailureReported = true;
                    event E;
                    E.m_Producer = "xlog.store"; E.m_Origin = { origin::type::System, "logs", 0 }; E.m_Severity = severity::Fatal; E.m_Kind = kind::Diagnostic; E.m_Channel = "logs.health";
                    E.m_Code = "LOGS.PERSISTENCE_FAILED"; SetMessage(E, "The Logs cannot be written to disk: " + St.m_Reason + "\nThis launch's history is kept in memory only.");
                    Emit(std::move(E));
                }
            std::lock_guard Lock(m_RingMutex);
            return m_Ring.size();
        }

        // ---- store access: host thread only ------------------------------------------------------------------------
        std::uint64_t Session() const noexcept          { return m_Session; }
        std::uint64_t StartWallMs() const noexcept      { return m_StartWall; }
        std::uint64_t Committed() const noexcept        { return m_NextSequence - 1; }
        std::size_t   Capacity() const noexcept         { return m_MaxEvents; }
        void          SetCapacity(std::size_t Max) noexcept { m_MaxEvents = std::max<std::size_t>(Max, 1024); }

        const event* FindEvent(std::uint64_t Seq) const noexcept
        {
            if (m_Segments.empty() || Seq < m_Segments.front().m_First) return nullptr;
            auto It = std::upper_bound(m_Segments.begin(), m_Segments.end(), Seq, [](std::uint64_t S, const segment& G) { return S < G.m_First; });
            if (It == m_Segments.begin()) return nullptr;
            --It;
            const std::size_t Index = static_cast<std::size_t>(Seq - It->m_First);
            return Index < It->m_Events.size() ? &It->m_Events[Index] : nullptr;
        }

        // Visits the retained events with AfterSeq < sequence <= UpToSeq, oldest first; stop by returning false.
        template<class F> void ForEachEvent(std::uint64_t AfterSeq, std::uint64_t UpToSeq, F&& Visit) const noexcept
        {
            for (const auto& G : m_Segments)
            {
                if (G.m_Events.empty() || G.m_First + G.m_Events.size() - 1 <= AfterSeq) continue;
                if (G.m_First > UpToSeq) break;
                const std::size_t From = AfterSeq >= G.m_First ? static_cast<std::size_t>(AfterSeq - G.m_First + 1) : 0;
                for (std::size_t i = From; i < G.m_Events.size(); ++i)
                {
                    if (G.m_Events[i].m_Key.m_Sequence > UpToSeq) return;
                    if (!Visit(G.m_Events[i])) return;
                }
            }
        }

        const operation* FindOperation(std::uint64_t Id) const noexcept { auto It = m_Operations.find(Id); return It == m_Operations.end() ? nullptr : &It->second; }
        const problem*   FindProblem(std::uint64_t Id) const noexcept   { auto It = m_Problems.find(Id);   return It == m_Problems.end() ? nullptr : &It->second; }
        const std::vector<std::uint64_t>& OperationOrder() const noexcept { return m_OperationOrder; }   // ascending id

        // The ruler's data (design 4.1): per second of the launch, how many events and the worst severity among them. Built as events are committed, so drawing is independent of how many there are.
        struct density_bucket { std::uint32_t m_Count = 0; severity m_Worst = severity::Trace; };
        const std::vector<density_bucket>& Density() const noexcept { return m_Density; }
        const std::vector<origin>&        Origins() const noexcept        { return m_Origins; }              // every origin that has produced an event, once each (the Source lens lists them)
        const std::vector<std::uint64_t>& ProblemOrder() const noexcept   { return m_ProblemOrder; }     // first seen first

        // ---- attachments (host thread)
        const std::vector<attachment>& Attachments() const noexcept { return m_Attachments; }
        std::uint64_t AddAttachment(attachment A) noexcept { A.m_Id = m_Attachments.size() + 1; const auto Id = A.m_Id; m_Attachments.push_back(std::move(A)); ++m_Revision; return Id; }
        std::uint64_t AttachmentBytes() const noexcept { std::uint64_t N = 0; for (const auto& A : m_Attachments) N += A.m_Bytes; return N; }

        // The sink, so a second one can be put beside it (a runtime that also speaks to the editor), and a one-line account of the remote runtimes for LogStatus (the host sets it).
        std::shared_ptr<record_sink> Sink() const noexcept { return m_pSink; }
        void SetRemoteStatus(std::function<std::string()> Fn) noexcept { m_RemoteStatus = std::move(Fn); }
        std::string RemoteStatus() const noexcept { return m_RemoteStatus ? m_RemoteStatus() : std::string(); }

        // ---- what an asset depends on (the About lens, "and what it depends on"). The editor registers a provider that knows its libraries; the Logs hold no dependency graph of their own.
        using dependency_provider = std::function<std::vector<ref>(std::string_view Asset)>;
        void SetDependencyProvider(dependency_provider Fn) noexcept { m_DependencyProvider = std::move(Fn); m_DepCache.clear(); }
        void SimulateDependencies(std::string Asset, std::vector<ref> Depends) noexcept { m_SimulatedDeps[std::move(Asset)] = std::move(Depends); m_DepCache.clear(); }   // a test's stand-in for the provider
        std::vector<ref> Dependencies(std::string_view Asset) const noexcept
        {
            const std::string Key(Asset);
            if (const auto S = m_SimulatedDeps.find(Key); S != m_SimulatedDeps.end()) return S->second;
            if (!m_DependencyProvider) return {};
            const std::uint64_t Time = Now();
            if (const auto C = m_DepCache.find(Key); C != m_DepCache.end() && Time - C->second.first < 2000000000ull) return C->second.second;      // a frame asks again and again: the answer is good for two seconds
            auto Found = m_DependencyProvider(Asset);
            m_DepCache[Key] = { Time, Found };
            return Found;
        }
        // ParseQuery plus what only the hub knows: with deps:yes the assets the wanted one depends on.
        filter Parse(std::string_view Query) const noexcept
        {
            filter F = ParseQuery(Query);
            if (F.m_Error.empty() && F.m_bDeps && !F.m_Asset.empty()) F.m_AssetAlso = Dependencies(F.m_Asset);
            return F;
        }

        // ---- persistence and earlier launches (xlog_store.h) -------------------------------------------------------------------------
        void AttachSink(std::shared_ptr<record_sink> pSink) noexcept { m_pSink = std::move(pSink); }
        sink_status SinkStatus() const noexcept { return m_pSink ? m_pSink->Status() : sink_status{}; }
        bool HasSink() const noexcept { return m_pSink != nullptr; }

        // Where this launch's records are written ("" = nowhere), and how the import of the earlier launches stands ("" = none was started, "running", "done n")
        void SetPersistenceInfo(std::string Directory) noexcept { std::lock_guard Lock(m_InfoMutex); m_PersistDir = std::move(Directory); }
        std::string PersistenceDirectory() const noexcept { std::lock_guard Lock(m_InfoMutex); return m_PersistDir; }
        void SetImportState(std::string State) noexcept { std::lock_guard Lock(m_InfoMutex); m_ImportState = std::move(State); }
        std::string ImportState() const noexcept { std::lock_guard Lock(m_InfoMutex); return m_ImportState; }

        // A hub that holds an EARLIER launch read back from its stream (LogCompare, the history): its own session id, and records replayed as they were committed then,
        // keys and times included. Never used on the live hub.
        void SetSession(std::uint64_t Session) noexcept { m_Session = Session; }
        void ReplayRecord(record&& R) noexcept
        {
            if (auto* pE = std::get_if<event>(&R)) { m_NextSequence = std::max<std::uint64_t>(pE->m_Key.m_Sequence, 1); CommitEvent(std::move(*pE)); }
            else Commit(std::move(R));
        }
        // An operation that was still running when the launch ended did not succeed and did not fail: it was abandoned (design 4, crash recovery)
        std::size_t AbandonRunningOperations() noexcept
        {
            std::size_t N = 0;
            for (auto& [Id, O] : m_Operations) if (O.m_Outcome == outcome::Running) { O.m_Outcome = outcome::Abandoned; ++N; }
            return N;
        }

        // Problems that were verified resolved in an earlier launch (identity -> that launch's id): when one of them appears again it is a regression across launches
        void SetPriorVerified(std::unordered_map<std::uint64_t, std::string> Map) noexcept { std::lock_guard Lock(m_InfoMutex); m_PriorVerified = std::move(Map); }

        // ---- rechecks: how a producer can be asked to look again (LogVerify) ---------------------------------------------------------------
        // A producer that can re-run what made a problem registers, for its channel prefix, a function that STARTS that recheck as a normal operation (a build, a compile) and
        // returns "" when it did, or why it cannot. The evidence that operation produces sets the verification state by itself: nothing here claims a result.
        using recheck_fn = std::function<std::string(const problem&)>;
        void SetRecheck(std::string ChannelPrefix, recheck_fn Fn) noexcept
        {
            for (auto& R : m_Rechecks) if (R.first == ChannelPrefix) { R.second = std::move(Fn); return; }          // one per prefix: every Level session registers the same one
            m_Rechecks.emplace_back(std::move(ChannelPrefix), std::move(Fn));
        }
        std::string Recheck(const problem& P) const noexcept
        {
            for (const auto& [Prefix, Fn] : m_Rechecks)
                if (P.m_Channel.compare(0, Prefix.size(), Prefix) == 0) return Fn ? Fn(P) : std::string("the producer's recheck is not available");
            return std::format("nothing can recheck the channel '{}' (its producer registered no recheck)", P.m_Channel);
        }

        // Bumped by everything a view could show: a committed record, an annotation, a new baseline. A view caches by it.
        std::uint64_t Revision() const noexcept { return m_Revision; }

        // ---- the person's decisions (host thread) ------------------------------------------------------------------
        annotation Annotation(std::uint64_t ProblemId) const noexcept { auto It = m_Annotations.find(ProblemId); return It == m_Annotations.end() ? annotation{} : It->second; }
        void SetAcknowledged(std::uint64_t ProblemId, bool b) noexcept { Annotate(ProblemId).m_bAcknowledged = b; ++m_Revision; UserChanged(); }
        void SetMuted(std::uint64_t ProblemId, bool b) noexcept        { Annotate(ProblemId).m_bMuted = b; ++m_Revision; UserChanged(); }

        // What the person decided lives past the launch (Project.config/Logs/<user>.logs.txt): the store loads it before the first event and saves on every change. An annotation whose problem
        // has not appeared in this launch is kept as it is: it applies the moment the problem does.
        const std::unordered_map<std::uint64_t, annotation>& Annotations() const noexcept { return m_Annotations; }
        const std::vector<saved_view>& SavedViews() const noexcept { return m_SavedViews; }
        void LoadUser(std::unordered_map<std::uint64_t, annotation> Annotations, std::vector<saved_view> Views) noexcept { m_Annotations = std::move(Annotations); m_SavedViews = std::move(Views); ++m_Revision; }
        void SetOnUserChange(std::function<void()> Fn) noexcept { m_OnUserChange = std::move(Fn); }
        const saved_view* FindView(std::string_view Name) const noexcept { for (const auto& V : m_SavedViews) if (V.m_Name == Name) return &V; return nullptr; }
        void SaveView(saved_view View) noexcept
        {
            for (auto& V : m_SavedViews) if (V.m_Name == View.m_Name) { V = std::move(View); UserChanged(); return; }
            m_SavedViews.push_back(std::move(View)); UserChanged();
        }
        bool DeleteView(std::string_view Name) noexcept
        {
            for (auto It = m_SavedViews.begin(); It != m_SavedViews.end(); ++It) if (It->m_Name == Name) { m_SavedViews.erase(It); UserChanged(); return true; }
            return false;
        }
        std::size_t MutedCount() const noexcept { std::size_t N = 0; for (const auto& [Id, A] : m_Annotations) if (A.m_bMuted && m_Problems.contains(Id)) ++N; return N; }

        // "New" means first seen after the baseline; by default that is the start of this launch (everything). Moving it is "I have seen these".
        std::uint64_t Baseline() const noexcept { return m_Baseline; }
        void SetBaseline(std::uint64_t Sequence) noexcept { m_Baseline = Sequence; ++m_Revision; }

        // A muted problem is not listed unless asked for; a Fatal one can never be hidden.
        bool InView(const problem& P, problem_view V, bool bShowMuted = false) const noexcept
        {
            const annotation A = Annotation(P.m_Id);
            if (A.m_bMuted && !bShowMuted && P.m_Severity < severity::Fatal) return false;
            switch (V)
            {
            case problem_view::New:    return P.m_FirstSeq > m_Baseline;
            case problem_view::Active: return !A.m_bAcknowledged;
            default:                   return true;
            }
        }

        status Status() const noexcept
        {
            status S;
            S.m_Session = m_Session; S.m_Committed = Committed(); S.m_Capacity = m_MaxEvents;
            { std::lock_guard Lock(m_RingMutex); S.m_Backlog = m_Ring.size(); }
            for (const auto& G : m_Segments) S.m_Events += G.m_Events.size();
            S.m_Problems = m_Problems.size(); S.m_Operations = m_Operations.size();
            for (int i = 0; i < 6; ++i) { S.m_BySeverity[i] = m_Counts[i]; S.m_Dropped[i] = m_Dropped[i].load(std::memory_order_relaxed); }
            for (const auto& [Id, P] : m_Problems) ++S.m_ProblemsBySeverity[static_cast<int>(P.m_Severity)];
            S.m_Expired = m_Expired;
            return S;
        }

    private:
        struct segment { std::uint64_t m_First = 0; std::vector<event> m_Events; };
        static constexpr std::size_t segment_size_v = 4096;

        void Push(record&& R, severity S, bool bBoundary = false) noexcept
        {
            std::lock_guard Lock(m_RingMutex);
            // Bounded: overflow never blocks a producer. Low levels go first; diagnostics and operation boundaries keep their room (up to twice the ring).
            const bool bFull = m_Ring.size() >= m_RingCap;
            if (bFull && !bBoundary && S < severity::Warning) { m_Dropped[static_cast<int>(S)].fetch_add(1, std::memory_order_relaxed); return; }
            if (m_Ring.size() >= m_RingCap * 2) { m_Dropped[static_cast<int>(S)].fetch_add(1, std::memory_order_relaxed); return; }
            m_Ring.push_back(std::move(R));
        }

        annotation& Annotate(std::uint64_t ProblemId) noexcept
        {
            auto& A = m_Annotations[ProblemId];
            if (const problem* P = FindProblem(ProblemId)) A.m_Label = P->m_Code.empty() ? P->m_Title : P->m_Code + "  " + P->m_Title;
            return A;
        }
        void UserChanged() noexcept { if (m_OnUserChange) m_OnUserChange(); }

        void Commit(record&& R) noexcept
        {
            ++m_Revision;
            if (m_pSink && !std::holds_alternative<event>(R)) m_pSink->Push(R);          // operation records as they are; events are handed over in CommitEvent, once they have their key
            if (auto* pE = std::get_if<event>(&R))            CommitEvent(std::move(*pE));
            else if (auto* pB = std::get_if<op_begin>(&R))    CommitBegin(std::move(*pB));
            else if (auto* pU = std::get_if<op_unit>(&R))     { if (auto It = m_Operations.find(pU->m_Id); It != m_Operations.end() && It->second.m_Units.size() < 4096) It->second.m_Units.push_back(std::move(pU->m_Unit)); }
            else if (auto* pC = std::get_if<op_coverage>(&R)) { if (auto It = m_Operations.find(pC->m_Id); It != m_Operations.end()) It->second.m_Coverage = pC->m_Coverage; }
            else if (auto* pX = std::get_if<op_end>(&R))      CommitEnd(*pX);
        }

        void CommitBegin(op_begin&& B) noexcept
        {
            operation O;
            O.m_Id = B.m_Id; O.m_Parent = B.m_Parent; O.m_Kind = std::move(B.m_Kind); O.m_Title = std::move(B.m_Title);
            O.m_Origin = std::move(B.m_Origin); O.m_Subject = std::move(B.m_Subject); O.m_Started = B.m_At; O.m_VerificationTarget = std::move(B.m_Target);
            O.m_DroppedAtBegin = DroppedImportant();
            m_OperationOrder.push_back(O.m_Id);
            m_Operations.emplace(O.m_Id, std::move(O));
            while (m_OperationOrder.size() > m_MaxOperations) { m_Operations.erase(m_OperationOrder.front()); m_OperationOrder.erase(m_OperationOrder.begin()); }
        }

        void CommitEnd(const op_end& X) noexcept
        {
            auto It = m_Operations.find(X.m_Id);
            if (It == m_Operations.end()) return;
            operation& O = It->second;
            O.m_Ended = X.m_At; O.m_Outcome = X.m_Outcome;
            // An operation that failed without a single error of its own must still be visible as a problem.
            if (X.m_Outcome == outcome::Failed && O.m_Errors == 0)
            {
                event E;
                E.m_ObservedAt = X.m_At; E.m_Producer = "xlion.logs"; E.m_Origin = O.m_Origin; E.m_Severity = severity::Error; E.m_Kind = kind::Diagnostic;
                E.m_Channel = O.m_Kind; E.m_Title = std::format("{} failed without diagnostic details", O.m_Title.empty() ? O.m_Kind : O.m_Title);
                E.m_Code = "OPERATION.FAILED_WITHOUT_DIAGNOSTICS"; E.m_Operation = O.m_Id; E.m_Discriminator = O.m_Kind;
                if (O.m_Subject.Valid()) E.m_Subjects.push_back(O.m_Subject);
                CommitEvent(std::move(E));
            }
            O.m_bEvidenceReady = true;      // everything pushed before the end record is committed: the ring is ordered
            O.m_bCollectorLoss = DroppedImportant() != O.m_DroppedAtBegin;
            JudgeProblems(O);
        }

        // Warnings and above the collector had to drop (the ring overflowed): evidence that may be missing.
        std::uint64_t DroppedImportant() const noexcept
        {
            std::uint64_t N = 0;
            for (int i = static_cast<int>(severity::Warning); i < 6; ++i) N += m_Dropped[i].load(std::memory_order_relaxed);
            return N;
        }

        // What a finished operation says about the problems of its verification target (design 5.2). Only within the coverage its producer declared:
        // unknown coverage, a failed operation, incomplete evidence or collector loss verify nothing.
        void JudgeProblems(const operation& O) noexcept
        {
            if (O.m_VerificationTarget.empty()) return;
            for (auto& [Id, P] : m_Problems)
            {
                if (P.m_Target != O.m_VerificationTarget) continue;
                const bool bOccurred = std::find(O.m_Problems.begin(), O.m_Problems.end(), Id) != O.m_Problems.end();
                if (bOccurred)
                {
                    P.m_Presence = presence::Observed;
                    if (O.m_Outcome == outcome::Succeeded && P.m_FirstSeq < O.m_FirstSeq) P.m_Verification = verification::Reproduced;       // it was here before, and the success did not remove it
                    continue;
                }
                bool bChecked = false;                                    // did this operation look at what produced the problem?
                if (O.m_Coverage == coverage_kind::Complete) bChecked = true;
                else if (O.m_Coverage == coverage_kind::Subjects) bChecked = !P.m_CheckUnit.empty() && std::find(O.m_Units.begin(), O.m_Units.end(), P.m_CheckUnit) != O.m_Units.end();
                if (O.m_Coverage == coverage_kind::Unknown) { P.m_Presence = presence::Unknown; continue; }
                P.m_Presence = presence::NotObserved;                      // not seen: said nothing about being fixed unless it was really looked at
                if (bChecked && O.m_Outcome == outcome::Succeeded && !O.m_bCollectorLoss && P.m_Verification != verification::Verified)
                { P.m_Verification = verification::Verified; P.m_VerifiedBy = O.m_Id; }
            }
        }

        void CommitEvent(event&& E) noexcept
        {
            E.m_Key = { m_Session, m_NextSequence++ };
            if (m_Segments.empty() || m_Segments.back().m_Events.size() >= segment_size_v)
            {
                m_Segments.push_back({ E.m_Key.m_Sequence, {} });
                m_Segments.back().m_Events.reserve(segment_size_v);
            }
            if (m_pSink) m_pSink->Push(record(E));
            ++m_Counts[static_cast<int>(E.m_Severity)];
            {
                const std::size_t Second = static_cast<std::size_t>(E.m_ObservedAt / 1000000000ull);
                if (Second < 1u << 20)                                   // a launch of twelve days: beyond that the strip stops growing
                {
                    if (Second >= m_Density.size()) m_Density.resize(Second + 1);
                    ++m_Density[Second].m_Count; if (E.m_Severity > m_Density[Second].m_Worst) m_Density[Second].m_Worst = E.m_Severity;
                }
            }
            if (std::none_of(m_Origins.begin(), m_Origins.end(), [&](const origin& O) { return O.m_Name == E.m_Origin.m_Name && O.m_Type == E.m_Origin.m_Type; })) m_Origins.push_back({ E.m_Origin.m_Type, E.m_Origin.m_Name, 0 });

            if (E.m_Operation)
            {
                if (auto It = m_Operations.find(E.m_Operation); It != m_Operations.end())
                {
                    ++It->second.m_Events;
                    if (!It->second.m_FirstSeq) It->second.m_FirstSeq = E.m_Key.m_Sequence;
                    It->second.m_LastSeq = E.m_Key.m_Sequence;
                    if (E.m_Severity >= severity::Error)        ++It->second.m_Errors;
                    else if (E.m_Severity == severity::Warning) ++It->second.m_Warnings;
                }
            }
            if (E.m_Kind == kind::Diagnostic && E.m_Severity >= severity::Warning) UpdateProblem(E);

            m_Segments.back().m_Events.push_back(std::move(E));

            std::size_t Total = 0;
            for (const auto& G : m_Segments) Total += G.m_Events.size();
            while (Total > m_MaxEvents && m_Segments.size() > 1) { Total -= m_Segments.front().m_Events.size(); m_Expired += m_Segments.front().m_Events.size(); m_Segments.pop_front(); }
        }

        void UpdateProblem(const event& E) noexcept
        {
            const auto Id = ProblemId(E);
            auto [It, bNew] = m_Problems.try_emplace(Id);
            problem& P = It->second;
            if (bNew)
            {
                if (!m_PriorVerified.empty())
                {
                    std::lock_guard Lock(m_InfoMutex);
                    if (auto Prior = m_PriorVerified.find(Id); Prior != m_PriorVerified.end()) { P.m_PreviousSession = Prior->second; P.m_Regressions = 1; }
                }
                P.m_Id = Id; P.m_Producer = E.m_Producer; P.m_Code = E.m_Code; P.m_Title = E.m_Title; P.m_Channel = E.m_Channel; P.m_OriginName = E.m_Origin.m_Name;
                P.m_FirstSeq = E.m_Key.m_Sequence; P.m_Site = E.m_Source;
                if (!E.m_Subjects.empty()) P.m_Subject = E.m_Subjects[0];
                P.m_Discriminator = E.m_Discriminator; P.m_bHeuristic = E.m_bHeuristic || E.m_Code.empty();
                m_ProblemOrder.push_back(Id);
            }
            const bool bNewOperation = E.m_Operation != 0 && E.m_Operation != P.m_LastOperation;
            if (E.m_Operation)
                if (auto Op = m_Operations.find(E.m_Operation); Op != m_Operations.end() && !Op->second.m_VerificationTarget.empty()) P.m_Target = Op->second.m_VerificationTarget;
            if (!bNew && bNewOperation)
            {
                // It is back. After a verification that is a regression; after the person acknowledged it, it is recurring and asks for attention again.
                if (P.m_Verification == verification::Verified) { ++P.m_Regressions; P.m_Verification = verification::Unverified; P.m_VerifiedBy = 0; }
                if (auto A = m_Annotations.find(P.m_Id); A != m_Annotations.end() && A->second.m_bAcknowledged) { P.m_bRecurring = true; A->second.m_bAcknowledged = false; ++m_Revision; }
            }
            if (!bNew || bNewOperation) P.m_Presence = presence::Observed;
            if (bNew) P.m_FirstAt = E.m_ObservedAt;
            P.m_LastAt = E.m_ObservedAt;
            P.m_LastSeq = E.m_Key.m_Sequence; ++P.m_Count;
            if (E.m_Severity > P.m_Severity) P.m_Severity = E.m_Severity;
            if (P.m_First.size() < problem_retained_v) P.m_First.push_back(E.m_Key.m_Sequence);
            else { P.m_Last.push_back(E.m_Key.m_Sequence); if (P.m_Last.size() > problem_retained_v) P.m_Last.erase(P.m_Last.begin()); }
            if (E.m_Operation)
            {
                P.m_LastOperation = E.m_Operation;
                if (auto Op = m_Operations.find(E.m_Operation); Op != m_Operations.end())
                    if (std::find(Op->second.m_Problems.begin(), Op->second.m_Problems.end(), Id) == Op->second.m_Problems.end()) Op->second.m_Problems.push_back(Id);
            }
            for (const auto& A : E.m_Attributes)
                if (A.m_Name == "unit") if (auto* pS = std::get_if<std::string>(&A.m_Value)) P.m_CheckUnit = *pS;
        }

        mutable std::mutex          m_RingMutex;
        std::deque<record>          m_Ring;
        std::size_t                 m_RingCap = 65536;
        std::atomic<std::uint64_t>  m_Dropped[6] = {};

        std::uint64_t               m_Session = 0;
        std::chrono::steady_clock::time_point m_Start;
        std::uint64_t               m_StartWall = 0;
        std::atomic<std::uint64_t>  m_NextOperation{ 1 };
        std::uint64_t               m_NextSequence = 1;

        std::deque<segment>         m_Segments;
        std::size_t                 m_MaxEvents = 100000;
        std::uint64_t               m_Counts[6] = {};
        std::uint64_t               m_Expired = 0;

        std::unordered_map<std::uint64_t, operation> m_Operations;
        std::vector<std::uint64_t>  m_OperationOrder;
        std::size_t                 m_MaxOperations = 5000;
        std::unordered_map<std::uint64_t, problem>   m_Problems;
        std::vector<std::uint64_t>  m_ProblemOrder;
        std::vector<origin>         m_Origins;
        std::vector<density_bucket> m_Density;
        std::shared_ptr<record_sink> m_pSink;
        bool                        m_bPersistenceFailureReported = false;
        mutable std::mutex          m_InfoMutex;
        std::string                 m_PersistDir, m_ImportState;
        std::unordered_map<std::uint64_t, std::string> m_PriorVerified;
        mutable std::mutex          m_PolicyMutex;
        std::vector<capture_rule>   m_Rules;
        severity                    m_DefaultMin = severity::Debug;
        std::atomic<std::uint64_t>  m_Excluded[6] = {};
        std::vector<std::pair<std::string, recheck_fn>> m_Rechecks;
        std::unordered_map<std::uint64_t, annotation> m_Annotations;
        std::vector<saved_view>     m_SavedViews;
        std::vector<attachment>     m_Attachments;
        std::function<std::string()> m_RemoteStatus;
        dependency_provider         m_DependencyProvider;
        std::unordered_map<std::string, std::vector<ref>> m_SimulatedDeps;
        mutable std::unordered_map<std::string, std::pair<std::uint64_t, std::vector<ref>>> m_DepCache;
        std::function<void()>       m_OnUserChange;
        std::uint64_t               m_Baseline = 0;
        std::uint64_t               m_Revision = 1;
    };

    // Does the problem match the query? The one rule for the pipe's LogProblems and for the window's list, so both show the same rows.
    inline bool ProblemMatches(const hub& H, const problem& P, const filter& F, severity Min = severity::Trace) noexcept
    {
        if (P.m_Severity < Min || P.m_Severity < F.m_Min) return false;
        if (!F.m_Channel.empty() && !details::Prefix(P.m_Channel, F.m_Channel)) return false;
        if (!F.m_NotChannel.empty() && details::Prefix(P.m_Channel, F.m_NotChannel)) return false;
        if (!F.m_Code.empty() && P.m_Code != F.m_Code) return false;
        if (!F.m_Origins.empty() && std::find(F.m_Origins.begin(), F.m_Origins.end(), P.m_OriginName) == F.m_Origins.end()) return false;
        if (!F.m_Producer.empty() && P.m_Producer != F.m_Producer) return false;
        if (P.m_LastAt < F.m_TimeFrom || P.m_FirstAt > F.m_TimeTo) return false;           // seen at some time inside the range
        if (!F.m_Asset.empty() && !details::AboutAsset(F, P.m_Subject)) return false;
        if (F.m_Operation)
        {
            const operation* O = H.FindOperation(F.m_Operation);
            if (!O || std::find(O->m_Problems.begin(), O->m_Problems.end(), P.m_Id) == O->m_Problems.end()) return false;
        }
        for (const auto& [Text, bBodyOnly] : F.m_Terms)
        {
            if (bBodyOnly) return false;           // a problem has no body of its own: its occurrences do (search them in Events)
            if (!details::ContainsNoCase(P.m_Title, Text) && !details::ContainsNoCase(P.m_Site.m_Path, Text) && !details::ContainsNoCase(P.m_Subject.m_Path, Text)
             && !details::ContainsNoCase(P.m_Discriminator, Text) && !details::ContainsNoCase(P.m_Code, Text)) return false;
        }
        return true;
    }

    inline void op_handle::Unit(std::string Name) noexcept { if (m_pHub && !m_bEnded) m_pHub->PushOperationRecord(record{ op_unit{ m_Id, std::move(Name) } }); }
    inline void op_handle::SetCoverage(coverage_kind C) noexcept { if (m_pHub && !m_bEnded) m_pHub->PushOperationRecord(record{ op_coverage{ m_Id, C } }); }
    inline void op_handle::End(outcome O) noexcept
    {
        if (!m_pHub || m_bEnded) return;
        m_bEnded = true;
        m_pHub->PushOperationRecord(record{ op_end{ m_Id, m_pHub->Now(), O } });
    }
}

#endif // XLOG_HUB_H
