// OpenXR implicit API layer: shows desktop windows as quad layers in D3D11 OpenXR apps.
//
// Flow:
//   xrCreateSession  -> if the app uses D3D11, remember its device, create spaces, start the
//                       config worker thread
//   xrEndFrame       -> (render thread, kept as cheap as possible) adopt new config, handle
//                       hotkeys/recenter, copy new window frames into our swapchains only when
//                       content changed, append XrCompositionLayerQuad layers
//   xrDestroySession -> release everything
//
// Anything that can block (file IO, window search, capture startup) runs on worker threads.

#include <windows.h>
#include <d3d11_1.h>
#include <wrl/client.h>

#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_PLATFORM_WIN32
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_loader_negotiation.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <vector>

#include "blitter.h"
#include "capture.h"
#include "config.h"
#include "log.h"
#include "shared.h"

using Microsoft::WRL::ComPtr;
using namespace wp;

#define LAYER_NAME "XR_APILAYER_NOVENDOR_window_panels"

// ---------------------------------------------------------------------------
// Next-in-chain functions

static PFN_xrGetInstanceProcAddr g_nextGetInstanceProcAddr = nullptr;

static struct {
  PFN_xrCreateSession CreateSession;
  PFN_xrDestroySession DestroySession;
  PFN_xrEndFrame EndFrame;
  PFN_xrEnumerateSwapchainFormats EnumerateSwapchainFormats;
  PFN_xrCreateSwapchain CreateSwapchain;
  PFN_xrDestroySwapchain DestroySwapchain;
  PFN_xrEnumerateSwapchainImages EnumerateSwapchainImages;
  PFN_xrAcquireSwapchainImage AcquireSwapchainImage;
  PFN_xrWaitSwapchainImage WaitSwapchainImage;
  PFN_xrReleaseSwapchainImage ReleaseSwapchainImage;
  PFN_xrCreateReferenceSpace CreateReferenceSpace;
  PFN_xrDestroySpace DestroySpace;
  PFN_xrLocateSpace LocateSpace;
} g_next{};

// ---------------------------------------------------------------------------
// Timing / math helpers

static int64_t QpcFreq() {
  static const int64_t f = [] {
    LARGE_INTEGER v;
    QueryPerformanceFrequency(&v);
    return v.QuadPart;
  }();
  return f;
}
static int64_t QpcNow() {
  LARGE_INTEGER v;
  QueryPerformanceCounter(&v);
  return v.QuadPart;
}

static constexpr float kDegToRad = 3.14159265358979f / 180.0f;

static XrQuaternionf QuatMul(const XrQuaternionf& a, const XrQuaternionf& b) {
  return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
          a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
          a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
          a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

static XrQuaternionf QuatAxis(float ax, float ay, float az, float radians) {
  float s = std::sin(radians * 0.5f);
  return {ax * s, ay * s, az * s, std::cos(radians * 0.5f)};
}

static XrVector3f Rotate(const XrQuaternionf& q, const XrVector3f& v) {
  // v' = v + 2w(u x v) + 2 u x (u x v)
  XrVector3f u{q.x, q.y, q.z};
  XrVector3f t{2 * (u.y * v.z - u.z * v.y), 2 * (u.z * v.x - u.x * v.z), 2 * (u.x * v.y - u.y * v.x)};
  return {v.x + q.w * t.x + (u.y * t.z - u.z * t.y),
          v.y + q.w * t.y + (u.z * t.x - u.x * t.z),
          v.z + q.w * t.z + (u.x * t.y - u.y * t.x)};
}

// ---------------------------------------------------------------------------
// Shared memory with the control app

static SharedBlock* g_shared = nullptr;
static HANDLE g_sharedMapping = nullptr;

static void OpenShared() {
  if (g_shared) return;
  g_sharedMapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                       sizeof(SharedBlock), kSharedName);
  if (!g_sharedMapping) return;
  g_shared = static_cast<SharedBlock*>(
      MapViewOfFile(g_sharedMapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedBlock)));
  if (!g_shared) return;
  g_shared->magic = kSharedMagic;
  g_shared->version = kSharedVersion;
  wchar_t exe[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  const wchar_t* name = wcsrchr(exe, L'\\');
  wcsncpy_s(g_shared->app, name ? name + 1 : exe, _TRUNCATE);
}

