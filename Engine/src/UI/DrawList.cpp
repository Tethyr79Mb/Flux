#include "Flux/UI/DrawList.hpp"

#include "Flux/UI/Font.hpp"

#include <algorithm>

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

    void DrawList::AddQuad(Math::Vector2f min, Math::Vector2f max, Math::Vector2f uvMin, Math::Vector2f uvMax,
                           Math::LinearColor color) {
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
        m_vertices.insert(m_vertices.end(), {topLeft, topRight, bottomRight, topLeft, bottomRight, bottomLeft});
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
        m_vertices.insert(m_vertices.end(), {Vertex{a, uv, color}, Vertex{b, uv, color}, Vertex{c, uv, color}});
    }

    void DrawList::Append(const DrawList& other) {
        m_vertices.insert(m_vertices.end(), other.m_vertices.begin(), other.m_vertices.end());
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
