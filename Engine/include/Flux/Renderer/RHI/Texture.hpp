#pragma once

// Textures, samplers et usage d'une texture par une passe.

#include "Flux/Renderer/RHI/RHITypes.hpp"

namespace Flux::RHI {
    struct SamplerDesc
    {
        Filter filter = Filter::Linear;
        Filter mipFilter = Filter::Linear;
        AddressMode address = AddressMode::Repeat;
        // Carte d'ombres : la lecture compare une profondeur donnée à celle de la texture (1 si elle est plus proche
        // ou égale, 0 sinon) ; filtrée, la comparaison adoucit le bord des ombres.
        bool compare = false;
        // Filtrage anisotrope (1 : aucun) : une texture vue de biais (le sol au loin) reste nette ; borné par le GPU.
        float anisotropy = 1.0f;
    };

    class Texture {
        public:
        virtual ~Texture() = default;
        virtual Extent GetExtent() const = 0;
        virtual Format GetFormat() const = 0;
    };

    // `discard` : le contenu d'avant ne sert plus (la passe efface tout ou réécrit tout), le GPU peut le jeter.
    struct TextureUse
    {
        const Texture* texture = nullptr;
        TextureAccess access = TextureAccess::None;
        bool discard = false;
    };

    class Sampler {
        public:
        virtual ~Sampler() = default;
    };
}