// ---------------------------------------------------------------------------
// Config worker: watches config.ini (and the control app's change counter) off the render thread

class ConfigWorker {
 public:
  void Start() {
    if (m_thread.joinable()) return;
    m_thread = std::jthread([this](std::stop_token st) { Run(st); });
  }
  void Stop() {
    if (!m_thread.joinable()) return;
    m_thread.request_stop();
    m_cv.notify_all();
    m_thread.join();
    m_thread = {};
  }
  // Render thread: non-blocking.
  bool TryTake(Config& out) {
    if (!m_hasPending.load(std::memory_order_acquire)) return false;
    std::unique_lock lock(m_mutex, std::try_to_lock);
    if (!lock.owns_lock() || !m_pending) return false;
    out = std::move(*m_pending);
    m_pending.reset();
    m_hasPending = false;
    return true;
  }

  // Render thread: queue an anchor to be written to anchors.ini by the worker.
  void QueueAnchor(const std::wstring& section, const SavedAnchor& a) {
    {
      std::lock_guard lock(m_mutex);
      m_anchorsToSave.emplace_back(section, a);
    }
    m_cv.notify_all();
  }

 private:
  void Run(std::stop_token st) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    const std::wstring path = ConfigPath();
    EnsureDefaultConfig(path);
    FILETIME lastWrite{};
    int32_t lastSeq = INT32_MIN;
    int64_t nextFileCheck = 0;
    bool first = true;

    while (!st.stop_requested()) {
      bool reload = first;
      if (g_shared) {
        int32_t seq = g_shared->configSeq;
        if (seq != lastSeq) {
          if (lastSeq != INT32_MIN) reload = true;
          lastSeq = seq;
        }
      }
      const int64_t now = QpcNow();
      WIN32_FILE_ATTRIBUTE_DATA attr{};
      bool haveAttr = false;
      if (reload || now >= nextFileCheck) {
        nextFileCheck = now + QpcFreq();  // manual edits: checked once per second
        haveAttr = GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attr) != 0;
        if (haveAttr && CompareFileTime(&attr.ftLastWriteTime, &lastWrite) != 0) reload = true;
      }
      if (reload) {
        if (haveAttr) lastWrite = attr.ftLastWriteTime;
        Config c = LoadConfig(path);
        Log("Config loaded: %zu panel(s)", c.panels.size());
        std::lock_guard lock(m_mutex);
        m_pending = std::move(c);
        m_hasPending = true;
        first = false;
      }

      std::vector<std::pair<std::wstring, SavedAnchor>> anchors;
      {
        std::lock_guard lock(m_mutex);
        anchors.swap(m_anchorsToSave);
      }
      for (auto& [section, a] : anchors) {
        SaveAnchor(section, a);
        Log("Saved recenter position for [%s] (%s space)", Narrow(section).c_str(), a.stage ? "stage" : "local");
      }

      std::unique_lock lock(m_mutex);
      m_cv.wait_for(lock, st, std::chrono::milliseconds(50), [&] { return !m_anchorsToSave.empty(); });
    }
  }

  std::mutex m_mutex;
  std::condition_variable_any m_cv;
  std::vector<std::pair<std::wstring, SavedAnchor>> m_anchorsToSave;
  std::optional<Config> m_pending;
  std::atomic<bool> m_hasPending{false};
  std::jthread m_thread;
};

// Intentionally leaked: no destructors may run at process exit (loader lock).
static ConfigWorker& g_configWorker = *new ConfigWorker;

// ---------------------------------------------------------------------------
// Per-session state

struct Panel {
  PanelConfig cfg;
  std::unique_ptr<WindowCapture> capture;
  XrSwapchain swapchain = XR_NULL_HANDLE;
  UINT scWidth = 0, scHeight = 0;
  std::vector<ComPtr<ID3D11RenderTargetView>> rtvs;
  ComPtr<ID3D11ShaderResourceView> srv;
  uint64_t srvGeneration = ~0ull;
  bool hasContent = false;      // swapchain holds a released image
  bool needsRedraw = false;     // look changed: re-render from the last capture
  bool renderPending = false;   // new content waiting to be drawn
  bool pendingRecenter = false;
  int64_t highlightUntil = 0;   // QPC time; border shown while selected via the control app
  XrPosef anchor{{0, 0, 0, 1}, {0, 0, 0}};
  XrSpace anchorSpace = XR_NULL_HANDLE;  // space the anchor is expressed in (null = LOCAL)
};

