// Re:Fine 6b+6c: see rml_d3d12.h. Written for this port on the D3D12 API alone.
#include "rml_d3d12.h"

#include <RmlUi/Core.h>
#include <RmlUi/Core/Log.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cstring>
#include <string>

// Re:Fine 6b+6c p3: the start screen's two pictures (PIC1.PNG, ICON0.PNG) are read at run time from
// the player's own disc folder, by a "disc:<name>" source. stb_image: public domain, third_party/stb.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "../../third_party/stb/stb_image.h"
extern "C" const char* ppu_vfs_root;

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace {

const char kShader[] = R"(
cbuffer C : register(b0) { float4x4 M; float2 T; };
struct VI { float2 p : POSITION; float4 c : COLOR; float2 uv : TEXCOORD; };
struct VO { float4 p : SV_Position; float4 c : COLOR; float2 uv : TEXCOORD; };
VO vs(VI i) { VO o; o.p = mul(M, float4(i.p + T, 0, 1)); o.c = i.c; o.uv = i.uv; return o; }
Texture2D tex : register(t0);
SamplerState smp : register(s0);
float4 ps(VO i) : SV_Target { return i.c * tex.Sample(smp, i.uv); }

// Gradients: what RmlUi asks for with linear-, radial- and conic-gradient decorators. The vertex
// texture coordinates are the element's own pixels; t along the gradient picks between the stops
// (premultiplied colours, eased between neighbours as in RmlUi's reference renderer).
cbuffer G : register(b1) { float4 PV; int4 FN; float4 POS[4]; float4 COL[16]; };
float stop_pos(int k) { return POS[k >> 2][k & 3]; }
float4 ps_grad(VO i) : SV_Target {
    float2 P = PV.xy, V = PV.zw;
    int f = FN.x, n = FN.y;
    float t;
    if (f == 0 || f == 3) { float2 d = i.uv - P; t = dot(V, d) / dot(V, V); }
    else if (f == 1 || f == 4) { t = length(V * (i.uv - P)); }
    else { float2 d = i.uv - P; float2 r = float2(V.x * d.x + V.y * d.y, -V.y * d.x + V.x * d.y); t = 0.5 + atan2(-r.x, r.y) / 6.2831853; }
    if (f >= 3) { float t0 = stop_pos(0), t1 = stop_pos(n - 1); t = t0 + (t - t0) - (t1 - t0) * floor((t - t0) / (t1 - t0)); }
    float4 c = COL[0];
    for (int k = 1; k < n; k++) c = lerp(c, COL[k], smoothstep(stop_pos(k - 1), stop_pos(k), t));
    return i.c * c;
}
)";

struct GradShader { float pv[4]; int fn[4]; float pos[16]; float col[16][4]; };   // the cbuffer G, as is
static_assert(sizeof(GradShader) == 352, "cbuffer G layout");

constexpr int kSlots = 256;
constexpr size_t kRingStart = 1u << 20;   // 1 MiB a frame: the menu is a few thousand vertices

ID3DBlob* compile(const char* entry, const char* target)
{
    ID3DBlob* code = nullptr; ID3DBlob* err = nullptr;
    const HRESULT hr = D3DCompile(kShader, sizeof kShader - 1, "port_menu", nullptr, nullptr, entry, target,
                                  D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (FAILED(hr)) {
        fprintf(stderr, "[menu] shader %s failed: %s\n", entry, err ? (const char*)err->GetBufferPointer() : "?");
        if (err) err->Release();
        return nullptr;
    }
    if (err) err->Release();
    return code;
}

ID3D12Resource* upload_buffer(ID3D12Device* dev, size_t size)
{
    D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC d = {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = size; d.Height = 1; d.DepthOrArraySize = 1;
    d.MipLevels = 1; d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* r = nullptr;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_GENERIC_READ,
                                            nullptr, IID_PPV_ARGS(&r))))
        return nullptr;
    return r;
}

} // namespace

