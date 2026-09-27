#include "Flux/UI/DrawList.hpp"

#include "Flux/UI/Font.hpp"

#include <algorithm>
#include <cmath>

namespace Flux::UI {
    namespace {
        constexpr char32_t ReplacementCharacter = U'?';

        // Décode le prochain point de code UTF-8 ; une séquence invalide donne ReplacementCharacter.
        char32_t NextCodepoint(std::string_view text, std::size_t& index) {
            const auto byte = [&](std::size_t i) { return static_cast<unsigned char>(text[i]); };
            const unsigned char lead = byte(index++);
            if (lead < 0x80) {
                return lead;
            }
            int length = 0;
            char32_t codepoint = 0;
            if ((lead & 0xE0) == 0xC0) { length = 1; codepoint = lead & 0x1F; }
            else if ((lead & 0xF0) == 0xE0) { length = 2; codepoint = lead & 0x0F; }
            else if ((lead & 0xF8) == 0xF0) { length = 3; codepoint = lead & 0x07; }
            else { return ReplacementCharacter; }

            for (int i = 0; i < length; ++i) {
                if (index >= text.size() || (byte(index) & 0xC0) != 0x80) {
                    return ReplacementCharacter;
                }
                codepoint = (codepoint << 6) | (byte(index++) & 0x3F);
            }
            return codepoint;
        }
    }

    DrawList::DrawList(const Font& font) : m_font(&font) {}

    void DrawList::Clear() {
        m_vertices.clear();
        m_commands.clear();
        ResetClipRect();
    }

    void DrawList::SetClipRect(Math::Vector2f min, Math::Vector2f max) {
        m_clipping = true;
        m_clipMin = min;
        m_clipMax = max;
    }

    void DrawList::ResetClipRect() {
        m_clipping = false;
    }

    void DrawList::PushVertices(TextureId texture, std::span<const Vertex> vertices) {
        if (vertices.empty()) {
            return;
        }
        if (m_commands.empty() || m_commands.back().texture != texture) {
            m_commands.push_back({texture, static_cast<std::uint32_t>(m_vertices.size()), 0});
        }
        m_vertices.insert(m_vertices.end(), vertices.begin(), vertices.end());
        m_commands.back().vertexCount += static_cast<std::uint32_t>(vertices.size());
    }

    void DrawList::AddQuad(Math::Vector2f min, Math::Vector2f max, Math::Vector2f uvMin, Math::Vector2f uvMax,
                           Math::LinearColor color, TextureId texture) {
        if (m_clipping) {
            // Quad aligné sur les axes : on rogne la géométrie et on interpole les UV d'autant.
            const float clippedMinX = std::max(min.x, m_clipMin.x);
            const float clippedMinY = std::max(min.y, m_clipMin.y);
            const float clippedMaxX = std::min(max.x, m_clipMax.x);
            const float clippedMaxY = std::min(max.y, m_clipMax.y);
            if (clippedMinX >= clippedMaxX || clippedMinY >= clippedMaxY) {
                return;
            }
            const auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
            const float width = max.x - min.x;
            const float height = max.y - min.y;
            const Math::Vector2f newUvMin{lerp(uvMin.x, uvMax.x, (clippedMinX - min.x) / width),
                                          lerp(uvMin.y, uvMax.y, (clippedMinY - min.y) / height)};
            const Math::Vector2f newUvMax{lerp(uvMin.x, uvMax.x, (clippedMaxX - min.x) / width),
                                          lerp(uvMin.y, uvMax.y, (clippedMaxY - min.y) / height)};
            min = {clippedMinX, clippedMinY};
            max = {clippedMaxX, clippedMaxY};
            uvMin = newUvMin;
            uvMax = newUvMax;
        }
        const Vertex topLeft{min, uvMin, color};
        const Vertex topRight{{max.x, min.y}, {uvMax.x, uvMin.y}, color};
        const Vertex bottomLeft{{min.x, max.y}, {uvMin.x, uvMax.y}, color};
        const Vertex bottomRight{max, uvMax, color};
        const Vertex quad[] = {topLeft, topRight, bottomRight, topLeft, bottomRight, bottomLeft};
        PushVertices(texture, quad);
    }

    void DrawList::AddRect(Math::Vector2f min, Math::Vector2f max, Math::LinearColor color) {
        const Math::Vector2f uv = m_font->GetSolidUV();
        AddQuad(min, max, uv, uv, color);
    }

    void DrawList::AddRectOutline(Math::Vector2f min, Math::Vector2f max, Math::LinearColor color, float thickness) {
        AddRect(min, {max.x, min.y + thickness}, color);
        AddRect({min.x, max.y - thickness}, max, color);
        AddRect({min.x, min.y + thickness}, {min.x + thickness, max.y - thickness}, color);
        AddRect({max.x - thickness, min.y + thickness}, {max.x, max.y - thickness}, color);
    }

