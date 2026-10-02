#ifndef XLOG_REMOTE_H
#define XLOG_REMOTE_H
#pragma once

// Remote runtimes (documentation/Editors/DESIGN_logs.md, 9.4): a game or a server running as a process of its own speaks into the Logs of the editor that started it. It uses the
// stream format of xlog_store.h as it is - the same framed records, the same decoders - over a named pipe: the runtime's hub gets a client_sink, the editor listens with a
// remote::server. The records arrive as events and operations of an origin of their own ("remote:<exe>", a System), with the editor's clock (their time is when they arrived: two
// processes' clocks are not comparable) and the editor's operation numbers; an operation whose process went away before it ended is Abandoned, as for any launch that ended in the dark.
// The editor writes the pipe's name to <Logs>/remote.txt so a runtime launched from it can find it. Windows only for now; elsewhere both ends are inert.

#include "xlog_store.h"

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#endif

namespace xlog::remote
{
    using namespace xlog::store;

    inline constexpr std::size_t max_frame_v = 1u << 20;

    //------------------------------------------------------------------------------------------------------------------
    // The runtime's side: a sink that writes every record of its hub to the editor's pipe
    //------------------------------------------------------------------------------------------------------------------
    class client_sink : public record_sink
    {
    public:
        client_sink(std::string Pipe, header_info Header, std::size_t Cap = 32768) : m_Pipe(std::move(Pipe)), m_Header(std::move(Header)), m_Cap(Cap) { m_Thread = std::thread([this] { Run(); }); }
        ~client_sink() override { { std::lock_guard Lock(m_Mutex); m_bClosing = true; } m_Wake.notify_one(); if (m_Thread.joinable()) m_Thread.join(); }

        void Push(const record& R) noexcept override
        {
            std::lock_guard Lock(m_Mutex);
            if (m_bClosing) return;
            if (m_Queue.size() >= m_Cap) { ++m_Dropped; return; }           // an editor that does not listen must never stall the game
            m_Queue.push_back(Frame(EncodeRecord(R)));
            m_Wake.notify_one();
        }
        sink_status Status() const noexcept override
        {
            std::lock_guard Lock(m_Mutex);
            return { m_Written, m_Queue.size(), m_Dropped, false, m_bConnected ? std::string() : std::string("not connected"), m_Pipe };
        }
        bool Connected() const noexcept { std::lock_guard Lock(m_Mutex); return m_bConnected; }

    private:
        void Run() noexcept
        {
#if defined(_WIN32)
            HANDLE hPipe = INVALID_HANDLE_VALUE;
            std::deque<std::string> Batch;
            for (;;)
            {
                {
                    std::unique_lock Lock(m_Mutex);
                    m_Wake.wait_for(Lock, std::chrono::milliseconds(hPipe == INVALID_HANDLE_VALUE ? 500 : 5000), [&] { return m_bClosing || !m_Queue.empty(); });
                    if (m_bClosing && m_Queue.empty()) break;
                    if (hPipe != INVALID_HANDLE_VALUE) Batch.swap(m_Queue);
                }
                if (hPipe == INVALID_HANDLE_VALUE)
                {
                    hPipe = CreateFileA(m_Pipe.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
                    if (hPipe == INVALID_HANDLE_VALUE) { if (m_bClosing) break; continue; }           // nobody listens (yet): the records wait, bounded
                    const std::string Hello = Frame(EncodeHeader(m_Header));
                    DWORD Done = 0;
                    if (!WriteFile(hPipe, Hello.data(), static_cast<DWORD>(Hello.size()), &Done, nullptr)) { CloseHandle(hPipe); hPipe = INVALID_HANDLE_VALUE; continue; }
                    { std::lock_guard Lock(m_Mutex); m_bConnected = true; Batch.swap(m_Queue); }
                }
                bool bBroken = false;
                for (const auto& F : Batch)
                {
                    DWORD Done = 0;
                    if (!WriteFile(hPipe, F.data(), static_cast<DWORD>(F.size()), &Done, nullptr)) { bBroken = true; break; }
                    std::lock_guard Lock(m_Mutex); ++m_Written;
                }
                Batch.clear();
                if (bBroken) { CloseHandle(hPipe); hPipe = INVALID_HANDLE_VALUE; std::lock_guard Lock(m_Mutex); m_bConnected = false; }
            }
            if (hPipe != INVALID_HANDLE_VALUE) CloseHandle(hPipe);
#endif
        }

        std::string                 m_Pipe;
        header_info                 m_Header;
        std::size_t                 m_Cap;
        mutable std::mutex          m_Mutex;
        std::condition_variable     m_Wake;
        std::deque<std::string>     m_Queue;
        std::uint64_t               m_Written = 0, m_Dropped = 0;
        bool                        m_bClosing = false, m_bConnected = false;
        std::thread                 m_Thread;
    };

    // Two sinks behind one hub (the launch's own file and the editor's pipe).
    class tee_sink : public record_sink
    {
    public:
        tee_sink(std::shared_ptr<record_sink> A, std::shared_ptr<record_sink> B) : m_A(std::move(A)), m_B(std::move(B)) {}
        void Push(const record& R) noexcept override { if (m_A) m_A->Push(R); if (m_B) m_B->Push(R); }
        sink_status Status() const noexcept override { return m_A ? m_A->Status() : m_B->Status(); }
    private:
        std::shared_ptr<record_sink> m_A, m_B;
    };

