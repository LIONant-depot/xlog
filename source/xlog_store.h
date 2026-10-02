#ifndef XLOG_STORE_H
#define XLOG_STORE_H
#pragma once

// Persistence (documentation/Editors/DESIGN_logs.md, section 4: "Durability and recovery"). Standard library only, so the headless host keeps its history too.
//
//   <project>/Cache/Logs/<session id>/stream.xlog     the ONE authoritative append-only stream: events and operation records, in the order they were committed
//                                    summary.txt      a projection (rebuildable from the stream): counts and how the launch ended
//                                    pinned           (optional) a file whose presence protects the session from retention
//   <project>/Cache/Logs/imported.txt                 the import checkpoint: the launches whose end has already been reported, so a tail is never imported twice
//
// Every record is one framed line:   R<payload length, 8 hex><FNV-1a 32 of the payload, 8 hex> <payload>\n     (payload: tab separated fields; \\, tab, CR and LF escaped)
// so the last COMPLETE record is identifiable and a torn final write is cut off when the stream is opened (and reported). Writing is a worker thread fed by a bounded queue: the
// host thread only copies the record in; a full queue drops and counts (PendingWrite / Dropped in LogStatus), a failing disk is a Fatal collector-health problem. A launch that
// ends cleanly writes a final marker; one that does not is CLASSIFIED at the next start, never assumed: a confirmed crash (a crash record exists), an interrupted shutdown (no
// marker, no crash record), or unknown (nothing readable). Operations still running in it were abandoned, in that launch.
#include "xlog_hub.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>
#include <unordered_set>
#if defined(_WIN32)
    #include <process.h>
#endif

namespace xlog::store
{
    inline constexpr int schema_v = 1;
    inline constexpr const char* stream_name_v = "stream.xlog";

    //------------------------------------------------------------------------------------------------------------------
    // Text: escaping, fields, references, attributes
    //------------------------------------------------------------------------------------------------------------------
    inline void Escape(std::string& Out, std::string_view In)
    {
        for (char c : In)
        {
            switch (c)
            {
            case '\\': Out += "\\\\"; break;
            case '\t': Out += "\\t";  break;
            case '\n': Out += "\\n";  break;
            case '\r': Out += "\\r";  break;
            default:   Out += c;
            }
        }
    }

    inline std::string Unescape(std::string_view In)
    {
        std::string Out;
        Out.reserve(In.size());
        for (std::size_t i = 0; i < In.size(); ++i)
        {
            if (In[i] != '\\' || i + 1 >= In.size()) { Out += In[i]; continue; }
            switch (In[++i])
            {
            case 't': Out += '\t'; break;
            case 'n': Out += '\n'; break;
            case 'r': Out += '\r'; break;
            default:  Out += In[i];
            }
        }
        return Out;
    }

    inline void Field(std::string& Out, std::string_view Text) { if (!Out.empty()) Out += '\t'; Escape(Out, Text); }
    template<class T> inline void Number(std::string& Out, T Value) { Field(Out, std::to_string(Value)); }

    inline std::vector<std::string> Split(std::string_view Text, char Separator)
    {
        std::vector<std::string> Out;
        std::size_t At = 0;
        for (;;)
        {
            const auto Next = Text.find(Separator, At);
            Out.emplace_back(Text.substr(At, Next == std::string_view::npos ? std::string_view::npos : Next - At));
            if (Next == std::string_view::npos) break;
            At = Next + 1;
        }
        return Out;
    }

    inline std::string RefText(const ref& R) { return std::format("{}:{}:{}:{}:{}:{}", static_cast<int>(R.m_Type), R.m_Id, R.m_Line, R.m_Column, R.m_Revision, R.m_Path); }

    inline ref ParseRef(std::string_view Text)
    {
        ref R;
        std::size_t At = 0;
        std::uint64_t V[5] = {};
        for (int i = 0; i < 5; ++i)
        {
            const auto Colon = Text.find(':', At);
            if (Colon == std::string_view::npos) return {};
            V[i] = std::strtoull(std::string(Text.substr(At, Colon - At)).c_str(), nullptr, 10);
            At = Colon + 1;
        }
        R.m_Type = static_cast<ref::type>(V[0]); R.m_Id = V[1]; R.m_Line = static_cast<std::int32_t>(V[2]); R.m_Column = static_cast<std::int32_t>(V[3]); R.m_Revision = V[4];
        R.m_Path = std::string(Text.substr(At));
        return R;
    }

    inline std::string AttributesText(const std::vector<attribute>& Attributes)
    {
        std::string Out;
        for (const auto& A : Attributes)
        {
            if (!Out.empty()) Out += '\x1e';
            Out += A.m_Name; Out += '\x1f';
            std::visit([&](const auto& V)
            {
                using T = std::decay_t<decltype(V)>;
                if constexpr (std::is_same_v<T, bool>)             { Out += 'b'; Out += V ? '1' : '0'; }
                else if constexpr (std::is_same_v<T, std::int64_t>)  { Out += 'i'; Out += std::to_string(V); }
                else if constexpr (std::is_same_v<T, std::uint64_t>) { Out += 'u'; Out += std::to_string(V); }
                else if constexpr (std::is_same_v<T, double>)        { Out += 'd'; Out += std::format("{:.17g}", V); }
                else                                                 { Out += 's'; Out += V; }
            }, A.m_Value);
        }
        return Out;
    }

    inline std::vector<attribute> ParseAttributes(std::string_view Text)
    {
        std::vector<attribute> Out;
        if (Text.empty()) return Out;
        for (const auto& Item : Split(Text, '\x1e'))
        {
            const auto Sep = Item.find('\x1f');
            if (Sep == std::string::npos || Sep + 1 >= Item.size()) continue;
            attribute A; A.m_Name = Item.substr(0, Sep);
            const char Kind = Item[Sep + 1]; const std::string Value = Item.substr(Sep + 2);
            switch (Kind)
            {
            case 'b': A.m_Value = Value == "1"; break;
            case 'i': A.m_Value = static_cast<std::int64_t>(std::strtoll(Value.c_str(), nullptr, 10)); break;
            case 'u': A.m_Value = static_cast<std::uint64_t>(std::strtoull(Value.c_str(), nullptr, 10)); break;
            case 'd': A.m_Value = std::strtod(Value.c_str(), nullptr); break;
            default:  A.m_Value = Value;
            }
            Out.push_back(std::move(A));
        }
        return Out;
    }

