#pragma once
// Shared-memory block between the layer (inside the game) and the control app.
// Layout is mirrored in control/ControlPanel.cs (byte offsets) - keep in sync.
#include <cstddef>
#include <cstdint>

namespace wp {

constexpr wchar_t kSharedName[] = L"Local\\XRWindowPanels_Shared_v1";
constexpr uint32_t kSharedMagic = 0x50575258;  // "XRWP"
constexpr uint32_t kSharedVersion = 1;
constexpr int kMaxSharedPanels = 16;

enum PanelState : int32_t {
  kStateDisabled = 0,
  kStateSearching = 1,  // window not found yet
  kStateCapturing = 2,  // capture running, no frame yet
  kStateShowing = 3,    // visible in VR
  kStateHidden = 4,     // hidden with Ctrl+F12
};

#pragma pack(push, 4)
struct SharedPanelStatus {    // 80 bytes
  wchar_t section[32];        // +0
  int32_t state;              // +64
  int32_t width;              // +68
  int32_t height;             // +72
  int32_t fps;                // +76
};

struct SharedBlock {
  uint32_t magic;             // +0
  uint32_t version;           // +4
  volatile int32_t configSeq; // +8   control app increments after writing config.ini
  int32_t reserved;           // +12
  int64_t heartbeat;          // +16  GetTickCount64() of the layer, updated ~2x/s
  wchar_t app[64];            // +24  game executable name
  int32_t panelCount;         // +152
  SharedPanelStatus panels[kMaxSharedPanels];  // +156
};
#pragma pack(pop)

static_assert(sizeof(SharedPanelStatus) == 80);
static_assert(offsetof(SharedBlock, panels) == 156);

}  // namespace wp
