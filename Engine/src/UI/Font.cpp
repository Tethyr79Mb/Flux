#include "Flux/UI/Font.hpp"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H

#include <msdfgen.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <format>

namespace Flux::UI {
    namespace {
        // Taille du bloc plein réservé en haut à gauche de l'atlas.
        constexpr int SolidBlockSize = 4;

        struct FreeTypeLibrary {
            FT_Library handle = nullptr;
            ~FreeTypeLibrary() { if (handle) FT_Done_FreeType(handle); }
        };

        struct FreeTypeFace {
            FT_Face handle = nullptr;
            ~FreeTypeFace() { if (handle) FT_Done_Face(handle); }
        };

        // Conversion du contour FreeType (unités de la police, y vers le haut) en forme msdfgen.
        struct OutlineContext {
            msdfgen::Shape* shape = nullptr;
            msdfgen::Contour* contour = nullptr;
            msdfgen::Point2 position{};
        };

        msdfgen::Point2 ToPoint(const FT_Vector& vector) {
            return {static_cast<double>(vector.x), static_cast<double>(vector.y)};
        }

        int MoveTo(const FT_Vector* to, void* user) {
            auto* context = static_cast<OutlineContext*>(user);
            if (!(context->contour && context->contour->edges.empty())) {
                context->contour = &context->shape->addContour();
            }
            context->position = ToPoint(*to);
            return 0;
        }

        int LineTo(const FT_Vector* to, void* user) {
            auto* context = static_cast<OutlineContext*>(user);
            const msdfgen::Point2 end = ToPoint(*to);
            if (end != context->position) {
                context->contour->addEdge(msdfgen::EdgeHolder(context->position, end));
                context->position = end;
            }
            return 0;
        }

        int ConicTo(const FT_Vector* control, const FT_Vector* to, void* user) {
            auto* context = static_cast<OutlineContext*>(user);
            const msdfgen::Point2 end = ToPoint(*to);
            if (end != context->position) {
                context->contour->addEdge(msdfgen::EdgeHolder(context->position, ToPoint(*control), end));
                context->position = end;
            }
            return 0;
        }

        int CubicTo(const FT_Vector* control1, const FT_Vector* control2, const FT_Vector* to, void* user) {
            auto* context = static_cast<OutlineContext*>(user);
            const msdfgen::Point2 end = ToPoint(*to);
            if (end != context->position
                || msdfgen::crossProduct(ToPoint(*control1) - end, ToPoint(*control2) - end) != 0.0) {
                context->contour->addEdge(msdfgen::EdgeHolder(context->position, ToPoint(*control1), ToPoint(*control2), end));
                context->position = end;
            }
            return 0;
        }

        bool LoadShape(FT_Outline* outline, msdfgen::Shape& shape) {
            OutlineContext context{.shape = &shape};
            FT_Outline_Funcs functions{};
            functions.move_to = MoveTo;
            functions.line_to = LineTo;
            functions.conic_to = ConicTo;
            functions.cubic_to = CubicTo;
            if (FT_Outline_Decompose(outline, &functions, &context) != 0) {
                return false;
            }
            if (!shape.contours.empty() && shape.contours.back().edges.empty()) {
                shape.contours.pop_back();
            }
            return true;
        }

        // Emplacement d'un glyphe rendu, avant empaquetage dans l'atlas.
        struct PendingGlyph {
            char32_t codepoint = 0;
            int width = 0;
            int height = 0;
            msdfgen::Bitmap<float, 3> bitmap;
            int x = 0;
            int y = 0;
        };

        std::uint8_t ToByte(float value) {
            return static_cast<std::uint8_t>(std::clamp(value * 255.0f + 0.5f, 0.0f, 255.0f));
        }
    }

