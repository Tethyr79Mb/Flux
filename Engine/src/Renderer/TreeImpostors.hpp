#pragma once

#include "Renderer/ModelCache.hpp"
#include "Flux/Renderer/RHI/RHI.hpp"

#include <array>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

namespace Flux {
    // Imposteurs des arbres des planètes (vertexImpostor dans shaders/Mesh.slang) : le modèle détaillé de chaque espèce
    // photographié une fois par le GPU, au premier besoin, sous Grid x Grid directions de la demi-sphère du haut
    // (vertexImpostorBake), dans deux atlas à niveaux de mip rangés dans la table des images de ModelCache : couleur
    // (alpha : couverture) et normales. Une capture par image au plus ; en attendant, le modèle lointain sert.
    // Mémoire : 2 x Size² x 4 octets plus les mips, environ 11 Mo par espèce.
    class TreeImpostors {
        public:
        // Doit correspondre à ImpostorGrid dans shaders/Mesh.slang.
        static constexpr std::uint32_t Grid = 8;
        static constexpr std::uint32_t Size = 1024; // 128 pixels par vue
        // Jusqu'à 4 pixels par vue : plus bas, les vues voisines se mélangeraient.
        static constexpr std::uint32_t MipLevels = 6;
        static constexpr RHI::Format ColorFormat = RHI::Format::RGBA8Srgb;
        static constexpr RHI::Format NormalFormat = RHI::Format::RGBA8Unorm;
        static constexpr RHI::Format DepthFormat = RHI::Format::D32Float;
        // Primitives capturées au plus par modèle (écorce, feuillage…).
        static constexpr std::uint32_t MaxPrimitives = 16;
        // Imposteur oublié (mémoire rendue) après autant d'images sans être demandé : planète quittée, espèce retirée.
        static constexpr std::uint64_t UnusedFrames = 600;

        static std::expected<std::unique_ptr<TreeImpostors>, std::string> Create(RHI::Device& device, ModelCache& models);
        ~TreeImpostors();
        TreeImpostors(const TreeImpostors&) = delete;
        TreeImpostors& operator=(const TreeImpostors&) = delete;

        // Case de la table des images de l'atlas de couleur de l'imposteur du modèle (normales : la suivante), 0 tant
        // qu'il n'est pas capturé : il est alors demandé. Un modèle relu (fichier changé) est capturé à nouveau.
        std::uint32_t Find(const GpuModel& model);
        // Fin de la préparation de l'image : oublie les imposteurs plus demandés, choisit la capture de l'image.
        void EndFrame(std::uint32_t frameIndex);

        // Capture de l'image (MeshRenderer::BakeImpostors), dans une passe dont color, normal et depth sont les cibles.
        // commands : une DrawIndexedCommand par primitive de model (Grid² instances, une par vue), à la suite.
        struct Capture
        {
            const GpuModel* model;
            const RHI::Texture* color;
            const RHI::Texture* normal;
            const RHI::Texture* depth;
            const RHI::Buffer* commands;
            std::uint32_t primitiveCount;
        };
        std::optional<Capture> GetCapture(std::uint32_t frameIndex) const;
        // La capture est enregistrée : l'imposteur sert dès l'image suivante (le GPU la fait avant).
        void Captured();

        private:
        TreeImpostors(RHI::Device& device, ModelCache& models) : m_device(device), m_models(models) {}

        struct Entry
        {
            // Ce qui change quand le modèle est relu à la même adresse.
            std::uint32_t page = 0;
            std::uint32_t firstVertex = 0;
            std::filesystem::file_time_type writeTime{};
            std::unique_ptr<RHI::Texture> color;
            std::unique_ptr<RHI::Texture> normal;
            std::uint32_t slot = 0; // 0 : pas de place dans la table
            bool ready = false;
            bool failed = false; // pas de mémoire ou de case : le modèle lointain reste
            std::uint64_t lastUsed = 0;
        };
        // Rend textures et cases quand le GPU ne les lit plus.
        void Retire(Entry& entry);

        RHI::Device& m_device;
        ModelCache& m_models;
        std::unordered_map<const GpuModel*, Entry> m_entries;
        std::unique_ptr<RHI::Texture> m_depth; // partagé par les captures, créé à la première
        std::array<std::unique_ptr<RHI::Buffer>, RHI::FramesInFlight> m_commands;
        const GpuModel* m_capture = nullptr;
        std::uint32_t m_captureFrame = 0;
        std::uint32_t m_capturePrimitives = 0;
        std::uint64_t m_frame = 0;
    };
}
