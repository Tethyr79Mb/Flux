#pragma once

// Agrandisseur temporel (FSR).

#include "Renderer/RHI/Texture.hpp"

#include <cstdint>

namespace Flux::RHI {
    class CommandList;

    // Ce que l'agrandisseur temporel (Upscaler) reçoit à chaque image. Textures lisibles (Sampled) à la taille de
    // rendu, sauf output : écrite par les compute shaders, à la taille de la vue (CreateRenderTarget storage).
    struct UpscaleInputs
    {
        const Texture* color = nullptr;  // la scène, HDR linéaire
        const Texture* depth = nullptr;  // reverse-Z infini
        const Texture* motion = nullptr; // où chaque pixel était à l'image précédente, moins où il est (en uv)
        // Facultatif : 0 à 1, où l'image du moment compte plus que l'historique (nuages, flammes : ce que le mouvement
        // ne décrit pas).
        const Texture* reactive = nullptr;
        const Texture* output = nullptr;
        Extent renderSize;               // partie dessinée de color, depth et motion
        float jitterX = 0.0f;            // décalage de la projection en pixels de rendu (x vers la droite, y vers le bas)
        float jitterY = 0.0f;
        float sharpness = 0.0f;          // 0 à 1
        float frameMilliseconds = 16.7f; // depuis l'image précédente
        float nearPlane = 0.1f;
        float verticalFov = 1.0f;        // radians
        bool reset = false;              // rien à reprendre des images précédentes (saut de la caméra)
    };

    // Agrandisseur temporel (FSR 3.1 d'AMD pour Vulkan) : la scène dessinée petite, décalée d'une fraction de pixel
    // différente à chaque image, devient une image à la taille de la vue. Garde son propre historique : un par taille
    // de vue (Device::CreateUpscaler à nouveau quand elle change).
    class Upscaler {
        public:
        virtual ~Upscaler() = default;
        // Nombre de décalages différents avant de recommencer (plus la scène est réduite, plus il en faut).
        virtual std::uint32_t GetJitterPhaseCount(std::uint32_t renderWidth) const = 0;
        // Hors de toute passe, les entrées déjà lisibles (Sampled) et output prête à l'écriture (Storage). Les barrières
        // internes à FSR sont posées et défaites par lui : les textures sortent dans l'usage où elles sont entrées.
        virtual void Dispatch(CommandList& commands, const UpscaleInputs& inputs) = 0;
    };
}
