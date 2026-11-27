#include <Unknwn.h>  // must precede C++/WinRT for classic COM interop

#include "capture.h"

#include "log.h"

#include <d3d11_1.h>
#include <dwmapi.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Graphics.DirectX.h>

#include <algorithm>
#include <chrono>
#include <cwctype>

using Microsoft::WRL::ComPtr;
namespace wgc = winrt::Windows::Graphics::Capture;
namespace wgdx = winrt::Windows::Graphics::DirectX;
namespace wgd3d = winrt::Windows::Graphics::DirectX::Direct3D11;

namespace wp {

static constexpr auto kPixelFormat = wgdx::DirectXPixelFormat::B8G8R8A8UIntNormalized;

struct Session {
  wgc::GraphicsCaptureItem item{nullptr};
  wgc::Direct3D11CaptureFramePool pool{nullptr};
  wgc::GraphicsCaptureSession session{nullptr};
  winrt::Windows::Graphics::SizeInt32 poolSize{};
  HWND hwnd = nullptr;

  void Close() {
    try {
      if (session) session.Close();
      if (pool) pool.Close();
    } catch (...) {
    }
    session = nullptr;
    pool = nullptr;
    item = nullptr;
    hwnd = nullptr;
  }
};

struct WindowCapture::Impl {
  wgd3d::IDirect3DDevice device{nullptr};
  Session pending;  // created by the worker, guarded by m_mutex
  Session active;   // render thread only
  wgc::Direct3D11CaptureFrame held{nullptr};  // newest frame waiting for its fps slot
};

// ---------------------------------------------------------------------------
// Timing

static int64_t QpcFreq() {
  static const int64_t f = [] {
    LARGE_INTEGER v;
    QueryPerformanceFrequency(&v);
    return v.QuadPart;
  }();
  return f;
}
static int64_t Now() {
  LARGE_INTEGER v;
  QueryPerformanceCounter(&v);
  return v.QuadPart;
}

// ---------------------------------------------------------------------------
// Window enumeration

static std::wstring Lower(std::wstring s) {
  std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return (wchar_t)towlower(c); });
  return s;
}

static std::wstring ProcessNameOf(HWND h) {
  DWORD pid = 0;
  GetWindowThreadProcessId(h, &pid);
  HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!proc) return {};
  wchar_t path[MAX_PATH];
  DWORD len = MAX_PATH;
  std::wstring name;
  if (QueryFullProcessImageNameW(proc, 0, path, &len)) {
    name = path;
    size_t slash = name.find_last_of(L"\\/");
    if (slash != std::wstring::npos) name = name.substr(slash + 1);
  }
  CloseHandle(proc);
  return name;
}

std::vector<WindowInfo> ListWindows() {
  std::vector<WindowInfo> out;
  EnumWindows(
      [](HWND h, LPARAM lp) -> BOOL {
        if (!IsWindowVisible(h)) return TRUE;
        BOOL cloaked = FALSE;
        DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
        if (cloaked) return TRUE;
        int len = GetWindowTextLengthW(h);
        if (len <= 0) return TRUE;
        std::wstring t(len + 1, L'\0');
        t.resize(GetWindowTextW(h, t.data(), len + 1));
        reinterpret_cast<std::vector<WindowInfo>*>(lp)->push_back({h, t, ProcessNameOf(h)});
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&out));
  return out;
}

HWND FindWindowMatch(const std::wstring& titleSubstring, const std::wstring& processName) {
  const std::wstring needle = Lower(titleSubstring);
  std::wstring proc = Lower(processName);
  if (!proc.empty() && proc.find(L'.') == std::wstring::npos) proc += L".exe";

  HWND best = nullptr;
  long long bestArea = 0;
  for (auto& w : ListWindows()) {
    if (!needle.empty() && Lower(w.title).find(needle) == std::wstring::npos) continue;
    if (!proc.empty() && Lower(w.process) != proc) continue;
    if (IsIconic(w.hwnd)) continue;  // minimized windows deliver no frames
    RECT r{};
    GetWindowRect(w.hwnd, &r);
    long long area = (long long)(r.right - r.left) * (r.bottom - r.top);
    if (!best || area > bestArea) {
      best = w.hwnd;
      bestArea = area;
    }
  }
  return best;
}

// ---------------------------------------------------------------------------

WindowCapture::WindowCapture(ID3D11Device* consumerDevice, std::wstring titleSubstring,
                             std::wstring processName, int maxFps)
    : m(std::make_unique<Impl>()),
      m_title(std::move(titleSubstring)),
      m_process(std::move(processName)),
      m_maxFps(maxFps),
      m_consumerDevice(consumerDevice) {
  m_worker = std::jthread([this](std::stop_token st) { WorkerLoop(st); });
}

WindowCapture::~WindowCapture() {
  m_worker.request_stop();
  m_cv.notify_all();
  if (m_worker.joinable()) m_worker.join();
  m->held = nullptr;
  CloseActive();
  m->pending.Close();
}