struct SessionState {
  XrSession handle = XR_NULL_HANDLE;
  ComPtr<ID3D11Device1> device;
  ComPtr<ID3D11DeviceContext1> ctx;
  ComPtr<ID3DDeviceContextState> ourState;
  Blitter blitter;

  XrSpace localSpace = XR_NULL_HANDLE;
  XrSpace viewSpace = XR_NULL_HANDLE;
  XrSpace stageSpace = XR_NULL_HANDLE;  // room-fixed; used for persisted recenter positions
  int64_t format = 0;
  bool srgbFormat = false;

  bool configLoaded = false;
  int recenterAllSeq = 0;
  int toggleAllSeq = 0;
  int selectSeq = 0;
  bool saveRecenter = false;
  std::vector<std::unique_ptr<Panel>> panels;
  std::vector<XrCompositionLayerQuad> quads;
  std::vector<const XrCompositionLayerBaseHeader*> layers;  // reused every frame

  bool visible = true;
  bool prevToggleKey = false, prevRecenterKey = false;
  int64_t nextStatus = 0;
};

static std::mutex g_sessionsMutex;
static auto& g_sessions = *new std::unordered_map<XrSession, std::unique_ptr<SessionState>>;

// ---------------------------------------------------------------------------
// Swapchains

static void DestroySwapchain(Panel& p) {
  p.rtvs.clear();
  p.srv.Reset();
  p.srvGeneration = ~0ull;
  if (p.swapchain != XR_NULL_HANDLE) g_next.DestroySwapchain(p.swapchain);
  p.swapchain = XR_NULL_HANDLE;
  p.scWidth = p.scHeight = 0;
  p.hasContent = false;
}

