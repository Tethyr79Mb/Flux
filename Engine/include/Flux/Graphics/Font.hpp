#pragma once

#include "Flux/Export.hpp"
#include "Flux/Math/Vector2.hpp"

#include <array>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace Flux::UI {
    // Métriques d'un glyphe, en « em » (1.0 = taille de la police), y vers le bas, origine sur la ligne de base.
    struct Glyph
    {
        Math::Vector2f planeMin;
        Math::Vector2f planeMax;
        Math::Vector2f uvMin;
        Math::Vector2f uvMax;
        float advance = 0.0f;
        bool visible = false; // false pour l'espace : avance seulement
    };

    struct FontAtlasSettings
    {
        float pixelsPerEm = 32.0f;   // résolution des glyphes dans l'atlas
        float distanceRange = 4.0f;  // étendue du champ de distance, en pixels d'atlas
        int atlasWidth = 512;
    };

    // Police chargée par FreeType et convertie en atlas MSDF (texte net à toutes les tailles).
    // Couvre le Latin-1 (ASCII et accents français). Côté CPU seulement : le renderer envoie l'atlas au GPU.
    class FLUX_API Font {
        public:
        static std::expected<std::unique_ptr<Font>, std::string> Load(const std::filesystem::path& path,
                                                                      const FontAtlasSettings& settings = {});

        static constexpr char32_t FirstCodepoint = 0;
        static constexpr char32_t LastCodepoint = 255;

        // nullptr si le caractère n'est pas couvert.
        const Glyph* FindGlyph(char32_t codepoint) const;

        float GetAscender() const { return m_ascender; }
        float GetLineHeight() const { return m_lineHeight; }

        Math::Vector2i GetAtlasSize() const { return m_atlasSize; }
        std::span<const std::uint8_t> GetAtlasPixels() const { return m_atlasPixels; } // RGBA8, ligne 0 en haut
        float GetDistanceRange() const { return m_distanceRange; }

        // UV d'un bloc plein : sert à dessiner des rectangles avec le même shader que le texte.
        Math::Vector2f GetSolidUV() const { return m_solidUV; }

        private:
        Font() = default;

        std::array<Glyph, LastCodepoint + 1> m_glyphs{};
        std::array<bool, LastCodepoint + 1> m_hasGlyph{};
        float m_ascender = 0.0f;
        float m_lineHeight = 0.0f;

        Math::Vector2i m_atlasSize;
        std::vector<std::uint8_t> m_atlasPixels;
        float m_distanceRange = 0.0f;
        Math::Vector2f m_solidUV;
    };
}