bool RmlD3D12::Init(ID3D12Device* dev, uint32_t rtv_format, uint32_t frame_count)
{
    dev_ = dev;
    frames_ = frame_count;
    // Root: 20 constants (matrix and translation) for the VS, one SRV table and a static sampler.
    D3D12_DESCRIPTOR_RANGE range = {};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; range.NumDescriptors = 1;
    D3D12_ROOT_PARAMETER p[3] = {};
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p[0].Constants.Num32BitValues = 20; p[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p[1].DescriptorTable.NumDescriptorRanges = 1; p[1].DescriptorTable.pDescriptorRanges = &range;
    p[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;   // the gradient's constants, in the frame's ring
    p[2].Descriptor.ShaderRegister = 1; p[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC ss = {};
    ss.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    ss.MaxLOD = D3D12_FLOAT32_MAX; ss.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rd = {};
    rd.NumParameters = 3; rd.pParameters = p; rd.NumStaticSamplers = 1; rd.pStaticSamplers = &ss;
    rd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ID3DBlob* sig = nullptr; ID3DBlob* err = nullptr;
    if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) {
        fprintf(stderr, "[menu] root signature: %s\n", err ? (const char*)err->GetBufferPointer() : "?");
        return false;
    }
    const HRESULT hr = dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&rs_));
    sig->Release();
    if (FAILED(hr)) return false;

    ID3DBlob* vs = compile("vs", "vs_5_0");
    ID3DBlob* ps = compile("ps", "ps_5_0");
    if (!vs || !ps) return false;
    const D3D12_INPUT_ELEMENT_DESC il[3] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };
    static_assert(sizeof(Rml::Vertex) == 20, "Rml::Vertex layout");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = rs_;
    pd.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pd.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pd.InputLayout = {il, 3};
    // RmlUi hands over premultiplied colours.
    D3D12_RENDER_TARGET_BLEND_DESC& b = pd.BlendState.RenderTarget[0];
    b.BlendEnable = TRUE;
    b.SrcBlend = D3D12_BLEND_ONE; b.DestBlend = D3D12_BLEND_INV_SRC_ALPHA; b.BlendOp = D3D12_BLEND_OP_ADD;
    b.SrcBlendAlpha = D3D12_BLEND_ONE; b.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA; b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    b.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = 0xFFFFFFFFu;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = FALSE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = (DXGI_FORMAT)rtv_format;
    pd.SampleDesc.Count = 1;
    const HRESULT hp = dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso_));
    ps->Release();
    if (FAILED(hp)) { vs->Release(); fprintf(stderr, "[menu] PSO failed 0x%08lX\n", (long)hp); return false; }
    // The same state with the gradient shader. Without it the menu still works, only flat.
    if (ID3DBlob* gps = compile("ps_grad", "ps_5_0")) {
        pd.PS = {gps->GetBufferPointer(), gps->GetBufferSize()};
        if (FAILED(dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&grad_pso_)))) {
            grad_pso_ = nullptr;
            fprintf(stderr, "[menu] gradient PSO failed: gradients are not drawn\n");
        }
        gps->Release();
    }
    vs->Release();

    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = kSlots;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap_)))) return false;
    srv_size_ = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    for (int s = kSlots - 1; s >= 0; s--) free_slots_.push_back(s);

    rings_.resize(frame_count);
    const uint8_t white[4] = {255, 255, 255, 255};
    white_ = MakeTexture(white, 1, 1);
    return white_ != nullptr;
}

