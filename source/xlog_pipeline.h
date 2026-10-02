#ifndef XLOG_PIPELINE_H
#define XLOG_PIPELINE_H
#pragma once

// The pipeline-message adapter: what a resource compiler (xresource_pipeline) prints, line by line, into events of an operation.
//
//   [Info] OUTPUT: D:\proj\Cache\...                         -> a log event
//   [Warning] Mip count clamped to 12                       -> a diagnostic (a problem), severity warning
//   [Error] Unsupported texture format 'exr'                -> a diagnostic (a problem), severity error
//     supported: png, dds                                   -> an untagged line belongs to the event above: its body
//   Title: [====>      ] 45%                                -> progress (debug level; a view shows a bar, not a line per step)
//   [COMPILATION_SUCCESS]                                   -> the compiler's own word that it finished; not an event
//
// The pipeline gives no stable codes, so its problems are labelled heuristic: the text with its numbers and quoted names taken out is the identity
// (TemplateOf), together with the asset the compile was about. Like the build adapter, a diagnostic and the lines that belong to it are ONE event.
#include "xlog_hub.h"

#include <regex>

namespace xlog
{
    class pipeline_output_adapter
    {
    public:
        // Subject: the asset being compiled (what every event is about). Channel: "asset.compile.<type>".
        pipeline_output_adapter(hub& Hub, op_handle& Op, ref Subject, std::string Channel = "asset.compile", std::string Producer = "xresource.pipeline") noexcept
            : m_Hub(Hub), m_Op(Op), m_Subject(std::move(Subject)), m_Channel(std::move(Channel)), m_Producer(std::move(Producer)) {}

        void Feed(std::string_view Line) noexcept
        {
            std::string Text(Line);
            while (!Text.empty() && (Text.back() == '\r' || Text.back() == '\n')) Text.pop_back();

            static const std::regex Tagged(R"(^\[(Info|Warning|Error)\]\s?(.*)$)");
            std::smatch M;
            if (std::regex_match(Text, M, Tagged))
            {
                Flush();
                m_Level = M[1].str() == "Error" ? severity::Error : M[1].str() == "Warning" ? severity::Warning : severity::Info;
                m_Pending = M[2].str();
                m_bPending = true;
                return;
            }
            if (Text == "[COMPILATION_SUCCESS]") { Flush(); return; }
            if (IsSeparator(Text)) { Flush(); return; }
            if (m_bPending && !Text.empty() && Text.find_first_not_of(" \t") != std::string::npos) { m_Pending += '\n'; m_Pending += Text; return; }
            if (Text.find_first_not_of(" \t") == std::string::npos) return;                // a blank line is not an event

            Flush();
            m_Level = severity::Info; m_Pending = Text; m_bPending = true;                 // a line the compiler did not tag (the command line it was started with)
        }

        void Finish() noexcept { Flush(); }

    private:
        static bool IsSeparator(const std::string& Text) noexcept
        {
            return Text.size() >= 8 && Text.find_first_not_of("-=") == std::string::npos;
        }

        // "Title: [====>      ] 45%"
        static bool IsProgress(const std::string& Text) noexcept
        {
            static const std::regex Bar(R"(\[[=> ]+\]\s*\d+%)");
            return std::regex_search(Text, Bar);
        }

        void Flush() noexcept
        {
            if (!m_bPending) return;
            m_bPending = false;
            event E;
            E.m_Producer = m_Producer;
            E.m_Origin = { origin::type::Tool, "xresource.compiler", 0 };
            E.m_Channel = m_Channel;
            E.m_Operation = m_Op.Id();
            if (m_Subject.Valid()) E.m_Subjects.push_back(m_Subject);
            E.m_Severity = m_Level;
            E.m_Kind = m_Level >= severity::Warning ? kind::Diagnostic : IsProgress(m_Pending) ? kind::Progress : kind::Log;
            if (E.m_Kind == kind::Progress) E.m_Severity = severity::Debug;
            if (E.m_Kind == kind::Diagnostic) E.m_bHeuristic = true;             // no code from the pipeline: grouped by what the text says
            SetMessage(E, m_Pending);
            m_Hub.Emit(std::move(E));
            m_Pending.clear();
        }

        hub&            m_Hub;
        op_handle&      m_Op;
        ref             m_Subject;
        std::string     m_Channel, m_Producer;
        std::string     m_Pending;
        severity        m_Level = severity::Info;
        bool            m_bPending = false;
    };

    // A compile's whole output in one call: the lines in order, then the end.
    inline void FeedPipelineOutput(pipeline_output_adapter& Adapter, std::string_view Output) noexcept
    {
        std::size_t Start = 0;
        while (Start <= Output.size())
        {
            const auto Eol = Output.find('\n', Start);
            Adapter.Feed(Output.substr(Start, Eol == std::string_view::npos ? Output.size() - Start : Eol - Start));
            if (Eol == std::string_view::npos) break;
            Start = Eol + 1;
        }
        Adapter.Finish();
    }
}

#endif // XLOG_PIPELINE_H
