#pragma once
#include <string>

namespace wp {

// %LOCALAPPDATA%\XRWindowPanels (created on first use)
std::wstring DataDir();

// Opens <DataDir>\<fileName> for logging (truncated). Safe to call more than once.
void LogInit(const std::wstring& fileName, bool alsoConsole = false);
void Log(const char* fmt, ...);

std::string Narrow(const std::wstring& s);

}  // namespace wp