static bool EnsureSwapchain(SessionState& s, Panel& p, UINT w, UINT h) {
  if (p.swapchain != XR_NULL_HANDLE && p.scWidth == w && p.scHeight == h) return true;
  DestroySwapchain(p);

  XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
  ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
  ci.format = s.format;
  ci.sampleCount = 1;
  ci.width = w;
  ci.height = h;
  ci.faceCount = 1;
  ci.arraySize = 1;
  ci.mipCount = 1;
  XrResult r = g_next.CreateSwapchain(s.handle, &ci, &p.swapchain);
  if (XR_FAILED(r)) {
    Log("xrCreateSwapchain failed: %d", r);
    p.swapchain = XR_NULL_HANDLE;
    return false;
  }

  uint32_t count = 0;
  g_next.EnumerateSwapchainImages(p.swapchain, 0, &count, nullptr);
  std::vector<XrSwapchainImageD3D11KHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
  g_next.EnumerateSwapchainImages(p.swapchain, count, &count,
                                  reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
  for (auto& img : images) {
    D3D11_RENDER_TARGET_VIEW_DESC rd{};
    rd.Format = (DXGI_FORMAT)s.format;
    rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11RenderTargetView> rtv;
    if (FAILED(s.device->CreateRenderTargetView(img.texture, &rd, &rtv))) {
      Log("CreateRenderTargetView on swapchain image failed");
      DestroySwapchain(p);
      return false;
    }
    p.rtvs.push_back(rtv);
  }
  p.scWidth = w;
  p.scHeight = h;
  Log("Swapchain %ux%u for [%s]", w, h, Narrow(p.cfg.section).c_str());
  return true;
}

// Returns true when the panel was drawn (false = retry next frame).
static bool RenderPanel(SessionState& s, Panel& p) {
  WindowCapture& cap = *p.capture;
  if (!EnsureSwapchain(s, p, cap.Width(), cap.Height())) return false;

  if (p.srvGeneration != cap.Generation() || !p.srv) {
    p.srv.Reset();
    if (FAILED(s.device->CreateShaderResourceView(cap.Texture(), nullptr, &p.srv))) return false;
    p.srvGeneration = cap.Generation();
  }
  if (!cap.BeginRead()) return false;  // capture side busy: try again next frame

  bool drawn = false;
  uint32_t index = 0;
  XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
  if (XR_SUCCEEDED(g_next.AcquireSwapchainImage(p.swapchain, &ai, &index))) {
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    if (XR_SUCCEEDED(g_next.WaitSwapchainImage(p.swapchain, &wi)) && index < p.rtvs.size()) {
      BlitParams bp;
      bp.opacity = p.cfg.opacity;
      bp.useSourceAlpha = p.cfg.useWindowAlpha;
      bp.chromaKey = p.cfg.chromaKey;
      memcpy(bp.keyColor, p.cfg.keyColor, sizeof(bp.keyColor));
      bp.keyTolerance = p.cfg.keyTolerance;
      bp.srgbTarget = s.srgbFormat;
      if (p.highlightUntil != 0) bp.borderPx = std::max(4u, std::min(p.scWidth, p.scHeight) / 60);
      s.blitter.Draw(s.ctx.Get(), p.srv.Get(), p.rtvs[index].Get(), p.scWidth, p.scHeight, bp);
      drawn = true;
    }
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    g_next.ReleaseSwapchainImage(p.swapchain, &ri);
  }
  cap.EndRead();
  if (drawn) p.hasContent = true;
  return drawn;
}

// ---------------------------------------------------------------------------
// Config / panels

static void ApplyConfig(SessionState& s, const Config& cfg) {
  const bool firstLoad = !s.configLoaded;
  const bool recenterAll = !firstLoad && cfg.recenterAllSeq != s.recenterAllSeq;
  s.recenterAllSeq = cfg.recenterAllSeq;
  if (!firstLoad && cfg.toggleAllSeq != s.toggleAllSeq) {
    s.visible = !s.visible;
    Log("Panels %s (control app)", s.visible ? "shown" : "hidden");
  }
  s.toggleAllSeq = cfg.toggleAllSeq;
  s.saveRecenter = cfg.saveRecenter;
  const bool highlight = !firstLoad && cfg.selectSeq != s.selectSeq;
  s.selectSeq = cfg.selectSeq;
  s.configLoaded = true;

  std::vector<std::unique_ptr<Panel>> old = std::move(s.panels);
  s.panels.clear();
  for (const auto& pc : cfg.panels) {
    std::unique_ptr<Panel> panel;
    for (auto& o : old) {
      if (o && o->cfg.section == pc.section) {
        panel = std::move(o);
        break;
      }
    }
    if (panel) {
      if (!pc.SameTarget(panel->cfg) || !pc.enabled) {
        panel->capture.reset();
        DestroySwapchain(*panel);
      }
      if (pc.recenterSeq != panel->cfg.recenterSeq) panel->pendingRecenter = true;
      if (!pc.SameLook(panel->cfg)) panel->needsRedraw = true;
    } else {
      panel = std::make_unique<Panel>();
      const SavedAnchor& sa = pc.savedAnchor;
      if (cfg.saveRecenter && sa.valid && (!sa.stage || s.stageSpace != XR_NULL_HANDLE)) {
        // Restore the position from the last recenter (previous session).
        panel->anchor = {QuatAxis(0, 1, 0, sa.yaw), {sa.x, sa.y, sa.z}};
        panel->anchorSpace = sa.stage ? s.stageSpace : s.localSpace;
        Log("Restored saved position for [%s]", Narrow(pc.section).c_str());
      } else {
        panel->pendingRecenter = !firstLoad;  // panels added while playing appear in front of you
      }
    }
    if (recenterAll) panel->pendingRecenter = true;
    panel->cfg = pc;
    if (pc.enabled && !panel->capture) {
      panel->capture = std::make_unique<WindowCapture>(s.device.Get(), pc.windowTitle, pc.processName,
                                                       pc.maxFps);
    }
    if (panel->capture) panel->capture->SetMaxFps(pc.maxFps);
    if (highlight) {
      const bool selected = pc.section == cfg.selected;
      if (selected || panel->highlightUntil != 0) panel->needsRedraw = true;
      panel->highlightUntil = selected ? QpcNow() + QpcFreq() * 3 / 2 : 0;  // 1.5 s
    }
    s.panels.push_back(std::move(panel));
  }
  for (auto& o : old) {
    if (o) DestroySwapchain(*o);
  }
}

static void HandleHotkeys(SessionState& s) {
  const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
  const bool toggle = ctrl && (GetAsyncKeyState(VK_F12) & 0x8000);
  const bool recenter = ctrl && (GetAsyncKeyState(VK_F11) & 0x8000);
  if (toggle && !s.prevToggleKey) {
    s.visible = !s.visible;
    Log("Panels %s", s.visible ? "shown" : "hidden");
  }
  if (recenter && !s.prevRecenterKey) {
    for (auto& p : s.panels) p->pendingRecenter = true;
  }
  s.prevToggleKey = toggle;
  s.prevRecenterKey = recenter;
}

static void ProcessRecenters(SessionState& s, XrTime displayTime) {
  bool any = false;
  for (auto& p : s.panels) any |= p->pendingRecenter;
  if (!any) return;

  // Prefer STAGE (fixed to the room) so the saved position survives runtime recenters
  // and new sessions; fall back to LOCAL.
  const bool useStage = s.stageSpace != XR_NULL_HANDLE;
  XrSpace base = useStage ? s.stageSpace : s.localSpace;
  XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
  const XrSpaceLocationFlags need =
      XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
  if (XR_FAILED(g_next.LocateSpace(s.viewSpace, base, displayTime, &loc)) ||
      (loc.locationFlags & need) != need) {
    return;  // tracking not ready: retry next frame
  }
  // Keep only the heading so panels stay upright.
  XrVector3f fwd = Rotate(loc.pose.orientation, {0, 0, -1});
  float yaw = std::atan2(-fwd.x, -fwd.z);
  XrPosef anchor{QuatAxis(0, 1, 0, yaw), loc.pose.position};
  SavedAnchor saved;
  saved.valid = true;
  saved.stage = useStage;
  saved.x = anchor.position.x;
  saved.y = anchor.position.y;
  saved.z = anchor.position.z;
  saved.yaw = yaw;
  for (auto& p : s.panels) {
    if (!p->pendingRecenter) continue;
    p->anchor = anchor;
    p->anchorSpace = base;
    p->pendingRecenter = false;
    if (s.saveRecenter) g_configWorker.QueueAnchor(p->cfg.section, saved);  // file IO off this thread
    Log("Recentered [%s]", Narrow(p->cfg.section).c_str());
  }
}

static XrPosef PanelPose(const Panel& p) {
  const PanelConfig& c = p.cfg;
  XrQuaternionf local = QuatMul(QuatAxis(0, 1, 0, c.yawDeg * kDegToRad),
                                QuatAxis(1, 0, 0, c.pitchDeg * kDegToRad));
  XrVector3f off = Rotate(p.anchor.orientation, {c.x, c.y, c.z});
  XrPosef pose;
  pose.orientation = QuatMul(p.anchor.orientation, local);
  pose.position = {p.anchor.position.x + off.x, p.anchor.position.y + off.y,
                   p.anchor.position.z + off.z};
  return pose;
}

static void WriteStatus(SessionState& s) {
  if (!g_shared) return;
  int n = 0;
  for (auto& pp : s.panels) {
    if (n >= kMaxSharedPanels) break;
    const Panel& p = *pp;
    SharedPanelStatus& st = g_shared->panels[n++];
    wcsncpy_s(st.section, p.cfg.section.c_str(), _TRUNCATE);
    if (!p.cfg.enabled || !p.capture) st.state = kStateDisabled;
    else if (!p.capture->IsCapturing()) st.state = kStateSearching;
    else if (!p.hasContent) st.state = kStateCapturing;
    else st.state = s.visible ? kStateShowing : kStateHidden;
    st.width = (int32_t)p.scWidth;
    st.height = (int32_t)p.scHeight;
    st.fps = p.capture ? p.capture->MeasuredFps() : 0;
  }
  g_shared->panelCount = n;
  g_shared->heartbeat = (int64_t)GetTickCount64();
}

static void UpdateSession(SessionState& s, XrTime displayTime) {
  Config cfg;
  if (g_configWorker.TryTake(cfg)) ApplyConfig(s, cfg);
  HandleHotkeys(s);
  ProcessRecenters(s, displayTime);

  const int64_t now = QpcNow();
  if (now >= s.nextStatus) {
    s.nextStatus = now + QpcFreq() / 2;
    WriteStatus(s);
  }
  if (!s.visible) return;  // hidden: no capture or GPU work at all

  for (auto& pp : s.panels) {  // end of the selection highlight: redraw without border
    if (pp->highlightUntil != 0 && now >= pp->highlightUntil) {
      pp->highlightUntil = 0;
      pp->needsRedraw = true;
    }
  }

  bool anyRender = false;
  for (auto& pp : s.panels) {
    Panel& p = *pp;
    if (!p.capture) continue;
    if (p.capture->Update()) p.renderPending = true;
    if (!p.capture->Texture()) {
      if (p.swapchain != XR_NULL_HANDLE) DestroySwapchain(p);
      p.renderPending = false;
      continue;
    }
    if (p.needsRedraw || !p.hasContent) p.renderPending = true;
    anyRender |= p.renderPending;
  }
  if (!anyRender) return;  // nothing changed: no D3D work this frame

  // Work in our own clean D3D11 state and give the game its state back afterwards.
  ComPtr<ID3DDeviceContextState> previous;
  s.ctx->SwapDeviceContextState(s.ourState.Get(), &previous);
  for (auto& pp : s.panels) {
    Panel& p = *pp;
    if (!p.renderPending) continue;
    if (RenderPanel(s, p)) {
      p.renderPending = false;
      p.needsRedraw = false;
    }
  }
  s.ctx->SwapDeviceContextState(previous.Get(), nullptr);
}

static void AppendLayers(SessionState& s) {
  s.quads.clear();
  if (!s.visible) return;
  for (auto& pp : s.panels) {
    Panel& p = *pp;
    if (!p.capture || !p.hasContent || p.swapchain == XR_NULL_HANDLE) continue;
    XrCompositionLayerQuad q{XR_TYPE_COMPOSITION_LAYER_QUAD};
    q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;  // premultiplied
    q.space = p.anchorSpace != XR_NULL_HANDLE ? p.anchorSpace : s.localSpace;
    q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    q.subImage.swapchain = p.swapchain;
    q.subImage.imageRect = {{0, 0}, {(int32_t)p.scWidth, (int32_t)p.scHeight}};
    q.subImage.imageArrayIndex = 0;
    q.pose = PanelPose(p);
    q.size = {p.cfg.widthM, p.cfg.widthM * (float)p.scHeight / (float)p.scWidth};
    s.quads.push_back(q);
  }
  // s.quads is not modified again until the next frame, so these pointers stay valid.
  for (auto& q : s.quads) s.layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&q));
}