    //------------------------------------------------------------------------------------------------------------------
    // Records <-> payloads, payloads <-> frames
    //------------------------------------------------------------------------------------------------------------------
    struct header_info { int m_Schema = schema_v; std::uint64_t m_Session = 0, m_StartWallMs = 0; std::uint32_t m_Pid = 0; std::string m_Exe; };

    inline std::string ClockOf(std::uint64_t Ns) { const std::uint64_t Ms = Ns / 1000000ull; return std::format("{:02}:{:02}.{:03}", Ms / 60000, (Ms / 1000) % 60, Ms % 1000); }

    inline std::uint32_t Fnv32(std::string_view Text) noexcept
    {
        std::uint32_t H = 2166136261u;
        for (unsigned char c : Text) { H ^= c; H *= 16777619u; }
        return H;
    }

    inline std::string EncodeHeader(const header_info& H)
    {
        std::string P = "H";
        Number(P, H.m_Schema); Field(P, Hex16(H.m_Session)); Number(P, H.m_StartWallMs); Number(P, H.m_Pid); Field(P, H.m_Exe);
        return P;
    }

    inline std::string EncodeClean(std::uint64_t WallMs) { std::string P = "Z"; Number(P, WallMs); return P; }

    inline std::string EncodeRecord(const record& R)
    {
        std::string P;
        if (const auto* pE = std::get_if<event>(&R))
        {
            const event& E = *pE;
            P = "E";
            Number(P, E.m_Key.m_Sequence); Number(P, E.m_ObservedAt); Field(P, E.m_Producer);
            Number(P, static_cast<int>(E.m_Origin.m_Type)); Field(P, E.m_Origin.m_Name); Number(P, E.m_Origin.m_Instance);
            Number(P, static_cast<int>(E.m_Severity)); Number(P, static_cast<int>(E.m_Kind)); Field(P, E.m_Channel); Field(P, E.m_Code);
            Number(P, E.m_Operation); Field(P, E.m_Discriminator); Number(P, (E.m_bHeuristic ? 1 : 0) | (E.m_bSummarized ? 2 : 0));
            Field(P, E.m_Title); Field(P, E.m_Body); Number(P, E.m_BodyLines); Field(P, RefText(E.m_Source));
            std::string Subjects;
            for (const auto& S : E.m_Subjects) { if (!Subjects.empty()) Subjects += '\x1e'; Subjects += RefText(S); }
            Field(P, Subjects); Field(P, AttributesText(E.m_Attributes)); Number(P, E.m_Cause.m_Sequence);
        }
        else if (const auto* pB = std::get_if<op_begin>(&R))
        {
            P = "B";
            Number(P, pB->m_Id); Number(P, pB->m_Parent); Number(P, pB->m_At); Field(P, pB->m_Kind); Field(P, pB->m_Title); Field(P, pB->m_Target);
            Number(P, static_cast<int>(pB->m_Origin.m_Type)); Field(P, pB->m_Origin.m_Name); Number(P, pB->m_Origin.m_Instance); Field(P, RefText(pB->m_Subject));
        }
        else if (const auto* pU = std::get_if<op_unit>(&R)) { P = "U"; Number(P, pU->m_Id); Field(P, pU->m_Unit); }
        else if (const auto* pC = std::get_if<op_coverage>(&R)) { P = "C"; Number(P, pC->m_Id); Number(P, static_cast<int>(pC->m_Coverage)); }
        else if (const auto* pX = std::get_if<op_end>(&R)) { P = "X"; Number(P, pX->m_Id); Number(P, pX->m_At); Number(P, static_cast<int>(pX->m_Outcome)); }
        return P;
    }

    inline std::string Frame(std::string_view Payload) { return std::format("R{:08X}{:08X} ", Payload.size(), Fnv32(Payload)).append(Payload).append("\n"); }

    // One decoded payload: a record, or the header, or the clean-shutdown marker
    struct decoded { char m_Tag = 0; record m_Record; header_info m_Header; std::uint64_t m_CleanWallMs = 0; };

    inline bool Decode(std::string_view Payload, decoded& Out)
    {
        if (Payload.empty()) return false;
        // fields are separated by raw tabs (escaped ones are \t): split first, unescape each
        std::vector<std::string> F;
        for (auto& Raw : Split(Payload, '\t')) F.push_back(Unescape(Raw));
        auto U = [&](std::size_t i) -> std::uint64_t { return i < F.size() ? std::strtoull(F[i].c_str(), nullptr, 10) : 0; };
        auto S = [&](std::size_t i) -> std::string { return i < F.size() ? F[i] : std::string(); };
        Out.m_Tag = Payload[0];
        switch (Out.m_Tag)
        {
        case 'H':
            if (F.size() < 6) return false;
            Out.m_Header.m_Schema = static_cast<int>(U(1)); Out.m_Header.m_Session = std::strtoull(F[2].c_str(), nullptr, 16); Out.m_Header.m_StartWallMs = U(3);
            Out.m_Header.m_Pid = static_cast<std::uint32_t>(U(4)); Out.m_Header.m_Exe = F[5];
            return true;
        case 'Z': Out.m_CleanWallMs = U(1); return true;
        case 'E':
        {
            if (F.size() < 21) return false;
            event E;
            E.m_Key = { 0, U(1) }; E.m_ObservedAt = U(2); E.m_Producer = F[3];
            E.m_Origin = { static_cast<origin::type>(U(4)), F[5], U(6) };
            E.m_Severity = static_cast<severity>(U(7)); E.m_Kind = static_cast<kind>(U(8)); E.m_Channel = F[9]; E.m_Code = F[10];
            E.m_Operation = U(11); E.m_Discriminator = F[12]; E.m_bHeuristic = (U(13) & 1) != 0; E.m_bSummarized = (U(13) & 2) != 0;
            E.m_Title = F[14]; E.m_Body = F[15]; E.m_BodyLines = static_cast<std::uint32_t>(U(16)); E.m_Source = ParseRef(F[17]);
            if (!F[18].empty()) for (const auto& R : Split(F[18], '\x1e')) E.m_Subjects.push_back(ParseRef(R));
            E.m_Attributes = ParseAttributes(F[19]); E.m_Cause = { 0, U(20) };
            Out.m_Record = std::move(E);
            return true;
        }
        case 'B':
        {
            if (F.size() < 11) return false;
            op_begin B; B.m_Id = U(1); B.m_Parent = U(2); B.m_At = U(3); B.m_Kind = F[4]; B.m_Title = F[5]; B.m_Target = F[6];
            B.m_Origin = { static_cast<origin::type>(U(7)), F[8], U(9) }; B.m_Subject = ParseRef(F[10]);
            Out.m_Record = std::move(B);
            return true;
        }
        case 'U': Out.m_Record = op_unit{ U(1), S(2) }; return true;
        case 'C': Out.m_Record = op_coverage{ U(1), static_cast<coverage_kind>(U(2)) }; return true;
        case 'X': Out.m_Record = op_end{ U(1), U(2), static_cast<outcome>(U(3)) }; return true;
        default:  return false;
        }
    }

