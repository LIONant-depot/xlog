include(${CMAKE_BINARY_DIR}/_deps/xcmake_tools/Common.cmake)
DefineInterfaceComponent(xlog "dependencies/xcore"
  "source/xlog.h"
  "source/xlog_hub.h"
  "source/xlog_view.h"
  "source/xlog_build.h"
  "source/xlog_pipeline.h"
  "source/xlog_commands.h"
  "README.md"
)

# The editor part: the Logs window and the diagnostics view. ImGui and xeditor's widgets; never part of a headless build.
DefineInterfaceComponent(xlog_editor "dependencies/xlog"
  "editor/xlog_tab.h"
  "editor/xlog_diagnostics.h"
)