    // Starts speaking into an editor's Logs: Pipe is the name the editor published (its remote.txt), or a name of your own for a pipe you made.
    inline void Connect(hub& Hub, std::string Pipe, std::string Exe)
    {
        header_info Header;
        Header.m_Session = Hub.Session(); Header.m_StartWallMs = WallMs(); Header.m_Exe = std::move(Exe);
#if defined(_WIN32)
        Header.m_Pid = static_cast<std::uint32_t>(GetCurrentProcessId());
#endif
        auto pClient = std::make_shared<client_sink>(std::move(Pipe), Header);
        Hub.AttachSink(Hub.Sink() ? std::static_pointer_cast<record_sink>(std::make_shared<tee_sink>(Hub.Sink(), pClient)) : std::static_pointer_cast<record_sink>(pClient));
    }

    // The pipe name an editor of this project published, "" if none.
    inline std::string Discover(const std::filesystem::path& Logs)
    {
        std::ifstream In(Logs / "remote.txt");
        std::string Name;
        std::getline(In, Name);
        while (!Name.empty() && (Name.back() == '\r' || Name.back() == ' ')) Name.pop_back();
        return Name;
    }

    //------------------------------------------------------------------------------------------------------------------
    // The editor's side: listens, and turns what arrives into events and operations of an origin of its own
    //------------------------------------------------------------------------------------------------------------------
    class server
    {
    public:
        server() noexcept = default;
        ~server() { Stop(); }
        server(const server&) = delete;
        server& operator=(const server&) = delete;

        // Listens on \\.\pipe\<Name>. False (and the reason) when the pipe cannot be created.
        bool Start(hub& Hub, const std::string& Name, std::string& Why) noexcept
        {
#if defined(_WIN32)
            if (m_bRunning) return true;
            m_Pipe = "\\\\.\\pipe\\" + Name;
            m_pHub = &Hub;
            m_bStop = false;
            HANDLE hFirst = Create(true);
            if (hFirst == INVALID_HANDLE_VALUE) { Why = std::format("the pipe {} cannot be created (error {})", m_Pipe, GetLastError()); return false; }
            m_bRunning = true;
            m_Accept = std::thread([this, hFirst] { Accept(hFirst); });
            return true;
#else
            (void)Hub; (void)Name; Why = "remote runtimes are only built for Windows"; return false;
#endif
        }

        void Stop() noexcept
        {
#if defined(_WIN32)
            if (!m_bRunning) return;
            m_bStop = true;
            HANDLE hWake = CreateFileA(m_Pipe.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);      // the accept thread is waiting for a client: this is one
            if (hWake != INVALID_HANDLE_VALUE) CloseHandle(hWake);
            if (m_Accept.joinable()) m_Accept.join();
            std::vector<std::thread> Workers;
            { std::lock_guard Lock(m_Mutex); for (HANDLE h : m_Open) CancelIoEx(h, nullptr); Workers.swap(m_Workers); }
            for (auto& W : Workers) if (W.joinable()) W.join();
            m_bRunning = false;
#endif
        }

        bool Running() const noexcept { return m_bRunning; }
        const std::string& Pipe() const noexcept { return m_Pipe; }
        std::uint64_t Connections() const noexcept { return m_Connections; }
        std::uint64_t Records() const noexcept { return m_Records; }
        std::uint64_t Rejected() const noexcept { return m_Rejected; }
        std::size_t Connected() const noexcept { std::lock_guard Lock(m_Mutex); return m_Open.size(); }

    private:
#if defined(_WIN32)
        HANDLE Create(bool bFirst) noexcept
        {
            return CreateNamedPipeA(m_Pipe.c_str(), PIPE_ACCESS_INBOUND | (bFirst ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0), PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS
                , PIPE_UNLIMITED_INSTANCES, 1 << 16, 1 << 16, 0, nullptr);
        }

        void Accept(HANDLE hPipe) noexcept
        {
            while (!m_bStop)
            {
                const bool bConnected = ConnectNamedPipe(hPipe, nullptr) ? true : GetLastError() == ERROR_PIPE_CONNECTED;
                if (m_bStop || !bConnected) { CloseHandle(hPipe); hPipe = INVALID_HANDLE_VALUE; if (m_bStop) return; }
                else
                {
                    ++m_Connections;
                    { std::lock_guard Lock(m_Mutex); m_Open.push_back(hPipe); m_Workers.emplace_back([this, hPipe] { Serve(hPipe); }); }
                    hPipe = INVALID_HANDLE_VALUE;
                }
                hPipe = Create(false);
                if (hPipe == INVALID_HANDLE_VALUE) return;
            }
            if (hPipe != INVALID_HANDLE_VALUE) CloseHandle(hPipe);
        }