    //------------------------------------------------------------------------------------------------------------------
    // Reading a stream: every complete record, and where the last complete one ends (a torn tail is what follows)
    //------------------------------------------------------------------------------------------------------------------
    struct read_result { std::uint64_t m_ValidBytes = 0, m_FileBytes = 0; bool m_bTorn = false; std::size_t m_Records = 0; };

    // Calls Visit(decoded) for every complete record. When bTruncate, a torn tail is cut off (the file ends at its last complete record).
    template<class F>
    inline read_result ReadStream(const std::filesystem::path& Path, F&& Visit, bool bTruncate)
    {
        read_result Result;
        std::error_code Ec;
        Result.m_FileBytes = std::filesystem::exists(Path, Ec) ? std::filesystem::file_size(Path, Ec) : 0;
        std::ifstream In(Path, std::ios::binary);
        if (!In) return Result;
        std::string Line;
        std::uint64_t Offset = 0;
        while (std::getline(In, Line))
        {
            const std::uint64_t LineBytes = Line.size() + (In.eof() ? 0 : 1);
            bool bOk = !In.eof() && Line.size() > 18 && Line[0] == 'R' && Line[17] == ' ';       // a final line without its line break was cut short
            std::string_view Payload;
            if (bOk)
            {
                const std::uint64_t Len = std::strtoull(Line.substr(1, 8).c_str(), nullptr, 16);
                const std::uint32_t Crc = static_cast<std::uint32_t>(std::strtoull(Line.substr(9, 8).c_str(), nullptr, 16));
                Payload = std::string_view(Line).substr(18);
                bOk = Payload.size() == Len && Fnv32(Payload) == Crc;
            }
            decoded D;
            if (!bOk || !Decode(Payload, D)) break;
            Offset += LineBytes;
            Result.m_ValidBytes = Offset;
            ++Result.m_Records;
            Visit(D);
        }
        Result.m_bTorn = Result.m_ValidBytes < Result.m_FileBytes;
        In.close();
        if (Result.m_bTorn && bTruncate) std::filesystem::resize_file(Path, Result.m_ValidBytes, Ec);
        return Result;
    }

