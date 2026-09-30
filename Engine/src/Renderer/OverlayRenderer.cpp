#include "Renderer/OverlayRenderer.hpp"

#include "Flux/Assets/Image.hpp"
#include "Flux/Platform/Filesystem.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdio>

namespace Flux {
    namespace {
        // Doit correspondre à OverlayParams dans shaders/Overlay.slang.
        struct OverlayParams
        {
            float screenSize[2];
            float atlasSize[2];
            float distanceRange;
            float mode; // 0 : atlas MSDF ; 1 : image
        };

        // Images chargées au plus : un groupe chacune, réservés d'avance.
        constexpr std::uint32_t MaxTextures = 256;
        // Intervalle entre deux vérifications de la date d'un fichier d'image (rechargé s'il a changé) :
        // assez court pour voir une retouche aussitôt, assez long pour ne pas interroger le disque à chaque image.
        constexpr std::chrono::milliseconds TextureCheckInterval{1000};
        // Taille minimale d'un vertex buffer d'overlay : évite de réallouer pour quelques caractères de plus.
        constexpr std::uint64_t MinBufferSize = 64 * 1024;
    }

    std::expected<std::unique_ptr<OverlayRenderer>, std::string> OverlayRenderer::Create(RHI::Device& device, RHI::Format colorFormat,
                                                                                         RHI::Format depthFormat) {
        std::unique_ptr<OverlayRenderer> overlay(new OverlayRenderer(device));
        const std::filesystem::path fontPath = Platform::GetExecutableDirectory() / "assets" / "fonts" / "Arimo.ttf";
        auto font = UI::Font::Load(fontPath);
        if (!font) {
            return std::unexpected(font.error());
        }
        overlay->m_font = std::move(*font);
        overlay->m_drawList = std::make_unique<UI::DrawList>(*overlay->m_font);

        // UNORM et pas SRGB : l'atlas contient des distances, pas des couleurs.
        const Math::Vector2i atlasSize = overlay->m_font->GetAtlasSize();
        auto atlas = device.CreateTexture(RHI::Format::RGBA8Unorm,
                                          {static_cast<std::uint32_t>(atlasSize.x), static_cast<std::uint32_t>(atlasSize.y)},
                                          overlay->m_font->GetAtlasPixels());
        if (!atlas) {
            return std::unexpected(atlas.error());
        }
        overlay->m_atlas = std::move(*atlas);

        // Filtrage linéaire obligatoire : le MSDF reconstruit le contour entre les texels. Convient aussi aux images.
        auto sampler = device.CreateSampler({.filter = RHI::Filter::Linear, .mipFilter = RHI::Filter::Nearest,
                                             .address = RHI::AddressMode::ClampToEdge});
        if (!sampler) {
            return std::unexpected(sampler.error());
        }
        overlay->m_sampler = std::move(*sampler);

        // L'atlas, puis les images.
        constexpr RHI::Binding binding{0, RHI::BindingType::Texture, RHI::ShaderStage::Fragment};
        auto layout = device.CreateBindGroupLayout({.bindings = {&binding, 1}, .maxGroups = 1 + MaxTextures});
        if (!layout) {
            return std::unexpected(layout.error());
        }
        overlay->m_layout = std::move(*layout);
        std::unique_ptr<RHI::BindGroup> atlasGroup = device.CreateBindGroup(*overlay->m_layout);
        if (!atlasGroup) {
            return std::unexpected("allocation du groupe de l'atlas");
        }
        atlasGroup->SetTexture(0, *overlay->m_atlas, *overlay->m_sampler);
        overlay->m_groups.push_back(std::move(atlasGroup)); // UI::FontTexture

        const RHI::VertexBufferLayout vertexBuffer{0, sizeof(UI::Vertex)};
        const std::array attributes{
            RHI::VertexAttribute{0, 0, RHI::Format::RG32Float, offsetof(UI::Vertex, position)},
            RHI::VertexAttribute{1, 0, RHI::Format::RG32Float, offsetof(UI::Vertex, uv)},
            RHI::VertexAttribute{2, 0, RHI::Format::RGBA32Float, offsetof(UI::Vertex, color)},
        };
        const RHI::BindGroupLayout* layouts[] = {overlay->m_layout.get()};
        RHI::PipelineDesc desc;
        desc.shaderPath = Platform::GetExecutableDirectory() / "shaders" / "Overlay.spv";
        desc.vertexBuffers = {&vertexBuffer, 1};
        desc.vertexAttributes = attributes;
        desc.bindGroupLayouts = layouts;
        desc.pushConstantsSize = sizeof(OverlayParams);
        desc.colorFormat = colorFormat;
        desc.depthFormat = depthFormat; // l'overlay passe par-dessus tout : pas de test de profondeur
        desc.blend = RHI::BlendMode::Alpha;
        auto pipeline = device.CreatePipeline(desc);
        if (!pipeline) {
            return std::unexpected(pipeline.error());
        }
        overlay->m_pipeline = std::move(*pipeline);
        return overlay;
    }

