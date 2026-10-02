#ifndef XLOG_BUILD_H
#define XLOG_BUILD_H
#pragma once

// The build adapter: MSBuild / cl / link / CMake output, line by line, into structured diagnostics (documentation/Editors/DESIGN_logs.md, section 2
// "the build is the first producer to get right").
//
//   D:\proj\a.h(80,12): error C2065: 'Kick': undeclared identifier [D:\proj\Game.vcxproj]      -> one event: code C2065, source a.h:80:12,
//     (compiling source file 'soccer_game.cpp')                                                    discriminator "Kick", unit soccer_game.cpp,
//     note: see declaration of ...                                                                 the indented / note lines are its BODY
//
// A diagnostic and the lines that belong to it are ONE event (title + body). The translation units MSBuild prints as it compiles them
// ("  soccer_game.cpp") are recorded on the operation: they are the units a successful incremental build really checked.
#include "xlog_hub.h"

#include <regex>

namespace xlog
{
    class build_output_adapter
    {
    public:
        build_output_adapter(hub& Hub, op_handle& Op, std::string Channel = "game.build") noexcept
            : m_Hub(Hub), m_Op(Op), m_Channel(std::move(Channel)) {}

        // One raw output line, without its line break.
        void Feed(std::string_view Line) noexcept
        {
            std::string Text(Line);
            while (!Text.empty() && (Text.back() == '\r' || Text.back() == '\n')) Text.pop_back();
            // MSBuild prefixes lines with "N>" when it builds projects in parallel
            static const std::regex Node(R"(^\s*\d+>)");
            if (std::smatch M; std::regex_search(Text, M, Node)) Text.erase(0, M.length());

            if (TryUnit(Text))   return;
            if (TryDiagnostic(Text)) return;
            if (m_bPending && Continues(Text)) { Append(Text); return; }

            Flush();
            if (Text.find_first_not_of(" \t") == std::string::npos) return;         // a blank line is not an event
            Plain(Text);
        }

        // The end of the output: the block still pending is complete.
        void Finish() noexcept { Flush(); }

        std::size_t Errors() const noexcept   { return m_Errors; }
        std::size_t Warnings() const noexcept { return m_Warnings; }

    private:
        // "  soccer_game.cpp": MSBuild naming the file it is compiling now
        bool TryUnit(const std::string& Text) noexcept
        {
            static const std::regex Unit(R"(^ {1,4}([A-Za-z0-9_.\-+/\\]+\.(?:cpp|cxx|cc|c|cu|rc|asm))\s*$)");
            std::smatch M;
            if (!std::regex_match(Text, M, Unit)) return false;
            Flush();
            m_Unit = M[1].str();
            if (m_Seen.insert(m_Unit)) m_Op.Unit(m_Unit);
            return true;
        }

        bool TryDiagnostic(const std::string& Text) noexcept
        {
            // cl / MSBuild: file(line[,col]): error|warning|note CODE: text
            static const std::regex Compiler(R"(^\s*(.+?)\((\d+)(?:,(\d+))?(?:,\d+,\d+)?\)\s*:\s*(fatal error|error|warning|note)\s*([A-Za-z]*\d*)\s*:\s*(.*)$)");
            // link / MSBuild / cl command line: tool-or-file : error|warning CODE : text
            static const std::regex Tool(R"(^\s*(.+?)\s*:\s*(fatal error|error|warning)\s+([A-Za-z]+\d+)\s*:\s*(.*)$)");
            // CMake: "CMake Error at path:line (call):" opens a block, "CMake Error: text" is one line
            static const std::regex CMakeBlock(R"(^CMake (Error|Warning)(?: \(dev\))? at (.+?):(\d+) \((.+?)\):\s*$)");
            static const std::regex CMakeLine(R"(^CMake (Error|Warning)(?: \(dev\))?:\s*(.*)$)");

            std::smatch M;
            if (std::regex_match(Text, M, Compiler))
            {
                const std::string Level = M[4].str();
                if (Level == "note" && m_bPending && SameFile(M[1].str())) { Append(Text); return true; }       // belongs to the diagnostic before it
                Flush();
                Open(Level, M[5].str(), M[6].str(), M[1].str(), std::atoi(M[2].str().c_str()), M[3].matched ? std::atoi(M[3].str().c_str()) : 0, "msvc.compiler", "cl", false);
                return true;
            }
            if (std::regex_match(Text, M, Tool))
            {
                Flush();
                const bool bLink = M[3].str().rfind("LNK", 0) == 0;
                Open(M[2].str(), M[3].str(), M[4].str(), M[1].str(), 0, 0, bLink ? "msvc.linker" : "msvc.tool", bLink ? "link" : "tool", false);
                return true;
            }
            if (std::regex_match(Text, M, CMakeBlock))
            {
                Flush();
                Open(M[1].str() == "Error" ? "error" : "warning", "", std::format("CMake {} in {}", M[1].str(), M[4].str()), M[2].str(), std::atoi(M[3].str().c_str()), 0, "cmake", "cmake", true);
                return true;
            }
            if (std::regex_match(Text, M, CMakeLine))
            {
                Flush();
                Open(M[1].str() == "Error" ? "error" : "warning", "", M[2].str(), {}, 0, 0, "cmake", "cmake", true);
                Flush();
                return true;
            }
            return false;
        }

