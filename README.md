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

Design, phases and acceptance tests: `documentation/Editors/DESIGN_logs.md` in xLION.
