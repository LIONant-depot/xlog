include(${CMAKE_BINARY_DIR}/_deps/xcmake_tools/Common.cmake)
DefineInterfaceComponent(xlog "dependencies/xcore"
  "source/xlog.h"
  "source/xlog_hub.h"
  "source/xlog_build.h"
  "source/xlog_commands.h"
  "source/xlog_tab.h"
  "README.md"
)
