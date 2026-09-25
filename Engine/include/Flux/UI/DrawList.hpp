#pragma once

#include "Flux/Export.hpp"
#include "Flux/Math/Color.hpp"
#include "Flux/Math/Vector2.hpp"

#include <span>
#include <string_view>
#include <vector>

namespace Flux::UI {
    class Font;

    // Doit correspondre à VertexInput dans shaders/Overlay.slang.
    struct Vertex
    {
        Math::Vector2f position; // pixels, origine en haut à gauche
        Math::Vector2f uv;
        Math::LinearColor color;
    };

    // Liste de primitives 2D reconstruite à chaque frame : rectangles et texte, dessinés en un seul appel.
    class FLUX_API DrawList {
        public:
        explicit DrawList(const Font& font);

        void Clear();

        void AddRect(Math::Vector2f min, Math::Vector2f max, Math::LinearColor color);

        // topLeft : coin haut-gauche de la première ligne ; '\n' passe à la ligne. Retourne la taille du texte.
        Math::Vector2f AddText(Math::Vector2f topLeft, std::string_view utf8, float pixelSize, Math::LinearColor color);
        Math::Vector2f MeasureText(std::string_view utf8, float pixelSize) const;

        std::span<const Vertex> GetVertices() const { return m_vertices; }
        const Font& GetFont() const { return *m_font; }

        private:
        // Parcourt le texte ; emit(glyphe, coin haut-gauche du glyphe) est appelé pour chaque glyphe visible.
        template <typename Emit>
        Math::Vector2f LayoutText(Math::Vector2f topLeft, std::string_view utf8, float pixelSize, Emit&& emit) const;

        void AddQuad(Math::Vector2f min, Math::Vector2f max, Math::Vector2f uvMin, Math::Vector2f uvMax, Math::LinearColor color);

        const Font* m_font;
        std::vector<Vertex> m_vertices;
    };
}