RmlD3D12::Texture* RmlD3D12::MakeTexture(const uint8_t* rgba, int w, int h)
{
    if (free_slots_.empty()) { Rml::Log::Message(Rml::Log::LT_ERROR, "menu: out of texture slots"); return nullptr; }
    D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d = {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = (UINT64)w; d.Height = (UINT)h;
    d.DepthOrArraySize = 1; d.MipLevels = 1; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1;
    auto* t = new Texture;
    if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST,
                                             nullptr, IID_PPV_ARGS(&t->res)))) { delete t; return nullptr; }
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp; UINT rows; UINT64 row_bytes, total;
    dev_->GetCopyableFootprints(&d, 0, 1, 0, &fp, &rows, &row_bytes, &total);
    t->upload = upload_buffer(dev_, (size_t)total);
    if (!t->upload) { t->res->Release(); delete t; return nullptr; }
    uint8_t* m = nullptr;
    t->upload->Map(0, nullptr, (void**)&m);
    for (int y = 0; y < h; y++) memcpy(m + (size_t)y * fp.Footprint.RowPitch, rgba + (size_t)y * w * 4, (size_t)w * 4);
    t->upload->Unmap(0, nullptr);
    t->slot = free_slots_.back(); free_slots_.pop_back();
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; sv.Texture2D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE ch = heap_->GetCPUDescriptorHandleForHeapStart();
    ch.ptr += (SIZE_T)t->slot * srv_size_;
    dev_->CreateShaderResourceView(t->res, &sv, ch);
    t->pending = true;
    if (cl_) RecordUpload(t); else to_upload_.push_back(t);
    return t;
}

void RmlD3D12::RecordUpload(Texture* t)
{
    D3D12_RESOURCE_DESC d = t->res->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp; UINT rows; UINT64 row_bytes, total;
    dev_->GetCopyableFootprints(&d, 0, 1, 0, &fp, &rows, &row_bytes, &total);
    D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
    dst.pResource = t->res; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.pResource = t->upload; src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = fp;
    cl_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = t->res; b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    cl_->ResourceBarrier(1, &b);
    Retire(t->upload);   // the copy reads it on the GPU this frame
    t->upload = nullptr;
    t->pending = false;
}

void RmlD3D12::Retire(ID3D12Resource* r)
{
    if (r) retired_.push_back({r, frame_no_});
}

void RmlD3D12::BeginFrame(ID3D12GraphicsCommandList* cl, uint64_t rtv, uint32_t w, uint32_t h, uint32_t fi)
{
    frame_no_++;
    // A frame index comes back only after its fence: what was retired frames_ frames ago is idle.
    for (size_t k = 0; k < retired_.size();) {
        if (frame_no_ - retired_[k].frame > frames_) { retired_[k].res->Release(); retired_[k] = retired_.back(); retired_.pop_back(); }
        else k++;
    }
    for (size_t k = 0; k < retired_slots_.size();) {
        if (frame_no_ - retired_slots_[k].frame > frames_) { free_slots_.push_back(retired_slots_[k].slot); retired_slots_[k] = retired_slots_.back(); retired_slots_.pop_back(); }
        else k++;
    }
    cl_ = cl; w_ = w; h_ = h; fi_ = fi;
    Ring& r = rings_[fi];
    if (r.want > r.cap || !r.buf) {
        const size_t cap = r.want > kRingStart ? r.want * 2 : kRingStart;
        if (r.buf) { r.buf->Unmap(0, nullptr); r.buf->Release(); }   // idle: this index's fence has passed
        r.buf = upload_buffer(dev_, cap);
        r.map = nullptr; r.cap = 0;
        if (r.buf && SUCCEEDED(r.buf->Map(0, nullptr, (void**)&r.map))) r.cap = cap;
    }
    r.used = 0; r.want = 0;
    for (Texture* t : to_upload_) RecordUpload(t);
    to_upload_.clear();

    D3D12_CPU_DESCRIPTOR_HANDLE h_rtv; h_rtv.ptr = (SIZE_T)rtv;
    cl->OMSetRenderTargets(1, &h_rtv, FALSE, nullptr);
    D3D12_VIEWPORT vp = {0, 0, (float)w, (float)h, 0.0f, 1.0f};
    cl->RSSetViewports(1, &vp);
    cl->SetGraphicsRootSignature(rs_);
    cl->SetPipelineState(pso_);
    cl->SetDescriptorHeaps(1, &heap_);
    if (r.buf) cl->SetGraphicsRootConstantBufferView(2, r.buf->GetGPUVirtualAddress());   // never read by ps
    cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    proj_ = Rml::Matrix4f::ProjectOrtho(0, (float)w, (float)h, 0, -10000, 10000);
    memcpy(mvp_, proj_.data(), sizeof mvp_);
    scissor_on_ = false;
    ApplyScissor();
}

