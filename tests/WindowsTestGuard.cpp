// Host tests built with MSVC: a failed assert prints its message and ends the test at once. The debug
// C runtime would otherwise wait on an "abort() has been called" dialog nobody answers, and ctest would
// report a timeout half an hour later instead of the failure.
#if defined(_MSC_VER)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <crtdbg.h>
#include <cstdlib>
#include <windows.h>

namespace {

struct NoDialogs {
  NoDialogs()
  {
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    const int kinds[] = {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT};
    for (const int kind : kinds)
    {
      _CrtSetReportMode(kind, _CRTDBG_MODE_FILE);
      _CrtSetReportFile(kind, _CRTDBG_FILE_STDERR);
    }
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
  }
};

const NoDialogs kNoDialogs;

}  // namespace
#endif
