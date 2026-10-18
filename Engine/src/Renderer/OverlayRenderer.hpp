#pragma once

#include "Flux/Assets/Image.hpp"
#include "Flux/Graphics/Font.hpp"
#include "Flux/UI/DrawList.hpp"
#include "Renderer/RHI/RHI.hpp"

#include <array>
#include <chrono>
#include <expected>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Flux {
    // Overlay 2D (FluxUI) : la liste de dessin de l'image, le texte (atlas MSDF de la police) et les images de
    // l'interface, dessinés par-dessus la scène sur toute la fenêtre.
    class OverlayRenderer {
        public:
        static std::expected<std::unique_ptr<OverlayRenderer>, std::string> Create(RHI::Device& device, RHI::Format colorFormat,
                                                                                    RHI::Format depthFormat);

        UI::DrawList& GetDrawList() { return *m_drawList; }

        // Voir Renderer::LoadTexture.
        std::optional<UI::TextureInfo> LoadTexture(const std::filesystem::path& path);
        // Voir Renderer::CreateTexture.
        std::optional<UI::TextureInfo> CreateTexture(const ImageData& image);
        // Voir Renderer::LoadFont.
        std::optional<UI::FontRef> LoadFont(const std::filesystem::path& path);
        std::uint32_t GetFontsLoading() const { return m_fontsLoading; }

        // Sommets de la liste envoyés au GPU. Après Device::BeginFrame : le buffer de l'image frameIndex est libre.
        bool Upload(std::uint32_t frameIndex);
        void Draw(RHI::CommandList& commands, std::uint32_t frameIndex, RHI::Extent extent) const;

        private:
        // Image de l'interface chargée depuis un fichier, et de quoi voir si le fichier a changé.
        struct Texture
        {
            std::unique_ptr<RHI::Texture> image;
            UI::TextureId id = UI::FontTexture; // FontTexture : pas (encore) chargée
            Math::Vector2i size;
            std::filesystem::file_time_type writeTime{};
            std::chrono::steady_clock::time_point checked{};
        };

        // Police chargée depuis un fichier, avec la texture de son atlas ; id FontTexture : pas encore prête (pending
        // valide), ou fichier illisible (pas réessayé).
        struct LoadedFont
        {
            std::future<std::expected<std::unique_ptr<UI::Font>, std::string>> pending; // atlas construit sur un autre fil
            std::unique_ptr<UI::Font> font;
            std::unique_ptr<RHI::Texture> atlas;
            UI::TextureId id = UI::FontTexture;
        };

        // Constantes du shader propres à une texture : un atlas MSDF a sa taille et son étendue de distance.
        struct GroupParams
        {
            Math::Vector2f atlasSize;
            float distanceRange = 0.0f;
            float mode = 1.0f; // 0 : atlas MSDF ; 1 : image
        };

        explicit OverlayRenderer(RHI::Device& device) : m_device(device) {}
        // Nouveau groupe (texture) ; nullptr au-delà de MaxTextures.
        RHI::BindGroup* AddGroup(UI::TextureId& id, const GroupParams& params, std::string_view name);
        // Police prête : son atlas envoyé au GPU, un groupe pour lui.
        void Install(LoadedFont& loaded, std::expected<std::unique_ptr<UI::Font>, std::string> font, std::string_view name);
        void Reload(const std::filesystem::path& path, Texture& texture);
        void Store(const ImageData& pixels, Texture& texture, std::string_view name);

        RHI::Device& m_device;
        std::unique_ptr<UI::Font> m_font;
        std::unique_ptr<UI::DrawList> m_drawList;
        std::unique_ptr<RHI::Sampler> m_sampler;
        std::unique_ptr<RHI::BindGroupLayout> m_layout;
        std::unique_ptr<RHI::Texture> m_atlas;
        // Images de l'interface, par chemin (UTF-8, normalisé).
        std::unordered_map<std::string, Texture> m_textures;
        // Images créées en mémoire (CreateTexture), gardées jusqu'à la fermeture.
        std::vector<std::unique_ptr<Texture>> m_created;
        // Polices des textes de la scène, par chemin (UTF-8, normalisé).
        std::unordered_map<std::string, LoadedFont> m_fonts;
        std::uint32_t m_fontsLoading = 0;
        // Un groupe par texture, indexé par UI::TextureId : [0] est l'atlas de la police. Rendus avant leur layout.
        std::vector<std::unique_ptr<RHI::BindGroup>> m_groups;
        std::vector<GroupParams> m_params; // même index que m_groups
        std::unique_ptr<RHI::Pipeline> m_pipeline;
        std::array<std::unique_ptr<RHI::Buffer>, RHI::FramesInFlight> m_buffers;
        std::uint32_t m_vertexCount = 0;
    };
}
