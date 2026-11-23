// Re:Fine N1 (0186): RmlUi render interface over the Vulkan executor of the runtime (rsx_vk_exec.h's overlay
// calls). The same features as rml_d3d12: textured triangles with a transform, scissor, textures made from bytes,
// and the linear, radial and conic gradients. No layers, filters or clip masks. Nothing of Vulkan is used here:
// the executor owns the device; this hands it meshes and pixels.
#pragma once

#include <RmlUi/Core/RenderInterface.h>
#include <cstdint>
#include <vector>

class RmlVk final : public Rml::RenderInterface {
public:
    // One frame, inside the executor's overlay hook: w and h are the presented image's size.
    void BeginFrame(uint32_t w, uint32_t h);
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
    void Draw(const Geometry* g, Rml::Vector2f translation, uint32_t tex, const void* grad);

    bool in_frame_ = false;
    uint32_t w_ = 0, h_ = 0;
    bool scissor_on_ = false;
    Rml::Rectanglei scissor_;
    float mvp_[16] = {};
    Rml::Matrix4f proj_;
};
