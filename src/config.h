#pragma once
#include <windows.h>

#include <string>
#include <vector>

namespace wp {

struct PanelConfig {
  std::wstring section;      // INI section name, e.g. "Panel1"
  std::wstring windowTitle;  // case-insensitive substring of the window title (optional)
  std::wstring processName;  // executable name, e.g. "chrome.exe" (optional)
  bool enabled = true;

  // Pose relative to the panel's anchor (LOCAL space, or the head pose at the last recenter).
  // Meters: +X right, +Y up, -Z forward. Degrees for yaw/pitch.
  float x = 0.0f, y = -0.15f, z = -0.7f;
  float yawDeg = 0.0f, pitchDeg = 0.0f;
  float widthM = 0.35f;  // height follows the window aspect ratio

  float opacity = 1.0f;
  bool useWindowAlpha = false;
  bool chromaKey = false;
  float keyColor[3] = {0, 0, 0};
  float keyTolerance = 0.10f;

  int maxFps = 30;       // capture updates per second (0 = unlimited)
  int recenterSeq = 0;   // bumped by the control app to recenter this panel

  bool SameTarget(const PanelConfig& o) const {
    return windowTitle == o.windowTitle && processName == o.processName;
  }
  bool SameLook(const PanelConfig& o) const {
    return opacity == o.opacity && useWindowAlpha == o.useWindowAlpha && chromaKey == o.chromaKey &&
           keyColor[0] == o.keyColor[0] && keyColor[1] == o.keyColor[1] &&
           keyColor[2] == o.keyColor[2] && keyTolerance == o.keyTolerance;
  }
};

struct Config {
  std::vector<PanelConfig> panels;
  int recenterAllSeq = 0;  // [Control] RecenterAll
};

std::wstring ConfigPath();  // <DataDir>\config.ini
void EnsureDefaultConfig(const std::wstring& path);
Config LoadConfig(const std::wstring& path);

}  // namespace wp
