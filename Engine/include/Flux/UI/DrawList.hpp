#pragma once

#include "Flux/Export.hpp"
#include "Flux/Math/Color.hpp"
#include "Flux/Math/Vector2.hpp"

#include <cstdint>
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

    // Texture échantillonnée par une primitive : 0 est l'atlas de la police (texte et rectangles pleins),
    // les autres viennent de Renderer::LoadTexture (images).
    using TextureId = std::uint32_t;
    inline constexpr TextureId FontTexture = 0;

    // Texture chargée : de quoi la dessiner à sa taille.
    struct TextureInfo
    {
        TextureId id = FontTexture;
        Math::Vector2i size; // pixels
    };

    // Suite de sommets consécutifs qui utilisent la même texture : un appel de dessin chacune.
    struct DrawCommand
    {
        TextureId texture = FontTexture;
        std::uint32_t firstVertex = 0;
        std::uint32_t vertexCount = 0;
    };

    // Liste de primitives 2D reconstruite à chaque frame : rectangles, texte et images. Les primitives qui se suivent
    // avec la même texture forment une seule commande (un seul appel de dessin) : du texte et des rectangles, quel
    // que soit leur nombre, n'en font qu'une.
    class FLUX_API DrawList {
        public:
        explicit DrawList(const Font& font);

        // Vide la liste et retire le rectangle de découpe.
        void Clear();

        // Tout ce qui est ajouté ensuite est découpé à ce rectangle (côté CPU : un seul appel de dessin reste possible).
        void SetClipRect(Math::Vector2f min, Math::Vector2f max);
        void ResetClipRect();

        void AddRect(Math::Vector2f min, Math::Vector2f max, Math::LinearColor color);
        // Contour intérieur au rectangle, d'épaisseur thickness pixels.
        void AddRectOutline(Math::Vector2f min, Math::Vector2f max, Math::LinearColor color, float thickness = 1.0f);
        // Triangle plein (flèches, coches) ; omis s'il dépasse du rectangle de découpe (pas de découpe partielle).
        void AddTriangle(Math::Vector2f a, Math::Vector2f b, Math::Vector2f c, Math::LinearColor color);
        // Segment épais (thickness pixels) ; avec un rectangle de découpe, seule sa partie intérieure est dessinée.
        void AddLine(Math::Vector2f a, Math::Vector2f b, Math::LinearColor color, float thickness = 1.0f);
        // Image (texture entière par défaut) étirée sur le rectangle, multipliée par tint (alpha compris) ;
        // découpée au rectangle de découpe comme un rectangle plein.
        void AddImage(Math::Vector2f min, Math::Vector2f max, TextureId texture, Math::LinearColor tint = {1.0f, 1.0f, 1.0f},
                      Math::Vector2f uvMin = {0.0f, 0.0f}, Math::Vector2f uvMax = {1.0f, 1.0f});
        // Ajoute tels quels les sommets d'une autre liste (panneaux assemblés dans leur ordre d'affichage).
        void Append(const DrawList& other);

        // topLeft : coin haut-gauche de la première ligne ; '\n' passe à la ligne. Retourne la taille du texte.
        Math::Vector2f AddText(Math::Vector2f topLeft, std::string_view utf8, float pixelSize, Math::LinearColor color);
        Math::Vector2f MeasureText(std::string_view utf8, float pixelSize) const;

        std::span<const Vertex> GetVertices() const { return m_vertices; }
        std::span<const DrawCommand> GetCommands() const { return m_commands; }
        const Font& GetFont() const { return *m_font; }

        private:
        // Parcourt le texte ; emit(glyphe, coin haut-gauche du glyphe) est appelé pour chaque glyphe visible.
        template <typename Emit>
        Math::Vector2f LayoutText(Math::Vector2f topLeft, std::string_view utf8, float pixelSize, Emit&& emit) const;

        void AddQuad(Math::Vector2f min, Math::Vector2f max, Math::Vector2f uvMin, Math::Vector2f uvMax, Math::LinearColor color,
                     TextureId texture = FontTexture);
        // Ajoute des sommets à la commande en cours, ou en ouvre une si la texture change.
        void PushVertices(TextureId texture, std::span<const Vertex> vertices);

        const Font* m_font;
        std::vector<Vertex> m_vertices;
        std::vector<DrawCommand> m_commands;
        bool m_clipping = false;
        Math::Vector2f m_clipMin;
        Math::Vector2f m_clipMax;
    };
}