void RmlD3D12::EndFrame() { cl_ = nullptr; }

Rml::CompiledGeometryHandle RmlD3D12::CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices)
{
    auto* g = new Geometry;
    g->v.assign(vertices.begin(), vertices.end());
    g->i.assign(indices.begin(), indices.end());
    return (Rml::CompiledGeometryHandle)g;
}

bool RmlD3D12::Upload(const Geometry* g, Rml::Vector2f translation)
{
    Ring& r = rings_[fi_];
    const size_t vb = g->v.size() * sizeof(Rml::Vertex), ib = g->i.size() * sizeof(int);
    const size_t at = (r.used + 15) & ~(size_t)15;
    r.want = (r.want > at ? r.want : at) + vb + ib;   // grows the ring the next time this index comes up
    if (at + vb + ib > r.cap) {
        static bool told;
        if (!told) { told = true; fprintf(stderr, "[menu] vertex ring full (%zu B): growing next frame\n", r.cap); }
        r.want = at + vb + ib + r.used;
        return false;
    }
    memcpy(r.map + at, g->v.data(), vb);
    memcpy(r.map + at + vb, g->i.data(), ib);
    r.used = at + vb + ib;
    const D3D12_GPU_VIRTUAL_ADDRESS base = r.buf->GetGPUVirtualAddress() + at;
    D3D12_VERTEX_BUFFER_VIEW vv = {base, (UINT)vb, (UINT)sizeof(Rml::Vertex)};
    D3D12_INDEX_BUFFER_VIEW iv = {base + vb, (UINT)ib, DXGI_FORMAT_R32_UINT};
    cl_->IASetVertexBuffers(0, 1, &vv);
    cl_->IASetIndexBuffer(&iv);
    float k[20];
    memcpy(k, mvp_, sizeof mvp_);
    k[16] = translation.x; k[17] = translation.y; k[18] = k[19] = 0;
    cl_->SetGraphicsRoot32BitConstants(0, 20, k, 0);
    return true;
}

void RmlD3D12::RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation, Rml::TextureHandle texture)
{
    auto* g = (Geometry*)geometry;
    if (!cl_ || !g || g->i.empty() || !Upload(g, translation)) return;
    Texture* t = texture ? (Texture*)texture : white_;
    D3D12_GPU_DESCRIPTOR_HANDLE gh = heap_->GetGPUDescriptorHandleForHeapStart();
    gh.ptr += (UINT64)t->slot * srv_size_;
    cl_->SetGraphicsRootDescriptorTable(1, gh);
    cl_->DrawIndexedInstanced((UINT)g->i.size(), 1, 0, 0, 0);
}

void RmlD3D12::ReleaseGeometry(Rml::CompiledGeometryHandle geometry) { delete (Geometry*)geometry; }

