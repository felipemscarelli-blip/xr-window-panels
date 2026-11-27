#pragma once
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace wp {

struct WindowInfo {
  HWND hwnd;
  std::wstring title;
  std::wstring process;  // executable file name, e.g. chrome.exe
};
std::vector<WindowInfo> ListWindows();
// Largest non-minimized window matching both filters (empty filter = any).
HWND FindWindowMatch(const std::wstring& titleSubstring, const std::wstring& processName);

// Captures one desktop window with Windows.Graphics.Capture.
//
// Performance design (the render thread must never stall):
//  * Window search, D3D device creation and capture-session startup run on a private
//    worker thread. The render thread only adopts a ready session.
//  * Capture uses its own D3D11 device (same GPU); frames are copied into a shared
//    keyed-mutex texture opened on the game's device. Mutex waits use timeout 0.
//  * Copies are rate-limited by maxFps; only new content triggers work.
class WindowCapture {
 public:
  WindowCapture(ID3D11Device* consumerDevice, std::wstring titleSubstring, std::wstring processName,
                int maxFps);
  ~WindowCapture();
  WindowCapture(const WindowCapture&) = delete;
  WindowCapture& operator=(const WindowCapture&) = delete;

  void SetMaxFps(int fps);

  // Render thread. Returns true when Texture() received new content. Never blocks.
  bool Update();

  // Lock the shared texture for the consumer device (non-blocking).
  bool BeginRead();
  void EndRead();

  ID3D11Texture2D* Texture() const { return m_consumerTex.Get(); }  // nullptr when no window
  UINT Width() const { return m_width; }
  UINT Height() const { return m_height; }
  uint64_t Generation() const { return m_generation; }
  bool IsCapturing() const { return m_active; }
  int MeasuredFps() const { return m_fps; }

 private:
  void WorkerLoop(std::stop_token st);
  bool CreateOwnDevice();
  void RequestStart();
  void CloseActive();
  bool EnsureSharedTexture(UINT w, UINT h);
  void ReleaseTextures();

  struct Impl;
  std::unique_ptr<Impl> m;

  const std::wstring m_title;
  const std::wstring m_process;
  std::atomic<int> m_maxFps;

  Microsoft::WRL::ComPtr<ID3D11Device> m_consumerDevice;
  Microsoft::WRL::ComPtr<ID3D11Device> m_ownDevice;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_ownCtx;

  // Render-thread state
  bool m_active = false;
  HWND m_hwnd = nullptr;
  int64_t m_lastCopy = 0, m_nextWindowCheck = 0, m_fpsWindowStart = 0;
  int m_fpsCount = 0, m_fps = 0;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> m_ownTex;
  Microsoft::WRL::ComPtr<IDXGIKeyedMutex> m_ownMutex;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> m_consumerTex;
  Microsoft::WRL::ComPtr<IDXGIKeyedMutex> m_consumerMutex;
  UINT m_width = 0, m_height = 0;
  uint64_t m_generation = 0;

  // Worker hand-off
  std::mutex m_mutex;
  std::condition_variable_any m_cv;
  bool m_needStart = true;            // guarded by m_mutex
  std::atomic<bool> m_hasPending{false};

  std::jthread m_worker;  // last member: stopped and joined first
};

}  // namespace wp
