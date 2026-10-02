#ifndef XLOG_H
#define XLOG_H
#pragma once

// xlog: events, problems and operations, recorded by any thread, committed by the host thread, queried by people (the Log tab) and by scripts and AI
// (the pipe commands). The umbrella gives the core; the rest is opt-in so a tool pays only for what it uses:
//
//   xlog_hub.h       the record types, the hub (ring, store, problems, operations), the query grammar        no dependencies beyond the standard library
//   xlog_build.h     the MSBuild / cl / link / CMake output adapter                                         <regex>
//   xlog_commands.h  the pipe commands (LogStatus, LogProblems, LogEvents, ...)                             xundo, xcmdline
//   (the window and the diagnostics view are the editor part: editor/xlog_tab.h, editor/xlog_diagnostics.h; not here)
#include "xlog_hub.h"

#endif // XLOG_H