static void DestroySessionState(SessionState& s) {
  for (auto& p : s.panels) DestroySwapchain(*p);
  s.panels.clear();
  if (s.localSpace != XR_NULL_HANDLE) g_next.DestroySpace(s.localSpace);
  if (s.viewSpace != XR_NULL_HANDLE) g_next.DestroySpace(s.viewSpace);
  if (s.stageSpace != XR_NULL_HANDLE) g_next.DestroySpace(s.stageSpace);
  s.stageSpace = XR_NULL_HANDLE;
  s.localSpace = s.viewSpace = XR_NULL_HANDLE;
}

static bool InitSessionState(SessionState& s, ID3D11Device* device) {
  if (FAILED(device->QueryInterface(IID_PPV_ARGS(&s.device)))) {
    Log("ID3D11Device1 not available");
    return false;
  }
  s.device->GetImmediateContext1(&s.ctx);
  D3D_FEATURE_LEVEL fl = s.device->GetFeatureLevel();
  UINT flags = (s.device->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED)
                   ? D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED
                   : 0;
  HRESULT hr = s.device->CreateDeviceContextState(flags, &fl, 1, D3D11_SDK_VERSION,
                                                  __uuidof(ID3D11Device1), nullptr, &s.ourState);
  if (FAILED(hr)) {
    Log("CreateDeviceContextState failed: 0x%08X", (unsigned)hr);
    return false;
  }
  if (!s.blitter.Init(s.device.Get())) return false;

  XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
  rs.poseInReferenceSpace = {{0, 0, 0, 1}, {0, 0, 0}};
  rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
  if (XR_FAILED(g_next.CreateReferenceSpace(s.handle, &rs, &s.localSpace))) return false;
  rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
  if (XR_FAILED(g_next.CreateReferenceSpace(s.handle, &rs, &s.viewSpace))) return false;
  rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;  // optional: not every runtime has it
  if (XR_FAILED(g_next.CreateReferenceSpace(s.handle, &rs, &s.stageSpace))) {
    s.stageSpace = XR_NULL_HANDLE;
    Log("STAGE space not available - saved positions use LOCAL space");
  }

  uint32_t n = 0;
  g_next.EnumerateSwapchainFormats(s.handle, 0, &n, nullptr);
  std::vector<int64_t> formats(n);
  g_next.EnumerateSwapchainFormats(s.handle, n, &n, formats.data());
  const int64_t preferred[] = {DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
                               DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM};
  for (int64_t want : preferred) {
    for (int64_t f : formats) {
      if (f == want && s.format == 0) s.format = f;
    }
  }
  if (s.format == 0) {
    Log("No supported 8-bit swapchain format");
    return false;
  }
  s.srgbFormat = s.format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || s.format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
  Log("Using swapchain format %lld", (long long)s.format);
  s.quads.reserve(kMaxSharedPanels);
  s.layers.reserve(32);
  return true;
}

