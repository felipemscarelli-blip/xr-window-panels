#include "blitter.h"

#include "log.h"

#include <d3dcompiler.h>

#include <cstring>

using Microsoft::WRL::ComPtr;

namespace wp {

static const char kShader[] = R"(
Texture2D src : register(t0);
SamplerState smp : register(s0);
cbuffer Params : register(b0) {
  float3 keyColor; float keyTolerance;
  float opacity; float chromaKey; float srgbTarget; float useSourceAlpha;
  float2 border; float2 pad;  // border thickness in UV units (0 = none)
};
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

VSOut VSMain(uint id : SV_VertexID) {
  VSOut o;
  o.uv = float2((id << 1) & 2, id & 2);
  o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
  return o;
}

float3 ToLinear(float3 c) {
  return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

float4 PSMain(VSOut i) : SV_Target {
  if (border.x > 0 && (any(i.uv < border) || any(i.uv > 1 - border))) {
    return float4(1.0, 0.75, 0.0, 1.0);  // highlight (amber), opaque
  }
  float4 c = src.Sample(smp, i.uv);
  float a = opacity;
  if (useSourceAlpha > 0.5) a *= c.a;
  if (chromaKey > 0.5) {
    float d = distance(c.rgb, keyColor);
    a *= saturate((d - keyTolerance) / max(keyTolerance * 0.5, 0.001));
  }
  float3 rgb = srgbTarget > 0.5 ? ToLinear(c.rgb) : c.rgb;
  return float4(rgb * a, a);
}
)";

struct alignas(16) CBData {
  float keyColor[3];
  float keyTolerance;
  float opacity;
  float chromaKey;
  float srgbTarget;
  float useSourceAlpha;
  float border[2];
  float pad[2];
};

static ComPtr<ID3DBlob> Compile(const char* entry, const char* target) {
  ComPtr<ID3DBlob> code, errors;
  HRESULT hr = D3DCompile(kShader, strlen(kShader), "blit", nullptr, nullptr, entry, target,
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
  if (FAILED(hr)) {
    Log("Shader compile failed (%s): %s", entry,
        errors ? (const char*)errors->GetBufferPointer() : "?");
    return nullptr;
  }
  return code;
}

bool Blitter::Init(ID3D11Device* device) {
  auto vs = Compile("VSMain", "vs_4_0");
  auto ps = Compile("PSMain", "ps_4_0");
  if (!vs || !ps) return false;
  if (FAILED(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &m_vs)))
    return false;
  if (FAILED(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &m_ps)))
    return false;

  D3D11_SAMPLER_DESC sd{};
  sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sd.MaxLOD = D3D11_FLOAT32_MAX;
  if (FAILED(device->CreateSamplerState(&sd, &m_sampler))) return false;

  D3D11_BUFFER_DESC bd{};
  bd.ByteWidth = sizeof(CBData);
  bd.Usage = D3D11_USAGE_DEFAULT;
  bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  if (FAILED(device->CreateBuffer(&bd, nullptr, &m_cb))) return false;

  D3D11_RASTERIZER_DESC rd{};
  rd.FillMode = D3D11_FILL_SOLID;
  rd.CullMode = D3D11_CULL_NONE;
  rd.DepthClipEnable = TRUE;
  if (FAILED(device->CreateRasterizerState(&rd, &m_rs))) return false;
  return true;
}

void Blitter::Draw(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* src,
                   ID3D11RenderTargetView* dst, UINT width, UINT height, const BlitParams& p) {
  CBData cb{};
  memcpy(cb.keyColor, p.keyColor, sizeof(cb.keyColor));
  cb.keyTolerance = p.keyTolerance;
  cb.opacity = p.opacity;
  cb.chromaKey = p.chromaKey ? 1.0f : 0.0f;
  cb.srgbTarget = p.srgbTarget ? 1.0f : 0.0f;
  cb.useSourceAlpha = p.useSourceAlpha ? 1.0f : 0.0f;
  if (p.borderPx > 0 && width > 0 && height > 0) {
    cb.border[0] = (float)p.borderPx / (float)width;
    cb.border[1] = (float)p.borderPx / (float)height;
  }
  ctx->UpdateSubresource(m_cb.Get(), 0, nullptr, &cb, 0, 0);

  D3D11_VIEWPORT vp{0, 0, (float)width, (float)height, 0, 1};
  ctx->OMSetRenderTargets(1, &dst, nullptr);
  ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
  ctx->OMSetDepthStencilState(nullptr, 0);
  ctx->RSSetViewports(1, &vp);
  ctx->RSSetState(m_rs.Get());
  ctx->IASetInputLayout(nullptr);
  ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ctx->VSSetShader(m_vs.Get(), nullptr, 0);
  ctx->PSSetShader(m_ps.Get(), nullptr, 0);
  ctx->PSSetShaderResources(0, 1, &src);
  ctx->PSSetSamplers(0, 1, m_sampler.GetAddressOf());
  ctx->PSSetConstantBuffers(0, 1, m_cb.GetAddressOf());
  ctx->Draw(3, 0);

  ID3D11ShaderResourceView* nullSrv = nullptr;
  ctx->PSSetShaderResources(0, 1, &nullSrv);
  ctx->OMSetRenderTargets(0, nullptr, nullptr);
}

}  // namespace wp
