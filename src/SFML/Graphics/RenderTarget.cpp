////////////////////////////////////////////////////////////
//
// SFML - Simple and Fast Multimedia Library
// Copyright (C) 2007-2018 Laurent Gomila (laurent@sfml-dev.org)
//
// This software is provided 'as-is', without any express or implied warranty.
// In no event will the authors be held liable for any damages arising from the use of this software.
//
// Permission is granted to anyone to use this software for any purpose,
// including commercial applications, and to alter it and redistribute it freely,
// subject to the following restrictions:
//
// 1. The origin of this software must not be misrepresented;
//    you must not claim that you wrote the original software.
//    If you use this software in a product, an acknowledgment
//    in the product documentation would be appreciated but is not required.
//
// 2. Altered source versions must be plainly marked as such,
//    and must not be misrepresented as being the original software.
//
// 3. This notice may not be removed or altered from any source distribution.
//
////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////
// Headers
////////////////////////////////////////////////////////////
#include <SFML/Graphics/RenderTarget.hpp>
#include <SFML/Graphics/Drawable.hpp>
#include <SFML/Graphics/Shader.hpp>
#include <SFML/Graphics/Texture.hpp>
#include <SFML/Graphics/VertexArray.hpp>
#include <SFML/Graphics/VertexBuffer.hpp>
#include <SFML/System/Err.hpp>
#include <cassert>
#include <iostream>
#include <algorithm>
#include <map>
#include <mutex>

#include <bgfx/bgfx.h>
#include <bgfx/embedded_shader.h>

#include "fs_debugdraw_lines.bin.h"
#include "vs_debugdraw_lines.bin.h"
#include "fs_debugdraw_fill_texture.bin.h"
#include "vs_debugdraw_fill_texture.bin.h"

static const bgfx::EmbeddedShader s_embeddedShaders[] =
{
    BGFX_EMBEDDED_SHADER(vs_debugdraw_lines),
    BGFX_EMBEDDED_SHADER(fs_debugdraw_lines),
    BGFX_EMBEDDED_SHADER(vs_debugdraw_fill_texture),
    BGFX_EMBEDDED_SHADER(fs_debugdraw_fill_texture),

    BGFX_EMBEDDED_SHADER_END()
};

namespace
{
    // Mutex to protect ID generation and our context-RenderTarget-map
    std::mutex mutex;

    // Unique identifier, used for identifying RenderTargets when
    // tracking the currently active RenderTarget within a given context
    sf::Uint64 getUniqueId()
    {
        std::lock_guard<std::mutex> lock(mutex);

        static sf::Uint64 id = 1; // start at 1, zero is "no RenderTarget"

        return id++;
    }

    // Map to help us detect whether a different RenderTarget
    // has been activated within a single context
    typedef std::map<sf::Uint64, sf::Uint64> ContextRenderTargetMap;
    ContextRenderTargetMap contextRenderTargetMap;

    // A bgfx vertexdecl to match our vertex types
    bgfx::VertexDecl defaultVertexDecl;

    // The handle to the texture uniform
    bgfx::UniformHandle defaultTextureUniform = { bgfx::kInvalidHandle };

    // The default program/shader to use
    bgfx::ProgramHandle fillProgram = { bgfx::kInvalidHandle };
    bgfx::ProgramHandle fillTextureProgram = { bgfx::kInvalidHandle };

}