void WindowCapture::SetMaxFps(int fps) { m_maxFps = fps; }

bool WindowCapture::CreateOwnDevice() {
  if (m_ownDevice) return true;
  // Same adapter as the consumer, otherwise the shared texture cannot be opened.
  ComPtr<IDXGIDevice> consumerDxgi;
  ComPtr<IDXGIAdapter> adapter;
  if (FAILED(m_consumerDevice.As(&consumerDxgi)) || FAILED(consumerDxgi->GetAdapter(&adapter))) {
    Log("Could not get the game's DXGI adapter");
    return false;
  }
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                      D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
  ComPtr<ID3D11Device> dev;
  ComPtr<ID3D11DeviceContext> ctx;
  HRESULT hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                 D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, _countof(levels),
                                 D3D11_SDK_VERSION, &dev, nullptr, &ctx);
  if (FAILED(hr)) {
    Log("D3D11CreateDevice for capture failed: 0x%08X", (unsigned)hr);
    return false;
  }
  // Capture work is low priority compared to the game.
  ComPtr<IDXGIDevice> dxgi;
  dev.As(&dxgi);
  dxgi->SetGPUThreadPriority(-1);
  try {
    winrt::com_ptr<::IInspectable> insp;
    winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(), insp.put()));
    m->device = insp.as<wgd3d::IDirect3DDevice>();
  } catch (winrt::hresult_error const& e) {
    Log("WinRT device wrap failed: 0x%08X", (unsigned)e.code());
    return false;
  }
  m_ownDevice = dev;
  m_ownCtx = ctx;
  return true;
}

void WindowCapture::WorkerLoop(std::stop_token st) {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
  // No init_apartment/uninit_apartment here: uninitializing COM when this thread exits would
  // unload GraphicsCapture.dll while the render thread still owns capture objects. C++/WinRT
  // joins the process-wide MTA implicitly (CoIncrementMTAUsage), which is what we want.
  bool loggedMissing = false;

  while (!st.stop_requested()) {
    {
      std::unique_lock lock(m_mutex);
      m_cv.wait(lock, st, [&] { return m_needStart; });
      if (st.stop_requested()) break;
    }

    HWND h = FindWindowMatch(m_title, m_process);
    if (h && CreateOwnDevice()) {
      Session s;
      try {
        auto interop =
            winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        winrt::check_hresult(interop->CreateForWindow(h, winrt::guid_of<wgc::GraphicsCaptureItem>(),
                                                      winrt::put_abi(s.item)));
        s.poolSize = s.item.Size();
        s.pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(m->device, kPixelFormat, 2,
                                                                     s.poolSize);
        s.session = s.pool.CreateCaptureSession(s.item);
        try { s.session.IsCursorCaptureEnabled(false); } catch (...) {}
        try { s.session.IsBorderRequired(false); } catch (...) {}  // yellow border (Win11)
        s.session.StartCapture();
        s.hwnd = h;

        wchar_t title[256] = {};
        GetWindowTextW(h, title, 256);
        Log("Capturing \"%s\" (%dx%d)", Narrow(title).c_str(), s.poolSize.Width, s.poolSize.Height);
        loggedMissing = false;

        std::lock_guard lock(m_mutex);
        m->pending.Close();
        m->pending = s;
        m_needStart = false;
        m_hasPending = true;
        continue;
      } catch (winrt::hresult_error const& e) {
        Log("Capture start failed: 0x%08X %s", (unsigned)e.code(),
            Narrow(std::wstring(e.message())).c_str());
        s.Close();
      }
    } else if (!loggedMissing) {
      Log("Window not found yet (title \"%s\", process \"%s\")", Narrow(m_title).c_str(),
          Narrow(m_process).c_str());
      loggedMissing = true;
    }

    // Retry in 1 s (wakes immediately on stop).
    std::unique_lock lock(m_mutex);
    m_cv.wait_for(lock, st, std::chrono::seconds(1), [] { return false; });
  }
}

void WindowCapture::RequestStart() {
  {
    std::lock_guard lock(m_mutex);
    m_needStart = true;
  }
  m_cv.notify_all();
}

void WindowCapture::CloseActive() {
  m->active.Close();
  m_active = false;
  m_hwnd = nullptr;
}

void WindowCapture::ReleaseTextures() {
  m_ownTex.Reset();
  m_ownMutex.Reset();
  m_consumerTex.Reset();
  m_consumerMutex.Reset();
  m_width = m_height = 0;
  ++m_generation;
}