    void DrawList::AddTriangle(Math::Vector2f a, Math::Vector2f b, Math::Vector2f c, Math::LinearColor color) {
        if (m_clipping) {
            // Pas de découpe partielle : un triangle (flèche, coche) est entier ou absent.
            const auto inside = [&](Math::Vector2f p) {
                return p.x >= m_clipMin.x && p.x <= m_clipMax.x && p.y >= m_clipMin.y && p.y <= m_clipMax.y;
            };
            if (!inside(a) || !inside(b) || !inside(c)) {
                return;
            }
        }
        const Math::Vector2f uv = m_font->GetSolidUV();
        const Vertex triangle[] = {{a, uv, color}, {b, uv, color}, {c, uv, color}};
        PushVertices(FontTexture, triangle);
    }

    void DrawList::AddLine(Math::Vector2f a, Math::Vector2f b, Math::LinearColor color, float thickness) {
        if (m_clipping) {
            // Découpe du segment (Liang-Barsky) au rectangle réduit de la demi-épaisseur : ses deux triangles
            // restent alors entiers dans le rectangle et ne sont pas écartés par AddTriangle.
            const float margin = thickness * 0.5f + 0.01f;
            const Math::Vector2f min{m_clipMin.x + margin, m_clipMin.y + margin};
            const Math::Vector2f max{m_clipMax.x - margin, m_clipMax.y - margin};
            const Math::Vector2f d = b - a;
            float t0 = 0.0f;
            float t1 = 1.0f;
            // Pour chaque bord : p * t <= q à l'intérieur.
            const float p[4] = {-d.x, d.x, -d.y, d.y};
            const float q[4] = {a.x - min.x, max.x - a.x, a.y - min.y, max.y - a.y};
            for (int i = 0; i < 4; ++i) {
                if (p[i] == 0.0f) {
                    if (q[i] < 0.0f) {
                        return; // parallèle au bord et dehors
                    }
                } else {
                    const float t = q[i] / p[i];
                    if (p[i] < 0.0f) {
                        t0 = std::max(t0, t);
                    } else {
                        t1 = std::min(t1, t);
                    }
                }
            }
            if (t0 > t1) {
                return;
            }
            const Math::Vector2f start = a;
            a = start + d * t0;
            b = start + d * t1;
        }
        const Math::Vector2f direction = b - a;
        const float length = std::sqrt(direction.x * direction.x + direction.y * direction.y);
        if (length <= 0.0f) {
            return;
        }
        const float half = thickness * 0.5f / length;
        const Math::Vector2f normal{-direction.y * half, direction.x * half};
        AddTriangle(a + normal, b + normal, b - normal, color);
        AddTriangle(a + normal, b - normal, a - normal, color);
    }

    void DrawList::AddImage(Math::Vector2f min, Math::Vector2f max, TextureId texture, Math::LinearColor tint,
                            Math::Vector2f uvMin, Math::Vector2f uvMax) {
        if (min.x >= max.x || min.y >= max.y) {
            return; // rectangle vide (ou retourné) : rien à dessiner, et pas de division par zéro à la découpe
        }
        AddQuad(min, max, uvMin, uvMax, tint, texture);
    }

    void DrawList::Append(const DrawList& other) {
        const std::span<const Vertex> vertices = other.m_vertices;
        for (const DrawCommand& command : other.m_commands) {
            PushVertices(command.texture, vertices.subspan(command.firstVertex, command.vertexCount));
        }
    }

    template <typename Emit>
    Math::Vector2f DrawList::LayoutText(Math::Vector2f topLeft, std::string_view utf8, float pixelSize, Emit&& emit) const {
        const float lineHeight = m_font->GetLineHeight() * pixelSize;
        const float baselineOffset = m_font->GetAscender() * pixelSize;

        Math::Vector2f pen{topLeft.x, topLeft.y + baselineOffset};
        float widest = 0.0f;
        int lines = 1;
        for (std::size_t index = 0; index < utf8.size();) {
            const char32_t codepoint = NextCodepoint(utf8, index);
            if (codepoint == U'\n') {
                widest = std::max(widest, pen.x - topLeft.x);
                pen = {topLeft.x, pen.y + lineHeight};
                ++lines;
                continue;
            }
            const Glyph* glyph = m_font->FindGlyph(codepoint);
            if (!glyph) {
                glyph = m_font->FindGlyph(ReplacementCharacter);
                if (!glyph) {
                    continue;
                }
            }
            if (glyph->visible) {
                emit(*glyph, pen);
            }
            pen.x += glyph->advance * pixelSize;
        }
        widest = std::max(widest, pen.x - topLeft.x);
        return {widest, lines * lineHeight};
    }

    Math::Vector2f DrawList::AddText(Math::Vector2f topLeft, std::string_view utf8, float pixelSize, Math::LinearColor color) {
        return LayoutText(topLeft, utf8, pixelSize, [&](const Glyph& glyph, Math::Vector2f pen) {
            const Math::Vector2f min{pen.x + glyph.planeMin.x * pixelSize, pen.y + glyph.planeMin.y * pixelSize};
            const Math::Vector2f max{pen.x + glyph.planeMax.x * pixelSize, pen.y + glyph.planeMax.y * pixelSize};
            AddQuad(min, max, glyph.uvMin, glyph.uvMax, color);
        });
    }

    Math::Vector2f DrawList::MeasureText(std::string_view utf8, float pixelSize) const {
        return LayoutText({}, utf8, pixelSize, [](const Glyph&, Math::Vector2f) {});
    }
}
