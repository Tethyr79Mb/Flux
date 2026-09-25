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
    }

    void DrawList::AddQuad(Math::Vector2f min, Math::Vector2f max, Math::Vector2f uvMin, Math::Vector2f uvMax,
                           Math::LinearColor color) {
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
