#include "config.h"

#include "log.h"

#include <cwchar>
#include <cwctype>
#include <unordered_map>

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

// ---------------------------------------------------------------------------
// In-memory INI snapshot.
//
// The control app replaces config.ini many times per second while a slider is dragged.
// GetPrivateProfileString reopens the file for every key, so a read that overlaps a replace
// could see a missing file (no panels -> panels destroyed and recreated) or a mix of old and
// new values. Reading the whole file with one open gives an atomic snapshot; if the open
// fails, the caller keeps the previous config.

class Ini {
 public:
  bool Load(const std::wstring& path) {
    std::string bytes;
    if (!ReadAll(path, bytes)) return false;
    Parse(Decode(bytes));
    return true;
  }

  std::wstring Get(const std::wstring& sec, const wchar_t* key, const wchar_t* def = L"") const {
    auto s = m_data.find(Lower(sec));
    if (s == m_data.end()) return def;
    auto k = s->second.find(Lower(key));
    return k == s->second.end() ? std::wstring(def) : k->second;
  }
  int GetInt(const std::wstring& sec, const wchar_t* key, int def) const {
    std::wstring v = Get(sec, key);
    return v.empty() ? def : _wtoi(v.c_str());
  }
  float GetFloat(const std::wstring& sec, const wchar_t* key, float def) const {
    std::wstring v = Get(sec, key);
    if (v.empty()) return def;
    for (auto& c : v) {
      if (c == L',') c = L'.';  // accept "0,5" as well as "0.5"
    }
    return wcstof(v.c_str(), nullptr);
  }
  bool GetBool(const std::wstring& sec, const wchar_t* key, bool def) const {
    std::wstring v = Get(sec, key);
    if (v.empty()) return def;
    wchar_t c = (wchar_t)towlower(v[0]);
    return c == L'1' || c == L't' || c == L's';
  }
  const std::vector<std::wstring>& Sections() const { return m_sections; }

 private:
  static std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
  }
  static std::wstring Trim(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return {};
    size_t b = s.find_last_not_of(L" \t\r\n");
    return s.substr(a, b - a + 1);
  }

  static bool ReadAll(const std::wstring& path, std::string& out) {
    for (int attempt = 0; attempt < 5; ++attempt) {
      // FILE_SHARE_DELETE so the control app can still replace the file while we read.
      HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (f != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER size{};
        bool ok = GetFileSizeEx(f, &size) && size.QuadPart < (16 << 20);
        if (ok) {
          out.resize((size_t)size.QuadPart);
          DWORD read = 0;
          ok = out.empty() || (ReadFile(f, out.data(), (DWORD)out.size(), &read, nullptr) && read == out.size());
        }
        CloseHandle(f);
        if (ok) return true;
      }
      DWORD err = GetLastError();
      if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
        if (attempt >= 2) return false;  // may be mid-replace: retry briefly, then give up
      }
      Sleep(10);
    }
    return false;
  }

  static std::wstring Decode(const std::string& b) {
    if (b.size() >= 2 && (unsigned char)b[0] == 0xFF && (unsigned char)b[1] == 0xFE) {  // UTF-16LE
      return std::wstring(reinterpret_cast<const wchar_t*>(b.data() + 2), (b.size() - 2) / 2);
    }
    size_t start = (b.size() >= 3 && (unsigned char)b[0] == 0xEF && (unsigned char)b[1] == 0xBB &&
                    (unsigned char)b[2] == 0xBF) ? 3 : 0;
    const char* src = b.data() + start;
    int len = (int)(b.size() - start);
    if (len <= 0) return {};
    UINT cp = CP_UTF8;
    int n = MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, src, len, nullptr, 0);
    if (n == 0) {  // not valid UTF-8: Notepad "ANSI"
      cp = CP_ACP;
      n = MultiByteToWideChar(cp, 0, src, len, nullptr, 0);
    }
    std::wstring w(n, L'\0');
    MultiByteToWideChar(cp, 0, src, len, w.data(), n);
    return w;
  }

  void Parse(const std::wstring& text) {
    std::wstring section;
    size_t pos = 0;
    while (pos <= text.size()) {
      size_t eol = text.find(L'\n', pos);
      if (eol == std::wstring::npos) eol = text.size();
      std::wstring line = Trim(text.substr(pos, eol - pos));
      pos = eol + 1;
      if (line.empty() || line[0] == L';' || line[0] == L'#') continue;
      if (line[0] == L'[') {
        size_t close = line.find(L']');
        section = Trim(line.substr(1, close == std::wstring::npos ? std::wstring::npos : close - 1));
        if (m_data.find(Lower(section)) == m_data.end()) m_sections.push_back(section);
        m_data[Lower(section)];
        continue;
      }
      size_t eq = line.find(L'=');
      if (eq == std::wstring::npos || section.empty()) continue;
      m_data[Lower(section)][Lower(Trim(line.substr(0, eq)))] = Trim(line.substr(eq + 1));
    }
  }

  std::unordered_map<std::wstring, std::unordered_map<std::wstring, std::wstring>> m_data;
  std::vector<std::wstring> m_sections;
};

