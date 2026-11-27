#pragma once
#include <d3d11.h>
#include <wrl/client.h>

namespace wp {

struct BlitParams {
  float opacity = 1.0f;
  bool useSourceAlpha = false;
  bool chromaKey = false;
  float keyColor[3] = {0, 0, 0};
  float keyTolerance = 0.1f;
  bool srgbTarget = false;  // target is an *_SRGB format: convert to linear in the shader
};

// Draws a texture into a render target with opacity / chroma key, writing
// premultiplied alpha. The caller is responsible for saving/restoring context state.
class Blitter {
 public:
  bool Init(ID3D11Device* device);
  void Draw(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* src, ID3D11RenderTargetView* dst,
            UINT width, UINT height, const BlitParams& p);

 private:
  Microsoft::WRL::ComPtr<ID3D11VertexShader> m_vs;
  Microsoft::WRL::ComPtr<ID3D11PixelShader> m_ps;
  Microsoft::WRL::ComPtr<ID3D11SamplerState> m_sampler;
  Microsoft::WRL::ComPtr<ID3D11Buffer> m_cb;
  Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_rs;
};

}  // namespace wp
