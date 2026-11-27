#include "config.h"

#include "log.h"

#include <cwchar>

namespace wp {

static const wchar_t kDefaultConfig[] =
    L"; XR Window Panels - configuracao\n"
    L"; Use o XRWindowPanelsControl.exe para editar, ou edite aqui (relido automaticamente).\n"
    L";\n"
    L"; Atalhos no jogo: Ctrl+F12 mostra/esconde, Ctrl+F11 recentraliza todos os paineis.\n"
    L";\n"
    L"; Window   parte do titulo da janela (opcional se Process estiver preenchido)\n"
    L"; Process  nome do executavel, ex.: chrome.exe (opcional)\n"
    L"; X Y Z    metros (X direita, Y cima, Z negativo = frente); Yaw/Pitch em graus\n"
    L"; Width    largura em metros; MaxFps = atualizacoes por segundo da captura\n"
    L"\n"
    L"[Panel1]\n"
    L"Window=Track Titan\n"
    L"Process=\n"
    L"Enabled=1\n"
    L"X=0\n"
    L"Y=-0.15\n"
    L"Z=-0.7\n"
    L"Yaw=0\n"
    L"Pitch=0\n"
    L"Width=0.35\n"
    L"Opacity=1\n"
    L"UseWindowAlpha=0\n"
    L"ChromaKey=0\n"
    L"KeyColor=#000000\n"
    L"KeyTolerance=0.1\n"
    L"MaxFps=30\n";

std::wstring ConfigPath() { return DataDir() + L"\\config.ini"; }

void EnsureDefaultConfig(const std::wstring& path) {
  if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return;
  // UTF-16LE with BOM so window titles with accents work; CRLF line endings for INI APIs.
  std::wstring text;
  for (const wchar_t* p = kDefaultConfig; *p; ++p) {
    if (*p == L'\n') text += L'\r';
    text += *p;
  }
  HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return;
  DWORD written = 0;
  const wchar_t bom = 0xFEFF;
  WriteFile(f, &bom, sizeof(bom), &written, nullptr);
  WriteFile(f, text.data(), (DWORD)(text.size() * sizeof(wchar_t)), &written, nullptr);
  CloseHandle(f);
  Log("Created default config at %s", Narrow(path).c_str());
}

static std::wstring ReadStr(const std::wstring& file, const wchar_t* sec, const wchar_t* key,
                            const wchar_t* def = L"") {
  wchar_t buf[1024];
  GetPrivateProfileStringW(sec, key, def, buf, 1024, file.c_str());
  return buf;
}

std::wstring AnchorsPath() { return DataDir() + L"\\anchors.ini"; }

void SaveAnchor(const std::wstring& section, const SavedAnchor& a) {
  const std::wstring path = AnchorsPath();
  wchar_t buf[64];
  auto put = [&](const wchar_t* key, float v) {
    swprintf_s(buf, L"%.4f", v);
    WritePrivateProfileStringW(section.c_str(), key, buf, path.c_str());
  };
  WritePrivateProfileStringW(section.c_str(), L"Space", a.stage ? L"stage" : L"local", path.c_str());
  put(L"X", a.x);
  put(L"Y", a.y);
  put(L"Z", a.z);
  put(L"Yaw", a.yaw);
}

static SavedAnchor LoadAnchor(const std::wstring& section) {
  const std::wstring path = AnchorsPath();
  SavedAnchor a;
  std::wstring space = ReadStr(path, section.c_str(), L"Space");
  if (space.empty()) return a;
  auto get = [&](const wchar_t* key) { return (float)_wtof(ReadStr(path, section.c_str(), key, L"0").c_str()); };
  a.valid = true;
  a.stage = space == L"stage";
  a.x = get(L"X");
  a.y = get(L"Y");
  a.z = get(L"Z");
  a.yaw = get(L"Yaw");
  return a;
}

static float ReadFloat(const std::wstring& file, const wchar_t* sec, const wchar_t* key,
                       float def) {
  std::wstring s = ReadStr(file, sec, key);
  if (s.empty()) return def;
  for (auto& c : s) {
    if (c == L',') c = L'.';  // accept "0,5" as well as "0.5"
  }
  return wcstof(s.c_str(), nullptr);
}

static bool ReadBool(const std::wstring& file, const wchar_t* sec, const wchar_t* key, bool def) {
  std::wstring s = ReadStr(file, sec, key);
  if (s.empty()) return def;
  return s[0] == L'1' || s[0] == L't' || s[0] == L'T' || s[0] == L's' || s[0] == L'S';
}

Config LoadConfig(const std::wstring& path) {
  Config cfg;
  cfg.recenterAllSeq = GetPrivateProfileIntW(L"Control", L"RecenterAll", 0, path.c_str());
  cfg.toggleAllSeq = GetPrivateProfileIntW(L"Control", L"ToggleAll", 0, path.c_str());
  cfg.selectSeq = GetPrivateProfileIntW(L"Control", L"SelectSeq", 0, path.c_str());
  cfg.selected = ReadStr(path, L"Control", L"Selected");
  cfg.saveRecenter = GetPrivateProfileIntW(L"Control", L"SaveRecenter", 0, path.c_str()) != 0;

  std::wstring names(32768, L'\0');
  DWORD n = GetPrivateProfileSectionNamesW(names.data(), (DWORD)names.size(), path.c_str());
  for (const wchar_t* p = names.c_str(); p < names.c_str() + n && *p; p += wcslen(p) + 1) {
    if (_wcsnicmp(p, L"Panel", 5) != 0) continue;
    const wchar_t* sec = p;
    PanelConfig pc;
    pc.section = sec;
    pc.windowTitle = ReadStr(path, sec, L"Window");
    pc.processName = ReadStr(path, sec, L"Process");
    if (pc.windowTitle.empty() && pc.processName.empty()) continue;
    pc.enabled = ReadBool(path, sec, L"Enabled", true);
    pc.x = ReadFloat(path, sec, L"X", pc.x);
    pc.y = ReadFloat(path, sec, L"Y", pc.y);
    pc.z = ReadFloat(path, sec, L"Z", pc.z);
    pc.yawDeg = ReadFloat(path, sec, L"Yaw", pc.yawDeg);
    pc.pitchDeg = ReadFloat(path, sec, L"Pitch", pc.pitchDeg);
    pc.widthM = ReadFloat(path, sec, L"Width", pc.widthM);
    if (pc.widthM < 0.02f) pc.widthM = 0.02f;
    pc.opacity = ReadFloat(path, sec, L"Opacity", pc.opacity);
    pc.useWindowAlpha = ReadBool(path, sec, L"UseWindowAlpha", false);
    pc.chromaKey = ReadBool(path, sec, L"ChromaKey", false);
    std::wstring key = ReadStr(path, sec, L"KeyColor", L"#000000");
    unsigned long rgb = wcstoul(key.c_str() + (key.size() && key[0] == L'#' ? 1 : 0), nullptr, 16);
    pc.keyColor[0] = ((rgb >> 16) & 0xFF) / 255.0f;
    pc.keyColor[1] = ((rgb >> 8) & 0xFF) / 255.0f;
    pc.keyColor[2] = (rgb & 0xFF) / 255.0f;
    pc.keyTolerance = ReadFloat(path, sec, L"KeyTolerance", pc.keyTolerance);
    pc.maxFps = GetPrivateProfileIntW(sec, L"MaxFps", 30, path.c_str());
    pc.recenterSeq = GetPrivateProfileIntW(sec, L"RecenterSeq", 0, path.c_str());
    pc.savedAnchor = LoadAnchor(pc.section);
    cfg.panels.push_back(std::move(pc));
  }
  return cfg;
}

}  // namespace wp
