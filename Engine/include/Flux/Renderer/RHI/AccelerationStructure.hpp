#pragma once

// Structures d'accélération du ray tracing (Device::HasRayTracing) : une basse par modèle (ses triangles), une haute
// pour la scène (ses instances, chacune une basse placée par une matrice). Les shaders y lancent des rayons (ray
// queries) sans parcourir les triangles un à un.

#include "Flux/Renderer/RHI/Buffer.hpp"

#include <array>
#include <cstdint>

namespace Flux::RHI {
    class AccelerationStructure;

    // Triangles d'une structure basse : positions (3 float au début de chaque sommet) et indices 32 bits relatifs au
    // premier sommet, lus dans des buffers créés avec BufferUsage::AccelerationInput. Lus à la construction seulement.
    struct AccelerationGeometry
    {
        const Buffer* vertices = nullptr;
        std::uint64_t vertexOffset = 0; // octets : le premier sommet
        std::uint32_t vertexStride = 0;
        std::uint32_t vertexCount = 0;
        const Buffer* indices = nullptr;
        std::uint64_t indexOffset = 0; // octets
        std::uint32_t indexCount = 0;  // multiple de 3
        // false : seuil d'alpha, le shader décide de chaque triangle rencontré (plus lent).
        bool opaque = true;
    };

    // Une instance d'une structure haute.
    struct AccelerationInstance
    {
        std::array<std::array<float, 4>, 3> transform{}; // lignes d'une matrice 3 x 4 (relative à la caméra)
        std::uint32_t customIndex = 0; // 24 bits, rendu au shader (indice des données de l'entité)
        std::uint8_t mask = 0xFF;      // rayons qui la voient (masque du shader & mask non nul)
        const AccelerationStructure* bottom = nullptr; // déjà construite (CommandList::BuildBottomLevels)
    };

    // Créée par Device::CreateBottomLevel ou CreateTopLevel, construite par la liste de commandes. Ne se détruit pas
    // pendant que le GPU la lit ou la construit (Device::Retire).
    class AccelerationStructure {
        public:
        virtual ~AccelerationStructure() = default;
        // Mémoire vidéo de la structure elle-même (octets).
        virtual std::uint64_t GetSize() const = 0;
        // Mémoire de travail que demande sa construction (octets), alignement compris : une place de cette taille dans
        // un buffer BufferUsage::AccelerationScratch.
        virtual std::uint64_t GetScratchSize() const = 0;
        // Structure haute : instances au plus (0 pour une basse).
        virtual std::uint32_t GetMaxInstances() const = 0;
    };
}
