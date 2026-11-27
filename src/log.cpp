#include "log.h"

#include <windows.h>
#include <shlobj.h>
#include <share.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace wp {

static std::mutex g_mutex;
static FILE* g_file = nullptr;
static bool g_console = false;

std::wstring DataDir() {
  static const std::wstring dir = [] {
    // Optional override (tests / portable setups).
    wchar_t custom[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"XRWP_DATA_DIR", custom, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
      CreateDirectoryW(custom, nullptr);
      return std::wstring(custom);
    }
    std::wstring d = L".";
    PWSTR p = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) {
      d = p;
    }
    CoTaskMemFree(p);
    d += L"\\XRWindowPanels";
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
  }();
  return dir;
}

void LogInit(const std::wstring& fileName, bool alsoConsole) {
  std::lock_guard lock(g_mutex);
  g_console = g_console || alsoConsole;
  if (g_file) return;
  g_file = _wfsopen((DataDir() + L"\\" + fileName).c_str(), L"w", _SH_DENYWR);
}

void Log(const char* fmt, ...) {
  char buf[2048];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  SYSTEMTIME t;
  GetLocalTime(&t);

  std::lock_guard lock(g_mutex);
  OutputDebugStringA("[XRWindowPanels] ");
  OutputDebugStringA(buf);
  OutputDebugStringA("\n");
  if (g_console) printf("%s\n", buf);
  if (g_file) {
    fprintf(g_file, "%02d:%02d:%02d.%03d %s\n", t.wHour, t.wMinute, t.wSecond,
            t.wMilliseconds, buf);
    fflush(g_file);
  }
}

std::string Narrow(const std::wstring& s) {
  if (s.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
  std::string out(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n, nullptr, nullptr);
  return out;
}

}  // namespace wp
