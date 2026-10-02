#ifndef XLOG_STDOUT_H
#define XLOG_STDOUT_H
#pragma once

// The stdout tap (documentation/Editors/DESIGN_logs.md, 9.2): what code that predates the Logs prints with printf and fprintf(stderr) becomes events, so it is in the same list as everything
// else. It is the legacy-grade path and is labelled so: a line carries no code and no subject, its severity is a guess (stderr is a warning, a line that says error/fatal/assert is an
// error), and the problem it forms is identified by the line's template (digits and paths do not make a new problem). Opt-in (LogStdout -On true), because it replaces the process'
// stdout and stderr with pipes. Everything read is written on to the original stdout and stderr, so the console, a redirect to a file and the smoke tests' log still see every byte.
// Headless-safe: no ImGui, no editor.

#include "xlog_hub.h"

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>

#if defined(_WIN32)
    #include <fcntl.h>
    #include <io.h>
#endif

namespace xlog
{
    class stdout_tap
    {
    public:
        stdout_tap() noexcept = default;
        ~stdout_tap() { Stop(); }
        stdout_tap(const stdout_tap&) = delete;
        stdout_tap& operator=(const stdout_tap&) = delete;

        bool Running() const noexcept { return m_bRunning; }
        std::uint64_t Lines() const noexcept { return m_Lines.load(std::memory_order_relaxed); }

        // False (and the reason) when the process' streams cannot be taken over.
        bool Start(hub& Hub, std::string& Why) noexcept
        {
            if (m_bRunning) return true;
#if defined(_WIN32)
            std::fflush(stdout); std::fflush(stderr);
            for (int i = 0; i < 2; ++i)
            {
                m_Saved[i] = _dup(i + 1);
                int Fds[2] = { -1, -1 };
                if (m_Saved[i] < 0 || _pipe(Fds, 1 << 16, _O_BINARY) != 0) { Why = "the process' streams cannot be duplicated"; Undo(i); return false; }
                m_Read[i] = Fds[0]; m_Write[i] = Fds[1];
                if (_dup2(Fds[1], i + 1) != 0) { Why = "the process' streams cannot be replaced"; Undo(i + 1); return false; }
            }
            setvbuf(stdout, nullptr, _IONBF, 0);                   // a printf reaches the pipe when it is called, not when a buffer fills
            m_pHub = &Hub;
            m_bRunning = true;
            for (int i = 0; i < 2; ++i) m_Thread[i] = std::thread([this, i] { Read(i); });
            return true;
#else
            Why = "the stdout tap is only built for Windows";
            return false;
#endif
        }

        void Stop() noexcept
        {
#if defined(_WIN32)
            if (!m_bRunning) return;
            std::fflush(stdout); std::fflush(stderr);
            for (int i = 0; i < 2; ++i) _dup2(m_Saved[i], i + 1);     // the originals are back: the pipes' write ends at 1 and 2 are closed by this
            for (int i = 0; i < 2; ++i) { _close(m_Write[i]); m_Write[i] = -1; }
            for (int i = 0; i < 2; ++i) if (m_Thread[i].joinable()) m_Thread[i].join();   // they read the end of the pipe and finish
            for (int i = 0; i < 2; ++i) { _close(m_Read[i]); _close(m_Saved[i]); m_Read[i] = m_Saved[i] = -1; }
            m_bRunning = false;
#endif
        }

    private:
#if defined(_WIN32)
        void Undo(int Count) noexcept        // a failed start puts back what it took
        {
            for (int i = 0; i < Count && i < 2; ++i) { if (m_Saved[i] >= 0) _dup2(m_Saved[i], i + 1); }
            for (int i = 0; i < 2; ++i)
            {
                if (m_Write[i] >= 0) _close(m_Write[i]);
                if (m_Read[i] >= 0)  _close(m_Read[i]);
                if (m_Saved[i] >= 0) _close(m_Saved[i]);
                m_Write[i] = m_Read[i] = m_Saved[i] = -1;
            }
        }

        static severity Guess(bool bStderr, std::string_view Line) noexcept
        {
            using details::ContainsNoCase;
            if (ContainsNoCase(Line, "fatal") || ContainsNoCase(Line, "assert") || ContainsNoCase(Line, "error")) return severity::Error;
            if (ContainsNoCase(Line, "warn")) return severity::Warning;
            return bStderr ? severity::Warning : severity::Info;
        }

        void Read(int Stream) noexcept
        {
            std::string Line;
            char Buffer[4096];
            for (;;)
            {
                const int N = _read(m_Read[Stream], Buffer, sizeof(Buffer));
                if (N <= 0) break;
                _write(m_Saved[Stream], Buffer, static_cast<unsigned>(N));                           // on to where it was going
                for (int i = 0; i < N; ++i)
                {
                    if (Buffer[i] == '\n') { Emit(Stream, Line); Line.clear(); }
                    else if (Buffer[i] != '\r' && Line.size() < 8192) Line += Buffer[i];
                }
            }
            if (!Line.empty()) Emit(Stream, Line);
        }

        void Emit(int Stream, const std::string& Line) noexcept
        {
            if (Line.empty() || !m_pHub) return;
            event E;
            const bool bErr = Stream == 1;
            E.m_Producer = bErr ? "process.stderr" : "process.stdout";
            E.m_Origin = { origin::type::System, "process", 0 };
            E.m_Severity = Guess(bErr, Line); E.m_Kind = kind::Log; E.m_Channel = bErr ? "process.stderr" : "process.stdout";
            E.m_bHeuristic = true;                                                                    // no code, no subject: its identity is the shape of the line
            SetMessage(E, Line);
            m_pHub->Emit(std::move(E));
            m_Lines.fetch_add(1, std::memory_order_relaxed);
        }

        int                  m_Saved[2] = { -1, -1 }, m_Read[2] = { -1, -1 }, m_Write[2] = { -1, -1 };
#endif
        std::thread          m_Thread[2];
        hub*                 m_pHub = nullptr;
        bool                 m_bRunning = false;
        std::atomic<std::uint64_t> m_Lines{ 0 };
    };
}

#endif // XLOG_STDOUT_H