        // One runtime: its frames in order, until it goes away
        void Serve(HANDLE hPipe) noexcept
        {
            std::string Buffer, Name = "runtime";
            std::unordered_map<std::uint64_t, op_handle> Operations;          // the runtime's operation id -> the handle of the editor's copy of it
            std::unordered_map<std::uint64_t, std::uint64_t> Local;           // and the editor's id of it: what events that belong to it are rewritten to
            std::uint32_t Pid = 0;
            char Chunk[8192];
            for (;;)
            {
                DWORD Got = 0;
                if (!ReadFile(hPipe, Chunk, sizeof(Chunk), &Got, nullptr) || Got == 0) break;
                Buffer.append(Chunk, Got);
                std::size_t At = 0;
                for (;;)
                {
                    const auto Eol = Buffer.find('\n', At);
                    if (Eol == std::string::npos) break;
                    Handle(std::string_view(Buffer).substr(At, Eol - At), Name, Pid, Operations, Local);
                    At = Eol + 1;
                }
                Buffer.erase(0, At);
                if (Buffer.size() > max_frame_v) { ++m_Rejected; break; }               // not a stream of ours: it is not read further
            }
            Operations.clear();                                                           // whatever the runtime left running is recorded Abandoned
            std::lock_guard Lock(m_Mutex);
            m_Open.erase(std::remove(m_Open.begin(), m_Open.end(), hPipe), m_Open.end());
            CloseHandle(hPipe);
        }

        void Handle(std::string_view Frame, std::string& Name, std::uint32_t& Pid, std::unordered_map<std::uint64_t, op_handle>& Operations, std::unordered_map<std::uint64_t, std::uint64_t>& Local) noexcept
        {
            // R<len 8 hex><fnv 8 hex> <payload>
            if (Frame.size() < 19 || Frame[0] != 'R' || Frame[17] != ' ') { ++m_Rejected; return; }
            const std::size_t Length = std::strtoull(std::string(Frame.substr(1, 8)).c_str(), nullptr, 16);
            const std::uint32_t Sum = static_cast<std::uint32_t>(std::strtoull(std::string(Frame.substr(9, 8)).c_str(), nullptr, 16));
            const std::string_view Payload = Frame.substr(18);
            if (Payload.size() != Length || Fnv32(Payload) != Sum) { ++m_Rejected; return; }
            decoded D;
            if (!Decode(Payload, D)) { ++m_Rejected; return; }
            ++m_Records;
            const auto Mine = [&](origin O) { O.m_Type = origin::type::System; O.m_Name = "remote:" + Name; O.m_Instance = Pid; return O; };
            if (D.m_Tag == 'H')
            {
                Pid = D.m_Header.m_Pid;
                const auto Slash = D.m_Header.m_Exe.find_last_of("\\/");
                std::string Stem = Slash == std::string::npos ? D.m_Header.m_Exe : D.m_Header.m_Exe.substr(Slash + 1);
                if (const auto Dot = Stem.rfind('.'); Dot != std::string::npos && Dot > 0) Stem.resize(Dot);
                if (!Stem.empty()) Name = Stem;
                return;
            }
            if (auto* pE = std::get_if<event>(&D.m_Record))
            {
                event E = std::move(*pE);
                E.m_Key = {}; E.m_Cause = {};
                E.m_Origin = Mine(E.m_Origin);
                if (E.m_Operation) { const auto It = Local.find(E.m_Operation); E.m_Operation = It == Local.end() ? 0 : It->second; }
                m_pHub->Emit(std::move(E));
            }
            else if (auto* pB = std::get_if<op_begin>(&D.m_Record))
            {
                const std::uint64_t Parent = pB->m_Parent && Local.contains(pB->m_Parent) ? Local[pB->m_Parent] : 0;
                op_handle H = m_pHub->Begin(pB->m_Kind, Mine(pB->m_Origin), pB->m_Subject, pB->m_Title, pB->m_Target, Parent);
                Local[pB->m_Id] = H.Id();
                Operations.erase(pB->m_Id);
                Operations.emplace(pB->m_Id, std::move(H));
            }
            else if (auto* pU = std::get_if<op_unit>(&D.m_Record)) { if (auto It = Operations.find(pU->m_Id); It != Operations.end()) It->second.Unit(pU->m_Unit); }
            else if (auto* pC = std::get_if<op_coverage>(&D.m_Record)) { if (auto It = Operations.find(pC->m_Id); It != Operations.end()) It->second.SetCoverage(pC->m_Coverage); }
            else if (auto* pX = std::get_if<op_end>(&D.m_Record)) { if (auto It = Operations.find(pX->m_Id); It != Operations.end()) { It->second.End(pX->m_Outcome); Operations.erase(It); } }
        }

        std::string                 m_Pipe;
        hub*                        m_pHub = nullptr;
        std::atomic<bool>           m_bStop{ false };
        bool                        m_bRunning = false;
        std::thread                 m_Accept;
        mutable std::mutex          m_Mutex;
        std::vector<std::thread>    m_Workers;
        std::vector<HANDLE>         m_Open;
        std::atomic<std::uint64_t>  m_Connections{ 0 }, m_Records{ 0 }, m_Rejected{ 0 };
#endif
    };
}

#endif // XLOG_REMOTE_H
