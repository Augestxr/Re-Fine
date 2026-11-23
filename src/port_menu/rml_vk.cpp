// Re:Fine N1 (0186): see rml_vk.h.
#include "rml_vk.h"
#include "rml_d3d12.h"   // rml_load_image_premul, rml_grad_compile: shared with the D3D12 renderer

#include <RmlUi/Core.h>
#include <cstring>

#include "rsx_vk_exec.h"   // the executor's overlay calls (libs/video)

static_assert(sizeof(Rml::Vertex) == 20, "Rml::Vertex layout");

void RmlVk::BeginFrame(uint32_t w, uint32_t h)
{
    w_ = w; h_ = h; in_frame_ = true;
    proj_ = Rml::Matrix4f::ProjectOrtho(0, (float)w, (float)h, 0, -10000, 10000);
    memcpy(mvp_, proj_.data(), sizeof mvp_);
    scissor_on_ = false;
}

void RmlVk::EndFrame() { in_frame_ = false; }

Rml::CompiledGeometryHandle RmlVk::CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices)
{
    auto* g = new Geometry;
    g->v.assign(vertices.begin(), vertices.end());
    g->i.assign(indices.begin(), indices.end());
    return (Rml::CompiledGeometryHandle)g;
}

void RmlVk::Draw(const Geometry* g, Rml::Vector2f translation, uint32_t tex, const void* grad)
{
    if (!in_frame_ || !g || g->i.empty() || g->v.empty()) return;   // the dry render at boot: nothing recorded
    rsx_vk_ui_cmd c;
    memset(&c, 0, sizeof c);
    c.verts = g->v.data(); c.nverts = (uint32_t)g->v.size();
    c.idx = g->i.data(); c.nidx = (uint32_t)g->i.size();
    memcpy(c.mvp, mvp_, sizeof c.mvp);
    c.tx = translation.x; c.ty = translation.y;
    c.tex = tex;
    if (scissor_on_) {
        c.sx = scissor_.Left(); c.sy = scissor_.Top();
        c.sw = scissor_.Right() - scissor_.Left(); c.sh = scissor_.Bottom() - scissor_.Top();
    } else { c.sx = 0; c.sy = 0; c.sw = (int)w_; c.sh = (int)h_; }
    c.grad = grad;
    rsx_vk_ui_draw(&c);
}

void RmlVk::RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation, Rml::TextureHandle texture)
{
    Draw((const Geometry*)geometry, translation, (uint32_t)texture, nullptr);
}

void RmlVk::ReleaseGeometry(Rml::CompiledGeometryHandle geometry) { delete (Geometry*)geometry; }

Rml::CompiledShaderHandle RmlVk::CompileShader(const Rml::String& name, const Rml::Dictionary& parameters)
{
    return (Rml::CompiledShaderHandle)rml_grad_compile(name, parameters);
}

void RmlVk::RenderShader(Rml::CompiledShaderHandle shader, Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation,
                         Rml::TextureHandle)
{
    if (shader) Draw((const Geometry*)geometry, translation, 0, (const void*)shader);
}

void RmlVk::ReleaseShader(Rml::CompiledShaderHandle shader) { rml_grad_free((void*)shader); }

Rml::TextureHandle RmlVk::LoadTexture(Rml::Vector2i& dim, const Rml::String& source)
{
    int w = 0, h = 0;
    unsigned char* px = rml_load_image_premul(source, &w, &h);
    if (!px) return 0;
    const uint32_t id = rsx_vk_ui_texture(px, (uint32_t)w, (uint32_t)h);
    rml_free_image(px);
    if (!id) return 0;
    dim = Rml::Vector2i(w, h);
    return (Rml::TextureHandle)id;
}

Rml::TextureHandle RmlVk::GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i dim)
{
    if (dim.x <= 0 || dim.y <= 0 || source.size() < (size_t)dim.x * dim.y * 4) return 0;
    return (Rml::TextureHandle)rsx_vk_ui_texture(source.data(), (uint32_t)dim.x, (uint32_t)dim.y);
}

void RmlVk::ReleaseTexture(Rml::TextureHandle texture) { rsx_vk_ui_texture_free((uint32_t)texture); }

void RmlVk::EnableScissorRegion(bool enable) { scissor_on_ = enable; }
void RmlVk::SetScissorRegion(Rml::Rectanglei region) { scissor_ = region; }

void RmlVk::SetTransform(const Rml::Matrix4f* transform)
{
    const Rml::Matrix4f m = transform ? proj_ * *transform : proj_;
    memcpy(mvp_, m.data(), sizeof mvp_);
}