// ---------------------------------------------------------------------------
// Hooks

static XrResult XRAPI_CALL Hook_xrCreateSession(XrInstance instance, const XrSessionCreateInfo* info,
                                                XrSession* session) {
  XrResult r = g_next.CreateSession(instance, info, session);
  if (XR_FAILED(r)) return r;

  const XrGraphicsBindingD3D11KHR* d3d11 = nullptr;
  for (auto p = reinterpret_cast<const XrBaseInStructure*>(info->next); p; p = p->next) {
    if (p->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR) {
      d3d11 = reinterpret_cast<const XrGraphicsBindingD3D11KHR*>(p);
    }
  }
  if (!d3d11) {
    Log("Session is not D3D11 - layer inactive for this session");
    return r;
  }

  try {
    auto state = std::make_unique<SessionState>();
    state->handle = *session;
    if (InitSessionState(*state, d3d11->device)) {
      Log("D3D11 session ready");
      OpenShared();
      g_configWorker.Start();
      std::lock_guard lock(g_sessionsMutex);
      g_sessions[*session] = std::move(state);
    } else {
      Log("Session init failed - layer inactive");
      DestroySessionState(*state);
    }
  } catch (...) {
    Log("Exception during session init");
  }
  return r;
}

static XrResult XRAPI_CALL Hook_xrDestroySession(XrSession session) {
  std::unique_ptr<SessionState> state;
  bool last = false;
  {
    std::lock_guard lock(g_sessionsMutex);
    auto it = g_sessions.find(session);
    if (it != g_sessions.end()) {
      state = std::move(it->second);
      g_sessions.erase(it);
    }
    last = g_sessions.empty();
  }
  if (state) {
    DestroySessionState(*state);
    if (last) g_configWorker.Stop();
    if (g_shared) g_shared->panelCount = 0;
    Log("Session destroyed");
  }
  return g_next.DestroySession(session);
}