// ---------------------------------------------------------------------------

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

static SavedAnchor LoadAnchor(const Ini& anchors, const std::wstring& section) {
  SavedAnchor a;
  std::wstring space = anchors.Get(section, L"Space");
  if (space.empty()) return a;
  a.valid = true;
  a.stage = space == L"stage";
  a.x = anchors.GetFloat(section, L"X", 0);
  a.y = anchors.GetFloat(section, L"Y", 0);
  a.z = anchors.GetFloat(section, L"Z", 0);
  a.yaw = anchors.GetFloat(section, L"Yaw", 0);
  return a;
}

bool LoadConfig(const std::wstring& path, Config& cfg) {
  Ini ini;
  if (!ini.Load(path)) return false;  // keep the previous config
  Ini anchors;
  anchors.Load(AnchorsPath());  // optional file

  cfg = Config{};
  cfg.recenterAllSeq = ini.GetInt(L"Control", L"RecenterAll", 0);
  cfg.toggleAllSeq = ini.GetInt(L"Control", L"ToggleAll", 0);
  cfg.selectSeq = ini.GetInt(L"Control", L"SelectSeq", 0);
  cfg.selected = ini.Get(L"Control", L"Selected");
  cfg.saveRecenter = ini.GetBool(L"Control", L"SaveRecenter", false);

  for (const auto& sec : ini.Sections()) {
    if (_wcsnicmp(sec.c_str(), L"Panel", 5) != 0) continue;
    PanelConfig pc;
    pc.section = sec;
    pc.windowTitle = ini.Get(sec, L"Window");
    pc.processName = ini.Get(sec, L"Process");
    if (pc.windowTitle.empty() && pc.processName.empty()) continue;
    pc.enabled = ini.GetBool(sec, L"Enabled", true);
    pc.x = ini.GetFloat(sec, L"X", pc.x);
    pc.y = ini.GetFloat(sec, L"Y", pc.y);
    pc.z = ini.GetFloat(sec, L"Z", pc.z);
    pc.yawDeg = ini.GetFloat(sec, L"Yaw", pc.yawDeg);
    pc.pitchDeg = ini.GetFloat(sec, L"Pitch", pc.pitchDeg);
    pc.widthM = ini.GetFloat(sec, L"Width", pc.widthM);
    if (pc.widthM < 0.02f) pc.widthM = 0.02f;
    pc.opacity = ini.GetFloat(sec, L"Opacity", pc.opacity);
    pc.useWindowAlpha = ini.GetBool(sec, L"UseWindowAlpha", false);
    pc.chromaKey = ini.GetBool(sec, L"ChromaKey", false);
    std::wstring key = ini.Get(sec, L"KeyColor", L"#000000");
    unsigned long rgb = wcstoul(key.c_str() + (key.size() && key[0] == L'#' ? 1 : 0), nullptr, 16);
    pc.keyColor[0] = ((rgb >> 16) & 0xFF) / 255.0f;
    pc.keyColor[1] = ((rgb >> 8) & 0xFF) / 255.0f;
    pc.keyColor[2] = (rgb & 0xFF) / 255.0f;
    pc.keyTolerance = ini.GetFloat(sec, L"KeyTolerance", pc.keyTolerance);
    pc.maxFps = ini.GetInt(sec, L"MaxFps", 30);
    pc.recenterSeq = ini.GetInt(sec, L"RecenterSeq", 0);
    pc.savedAnchor = LoadAnchor(anchors, pc.section);
    cfg.panels.push_back(std::move(pc));
  }
  return true;
}

}  // namespace wp
