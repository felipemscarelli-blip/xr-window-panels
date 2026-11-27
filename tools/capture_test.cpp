// Desktop test for the capture path used by the layer.
//
//   capture_test --list                      list capturable windows
//   capture_test "<title part>" [out.bmp] [--chroma #RRGGBB]
//
// Uses a separate "consumer" D3D11 device (like the game's), so it exercises the
// shared-texture handoff and the blit shader, then writes the result to a BMP.

#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <cstdio>
#include <string>
#include <vector>

#include "blitter.h"
#include "capture.h"
#include "config.h"
#include "log.h"

using Microsoft::WRL::ComPtr;
using namespace wp;

static bool WriteBmp(const std::wstring& path, const uint8_t* data, UINT pitch, UINT w, UINT h) {
  BITMAPFILEHEADER fh{};
  BITMAPINFOHEADER ih{};
  ih.biSize = sizeof(ih);
  ih.biWidth = (LONG)w;
  ih.biHeight = -(LONG)h;  // top-down
  ih.biPlanes = 1;
  ih.biBitCount = 32;
  ih.biCompression = BI_RGB;
  fh.bfType = 0x4D42;
  fh.bfOffBits = sizeof(fh) + sizeof(ih);
  fh.bfSize = fh.bfOffBits + w * h * 4;
  FILE* f = nullptr;
  if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) return false;
  fwrite(&fh, sizeof(fh), 1, f);
  fwrite(&ih, sizeof(ih), 1, f);
  for (UINT y = 0; y < h; ++y) fwrite(data + (size_t)y * pitch, 4, w, f);
  fclose(f);
  return true;
}

int wmain(int argc, wchar_t** argv) {
  SetConsoleOutputCP(CP_UTF8);
  LogInit(L"capture_test.log", true);

  if (argc >= 2 && std::wstring(argv[1]) == L"--config") {
    Config cfg = LoadConfig(ConfigPath());
    printf("%s: RecenterAll=%d, %zu panel(s)\n", Narrow(ConfigPath()).c_str(), cfg.recenterAllSeq,
           cfg.panels.size());
    for (auto& p : cfg.panels) {
      printf("  [%s] window=\"%s\" process=\"%s\" enabled=%d pos=(%.3f %.3f %.3f) yaw=%.1f pitch=%.1f "
             "width=%.3f opacity=%.2f chroma=%d key=(%.2f %.2f %.2f) tol=%.2f fps=%d recenter=%d\n",
             Narrow(p.section).c_str(), Narrow(p.windowTitle).c_str(), Narrow(p.processName).c_str(),
             p.enabled, p.x, p.y, p.z, p.yawDeg, p.pitchDeg, p.widthM, p.opacity, p.chromaKey,
             p.keyColor[0], p.keyColor[1], p.keyColor[2], p.keyTolerance, p.maxFps, p.recenterSeq);
    }
    return 0;
  }

  if (argc < 2 || std::wstring(argv[1]) == L"--list") {
    printf("Windows:\n");
    for (auto& w : ListWindows()) {
      RECT r{};
      GetWindowRect(w.hwnd, &r);
      printf("  %-60s %ldx%ld%s\n", Narrow(w.title).c_str(), r.right - r.left, r.bottom - r.top,
             IsIconic(w.hwnd) ? "  [minimizada]" : "");
    }
    if (argc < 2) printf("\nUsage: capture_test \"<title part>\" [out.bmp] [--chroma #RRGGBB]\n");
    return 0;
  }

  std::wstring title = argv[1];
  std::wstring out = L"capture.bmp";
  BlitParams bp;
  for (int i = 2; i < argc; ++i) {
    std::wstring a = argv[i];
    if (a == L"--chroma" && i + 1 < argc) {
      std::wstring c = argv[++i];
      unsigned long rgb = wcstoul(c.c_str() + (c[0] == L'#' ? 1 : 0), nullptr, 16);
      bp.chromaKey = true;
      bp.keyColor[0] = ((rgb >> 16) & 0xFF) / 255.0f;
      bp.keyColor[1] = ((rgb >> 8) & 0xFF) / 255.0f;
      bp.keyColor[2] = (rgb & 0xFF) / 255.0f;
    } else if (a == L"--border" && i + 1 < argc) {
      bp.borderPx = _wtoi(argv[++i]);
    } else {
      out = a;
    }
  }

  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> ctx;
  HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                 D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
                                 &device, nullptr, &ctx);
  if (FAILED(hr)) {
    printf("D3D11CreateDevice failed 0x%08X\n", (unsigned)hr);
    return 1;
  }

  WindowCapture cap(device.Get(), title, L"", 0);
  bool got = false;
  for (int i = 0; i < 100 && !got; ++i) {  // up to ~5 s (search/start run on a worker thread)
    got = cap.Update();
    if (!got) Sleep(50);
  }
  if (!got) {
    printf("FAIL: no frame captured for \"%s\" (is the window open?)\n", Narrow(title).c_str());
    return 2;
  }
  UINT w = cap.Width(), h = cap.Height();

  Blitter blitter;
  if (!blitter.Init(device.Get())) {
    printf("FAIL: blitter init\n");
    return 3;
  }

  D3D11_TEXTURE2D_DESC td{};
  td.Width = w;
  td.Height = h;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_RENDER_TARGET;
  ComPtr<ID3D11Texture2D> target;
  ComPtr<ID3D11RenderTargetView> rtv;
  ComPtr<ID3D11ShaderResourceView> srv;
  device->CreateTexture2D(&td, nullptr, &target);
  device->CreateRenderTargetView(target.Get(), nullptr, &rtv);
  if (FAILED(device->CreateShaderResourceView(cap.Texture(), nullptr, &srv))) {
    printf("FAIL: SRV on shared texture\n");
    return 4;
  }

  if (!cap.BeginRead()) {
    printf("FAIL: keyed mutex\n");
    return 5;
  }
  blitter.Draw(ctx.Get(), srv.Get(), rtv.Get(), w, h, bp);
  cap.EndRead();

  td.Usage = D3D11_USAGE_STAGING;
  td.BindFlags = 0;
  td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ComPtr<ID3D11Texture2D> staging;
  device->CreateTexture2D(&td, nullptr, &staging);
  ctx->CopyResource(staging.Get(), target.Get());
  D3D11_MAPPED_SUBRESOURCE map;
  if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map))) {
    printf("FAIL: map\n");
    return 6;
  }
  bool ok = WriteBmp(out, (const uint8_t*)map.pData, map.RowPitch, w, h);
  ctx->Unmap(staging.Get(), 0);

  printf("%s: %ux%u -> %s\n", ok ? "OK" : "FAIL writing file", w, h, Narrow(out).c_str());
  return ok ? 0 : 7;
}