namespace sf
{
////////////////////////////////////////////////////////////
RenderTarget::RenderTarget() :
m_defaultView(),
m_view       (),
m_cache      (),
m_id         (0)
{
    m_cache.glStatesSet = false;
}

////////////////////////////////////////////////////////////
RenderTarget::RenderTarget(const sf::ContextSettings & settings) :
m_contextSettings(settings)
{
}

////////////////////////////////////////////////////////////
RenderTarget::~RenderTarget()
{
}


////////////////////////////////////////////////////////////
void RenderTarget::clear(const Color& color)
{
    const auto rect = getViewport(getView());
    bgfx::setViewRect(0, rect.left, rect.top, rect.width, rect.height);
    bgfx::setViewClear(0, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, color.toInteger());
}


////////////////////////////////////////////////////////////
void RenderTarget::setView(const View& view)
{
    m_view = view;
    m_cache.viewChanged = true;
}


////////////////////////////////////////////////////////////
const View& RenderTarget::getView() const
{
    return m_view;
}


////////////////////////////////////////////////////////////
const View& RenderTarget::getDefaultView() const
{
    return m_defaultView;
}


////////////////////////////////////////////////////////////
IntRect RenderTarget::getViewport(const View& view) const
{
    float width  = static_cast<float>(getSize().x);
    float height = static_cast<float>(getSize().y);
    const FloatRect& viewport = view.getViewport();

    return IntRect(static_cast<int>(0.5f + width  * viewport.left),
                   static_cast<int>(0.5f + height * viewport.top),
                   static_cast<int>(0.5f + width  * viewport.width),
                   static_cast<int>(0.5f + height * viewport.height));
}


////////////////////////////////////////////////////////////
Vector2f RenderTarget::mapPixelToCoords(const Vector2i& point) const
{
    return mapPixelToCoords(point, getView());
}


////////////////////////////////////////////////////////////
Vector2f RenderTarget::mapPixelToCoords(const Vector2i& point, const View& view) const
{
    // First, convert from viewport coordinates to homogeneous coordinates
    Vector2f normalized;
    IntRect viewport = getViewport(view);
    normalized.x = -1.f + 2.f * (point.x - viewport.left) / viewport.width;
    normalized.y =  1.f - 2.f * (point.y - viewport.top)  / viewport.height;

    // Then transform by the inverse of the view matrix
    return view.getInverseTransform().transformPoint(normalized);
}


////////////////////////////////////////////////////////////
Vector2i RenderTarget::mapCoordsToPixel(const Vector2f& point) const
{
    return mapCoordsToPixel(point, getView());
}


////////////////////////////////////////////////////////////
Vector2i RenderTarget::mapCoordsToPixel(const Vector2f& point, const View& view) const
{
    // First, transform the point by the view matrix
    Vector2f normalized = view.getTransform().transformPoint(point);

    // Then convert to viewport coordinates
    Vector2i pixel;
    IntRect viewport = getViewport(view);
    pixel.x = static_cast<int>(( normalized.x + 1.f) / 2.f * viewport.width  + viewport.left);
    pixel.y = static_cast<int>((-normalized.y + 1.f) / 2.f * viewport.height + viewport.top);

    return pixel;
}


////////////////////////////////////////////////////////////
void RenderTarget::draw(const Drawable& drawable, const RenderStates& states)
{
    drawable.draw(*this, states);
}


////////////////////////////////////////////////////////////
void RenderTarget::draw(const Vertex* vertices, std::size_t vertexCount,
    PrimitiveType type, const RenderStates& states)
{
    bgfx::TransientVertexBuffer tvb;
    bgfx::allocTransientVertexBuffer(&tvb, vertexCount, defaultVertexDecl);
    sf::Vector2u textureSize = { 1,1 };
    if (states.texture)
    {
        textureSize = states.texture->getSize();
    }
       
    for (int i = 0; i < vertexCount; ++i)
    {
        Vertex* data = reinterpret_cast<Vertex*>(tvb.data);
        data[i] = vertices[i];
        data[i].texCoords.x /= textureSize.x;
        data[i].texCoords.y /= textureSize.y;
    }
    bgfx::setVertexBuffer(0, &tvb);

    auto state = BGFX_STATE_WRITE_RGB | BGFX_STATE_BLEND_ALPHA;
    switch (type)
    {
    case PrimitiveType::Triangles:
        state |= BGFX_STATE_DEFAULT;
        break;
    case PrimitiveType::TriangleStrip:
        state |= BGFX_STATE_PT_TRISTRIP;
        break;
    case PrimitiveType::Points:
        state |= BGFX_STATE_PT_POINTS;
        break;
    case PrimitiveType::Lines:
        state |= BGFX_STATE_PT_LINES;
        break;
    case PrimitiveType::LineStrip:
        state |= BGFX_STATE_PT_LINESTRIP;
        break;
    case PrimitiveType::TriangleFan:
        // Not supported by bgfx, so emulate it with an index buffer
        bgfx::TransientIndexBuffer idb;
        auto indexCount = vertexCount * 3;
        bgfx::allocTransientIndexBuffer(&idb, indexCount);
        std::uint16_t* indices = reinterpret_cast<std::uint16_t*>(idb.data);
        for (int v = 0, i = 0; v < vertexCount; ++v, ++i)
        {
            indices[i] = 0;
            indices[++i] = v;
            indices[++i] = v+1;
        }
        bgfx::setIndexBuffer(&idb);
        break;
    }
    bgfx::setState(state);
    bgfx::setTransform(states.transform.getMatrix());
    if (states.texture)
    {
        bgfx::UniformHandle texUniform = bgfx::createUniform("s_texColor", bgfx::UniformType::Int1);
        bgfx::setTexture(0, texUniform, { static_cast<std::uint16_t>(states.texture->getNativeHandle()) });
        bgfx::submit(0, fillTextureProgram);
    }
    else
    {
        bgfx::submit(0, fillProgram);
    }
}


////////////////////////////////////////////////////////////
void RenderTarget::draw(const VertexBuffer& vertexBuffer, const RenderStates& states)
{
    draw(vertexBuffer, 0, vertexBuffer.getVertexCount(), states);
}


////////////////////////////////////////////////////////////
void RenderTarget::draw(const VertexBuffer& vertexBuffer, std::size_t firstVertex,
                        std::size_t vertexCount, const RenderStates& states)
{
    //TODO
    assert(false);
}

////////////////////////////////////////////////////////////
void RenderTarget::initialize()
{
    // Generate a unique ID for this RenderTarget to track
    // whether it is active within a specific context
    m_id = getUniqueId();

    // Initialise bgfx
    bgfx::Init init;
    init.resolution.width = getSize().x;
    init.resolution.height = getSize().y;
    init.resolution.reset = BGFX_RESET_VSYNC | BGFX_RESET_HIDPI;

    switch (m_contextSettings.backend)
    {
    case ContextSettings::Backend::OpenGL:
        //init.type = bgfx::RendererType::OpenGL;
        break;

    default:
        break;
    }

    bgfx::init(init);

    //bgfx::setDebug(BGFX_DEBUG_STATS);

    // Setup the default and current views
    m_defaultView.reset(FloatRect(0, 0, static_cast<float>(getSize().x), static_cast<float>(getSize().y)));
    m_view = m_defaultView;
    applyCurrentView();
    bgfx::setViewMode(0, bgfx::ViewMode::Sequential);

    // As I'm using the bgfx debug shaders for now, the colour it expects is 4 floats
    defaultVertexDecl
        .begin()
        .add(bgfx::Attrib::Position, 2, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true)
        .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
        .end();

    fillProgram = bgfx::createProgram(
        bgfx::createEmbeddedShader(s_embeddedShaders, bgfx::getRendererType(), "vs_debugdraw_lines"),
        bgfx::createEmbeddedShader(s_embeddedShaders, bgfx::getRendererType(), "fs_debugdraw_lines"),
        true);

    fillTextureProgram = bgfx::createProgram(
        bgfx::createEmbeddedShader(s_embeddedShaders, bgfx::getRendererType(), "vs_debugdraw_fill_texture"),
        bgfx::createEmbeddedShader(s_embeddedShaders, bgfx::getRendererType(), "fs_debugdraw_fill_texture"),
        true);
}


////////////////////////////////////////////////////////////
void RenderTarget::applyCurrentView()
{
    // Set the viewport
    const auto viewport = m_view.getViewport();
    //bgfx::setViewRect(0, viewport.left, viewport.top, viewport.width, viewport.height);

    // And the view transform
    const auto transform = m_view.getTransform();
    bgfx::setViewTransform(0, transform.getMatrix(), /* wat dis */ 0);

    m_cache.viewChanged = false;
}


////////////////////////////////////////////////////////////
void RenderTarget::applyBlendMode(const BlendMode& mode)
{
    // TODO
    // something to do with bgfx::setState()?
    m_cache.lastBlendMode = mode;
}


////////////////////////////////////////////////////////////
void RenderTarget::applyTransform(const Transform& transform)
{
   //bgfx::setTransform(transform.getMatrix());
}


////////////////////////////////////////////////////////////
void RenderTarget::applyTexture(const Texture* texture)
{
    Texture::bind(texture, Texture::CoordinateType::Pixels);

    m_cache.lastTextureId = texture ? texture->m_cacheId : 0;
}


////////////////////////////////////////////////////////////
void RenderTarget::applyShader(const Shader* shader)
{
    Shader::bind(shader);
}


////////////////////////////////////////////////////////////
void RenderTarget::setupDraw(bool useVertexCache, const RenderStates& states)
{

    if (!useVertexCache)
    {
        applyTransform(states.transform);
    }

    // Apply the view
    if (!m_cache.enable || m_cache.viewChanged)
        applyCurrentView();

    // Apply the blend mode
    if (!m_cache.enable || (states.blendMode != m_cache.lastBlendMode))
        applyBlendMode(states.blendMode);

    // Apply the texture
    if (!m_cache.enable || (states.texture && states.texture->m_fboAttachment))
    {
        // If the texture is an FBO attachment, always rebind it
        // in order to inform the OpenGL driver that we want changes
        // made to it in other contexts to be visible here as well
        // This saves us from having to call glFlush() in
        // RenderTextureImplFBO which can be quite costly
        // See: https://www.khronos.org/opengl/wiki/Memory_Model
        applyTexture(states.texture);
    }
    else
    {
        Uint64 textureId = states.texture ? states.texture->m_cacheId : 0;
        if (textureId != m_cache.lastTextureId)
            applyTexture(states.texture);
    }

    // Apply the shader
    if (states.shader)
        applyShader(states.shader);
}


////////////////////////////////////////////////////////////
void RenderTarget::drawPrimitives(PrimitiveType type, std::size_t firstVertex, std::size_t vertexCount)
{
    //TODO
    // Draw the damn primitives...
}


////////////////////////////////////////////////////////////
void RenderTarget::cleanupDraw(const RenderStates& states)
{
    // Unbind the shader, if any
    if (states.shader)
        applyShader(nullptr);

    // If the texture we used to draw belonged to a RenderTexture, then forcibly unbind that texture.
    // This prevents a bug where some drivers do not clear RenderTextures properly.
    if (states.texture && states.texture->m_fboAttachment)
        applyTexture(nullptr);

    // Re-enable the cache at the end of the draw if it was disabled
    m_cache.enable = true;
}

} // namespace sf


////////////////////////////////////////////////////////////
// Render states caching strategies
//
// * View
//   If SetView was called since last draw, the projection
//   matrix is updated. We don't need more, the view doesn't
//   change frequently.
//
// * Transform
//   The transform matrix is usually expensive because each
//   entity will most likely use a different transform. This can
//   lead, in worst case, to changing it every 4 vertices.
//   To avoid that, when the vertex count is low enough, we
//   pre-transform them and therefore use an identity transform
//   to render them.
//
// * Blending mode
//   Since it overloads the == operator, we can easily check
//   whether any of the 6 blending components changed and,
//   thus, whether we need to update the blend mode.
//
// * Texture
//   Storing the pointer or OpenGL ID of the last used texture
//   is not enough; if the sf::Texture instance is destroyed,
//   both the pointer and the OpenGL ID might be recycled in
//   a new texture instance. We need to use our own unique
//   identifier system to ensure consistent caching.
//
// * Shader
//   Shaders are very hard to optimize, because they have
//   parameters that can be hard (if not impossible) to track,
//   like matrices or textures. The only optimization that we
//   do is that we avoid setting a null shader if there was
//   already none for the previous draw.
//
////////////////////////////////////////////////////////////
