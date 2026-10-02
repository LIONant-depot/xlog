# xlog

One place where "something happened" is recorded, for the editors, their plugins, the compilers' adapters and the tools to share.

- **Events** are immutable occurrences (a title line, an optional multi-line body, typed attributes, typed source references).
- **Problems** are derived: the diagnostics that share an identity (producer, code, site, subject, discriminator), with a count.
- **Operations** are units of work with an outcome (a build, a compile, a load), so "did it work, and why not" has an answer.
- **Sessions** are one launch.

```cpp
#include "dependencies/xlog/source/xlog.h"

auto* pLogs = xlog::hub::current();                       // the owner (xeditor::host in the editors) made it current
auto Op = pLogs->Begin("asset.compile", { xlog::origin::type::Tool, "texture", 0 }, subject, "Compile Face.exr");
xlog::event E; E.m_Producer = "xtexture.compiler"; E.m_Kind = xlog::kind::Diagnostic; E.m_Severity = xlog::severity::Error;
E.m_Channel = "asset.compile.texture"; E.m_Code = "TEX.UNSUPPORTED_FORMAT"; E.m_Operation = Op.Id();
xlog::SetMessage(E, "Unsupported texture format\n  format: exr, 32 bit\n  supported: png, dds");
pLogs->Emit(std::move(E));                                 // any thread; never blocks
Op.Fail();                                                 // an operation dropped without an outcome is recorded Abandoned
```

The host thread calls `hub.Drain()` once per frame (or per loop iteration in a headless host); queries drain first, so a query sees everything pushed before it.

## Two parts

- **`source/`: the runtime.** The hub, the store, the query grammar, the view state and its list builders, the build and pipeline adapters, the pipe commands. Standard library, xundo and xcmdline only: **no ImGui, no editor**, so a headless host, a compiler process or a test harness uses exactly the same service.
- **`editor/`: the editor part.** `xlog_tab.h` (the Logs window: Problems | Events) and `xlog_diagnostics.h` (what an asset's last compile said, for the resource editors' Feedback). They need ImGui and xeditor's `widgets.h` (the editors' one search box); a build without an editor never includes them.

Design, phases and acceptance tests: `documentation/Editors/DESIGN_logs.md` in xLION.