    //------------------------------------------------------------------------------------------------------------------
    // The writer
    //------------------------------------------------------------------------------------------------------------------
    inline std::uint64_t WallMs() noexcept { return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()); }

    struct summary_counts { std::uint64_t m_Events = 0, m_Errors = 0, m_Warnings = 0, m_Operations = 0, m_Problems = 0; };

    class file_sink : public record_sink
    {
    public:
        file_sink(header_info Header, std::size_t QueueCap = 65536) : m_Header(std::move(Header)), m_Cap(QueueCap) { m_Thread = std::thread([this] { Run(); }); }
        ~file_sink() override { Close(true); }
        file_sink(const file_sink&) = delete;
        file_sink& operator=(const file_sink&) = delete;

        // Where the launch's records go: the Logs folder of the project. Until it is known, records wait (bounded) in the queue.
        void SetDirectory(std::filesystem::path Logs)
        {
            { std::lock_guard Lock(m_Mutex); m_Dir = std::move(Logs); m_bDirSet = true; }
            m_Wake.notify_one();
        }

        void Push(const record& R) noexcept override
        {
            std::lock_guard Lock(m_Mutex);
            if (m_bClosing) return;
            if (m_Queue.size() >= m_Cap) { ++m_Dropped; return; }
            const bool bWasEmpty = m_Queue.empty();
            m_Queue.push_back(R);
            if (bWasEmpty) m_Wake.notify_one();
        }

        sink_status Status() const noexcept override
        {
            std::lock_guard Lock(m_Mutex);
            return { m_Written, m_Queue.size(), m_Dropped, m_bFailed, m_Reason, m_StreamPath.string() };
        }

        // Ends the launch: what is queued is written, and (cleanly) the marker that says the launch ended on purpose. Idempotent.
        void Close(bool bClean) noexcept
        {
            { std::lock_guard Lock(m_Mutex); if (!m_bClosing) m_bCleanClose = bClean; m_bClosing = true; }
            m_Wake.notify_one();
            if (m_Thread.joinable()) m_Thread.join();
        }

        std::filesystem::path SessionDirectory() const { std::lock_guard Lock(m_Mutex); return m_StreamPath.parent_path(); }

    private:
        void Run() noexcept
        {
            std::ofstream Out;
            std::deque<record> Batch;
            summary_counts Counts;
            std::unordered_set<std::uint64_t> Problems;
            for (;;)
            {
                bool bClosing = false;
                {
                    std::unique_lock Lock(m_Mutex);
                    m_Wake.wait(Lock, [&] { return m_bClosing || (m_bDirSet && !m_Queue.empty()); });
                    bClosing = m_bClosing;
                    if (m_bDirSet) Batch.swap(m_Queue);
                }
                if (!Batch.empty() || (bClosing && m_bDirSet))
                {
                    if (!Out.is_open() && !Open(Out)) { std::lock_guard Lock(m_Mutex); m_Dropped += Batch.size(); Batch.clear(); }
                    std::string Text;
                    for (const auto& R : Batch)
                    {
                        Text += Frame(EncodeRecord(R));
                        if (const auto* pE = std::get_if<event>(&R))
                        {
                            ++Counts.m_Events;
                            if (pE->m_Severity >= severity::Error) ++Counts.m_Errors; else if (pE->m_Severity == severity::Warning) ++Counts.m_Warnings;
                            if (pE->m_Kind == kind::Diagnostic && pE->m_Severity >= severity::Warning) Problems.insert(ProblemId(*pE));
                        }
                        else if (std::holds_alternative<op_begin>(R)) ++Counts.m_Operations;
                    }
                    if (bClosing && m_bCleanClose && Out.is_open()) Text += Frame(EncodeClean(WallMs()));
                    if (Out.is_open())
                    {
                        Out.write(Text.data(), static_cast<std::streamsize>(Text.size()));
                        Out.flush();
                        std::lock_guard Lock(m_Mutex);
                        if (!Out) { m_bFailed = true; m_Reason = "writing the stream failed"; }
                        else m_Written += Batch.size();
                    }
                    Batch.clear();
                }
                if (bClosing)
                {
                    std::lock_guard Lock(m_Mutex);
                    if (m_Queue.empty() || !m_bDirSet) break;
                }
            }
            Counts.m_Problems = Problems.size();
            if (Out.is_open())
            {
                Out.close();
                if (m_bCleanClose) WriteSummary(Counts);
            }
        }

        bool Open(std::ofstream& Out) noexcept
        {
            std::error_code Ec;
            std::filesystem::path Path;
            { std::lock_guard Lock(m_Mutex); Path = m_Dir / Hex16(m_Header.m_Session) / stream_name_v; }
            std::filesystem::create_directories(Path.parent_path(), Ec);
            Out.open(Path, std::ios::binary | std::ios::app);
            std::lock_guard Lock(m_Mutex);
            if (!Out) { m_bFailed = true; m_Reason = "could not open " + Path.string(); return false; }
            m_StreamPath = Path;
            const std::string Head = Frame(EncodeHeader(m_Header));
            Out.write(Head.data(), static_cast<std::streamsize>(Head.size()));
            return true;
        }

        void WriteSummary(const summary_counts& C) noexcept
        {
            std::ofstream Out(m_StreamPath.parent_path() / "summary.txt");
            if (!Out) return;
            Out << "clean=1\nend=" << WallMs() << "\nevents=" << C.m_Events << "\nerrors=" << C.m_Errors << "\nwarnings=" << C.m_Warnings << "\noperations=" << C.m_Operations << "\nproblems=" << C.m_Problems << "\n";
        }

        header_info                 m_Header;
        std::size_t                 m_Cap;
        mutable std::mutex          m_Mutex;
        std::condition_variable     m_Wake;
        std::deque<record>          m_Queue;
        std::filesystem::path       m_Dir, m_StreamPath;
        bool                        m_bDirSet = false, m_bClosing = false, m_bCleanClose = true, m_bFailed = false;
        std::string                 m_Reason;
        std::uint64_t               m_Written = 0, m_Dropped = 0;
        std::thread                 m_Thread;
    };

    //------------------------------------------------------------------------------------------------------------------
    // The launches on disk: what is known of each, and how it ended
    //------------------------------------------------------------------------------------------------------------------
    enum class termination : std::uint8_t { Current, Clean, ConfirmedCrash, Interrupted, Unknown };
    inline constexpr const char* TerminationName(termination T) noexcept
    {
        constexpr const char* N[] = { "current", "clean", "confirmed-crash", "interrupted", "unknown" };
        return N[static_cast<int>(T)];
    }
    inline termination ParseTermination(std::string_view Text) noexcept
    {
        for (int i = 0; i < 5; ++i) if (Text == TerminationName(static_cast<termination>(i))) return static_cast<termination>(i);
        return termination::Unknown;
    }

    struct session_info
    {
        std::string             m_Id;                   // 16 hex digits
        std::filesystem::path   m_Dir;
        header_info             m_Header;
        bool                    m_bHeader = false;      // the stream's first record could be read
        bool                    m_bClean = false;       // the launch wrote its end marker
        std::uint64_t           m_EndWallMs = 0;
        termination             m_Termination = termination::Unknown;
        summary_counts          m_Counts;
        bool                    m_bTorn = false;        // its stream ended in a half-written record
        std::uint64_t           m_Bytes = 0;
        bool                    m_bPinned = false;
        std::size_t             m_Abandoned = 0;        // operations that were still running when it ended
        std::string             m_CrashText;            // the crash record found for it, when there is one
        std::vector<std::string> m_Tail;                // its last events, as a line each
    };

    inline std::filesystem::path StreamOf(const std::filesystem::path& SessionDir) { return SessionDir / stream_name_v; }

    // Reads a launch's stream completely: the header, how it ended, the counts, the last events, the operations that never ended. A torn tail is cut off when bTruncate.
    inline session_info ScanSession(const std::filesystem::path& Dir, bool bTruncate)
    {
        session_info Info;
        Info.m_Id = Dir.filename().string(); Info.m_Dir = Dir;
        std::error_code Ec;
        Info.m_bPinned = std::filesystem::exists(Dir / "pinned", Ec);
        std::unordered_set<std::uint64_t> Problems, Running;
        std::deque<std::string> Tail;
        const auto Result = ReadStream(StreamOf(Dir), [&](const decoded& D)
        {
            if (D.m_Tag == 'H') { Info.m_Header = D.m_Header; Info.m_bHeader = true; }
            else if (D.m_Tag == 'Z') { Info.m_bClean = true; Info.m_EndWallMs = D.m_CleanWallMs; }
            else if (const auto* pE = std::get_if<event>(&D.m_Record))
            {
                ++Info.m_Counts.m_Events;
                if (pE->m_Severity >= severity::Error) ++Info.m_Counts.m_Errors; else if (pE->m_Severity == severity::Warning) ++Info.m_Counts.m_Warnings;
                if (pE->m_Kind == kind::Diagnostic && pE->m_Severity >= severity::Warning) Problems.insert(ProblemId(*pE));
                Tail.push_back(std::format("{}  {}  {}  {}", ClockOf(pE->m_ObservedAt), SeverityName(pE->m_Severity), pE->m_Channel, pE->m_Title));
                if (Tail.size() > 8) Tail.pop_front();
            }
            else if (const auto* pB = std::get_if<op_begin>(&D.m_Record)) { ++Info.m_Counts.m_Operations; Running.insert(pB->m_Id); }
            else if (const auto* pX = std::get_if<op_end>(&D.m_Record)) Running.erase(pX->m_Id);
        }, bTruncate);
        Info.m_Counts.m_Problems = Problems.size();
        Info.m_bTorn = Result.m_bTorn;
        Info.m_Bytes = Result.m_FileBytes;
        Info.m_Abandoned = Info.m_bClean ? 0 : Running.size();
        Info.m_Tail.assign(Tail.begin(), Tail.end());
        if (Info.m_EndWallMs == 0) { auto T = std::filesystem::last_write_time(StreamOf(Dir), Ec); if (!Ec) Info.m_EndWallMs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()
            - std::chrono::duration_cast<std::chrono::milliseconds>(std::filesystem::file_time_type::clock::now() - T).count()); }
        Info.m_Termination = !Info.m_bHeader ? termination::Unknown : Info.m_bClean ? termination::Clean : termination::Interrupted;
        return Info;
    }

    inline void WriteSummary(const session_info& I)
    {
        std::ofstream Out(I.m_Dir / "summary.txt");
        if (!Out) return;
        std::string Crash;
        Escape(Crash, I.m_CrashText);
        Out << "clean=" << (I.m_bClean ? 1 : 0) << "\nend=" << I.m_EndWallMs << "\nevents=" << I.m_Counts.m_Events << "\nerrors=" << I.m_Counts.m_Errors << "\nwarnings=" << I.m_Counts.m_Warnings
            << "\noperations=" << I.m_Counts.m_Operations << "\nproblems=" << I.m_Counts.m_Problems << "\ntermination=" << TerminationName(I.m_Termination) << "\nabandoned=" << I.m_Abandoned
            << "\ntorn=" << (I.m_bTorn ? 1 : 0) << "\ncrash=" << Crash << "\n";
    }

    // The first record only (the launch's header)
    inline bool ReadHeader(const std::filesystem::path& Dir, header_info& Out)
    {
        std::ifstream In(StreamOf(Dir), std::ios::binary);
        std::string Line;
        if (!In || !std::getline(In, Line) || Line.size() < 19 || Line[0] != 'R') return false;
        decoded D;
        return Decode(std::string_view(Line).substr(18), D) && D.m_Tag == 'H' && (Out = D.m_Header, true);
    }

    // What is known of a launch without reading its whole stream when the summary says it (the summary is a projection: rebuilt from the stream when it is missing)
    inline session_info ReadInfo(const std::filesystem::path& Dir)
    {
        std::error_code Ec;
        const auto SummaryPath = Dir / "summary.txt";
        if (std::filesystem::exists(SummaryPath, Ec))
        {
            session_info Info;
            Info.m_Id = Dir.filename().string(); Info.m_Dir = Dir;
            Info.m_bHeader = ReadHeader(Dir, Info.m_Header);
            Info.m_Bytes = std::filesystem::exists(StreamOf(Dir), Ec) ? std::filesystem::file_size(StreamOf(Dir), Ec) : 0;
            Info.m_bPinned = std::filesystem::exists(Dir / "pinned", Ec);
            std::ifstream In(SummaryPath);
            std::string Line; bool bTermination = false;
            while (std::getline(In, Line))
            {
                const auto Eq = Line.find('=');
                if (Eq == std::string::npos) continue;
                const std::string Key = Line.substr(0, Eq), Value = Line.substr(Eq + 1);
                const auto N = std::strtoull(Value.c_str(), nullptr, 10);
                if (Key == "clean") Info.m_bClean = N != 0; else if (Key == "end") Info.m_EndWallMs = N; else if (Key == "events") Info.m_Counts.m_Events = N;
                else if (Key == "errors") Info.m_Counts.m_Errors = N; else if (Key == "warnings") Info.m_Counts.m_Warnings = N; else if (Key == "operations") Info.m_Counts.m_Operations = N;
                else if (Key == "problems") Info.m_Counts.m_Problems = N; else if (Key == "abandoned") Info.m_Abandoned = N; else if (Key == "torn") Info.m_bTorn = N != 0;
                else if (Key == "crash") Info.m_CrashText = Unescape(Value);
                else if (Key == "termination") { Info.m_Termination = ParseTermination(Value); bTermination = true; }
            }
            if (!bTermination) Info.m_Termination = Info.m_bClean ? termination::Clean : termination::Interrupted;
            return Info;
        }
        return ScanSession(Dir, false);
    }

    inline std::vector<std::filesystem::path> SessionDirectories(const std::filesystem::path& Logs)
    {
        std::vector<std::filesystem::path> Out;
        std::error_code Ec;
        if (!std::filesystem::exists(Logs, Ec)) return Out;
        for (const auto& Entry : std::filesystem::directory_iterator(Logs, Ec))
            if (Entry.is_directory(Ec) && std::filesystem::exists(StreamOf(Entry.path()), Ec)) Out.push_back(Entry.path());
        return Out;
    }

    // A launch read back from its stream into a hub of its own (never the live one): problems, operations and events as they were then
    inline std::unique_ptr<hub> LoadSession(const std::filesystem::path& Dir, session_info* pInfo = nullptr)
    {
        auto pHub = std::make_unique<hub>();
        pHub->release_current();
        header_info Header;
        ReadHeader(Dir, Header);
        pHub->SetSession(Header.m_Session);
        const session_info Info = ReadInfo(Dir);
        ReadStream(StreamOf(Dir), [&](const decoded& D) { if (D.m_Tag == 'E' || D.m_Tag == 'B' || D.m_Tag == 'U' || D.m_Tag == 'C' || D.m_Tag == 'X') { record R = D.m_Record; pHub->ReplayRecord(std::move(R)); } }, false);
        if (!Info.m_bClean) pHub->AbandonRunningOperations();
        if (pInfo) *pInfo = Info;
        return pHub;
    }

    //------------------------------------------------------------------------------------------------------------------
    // The crash record: what xeditor::diagnostics appended to LevelEditor.problems.log for a process ("[pid N t=<epoch seconds>] SEH exception ...")
    //------------------------------------------------------------------------------------------------------------------
    inline std::string FindCrashRecord(const std::filesystem::path& ProblemsLog, std::uint32_t Pid, std::uint64_t StartWallMs)
    {
        std::ifstream In(ProblemsLog);
        if (!In) return {};
        const std::string Prefix = std::format("[pid {} t=", Pid);
        std::string Line, Out;
        int After = 0;
        while (std::getline(In, Line))
        {
            if (Line.compare(0, Prefix.size(), Prefix) != 0) continue;
            const std::uint64_t T = std::strtoull(Line.c_str() + Prefix.size(), nullptr, 10) * 1000ull;
            if (T + 5000 < StartWallMs) continue;                                // a process of the same id from long before this launch
            const bool bProblem = Line.find("SEH exception") != std::string::npos || Line.find("CRT report") != std::string::npos || Line.find("terminate:") != std::string::npos;
            if (bProblem && Out.empty()) After = 7;
            if (After > 0) { Out += Line.substr(Line.find(']') == std::string::npos ? 0 : Line.find(']') + 2); Out += '\n'; --After; }
        }
        return Out;
    }

    //------------------------------------------------------------------------------------------------------------------
    // Importing the earlier launches (once each), retention
    //------------------------------------------------------------------------------------------------------------------
    struct import_options
    {
        std::filesystem::path   m_Logs;
        std::function<std::string(std::uint32_t Pid, std::uint64_t StartWallMs)> m_CrashRecord;       // the crash text for a process, or ""
        std::size_t             m_Keep = 20;                                     // launches kept (design 10.9: 20 sessions or 200 MB)
        std::uint64_t           m_MaxBytes = 200ull * 1024 * 1024;
        std::size_t             m_HistorySessions = 3;                           // earlier launches read for what was verified in them
        std::uint64_t           m_MaxHistoryBytes = 8ull * 1024 * 1024;
    };

    // Keeps the newest m_Keep launches and m_MaxBytes: a clean launch is evicted before one that ended badly, the pinned and the current one never.
    inline std::size_t Prune(const import_options& O, const std::string& CurrentId)
    {
        std::vector<session_info> All;
        for (const auto& Dir : SessionDirectories(O.m_Logs)) if (Dir.filename().string() != CurrentId) All.push_back(ReadInfo(Dir));
        std::uint64_t Total = 0;
        for (const auto& I : All) Total += I.m_Bytes;
        std::size_t Count = All.size() + 1;
        std::vector<const session_info*> Candidates;
        for (const auto& I : All) if (!I.m_bPinned) Candidates.push_back(&I);
        std::sort(Candidates.begin(), Candidates.end(), [](const session_info* A, const session_info* B)
        {
            const int RankA = A->m_Termination == termination::Clean ? 0 : 1, RankB = B->m_Termination == termination::Clean ? 0 : 1;
            return RankA != RankB ? RankA < RankB : A->m_Header.m_StartWallMs < B->m_Header.m_StartWallMs;
        });
        std::size_t Removed = 0;
        for (const session_info* I : Candidates)
        {
            if (Count <= O.m_Keep && Total <= O.m_MaxBytes) break;
            std::error_code Ec;
            std::filesystem::remove_all(I->m_Dir, Ec);
            if (!Ec) { --Count; Total -= std::min(Total, I->m_Bytes); ++Removed; }
        }
        return Removed;
    }

    // Reads the earlier launches: classifies how each ended, reports the ones that did not end on purpose (once: the checkpoint), learns what was verified in the last few, prunes.
    inline void RunImport(hub& Hub, const import_options& O, const std::string& CurrentId, const std::atomic<bool>& bStop)
    {
        Hub.SetImportState("running");
        std::error_code Ec;
        std::unordered_set<std::string> Done;
        {
            std::ifstream In(O.m_Logs / "imported.txt");
            std::string Line;
            while (std::getline(In, Line)) if (!Line.empty()) Done.insert(Line);
        }
        std::vector<session_info> Previous;
        for (const auto& Dir : SessionDirectories(O.m_Logs)) if (Dir.filename().string() != CurrentId) Previous.push_back(ReadInfo(Dir));
        std::sort(Previous.begin(), Previous.end(), [](const session_info& A, const session_info& B) { return A.m_Header.m_StartWallMs < B.m_Header.m_StartWallMs; });

        std::size_t Reported = 0;
        for (auto& I : Previous)
        {
            if (bStop.load()) return;
            if (I.m_bClean || Done.contains(I.m_Id)) continue;
            // how it ended is established from its stream and the crash records, never assumed
            I = ScanSession(I.m_Dir, /*bTruncate*/ true);
            if (I.m_bHeader && !I.m_bClean && O.m_CrashRecord) { I.m_CrashText = O.m_CrashRecord(I.m_Header.m_Pid, I.m_Header.m_StartWallMs); if (!I.m_CrashText.empty()) I.m_Termination = termination::ConfirmedCrash; }
            WriteSummary(I);

            if (I.m_Termination != termination::Clean)
            {
                const bool bCrash = I.m_Termination == termination::ConfirmedCrash;
                event E;
                E.m_Producer = "xlog.import"; E.m_Origin = { origin::type::System, "logs", 0 };
                E.m_Kind = kind::Diagnostic; E.m_Channel = "session.previous";
                E.m_Severity = bCrash ? severity::Error : severity::Warning;
                E.m_Code = bCrash ? "SESSION.CRASHED" : I.m_Termination == termination::Interrupted ? "SESSION.INTERRUPTED" : "SESSION.UNKNOWN_END";
                // the same kind of end is the same problem (its signature: the first line of the crash record, or the kind of end)
                E.m_Discriminator = bCrash ? I.m_CrashText.substr(0, I.m_CrashText.find('\n')) : std::string(TerminationName(I.m_Termination));
                std::string Text = bCrash ? "The previous launch ended in a confirmed crash" : I.m_Termination == termination::Interrupted ? "The previous launch ended without a clean shutdown" : "How the previous launch ended is unknown";
                Text += std::format("\nOriginal session={}  Observed during={}  Classification={}", I.m_Id, CurrentId, TerminationName(I.m_Termination));
                Text += std::format("\nStarted={}ms  Events={}  Errors={}  Operations abandoned={}{}", I.m_Header.m_StartWallMs, I.m_Counts.m_Events, I.m_Counts.m_Errors, I.m_Abandoned, I.m_bTorn ? "  The stream ended in a half-written record (cut off)" : "");
                if (bCrash) Text += "\nCrash record:\n" + I.m_CrashText;
                else Text += "\nNo crash record exists for it: a missing end marker alone does not say what happened.";
                if (!I.m_Tail.empty()) { Text += "\nLast events of that launch:"; for (const auto& L : I.m_Tail) Text += "\n  " + L; }
                SetMessage(E, Text);
                E.m_Attributes = { { "original_session", I.m_Id }, { "observed_during", CurrentId }, { "classification", std::string(TerminationName(I.m_Termination)) }, { "torn", I.m_bTorn }, { "abandoned_operations", static_cast<std::uint64_t>(I.m_Abandoned) } };
                Hub.Emit(std::move(E));
                ++Reported;
            }
            std::ofstream Checkpoint(O.m_Logs / "imported.txt", std::ios::app);
            Checkpoint << I.m_Id << "\n";
        }

        // What was verified in the last few launches: a problem that comes back after that is a regression across launches
        std::unordered_map<std::uint64_t, std::string> Verified;
        std::size_t Loaded = 0;
        std::vector<const session_info*> Recent;
        for (auto It = Previous.rbegin(); It != Previous.rend() && Recent.size() < O.m_HistorySessions; ++It) if (It->m_Bytes <= O.m_MaxHistoryBytes) Recent.push_back(&*It);
        for (auto It = Recent.rbegin(); It != Recent.rend(); ++It)                  // oldest first: a later launch overrides what an earlier one said
        {
            if (bStop.load()) return;
            auto pOld = LoadSession((*It)->m_Dir);
            for (auto Id : pOld->ProblemOrder())
                if (const problem* P = pOld->FindProblem(Id)) { if (P->m_Verification == verification::Verified) Verified[Id] = (*It)->m_Id; else Verified.erase(Id); }
            ++Loaded;
        }
        Hub.SetPriorVerified(std::move(Verified));

        const std::size_t Removed = Prune(O, CurrentId);
        Hub.SetImportState(std::format("done reported={} history={} pruned={}", Reported, Loaded, Removed));
    }

    // The launch's own persistence: the writer, and the import of the earlier launches on a worker thread
    class persistence : public file_sink
    {
    public:
        persistence(header_info Header, import_options Options) : file_sink(std::move(Header)), m_Options(std::move(Options)) {}
        ~persistence() override { m_bStop = true; if (m_Import.joinable()) m_Import.join(); }
        void StartImport(hub& Hub, const std::string& CurrentId) { m_Import = std::thread([this, &Hub, CurrentId] { RunImport(Hub, m_Options, CurrentId, m_bStop); }); }
    private:
        import_options      m_Options;
        std::atomic<bool>   m_bStop{ false };
        std::thread         m_Import;
    };

    //------------------------------------------------------------------------------------------------------------------
    // What the person decided (design 5.2, 6.6): acknowledgements, mutes and saved views, in a file of their own that outlives the launch. Lines, tab separated:
    //   A <problem id> <acknowledged 0|1> <muted 0|1> <label>            V <name> <page> <state> <show muted 0|1> <query>
    // The team's views live in a second file of the project that goes into source control; the person's own file wins a name clash. Every change saves (the file is small
    // and the changes are clicks); a problem's id is its identity, so a decision made in one launch applies to the same problem in the next one.
    //------------------------------------------------------------------------------------------------------------------
    struct user_files { std::filesystem::path m_Personal, m_Team; };

    inline bool WriteWhole(const std::filesystem::path& Path, const std::string& Text)
    {
        std::error_code Ec;
        std::filesystem::create_directories(Path.parent_path(), Ec);
        const auto Temp = std::filesystem::path(Path).concat(".tmp");
        {
            std::ofstream Out(Temp, std::ios::binary | std::ios::trunc);
            if (!Out) return false;
            Out << Text;
            if (!Out.good()) return false;
        }
        std::filesystem::rename(Temp, Path, Ec);                              // replaces: a crash in the middle leaves the old file whole
        return !Ec;
    }

    inline bool SaveUserState(const hub& Hub, const user_files& Files)
    {
        std::string Personal = "# xlog: what you decided about problems, and the views you keep (one line each; safe to edit)\n", Team = "# xlog: the team's saved views of the Logs (shared through source control)\n";
        std::vector<std::uint64_t> Ids;
        for (const auto& [Id, A] : Hub.Annotations()) if (A.m_bAcknowledged || A.m_bMuted) Ids.push_back(Id);
        std::sort(Ids.begin(), Ids.end());
        for (const auto Id : Ids)
        {
            const auto& A = Hub.Annotations().at(Id);
            std::string Line;
            Field(Line, "A"); Field(Line, Hex16(Id)); Field(Line, A.m_bAcknowledged ? "1" : "0"); Field(Line, A.m_bMuted ? "1" : "0"); Field(Line, A.m_Label);
            Personal += Line + '\n';
        }
        bool bAnyTeam = false;
        for (const auto& V : Hub.SavedViews())
        {
            std::string Line;
            Field(Line, "V"); Field(Line, V.m_Name); Number(Line, V.m_Page); Number(Line, static_cast<int>(V.m_State)); Field(Line, V.m_bShowMuted ? "1" : "0"); Field(Line, V.m_Query);
            (V.m_bTeam ? Team : Personal) += Line + '\n';
            bAnyTeam |= V.m_bTeam;
        }
        bool bOk = Files.m_Personal.empty() || WriteWhole(Files.m_Personal, Personal);
        if (!Files.m_Team.empty() && (bAnyTeam || std::filesystem::exists(Files.m_Team))) bOk = WriteWhole(Files.m_Team, Team) && bOk;
        return bOk;
    }

    inline void ReadUserFile(const std::filesystem::path& Path, bool bTeam, std::unordered_map<std::uint64_t, annotation>& Annotations, std::vector<saved_view>& Views)
    {
        std::ifstream In(Path, std::ios::binary);
        for (std::string Line; std::getline(In, Line);)
        {
            if (!Line.empty() && Line.back() == '') Line.pop_back();
            if (Line.empty() || Line[0] == '#') continue;
            std::vector<std::string> F;
            for (auto& Raw : Split(Line, '	')) F.push_back(Unescape(Raw));
            if (F[0] == "A" && F.size() >= 5 && !bTeam)
            {
                annotation A; A.m_bAcknowledged = F[2] == "1"; A.m_bMuted = F[3] == "1"; A.m_Label = F[4];
                Annotations[std::strtoull(F[1].c_str(), nullptr, 16)] = std::move(A);
            }
            else if (F[0] == "V" && F.size() >= 6)
            {
                saved_view V; V.m_Name = F[1]; V.m_Page = std::atoi(F[2].c_str()); V.m_State = static_cast<std::uint8_t>(std::clamp(std::atoi(F[3].c_str()), 0, 2)); V.m_bShowMuted = F[4] == "1"; V.m_Query = F[5]; V.m_bTeam = bTeam;
                std::erase_if(Views, [&](const saved_view& Old) { return Old.m_Name == V.m_Name; });        // the later file (the person's) wins
                Views.push_back(std::move(V));
            }
        }
    }

    // Loads the person's decisions into the hub and keeps the files up to date from then on.
    inline void StartUserState(hub& Hub, user_files Files)
    {
        std::unordered_map<std::uint64_t, annotation> Annotations; std::vector<saved_view> Views;
        if (!Files.m_Team.empty()) ReadUserFile(Files.m_Team, true, Annotations, Views);
        if (!Files.m_Personal.empty()) ReadUserFile(Files.m_Personal, false, Annotations, Views);
        Hub.LoadUser(std::move(Annotations), std::move(Views));
        Hub.SetOnUserChange([pHub = &Hub, Files = std::move(Files)] { if (SaveUserState(*pHub, Files)) return; event E; E.m_Producer = "xlog.store"; E.m_Origin = { origin::type::System, "logs", 0 }; E.m_Severity = severity::Warning; E.m_Kind = kind::Diagnostic; E.m_Channel = "logs.health"; E.m_Code = "LOGS.USER_STATE_NOT_SAVED"; SetMessage(E, "The Logs could not save your decisions and views (acknowledged, muted, saved views)."); pHub->Emit(std::move(E)); });
    }

    //------------------------------------------------------------------------------------------------------------------
    // Attachments: a file copied into <Logs>/<session>/attachments/, listed in attachments.txt next to the stream (id, event, operation, bytes, name). The folder goes with the
    // session (retention removes it with it). A file over 8 MB or past 64 MB for the launch is refused: the Logs are not a place to keep captures.
    //------------------------------------------------------------------------------------------------------------------
    inline constexpr std::uint64_t attachment_max_v = 8ull << 20, attachments_max_v = 64ull << 20;

    inline std::string SafeFileName(std::string Name)
    {
        for (char& c : Name) if (std::string_view("\\/:*?\"<>|\t\r\n").find(c) != std::string_view::npos) c = '_';
        if (Name.empty() || Name == "." || Name == "..") Name = "attachment";
        if (Name.size() > 80) Name.resize(80);
        return Name;
    }

    // False with the reason (Why) when it was not kept.
    inline bool Attach(hub& Hub, const std::filesystem::path& Source, std::uint64_t Event, std::uint64_t Operation, std::string Name, std::string& Why)
    {
        std::error_code Ec;
        if (Hub.PersistenceDirectory().empty()) { Why = "the launch is not being kept on disk, so there is nowhere to put an attachment"; return false; }
        if (Event ? !Hub.FindEvent(Event) : (Operation ? !Hub.FindOperation(Operation) : true)) { Why = Event || Operation ? "there is no such event or operation" : "an attachment belongs to an event (-Event) or an operation (-Operation)"; return false; }
        if (!std::filesystem::is_regular_file(Source, Ec)) { Why = "the file does not exist"; return false; }
        const std::uint64_t Bytes = std::filesystem::file_size(Source, Ec);
        if (Ec || Bytes > attachment_max_v) { Why = std::format("the file is larger than {} MB", attachment_max_v >> 20); return false; }
        if (Hub.AttachmentBytes() + Bytes > attachments_max_v) { Why = std::format("the launch already holds {} MB of attachments", Hub.AttachmentBytes() >> 20); return false; }
        if (Name.empty()) Name = Source.filename().string();
        Name = SafeFileName(std::move(Name));
        const auto Session = std::filesystem::path(Hub.PersistenceDirectory()) / Hex16(Hub.Session());
        const auto Dir = Session / "attachments";
        std::filesystem::create_directories(Dir, Ec);
        const std::uint64_t Id = Hub.Attachments().size() + 1;
        const auto Stored = Dir / std::format("{}-{}", Id, Name);
        std::filesystem::copy_file(Source, Stored, std::filesystem::copy_options::overwrite_existing, Ec);
        if (Ec) { Why = "the file could not be copied: " + Ec.message(); return false; }
        attachment A; A.m_Event = Event; A.m_Operation = Event ? 0 : Operation; A.m_Bytes = Bytes; A.m_Name = Name; A.m_Path = Stored.string();
        Hub.AddAttachment(A);
        std::string Line;
        Field(Line, "T"); Number(Line, Id); Number(Line, A.m_Event); Number(Line, A.m_Operation); Number(Line, Bytes); Field(Line, Name);
        std::ofstream(Session / "attachments.txt", std::ios::binary | std::ios::app) << Line << '\n';
        return true;
    }

    // Starts keeping the launch: records go to <Options.m_Logs>/<session id>/stream.xlog, and the earlier launches are imported in the background. The hub must outlive the call's sink
    // (it does: the hub owns it).
    inline void StartPersistence(hub& Hub, import_options Options, std::string Exe = {})
    {
        header_info Header;
        Header.m_Session = Hub.Session(); Header.m_StartWallMs = Hub.StartWallMs(); Header.m_Exe = std::move(Exe);
#if defined(_WIN32)
        Header.m_Pid = static_cast<std::uint32_t>(_getpid());
#endif
        const std::filesystem::path Logs = Options.m_Logs;
        auto pSink = std::make_shared<persistence>(Header, std::move(Options));
        pSink->SetDirectory(Logs);
        Hub.AttachSink(pSink);
        Hub.SetPersistenceInfo(Logs.string());
        pSink->StartImport(Hub, Hex16(Hub.Session()));
    }
}

#endif // XLOG_STORE_H