// Gradients. The parameters are RmlUi's (p0/p1, center/radius, center/angle, color_stop_list), read as its
// reference GL3 renderer reads them (RmlUi, MIT; THIRD_PARTY_NOTICES.md); the D3D12 side is ours.
void* rml_grad_compile(const Rml::String& name, const Rml::Dictionary& parameters)
{
    auto* s = new GradShader();
    const bool rep = Rml::Get(parameters, "repeating", false);
    Rml::Vector2f p, v;
    if (name == "linear-gradient") {
        s->fn[0] = rep ? 3 : 0;
        p = Rml::Get(parameters, "p0", Rml::Vector2f(0.f));
        v = Rml::Get(parameters, "p1", Rml::Vector2f(0.f)) - p;
    } else if (name == "radial-gradient") {
        s->fn[0] = rep ? 4 : 1;
        p = Rml::Get(parameters, "center", Rml::Vector2f(0.f));
        v = Rml::Vector2f(1.f) / Rml::Get(parameters, "radius", Rml::Vector2f(1.f));
    } else if (name == "conic-gradient") {
        s->fn[0] = rep ? 5 : 2;
        p = Rml::Get(parameters, "center", Rml::Vector2f(0.f));
        const float a = Rml::Get(parameters, "angle", 0.f);
        v = {Rml::Math::Cos(a), Rml::Math::Sin(a)};
    } else {
        Rml::Log::Message(Rml::Log::LT_WARNING, "menu: shader '%s' not supported", name.c_str());
        delete s;
        return nullptr;
    }
    s->pv[0] = p.x; s->pv[1] = p.y; s->pv[2] = v.x; s->pv[3] = v.y;
    auto it = parameters.find("color_stop_list");
    if (it == parameters.end() || it->second.GetType() != Rml::Variant::COLORSTOPLIST) { delete s; return nullptr; }
    const Rml::ColorStopList& stops = it->second.GetReference<Rml::ColorStopList>();
    const int n = (int)(stops.size() < 16 ? stops.size() : 16);
    if (n < 1) { delete s; return nullptr; }
    s->fn[1] = n;
    for (int k = 0; k < n; k++) {
        s->pos[k] = stops[k].position.number;
        const Rml::ColourbPremultiplied c = stops[k].color;
        s->col[k][0] = c.red / 255.f; s->col[k][1] = c.green / 255.f; s->col[k][2] = c.blue / 255.f; s->col[k][3] = c.alpha / 255.f;
    }
    return s;
}

void rml_grad_free(void* g) { delete (GradShader*)g; }

Rml::CompiledShaderHandle RmlD3D12::CompileShader(const Rml::String& name, const Rml::Dictionary& parameters)
{
    return (Rml::CompiledShaderHandle)rml_grad_compile(name, parameters);
}

void RmlD3D12::RenderShader(Rml::CompiledShaderHandle shader, Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation,
                            Rml::TextureHandle)
{
    auto* g = (Geometry*)geometry;
    auto* s = (GradShader*)shader;
    if (!cl_ || !g || !s || g->i.empty() || !grad_pso_) return;
    Ring& r = rings_[fi_];
    const size_t at = (r.used + 255) & ~(size_t)255;   // a CBV starts on 256 bytes
    r.want = (r.want > at ? r.want : at) + sizeof(GradShader);
    if (at + sizeof(GradShader) > r.cap) { r.want += r.used; return; }
    memcpy(r.map + at, s, sizeof(GradShader));
    r.used = at + sizeof(GradShader);
    if (!Upload(g, translation)) return;
    cl_->SetPipelineState(grad_pso_);
    cl_->SetGraphicsRootConstantBufferView(2, r.buf->GetGPUVirtualAddress() + at);
    D3D12_GPU_DESCRIPTOR_HANDLE gh = heap_->GetGPUDescriptorHandleForHeapStart();
    gh.ptr += (UINT64)white_->slot * srv_size_;
    cl_->SetGraphicsRootDescriptorTable(1, gh);
    cl_->DrawIndexedInstanced((UINT)g->i.size(), 1, 0, 0, 0);
    cl_->SetPipelineState(pso_);
}

void RmlD3D12::ReleaseShader(Rml::CompiledShaderHandle shader) { delete (GradShader*)shader; }