bool WindowCapture::EnsureSharedTexture(UINT w, UINT h) {
  if (m_ownTex && w == m_width && h == m_height) return true;
  ReleaseTextures();

  D3D11_TEXTURE2D_DESC d{};
  d.Width = w;
  d.Height = h;
  d.MipLevels = 1;
  d.ArraySize = 1;
  d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  d.SampleDesc.Count = 1;
  d.Usage = D3D11_USAGE_DEFAULT;
  d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
  d.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
  HRESULT hr = m_ownDevice->CreateTexture2D(&d, nullptr, &m_ownTex);
  if (FAILED(hr)) {
    Log("CreateTexture2D (shared) failed: 0x%08X", (unsigned)hr);
    return false;
  }
  m_ownTex.As(&m_ownMutex);

  ComPtr<IDXGIResource1> res;
  HANDLE handle = nullptr;
  m_ownTex.As(&res);
  hr = res->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                               nullptr, &handle);
  if (FAILED(hr)) {
    Log("CreateSharedHandle failed: 0x%08X", (unsigned)hr);
    ReleaseTextures();
    return false;
  }
  ComPtr<ID3D11Device1> consumer1;
  hr = m_consumerDevice.As(&consumer1);
  if (SUCCEEDED(hr)) hr = consumer1->OpenSharedResource1(handle, IID_PPV_ARGS(&m_consumerTex));
  CloseHandle(handle);
  if (FAILED(hr)) {
    Log("OpenSharedResource1 failed: 0x%08X", (unsigned)hr);
    ReleaseTextures();
    return false;
  }
  m_consumerTex.As(&m_consumerMutex);
  m_width = w;
  m_height = h;
  return true;
}

bool WindowCapture::Update() {
  const int64_t now = Now();

  // Adopt a session the worker finished starting (never wait for the worker).
  if (m_hasPending.load(std::memory_order_acquire)) {
    std::unique_lock lock(m_mutex, std::try_to_lock);
    if (lock.owns_lock()) {
      CloseActive();
      m->active = m->pending;
      m->pending = Session{};
      m_hasPending = false;
      m_hwnd = m->active.hwnd;
      m_active = true;
      m_nextWindowCheck = now + QpcFreq() / 2;
    }
  }
  if (!m_active) return false;

  // Cheap liveness check, twice per second.
  if (now >= m_nextWindowCheck) {
    m_nextWindowCheck = now + QpcFreq() / 2;
    if (!IsWindow(m_hwnd)) {
      Log("Window closed - searching again");
      m->held = nullptr;
      CloseActive();
      ReleaseTextures();
      RequestStart();
      return false;
    }
  }

  try {
    // Keep only the newest frame.
    for (auto f = m->active.pool.TryGetNextFrame(); f; f = m->active.pool.TryGetNextFrame()) {
      if (m->held) m->held.Close();
      m->held = f;
    }
    if (!m->held) return false;

    const int fps = m_maxFps.load(std::memory_order_relaxed);
    if (fps > 0 && m_lastCopy != 0 && now - m_lastCopy < QpcFreq() / fps) return false;

    auto cs = m->held.ContentSize();
    auto access =
        m->held.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    ComPtr<ID3D11Texture2D> src;
    winrt::check_hresult(access->GetInterface(IID_PPV_ARGS(&src)));
    D3D11_TEXTURE2D_DESC sd;
    src->GetDesc(&sd);
    UINT w = std::min<UINT>((UINT)std::max(cs.Width, 0), sd.Width);
    UINT h = std::min<UINT>((UINT)std::max(cs.Height, 0), sd.Height);

    bool copied = false;
    if (w > 0 && h > 0 && EnsureSharedTexture(w, h) && m_ownMutex->AcquireSync(0, 0) == S_OK) {
      D3D11_BOX box{0, 0, 0, w, h, 1};
      m_ownCtx->CopySubresourceRegion(m_ownTex.Get(), 0, 0, 0, 0, src.Get(), 0, &box);
      m_ownMutex->ReleaseSync(0);
      m_ownCtx->Flush();
      copied = true;
    }
    if (!copied && w > 0 && h > 0 && m_ownTex) return false;  // mutex busy: retry next frame

    m->held.Close();
    m->held = nullptr;

    // Window was resized: resize the pool (takes effect on following frames).
    if (cs.Width != m->active.poolSize.Width || cs.Height != m->active.poolSize.Height) {
      m->active.poolSize = cs;
      m->active.pool.Recreate(m->device, kPixelFormat, 2, cs);
    }

    if (copied) {
      m_lastCopy = now;
      ++m_fpsCount;
      if (now - m_fpsWindowStart >= QpcFreq()) {
        m_fps = m_fpsCount;
        m_fpsCount = 0;
        m_fpsWindowStart = now;
      }
    }
    return copied;
  } catch (winrt::hresult_error const& e) {
    Log("Capture frame error: 0x%08X - restarting", (unsigned)e.code());
    m->held = nullptr;
    CloseActive();
    RequestStart();
    return false;
  }
}

bool WindowCapture::BeginRead() {
  return m_consumerMutex && m_consumerMutex->AcquireSync(0, 0) == S_OK;
}

void WindowCapture::EndRead() {
  if (m_consumerMutex) m_consumerMutex->ReleaseSync(0);
}

}  // namespace wp
