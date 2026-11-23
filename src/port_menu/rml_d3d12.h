// Re:Fine 6b+6c: RmlUi render interface over the D3D12 device and command list of the
// runtime's backend (port_overlay.h). Our own, minimal: textured triangles with a transform,
// scissor, textures made from bytes (the font atlas), and (2026-11-11, the menu redesign) RmlUi's
// linear, radial and conic gradients as a pixel shader. No layers, filters or clip masks.
#pragma once

#include <RmlUi/Core/RenderInterface.h>
#include <cstdint>
#include <vector>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12RootSignature;
struct ID3D12PipelineState;
struct ID3D12DescriptorHeap;
struct ID3D12Resource;

class RmlD3D12 final : public Rml::RenderInterface {
public:
    // GPU objects on first use (the device only exists in the draw hook). false: no menu.
    bool Init(ID3D12Device* dev, uint32_t rtv_format, uint32_t frame_count);
    // One frame: cl records, the swap buffer is bound as the render target.
    void BeginFrame(ID3D12GraphicsCommandList* cl, uint64_t rtv, uint32_t w, uint32_t h, uint32_t fi);
    void EndFrame();

    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices) override;
    void RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation, Rml::TextureHandle texture) override;
    void ReleaseGeometry(Rml::CompiledGeometryHandle geometry) override;
    Rml::TextureHandle LoadTexture(Rml::Vector2i& texture_dimensions, const Rml::String& source) override;
    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i source_dimensions) override;
    void ReleaseTexture(Rml::TextureHandle texture) override;
    void EnableScissorRegion(bool enable) override;
    void SetScissorRegion(Rml::Rectanglei region) override;
    void SetTransform(const Rml::Matrix4f* transform) override;
    Rml::CompiledShaderHandle CompileShader(const Rml::String& name, const Rml::Dictionary& parameters) override;
    void RenderShader(Rml::CompiledShaderHandle shader, Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation,
                      Rml::TextureHandle texture) override;
    void ReleaseShader(Rml::CompiledShaderHandle shader) override;

private:
    struct Geometry { std::vector<Rml::Vertex> v; std::vector<int> i; };
    struct Texture { ID3D12Resource* res = nullptr; ID3D12Resource* upload = nullptr; int slot = 0; bool pending = false; };
    struct Retired { ID3D12Resource* res; uint64_t frame; };

    void ApplyScissor();
    // Copies the geometry into this frame's ring and sets the buffers and the VS constants; false: no room.
    bool Upload(const Geometry* g, Rml::Vector2f translation);
    void RecordUpload(Texture* t);
    Texture* MakeTexture(const uint8_t* rgba, int w, int h);
    void Retire(ID3D12Resource* r);

    ID3D12Device* dev_ = nullptr;
    ID3D12RootSignature* rs_ = nullptr;
    ID3D12PipelineState* pso_ = nullptr;
    ID3D12PipelineState* grad_pso_ = nullptr;   // gradients (CompileShader); null: they are not drawn
    ID3D12DescriptorHeap* heap_ = nullptr;
    uint32_t srv_size_ = 0, frames_ = 0;
    std::vector<int> free_slots_;
    Texture* white_ = nullptr;
    std::vector<Texture*> to_upload_;   // made outside a frame: recorded at the next BeginFrame
    std::vector<Retired> retired_;      // freed once frames_ frames have gone by
    struct RetiredSlot { int slot; uint64_t frame; };
    std::vector<RetiredSlot> retired_slots_;

    // Per frame index: an upload-heap ring for this frame's vertices and indices.
    struct Ring { ID3D12Resource* buf = nullptr; uint8_t* map = nullptr; size_t cap = 0, used = 0; size_t want = 0; };
    std::vector<Ring> rings_;

    ID3D12GraphicsCommandList* cl_ = nullptr;
    uint32_t w_ = 0, h_ = 0, fi_ = 0;
    uint64_t frame_no_ = 0;
    bool scissor_on_ = false;
    Rml::Rectanglei scissor_;
    float mvp_[16] = {};
    Rml::Matrix4f proj_;
};

// Shared with the Vulkan renderer (rml_vk.cpp): a picture's pixels (RGBA8, premultiplied; null when missing)
// and RmlUi's gradient parameters as the 352-byte block both shaders read (null: not a gradient we draw).
unsigned char* rml_load_image_premul(const Rml::String& source, int* w, int* h);
void rml_free_image(unsigned char* px);
void* rml_grad_compile(const Rml::String& name, const Rml::Dictionary& parameters);
void rml_grad_free(void* g);