static XrResult XRAPI_CALL Hook_xrEndFrame(XrSession session, const XrFrameEndInfo* info) {
  SessionState* s = nullptr;
  {
    std::lock_guard lock(g_sessionsMutex);
    auto it = g_sessions.find(session);
    if (it != g_sessions.end()) s = it->second.get();
  }
  if (!s || !info) return g_next.EndFrame(session, info);

  s->layers.assign(info->layers, info->layers + info->layerCount);
  try {
    UpdateSession(*s, info->displayTime);
    AppendLayers(*s);
  } catch (...) {
    Log("Exception in xrEndFrame hook");
  }
  if (s->layers.size() == info->layerCount) return g_next.EndFrame(session, info);
  XrFrameEndInfo copy = *info;
  copy.layerCount = (uint32_t)s->layers.size();
  copy.layers = s->layers.data();
  return g_next.EndFrame(session, &copy);
}

// ---------------------------------------------------------------------------
// Loader interface

static XrResult XRAPI_CALL Layer_xrGetInstanceProcAddr(XrInstance instance, const char* name,
                                                       PFN_xrVoidFunction* function);

static XrResult XRAPI_CALL Layer_xrCreateApiLayerInstance(const XrInstanceCreateInfo* info,
                                                          const XrApiLayerCreateInfo* layerInfo,
                                                          XrInstance* instance) {
  LogInit(L"layer.log");
  if (!layerInfo || !layerInfo->nextInfo) return XR_ERROR_INITIALIZATION_FAILED;

  g_nextGetInstanceProcAddr = layerInfo->nextInfo->nextGetInstanceProcAddr;
  XrApiLayerCreateInfo chain = *layerInfo;
  chain.nextInfo = layerInfo->nextInfo->next;
  XrResult r = layerInfo->nextInfo->nextCreateApiLayerInstance(info, &chain, instance);
  if (XR_FAILED(r)) return r;

  Log("Layer loaded into \"%s\"", info->applicationInfo.applicationName);

#define LOAD(fn) g_nextGetInstanceProcAddr(*instance, "xr" #fn, reinterpret_cast<PFN_xrVoidFunction*>(&g_next.fn))
  LOAD(CreateSession);
  LOAD(DestroySession);
  LOAD(EndFrame);
  LOAD(EnumerateSwapchainFormats);
  LOAD(CreateSwapchain);
  LOAD(DestroySwapchain);
  LOAD(EnumerateSwapchainImages);
  LOAD(AcquireSwapchainImage);
  LOAD(WaitSwapchainImage);
  LOAD(ReleaseSwapchainImage);
  LOAD(CreateReferenceSpace);
  LOAD(DestroySpace);
  LOAD(LocateSpace);
#undef LOAD
  return r;
}

