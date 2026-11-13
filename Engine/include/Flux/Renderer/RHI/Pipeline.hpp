#pragma once

// Pipelines graphiques et de calcul.

#include "Flux/Renderer/RHI/RHITypes.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>

namespace Flux::RHI {
    // Cible de couleur après la principale (PipelineDesc::extraColors).
    struct ExtraColorTarget
    {
        Format format = Format::Undefined; // celui de la texture de la passe
        // false : laissée telle quelle (le shader n'a pas à l'écrire).
        bool write = false;
        // Mélangée comme la cible principale (PipelineDesc::blend) ; sinon remplacée. Des données (mouvement,
        // normales) ne se mélangent pas : seulement une lumière mise à part (soleil des ombres en rayons).
        bool blend = false;
    };

    struct VertexBufferLayout
    {
        std::uint32_t binding = 0;
        std::uint32_t stride = 0;
    };

    struct VertexAttribute
    {
        std::uint32_t location = 0;
        std::uint32_t binding = 0;
        Format format = Format::Undefined;
        std::uint32_t offset = 0;
    };

    class BindGroupLayout;

    struct PipelineDesc
    {
        // Programme compilé (SPIR-V pour Vulkan) contenant les deux points d'entrée.
        std::filesystem::path shaderPath;
        const char* vertexEntryPoint = "vertexMain";
        const char* fragmentEntryPoint = "fragmentMain"; // nullptr : aucun (passe de profondeur seule)
        // Pipeline à mesh shaders (Device::HasMeshShaders) : étape de maillage, et de tâche avant elle, à la place
        // des sommets (vertexEntryPoint, vertexBuffers et vertexAttributes ignorés) ; dessiné par DrawMeshTasks.
        const char* taskEntryPoint = nullptr;
        const char* meshEntryPoint = nullptr;
        std::span<const VertexBufferLayout> vertexBuffers;
        std::span<const VertexAttribute> vertexAttributes;
        // Groupe i : layouts[i] (BindGroup lié à l'indice i).
        std::span<const BindGroupLayout* const> bindGroupLayouts;
        // Petites constantes envoyées avec chaque appel de dessin (CommandList::PushConstants), vues des deux
        // étapes ; 128 octets au plus sont garantis partout.
        std::uint32_t pushConstantsSize = 0;
        Format colorFormat = Format::Undefined; // Undefined : pas de couleur (passe de profondeur seule)
        // Cibles suivantes de la passe (RenderPassDesc::extraColorTargets, dans le même ordre : SV_Target1, 2…),
        // jusqu'à la première de format Undefined.
        std::array<ExtraColorTarget, MaxColorTargets - 1> extraColors{};
        // false : la couleur n'est pas écrite, seulement la profondeur (pré-passe de profondeur dans une passe avec
        // couleur).
        bool writeColor = true;
        // Doit correspondre à la passe, même si ce pipeline ne teste pas la profondeur.
        Format depthFormat = Format::Undefined;
        bool depthTest = false;
        bool depthWrite = false;
        CompareOp depthCompare = CompareOp::Greater; // reverse-Z : plus proche = plus grand
        CullMode cullMode = CullMode::None;
        BlendMode blend = BlendMode::Opaque;
        // Décalage de la profondeur écrite (carte d'ombres) : constant, et selon la pente de la face.
        float depthBiasConstant = 0.0f;
        float depthBiasSlope = 0.0f;
        // Profondeur hors de [0, 1] ramenée au bord au lieu d'être coupée (cartes d'ombres : un obstacle plus loin vers
        // le soleil que la carte n'en couvre y est aplati devant tout, il fait encore de l'ombre).
        bool depthClamp = false;
    };

    // Pipeline de calcul : un point d'entrée [shader("compute")] lancé par CommandList::Dispatch.
    struct ComputePipelineDesc
    {
        std::filesystem::path shaderPath;
        const char* entryPoint = "computeMain";
        std::span<const BindGroupLayout* const> bindGroupLayouts;
        std::uint32_t pushConstantsSize = 0; // 128 octets au plus
    };

    class Pipeline {
        public:
        virtual ~Pipeline() = default;
    };
}