// A picture's pixels, RGBA8 with premultiplied alpha (what RmlUi wants), or null. Free with rml_free_image.
// Only "disc:<file>" (from PS3_GAME next to the game's files) in the game; a plain file path in the installer
// (no disc there). A missing picture is said and gives null: the caller keeps its fallback colours, nothing is faked.
unsigned char* rml_load_image_premul(const Rml::String& source, int* w_out, int* h_out)
{
    int w = 0, h = 0, n = 0;
    unsigned char* px = nullptr;
    if (!ppu_vfs_root) {
        px = stbi_load(source.c_str(), &w, &h, &n, 4);
        if (!px) { Rml::Log::Message(Rml::Log::LT_INFO, "image %s not found", source.c_str()); return nullptr; }
    } else {
        if (source.compare(0, 5, "disc:") != 0) {
            Rml::Log::Message(Rml::Log::LT_WARNING, "menu: image source not supported (%s)", source.c_str());
            return nullptr;
        }
        const std::string name = source.substr(5);
        if (name == "PIC1.PNG" && getenv("REFINE_LAUNCHER_NOPIC")) return nullptr;   // diagnostic: the fallback look
        static const char* const kDirs[] = {"%s/PS3_GAME/%s", "%s/../%s", "%s/%s"};   // disc root, USRDIR, game dir
        for (const char* fmt : kDirs) {
            char path[1200];
            snprintf(path, sizeof path, fmt, ppu_vfs_root, name.c_str());
            px = stbi_load(path, &w, &h, &n, 4);
            if (px) break;
        }
        if (!px) { Rml::Log::Message(Rml::Log::LT_INFO, "menu: %s not found on the disc folder", name.c_str()); return nullptr; }
    }
    for (size_t i = 0; i < (size_t)w * h; i++) {
        const unsigned a = px[i * 4 + 3];
        px[i * 4 + 0] = (unsigned char)(px[i * 4 + 0] * a / 255);
        px[i * 4 + 1] = (unsigned char)(px[i * 4 + 1] * a / 255);
        px[i * 4 + 2] = (unsigned char)(px[i * 4 + 2] * a / 255);
    }
    *w_out = w; *h_out = h;
    return px;
}
void rml_free_image(unsigned char* px) { stbi_image_free(px); }

Rml::TextureHandle RmlD3D12::LoadTexture(Rml::Vector2i& dim, const Rml::String& source)
{
    int w = 0, h = 0;
    unsigned char* px = rml_load_image_premul(source, &w, &h);
    if (!px) return 0;
    Texture* t = MakeTexture(px, w, h);
    rml_free_image(px);
    if (!t) return 0;
    dim = Rml::Vector2i(w, h);
    return (Rml::TextureHandle)t;
}

Rml::TextureHandle RmlD3D12::GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i dim)
{
    if (dim.x <= 0 || dim.y <= 0 || source.size() < (size_t)dim.x * dim.y * 4) return 0;
    return (Rml::TextureHandle)MakeTexture(source.data(), dim.x, dim.y);
}

void RmlD3D12::ReleaseTexture(Rml::TextureHandle texture)
{
    auto* t = (Texture*)texture;
    if (!t) return;
    for (size_t k = 0; k < to_upload_.size(); k++)
        if (to_upload_[k] == t) { to_upload_.erase(to_upload_.begin() + (long)k); break; }
    Retire(t->res);
    Retire(t->upload);
    retired_slots_.push_back({t->slot, frame_no_});   // its SRV may still be read by a frame in flight
    delete t;
}

void RmlD3D12::EnableScissorRegion(bool enable) { scissor_on_ = enable; ApplyScissor(); }
void RmlD3D12::SetScissorRegion(Rml::Rectanglei region) { scissor_ = region; ApplyScissor(); }

void RmlD3D12::ApplyScissor()
{
    if (!cl_) return;
    D3D12_RECT sc = {0, 0, (LONG)w_, (LONG)h_};
    if (scissor_on_) {
        sc.left = scissor_.Left() > 0 ? scissor_.Left() : 0;
        sc.top = scissor_.Top() > 0 ? scissor_.Top() : 0;
        sc.right = scissor_.Right() < (int)w_ ? scissor_.Right() : (LONG)w_;
        sc.bottom = scissor_.Bottom() < (int)h_ ? scissor_.Bottom() : (LONG)h_;
        if (sc.right < sc.left) sc.right = sc.left;
        if (sc.bottom < sc.top) sc.bottom = sc.top;
    }
    cl_->RSSetScissorRects(1, &sc);
}

void RmlD3D12::SetTransform(const Rml::Matrix4f* transform)
{
    const Rml::Matrix4f m = transform ? proj_ * *transform : proj_;
    memcpy(mvp_, m.data(), sizeof mvp_);
}
