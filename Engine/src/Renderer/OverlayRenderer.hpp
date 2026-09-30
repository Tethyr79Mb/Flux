#pragma once

#include "Flux/Graphics/Font.hpp"
#include "Flux/UI/DrawList.hpp"
#include "Renderer/RHI/RHI.hpp"

#include <array>
#include <chrono>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
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

        explicit OverlayRenderer(RHI::Device& device) : m_device(device) {}
        void Reload(const std::filesystem::path& path, Texture& texture);

        RHI::Device& m_device;
        std::unique_ptr<UI::Font> m_font;
        std::unique_ptr<UI::DrawList> m_drawList;
        std::unique_ptr<RHI::Sampler> m_sampler;
        std::unique_ptr<RHI::BindGroupLayout> m_layout;
        std::unique_ptr<RHI::Texture> m_atlas;
        // Images de l'interface, par chemin (UTF-8, normalisé).
        std::unordered_map<std::string, Texture> m_textures;
        // Un groupe par texture, indexé par UI::TextureId : [0] est l'atlas de la police. Rendus avant leur layout.
        std::vector<std::unique_ptr<RHI::BindGroup>> m_groups;
        std::unique_ptr<RHI::Pipeline> m_pipeline;
        std::array<std::unique_ptr<RHI::Buffer>, RHI::FramesInFlight> m_buffers;
        std::uint32_t m_vertexCount = 0;
    };
}