        bool Continues(const std::string& Text) const noexcept
        {
            if (Text.empty()) return false;                                     // a blank line ends the block
            if (Text[0] != ' ' && Text[0] != '\t') return false;
            static const std::regex Output(R"(^\s+\S+ -> )");                   // "  Game.vcxproj -> D:\x\Game.dll": MSBuild's own line, not part of a diagnostic
            return !std::regex_search(Text, Output);                            // indented: excerpts, "(compiling source file ...)", cmake's quoted call stack
        }

        bool SameFile(const std::string& File) const noexcept { return m_Pending.m_Source.m_Path == Trim(File); }
        static std::string Trim(std::string S) noexcept
        {
            const auto B = S.find_first_not_of(" \t");
            return B == std::string::npos ? std::string{} : S.substr(B);
        }

        void Open(const std::string& Level, const std::string& Code, std::string Message, const std::string& File, int Line, int Column,
                  const char* Producer, const char* Tool, bool bHeuristic) noexcept
        {
            m_Pending = event{};
            m_PendingBody.clear();
            m_bCMake = std::string_view(Producer) == "cmake";

            // MSBuild appends the project the line came from: "... [D:\x\y.vcxproj]"
            static const std::regex Project(R"(\s+\[([^\]]+\.(?:vcxproj|csproj|proj))\]\s*$)");
            std::string ProjectName;
            if (std::smatch P; std::regex_search(Message, P, Project)) { ProjectName = P[1].str(); Message.erase(P.position(0)); }

            event& E = m_Pending;
            E.m_Producer = Producer;
            E.m_Origin = { origin::type::Tool, Level == "note" ? "msbuild" : (m_bCMake ? "cmake" : "msbuild"), 0 };
            E.m_Kind = kind::Diagnostic;
            E.m_Channel = m_Channel;
            const bool bFatal = Level == "fatal error";
            E.m_Severity = (Level == "error" || bFatal) ? severity::Error : Level == "warning" ? severity::Warning : severity::Info;
            if (E.m_Severity == severity::Info) E.m_Kind = kind::Log;
            E.m_Code = Code;
            E.m_Title = Message;
            E.m_Operation = m_Op.Id();
            E.m_bHeuristic = bHeuristic;
            if (!File.empty())
            {
                E.m_Source = { ref::type::File, Trim(File), 0, Line, Column, 0 };
                E.m_Subjects.push_back({ ref::type::File, Trim(File), 0, Line, Column, 0 });
            }
            E.m_Discriminator = Discriminator(Message);
            E.m_Attributes.push_back({ "tool", std::string(Tool) });
            if (!m_Unit.empty())     E.m_Attributes.push_back({ "unit", m_Unit });
            if (!ProjectName.empty()) E.m_Attributes.push_back({ "project", ProjectName });
            if (bFatal)              E.m_Attributes.push_back({ "fatal", true });
            m_bPending = true;
        }

        // The first quoted name in the text: the identifier / symbol that makes this occurrence a different problem from the next one on the same line.
        static std::string Discriminator(const std::string& Text) noexcept
        {
            const auto A = Text.find('\''), Q = Text.find('"');
            const auto Open = std::min(A, Q);
            if (Open == std::string::npos) return {};
            const char Close = Text[Open];
            const auto End = Text.find(Close, Open + 1);
            if (End == std::string::npos) return {};
            return Text.substr(Open + 1, std::min<std::size_t>(End - Open - 1, 200));
        }

        void Append(const std::string& Line) noexcept
        {
            if (!m_PendingBody.empty()) m_PendingBody += '\n';
            m_PendingBody += Line;
        }

        void Flush() noexcept
        {
            if (!m_bPending) return;
            m_bPending = false;
            event E = std::move(m_Pending);
            if (!m_PendingBody.empty())
            {
                std::string Text = E.m_Title + "\n" + m_PendingBody;
                const auto Title = E.m_Title;
                SetMessage(E, Text);
                E.m_Title = Title;
            }
            if (E.m_Severity == severity::Error)        ++m_Errors;
            else if (E.m_Severity == severity::Warning) ++m_Warnings;
            m_Hub.Emit(std::move(E));
            m_Pending = event{};
            m_PendingBody.clear();
        }

        // Output that is not a diagnostic: kept (the old Log tab showed it all), as a plain Info line.
        void Plain(const std::string& Text) noexcept
        {
            event E;
            E.m_Producer = "msbuild.output"; E.m_Origin = { origin::type::Tool, "msbuild", 0 };
            E.m_Severity = severity::Info; E.m_Kind = kind::Log; E.m_Channel = m_Channel; E.m_Operation = m_Op.Id();
            SetMessage(E, Text);
            m_Hub.Emit(std::move(E));
        }

        hub&                            m_Hub;
        op_handle&                      m_Op;
        std::string                     m_Channel;
        std::string                     m_Unit;
        struct seen_set { std::vector<std::string> m_V; bool insert(const std::string& S) { for (auto& x : m_V) if (x == S) return false; m_V.push_back(S); return true; } } m_Seen;
        event                           m_Pending;
        std::string                     m_PendingBody;
        bool                            m_bPending = false;
        bool                            m_bCMake = false;
        std::size_t                     m_Errors = 0, m_Warnings = 0;
    };
}

#endif // XLOG_BUILD_H
