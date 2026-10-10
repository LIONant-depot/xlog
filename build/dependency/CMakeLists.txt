include(${CMAKE_BINARY_DIR}/_deps/xcmake_tools/Common.cmake)
# One component, named like the repo (a component's files live in dependencies/<component name>). The editor part (the Logs window and the diagnostics view: ImGui and xeditor's widgets)
# is listed in the same component; these are headers only, so a headless build compiles nothing of them.
DefineInterfaceComponent(xlog "dependencies/xcore"
  "source/xlog.h"
  "source/xlog_hub.h"
  "source/xlog_view.h"
  "source/xlog_build.h"
  "source/xlog_pipeline.h"
  "source/xlog_commands.h"
  "editor/xlog_tab.h"
  "editor/xlog_diagnostics.h"
  "editor/xlog_badge.h"
  "README.md"
)