static XrResult XRAPI_CALL Layer_xrGetInstanceProcAddr(XrInstance instance, const char* name,
                                                       PFN_xrVoidFunction* function) {
  if (strcmp(name, "xrGetInstanceProcAddr") == 0) {
    *function = reinterpret_cast<PFN_xrVoidFunction>(Layer_xrGetInstanceProcAddr);
    return XR_SUCCESS;
  }
  XrResult r = g_nextGetInstanceProcAddr(instance, name, function);
  if (XR_FAILED(r)) return r;
  if (strcmp(name, "xrCreateSession") == 0) {
    *function = reinterpret_cast<PFN_xrVoidFunction>(Hook_xrCreateSession);
  } else if (strcmp(name, "xrDestroySession") == 0) {
    *function = reinterpret_cast<PFN_xrVoidFunction>(Hook_xrDestroySession);
  } else if (strcmp(name, "xrEndFrame") == 0) {
    *function = reinterpret_cast<PFN_xrVoidFunction>(Hook_xrEndFrame);
  }
  return r;
}

extern "C" __declspec(dllexport) XrResult XRAPI_CALL
xrNegotiateLoaderApiLayerInterface(const XrNegotiateLoaderInfo* loaderInfo, const char* layerName,
                                   XrNegotiateApiLayerRequest* request) {
  LogInit(L"layer.log");
  if (!loaderInfo || !request || loaderInfo->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
      request->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST) {
    return XR_ERROR_INITIALIZATION_FAILED;
  }
  if (layerName && strcmp(layerName, LAYER_NAME) != 0) return XR_ERROR_INITIALIZATION_FAILED;
  if (loaderInfo->minInterfaceVersion > XR_CURRENT_LOADER_API_LAYER_VERSION ||
      loaderInfo->maxInterfaceVersion < XR_CURRENT_LOADER_API_LAYER_VERSION) {
    return XR_ERROR_INITIALIZATION_FAILED;
  }
  request->layerInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
  request->layerApiVersion =
      loaderInfo->maxApiVersion < XR_CURRENT_API_VERSION ? loaderInfo->maxApiVersion : XR_CURRENT_API_VERSION;
  request->getInstanceProcAddr = Layer_xrGetInstanceProcAddr;
  request->createApiLayerInstance = Layer_xrCreateApiLayerInstance;
  return XR_SUCCESS;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(hinst);
    LogInit(L"layer.log");
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    Log("DLL loaded into %s", Narrow(exe).c_str());
  }
  return TRUE;
}