    // Lit le fichier et remplace l'image de la texture ; en cas d'échec, l'ancienne image (s'il y en a une) reste.
    void OverlayRenderer::Reload(const std::filesystem::path& path, Texture& texture) {
        auto pixels = LoadImageFile(path);
        if (!pixels) {
            std::fprintf(stderr, "Flux : image %s\n", pixels.error().c_str());
            return;
        }
        // SRGB : les couleurs d'une image sont en sRGB, le GPU les ramène en linéaire à la lecture.
        auto image = m_device.CreateTexture(RHI::Format::RGBA8Srgb,
                                            {static_cast<std::uint32_t>(pixels->size.x), static_cast<std::uint32_t>(pixels->size.y)},
                                            pixels->rgba);
        if (!image) {
            std::fprintf(stderr, "Flux : image %s : %s\n", Platform::ToUtf8(path).c_str(), image.error().c_str());
            return;
        }
        if (texture.id == UI::FontTexture) {
            if (m_groups.size() > MaxTextures) {
                std::fprintf(stderr, "Flux : plus de %u images chargées, %s ignorée\n", MaxTextures, Platform::ToUtf8(path).c_str());
                return;
            }
            std::unique_ptr<RHI::BindGroup> group = m_device.CreateBindGroup(*m_layout);
            if (!group) {
                return;
            }
            texture.id = static_cast<UI::TextureId>(m_groups.size());
            m_groups.push_back(std::move(group));
        } else {
            // Rechargement (fichier retouché) : les images en vol dessinent peut-être encore l'ancienne image,
            // et un groupe ne se modifie pas pendant qu'il sert. Rare : attendre le GPU suffit.
            m_device.WaitIdle();
        }
        texture.image = std::move(*image);
        texture.size = pixels->size;
        m_groups[texture.id]->SetTexture(0, *texture.image, *m_sampler);
    }

    std::optional<UI::TextureInfo> OverlayRenderer::LoadTexture(const std::filesystem::path& path) {
        const auto now = std::chrono::steady_clock::now();
        auto [found, created] = m_textures.try_emplace(Platform::ToGenericUtf8(path.lexically_normal()));
        Texture& texture = found->second;
        if (created || now - texture.checked >= TextureCheckInterval) {
            texture.checked = now;
            std::error_code error;
            const std::filesystem::file_time_type writeTime = std::filesystem::last_write_time(path, error);
            // Fichier absent : essayé une fois (pour le signaler), puis de nouveau quand il apparaît.
            if (created || (!error && writeTime != texture.writeTime)) {
                texture.writeTime = error ? std::filesystem::file_time_type{} : writeTime;
                Reload(path, texture);
            }
        }
        if (texture.id == UI::FontTexture) {
            return std::nullopt;
        }
        return UI::TextureInfo{texture.id, texture.size};
    }

    bool OverlayRenderer::Upload(std::uint32_t frameIndex) {
        const std::span<const UI::Vertex> vertices = m_drawList->GetVertices();
        m_vertexCount = static_cast<std::uint32_t>(vertices.size());
        if (vertices.empty()) {
            return true;
        }
        const std::uint64_t bytes = vertices.size_bytes();
        std::unique_ptr<RHI::Buffer>& buffer = m_buffers[frameIndex];
        if (!buffer || buffer->GetSize() < bytes) {
            auto grown = m_device.CreateBuffer({std::bit_ceil(std::max(bytes, MinBufferSize)), RHI::BufferUsage::Vertex,
                                                RHI::MemoryType::Upload});
            if (!grown) {
                std::fprintf(stderr, "Flux : %s\n", grown.error().c_str());
                return false;
            }
            buffer = std::move(*grown);
        }
        buffer->Write(vertices.data(), bytes);
        return true;
    }

    void OverlayRenderer::Draw(RHI::CommandList& commands, std::uint32_t frameIndex, RHI::Extent extent) const {
        if (m_vertexCount == 0) {
            return;
        }
        const Math::Vector2i atlasSize = m_font->GetAtlasSize();
        OverlayParams params{
            {static_cast<float>(extent.width), static_cast<float>(extent.height)},
            {static_cast<float>(atlasSize.x), static_cast<float>(atlasSize.y)},
            m_font->GetDistanceRange(),
            0.0f,
        };

        // L'interface couvre toute la fenêtre, la scène seulement sa zone.
        commands.SetViewport({0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height)});
        commands.SetScissor({0, 0, extent.width, extent.height});
        commands.BindPipeline(*m_pipeline);
        const RHI::Buffer* vertexBuffer = m_buffers[frameIndex].get();
        commands.BindVertexBuffers(0, {&vertexBuffer, 1});
        // Un appel par suite de sommets de même texture, dans l'ordre de la liste (le dernier par-dessus).
        UI::TextureId bound = 0;
        bool anyBound = false;
        for (const UI::DrawCommand& command : m_drawList->GetCommands()) {
            if (command.texture >= m_groups.size()) {
                continue; // identifiant inconnu (liste remplie à la main) : ignoré plutôt qu'un plantage GPU
            }
            if (!anyBound || command.texture != bound) {
                bound = command.texture;
                anyBound = true;
                params.mode = bound == UI::FontTexture ? 0.0f : 1.0f;
                commands.SetBindGroup(0, *m_groups[bound]);
                commands.PushConstants(&params, sizeof(params));
            }
            commands.Draw(command.vertexCount, command.firstVertex);
        }
    }
}