    std::expected<std::unique_ptr<Font>, std::string> Font::Load(const std::filesystem::path& path,
                                                                 const FontAtlasSettings& settings) {
        FreeTypeLibrary library;
        if (FT_Init_FreeType(&library.handle) != 0) {
            return std::unexpected("initialisation de FreeType");
        }
        FreeTypeFace face;
        if (FT_New_Face(library.handle, path.string().c_str(), 0, &face.handle) != 0) {
            return std::unexpected("police introuvable ou illisible : " + path.string());
        }

        std::unique_ptr<Font> font(new Font());
        const double unitsPerEm = face.handle->units_per_EM;
        const double scale = settings.pixelsPerEm / unitsPerEm; // unités de police → pixels d'atlas
        const double padding = settings.distanceRange;          // pixels autour de chaque glyphe
        font->m_ascender = static_cast<float>(face.handle->ascender / unitsPerEm);
        font->m_lineHeight = static_cast<float>(face.handle->height / unitsPerEm);
        font->m_distanceRange = settings.distanceRange;

        std::vector<PendingGlyph> pending;
        for (char32_t codepoint = FirstCodepoint; codepoint <= LastCodepoint; ++codepoint) {
            const FT_UInt index = FT_Get_Char_Index(face.handle, codepoint);
            if (index == 0 || FT_Load_Glyph(face.handle, index, FT_LOAD_NO_SCALE) != 0) {
                continue;
            }

            Glyph& glyph = font->m_glyphs[codepoint];
            glyph.advance = static_cast<float>(face.handle->glyph->metrics.horiAdvance / unitsPerEm);
            font->m_hasGlyph[codepoint] = true;

            msdfgen::Shape shape;
            if (!LoadShape(&face.handle->glyph->outline, shape) || shape.contours.empty()) {
                continue; // espace et caractères invisibles
            }
            shape.normalize();
            msdfgen::edgeColoringSimple(shape, 3.0);

            double left = 0, bottom = 0, right = 0, top = 0;
            shape.bound(left, bottom, right, top);
            const int width = static_cast<int>(std::ceil((right - left) * scale + 2.0 * padding));
            const int height = static_cast<int>(std::ceil((top - bottom) * scale + 2.0 * padding));

            // Coin bas-gauche du bitmap, en unités de police.
            const double originX = left - padding / scale;
            const double originY = bottom - padding / scale;

            PendingGlyph rendered{codepoint, width, height, msdfgen::Bitmap<float, 3>(width, height)};
            const msdfgen::Projection projection(msdfgen::Vector2(scale), msdfgen::Vector2(-originX, -originY));
            msdfgen::generateMSDF(rendered.bitmap, shape, projection, msdfgen::Range(settings.distanceRange / scale));

            // Métriques en em, y vers le bas (écran).
            glyph.planeMin = {static_cast<float>(originX / unitsPerEm),
                              static_cast<float>(-(originY + height / scale) / unitsPerEm)};
            glyph.planeMax = {static_cast<float>((originX + width / scale) / unitsPerEm),
                              static_cast<float>(-originY / unitsPerEm)};
            glyph.visible = true;
            pending.push_back(std::move(rendered));
        }
        if (pending.empty()) {
            return std::unexpected("aucun glyphe exploitable dans " + path.string());
        }

        // Empaquetage par étagères, du plus haut au plus bas, après le bloc plein.
        std::sort(pending.begin(), pending.end(), [](const PendingGlyph& a, const PendingGlyph& b) { return a.height > b.height; });
        const int atlasWidth = settings.atlasWidth;
        int cursorX = SolidBlockSize + 1;
        int cursorY = 0;
        int shelfHeight = SolidBlockSize;
        for (PendingGlyph& glyph : pending) {
            if (glyph.width > atlasWidth) {
                return std::unexpected(std::format("glyphe U+{:04X} plus large que l'atlas", static_cast<unsigned>(glyph.codepoint)));
            }
            if (cursorX + glyph.width > atlasWidth) {
                cursorX = 0;
                cursorY += shelfHeight + 1;
                shelfHeight = 0;
            }
            glyph.x = cursorX;
            glyph.y = cursorY;
            cursorX += glyph.width + 1;
            shelfHeight = std::max(shelfHeight, glyph.height);
        }
        const int atlasHeight = std::bit_ceil(static_cast<unsigned>(cursorY + shelfHeight));

        font->m_atlasSize = {atlasWidth, atlasHeight};
        font->m_atlasPixels.assign(static_cast<std::size_t>(atlasWidth) * atlasHeight * 4, 0);
        auto pixel = [&](int x, int y) { return &font->m_atlasPixels[(static_cast<std::size_t>(y) * atlasWidth + x) * 4]; };

        for (int y = 0; y < SolidBlockSize; ++y) {
            for (int x = 0; x < SolidBlockSize; ++x) {
                std::uint8_t* p = pixel(x, y);
                p[0] = p[1] = p[2] = p[3] = 255;
            }
        }
        font->m_solidUV = {SolidBlockSize * 0.5f / atlasWidth, SolidBlockSize * 0.5f / atlasHeight};

        for (const PendingGlyph& glyph : pending) {
            // msdfgen range ses lignes du bas vers le haut ; l'atlas du haut vers le bas.
            for (int y = 0; y < glyph.height; ++y) {
                for (int x = 0; x < glyph.width; ++x) {
                    const float* source = glyph.bitmap(x, glyph.height - 1 - y);
                    std::uint8_t* destination = pixel(glyph.x + x, glyph.y + y);
                    destination[0] = ToByte(source[0]);
                    destination[1] = ToByte(source[1]);
                    destination[2] = ToByte(source[2]);
                    destination[3] = 255;
                }
            }
            Glyph& metrics = font->m_glyphs[glyph.codepoint];
            metrics.uvMin = {static_cast<float>(glyph.x) / atlasWidth, static_cast<float>(glyph.y) / atlasHeight};
            metrics.uvMax = {static_cast<float>(glyph.x + glyph.width) / atlasWidth,
                             static_cast<float>(glyph.y + glyph.height) / atlasHeight};
        }
        return font;
    }

    const Glyph* Font::FindGlyph(char32_t codepoint) const {
        if (codepoint > LastCodepoint || !m_hasGlyph[codepoint]) {
            return nullptr;
        }
        return &m_glyphs[codepoint];
    }
}
