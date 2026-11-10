#pragma once

// Liste de commandes d'une image et passes de dessin.

#include "Flux/Math/Color.hpp"
#include "Flux/Renderer/RHI/AccelerationStructure.hpp"
#include "Flux/Renderer/RHI/BindGroup.hpp"
#include "Flux/Renderer/RHI/Buffer.hpp"
#include "Flux/Renderer/RHI/Pipeline.hpp"
#include "Flux/Renderer/RHI/Texture.hpp"

#include <cstdint>
#include <span>

namespace Flux::RHI {
    // Une commande de CommandList::DrawIndexedIndirect, comme VkDrawIndexedIndirectCommand. firstInstance arrive
    // au vertex shader (SV_VulkanInstanceID) : les shaders de Mesh.slang y lisent l'indice de leurs données.
    struct DrawIndexedCommand
    {
        std::uint32_t indexCount;
        std::uint32_t instanceCount;
        std::uint32_t firstIndex;
        std::int32_t vertexOffset;
        std::uint32_t firstInstance;
    };

    // Passe qui dessine dans l'image de la swapchain, effacée, avec un tampon de profondeur effacé (s'il y en a un).
    // Sans couleur (color = false) : profondeur seule dans `depth` (carte d'ombres, CreateRenderTarget sampled).
    // Avec colorTarget : dans une texture (CreateRenderTarget sampled, de couleur) ou dans la face colorLayer du niveau
    // de mip colorMip d'une texture cube (CreateCubeRenderTarget), avec ou sans profondeur.
    // Les textures doivent déjà servir de cibles (UseTextures : ColorTarget, DepthTarget) ; la passe ne change pas leur
    // usage : pour les lire ensuite, UseTextures(Sampled). Seule l'image de la swapchain est préparée par la passe.
    struct RenderPassDesc
    {
        Math::LinearColor clearColor;
        const Texture* depth = nullptr; // Format::D32Float, de la taille de la cible (swapchain, texture, carte d'ombres)
        float clearDepth = 0.0f;        // reverse-Z : 0 = infiniment loin
        bool color = true;
        const Texture* colorTarget = nullptr;
        std::uint32_t colorLayer = 0;
        std::uint32_t colorMip = 0; // texture cube seulement : la passe couvre ce niveau (sa taille)
        // Avec colorTarget seulement : seconde texture de la même taille, effacée à 0.
        const Texture* secondColorTarget = nullptr;
        // Tampon de profondeur gardé après la passe, pour être lu ensuite (CreateRenderTarget sampled).
        bool keepDepth = false;
        // La profondeur d'avant est gardée au lieu d'être effacée (une passe précédente l'a déjà remplie) ; sans
        // couleur, CommandList::ClearDepth efface les parties à redessiner.
        bool loadDepth = false;
        // Avec colorTarget seulement : la couleur d'avant (et la seconde) est gardée au lieu d'être effacée, la passe
        // continue ce qu'une précédente a dessiné.
        bool loadColor = false;
    };

    // Listes parallèles au plus par soumission (une image, un Device::Submit), tous Fork compris.
    inline constexpr std::uint32_t MaxParallelCommandLists = 64;
    // Listes secondaires au plus par soumission, toutes BeginParallelRenderPass comprises.
    inline constexpr std::uint32_t MaxSecondaryCommandLists = 64;

    // Commandes d'une image, enregistrées entre Device::BeginFrame et Device::EndFrame.
    class CommandList {
        public:
        virtual ~CommandList() = default;
        // Insère ici `count` listes à remplir en parallèle, jouées par le GPU dans l'ordre du tableau : après ce que
        // cette liste a enregistré jusqu'ici, avant ce qu'elle enregistrera ensuite. Chaque liste est remplie par un
        // seul fil à la fois, et finie (passes fermées) avant Device::EndFrame ou la fin de Submit. Hors d'une passe,
        // sur la liste de l'image (ou de Submit) seulement. Cette liste perd ensuite pipeline et groupes liés.
        // UseTextures suit l'usage que chaque texture a noté à l'enregistrement : une texture change d'usage dans une
        // seule des listes parallèles, et aucune autre liste n'y touche avant que celle-ci soit remplie (ou ses
        // barrières sont prévues avant, par PlanTextures). Vide au-delà de MaxParallelCommandLists, ou sur une liste
        // parallèle : enregistrer alors dans cette liste-ci.
        virtual std::span<CommandList* const> Fork(std::uint32_t count) = 0;
        // Prépare chaque texture à son nouvel usage, en une barrière pour toutes : attend ce qui l'écrivait ou la
        // lisait avant, change son layout. Sans effet pour une texture déjà lue (Sampled) et relue. Hors d'une passe.
        virtual void UseTextures(std::span<const TextureUse> uses) = 0;
        // Prévoit des barrières sans rien enregistrer : dans l'ordre, chaque texture note son nouvel usage et `from`
        // reçoit celui d'avant. Ces TextureUse passés plus tard à UseTextures, dans n'importe quelle liste, posent
        // leurs barrières sans relire ni changer l'usage noté : l'enregistrement en parallèle garde l'ordre prévu.
        virtual void PlanTextures(std::span<TextureUse> uses) = 0;
        void Use(const Texture& texture, TextureAccess access, bool discard = false) {
            const TextureUse use{&texture, access, discard};
            UseTextures({&use, 1});
        }
        virtual void BeginRenderPass(const RenderPassDesc& desc) = 0;
        // Passe de dessin remplie par `count` listes secondaires, une par fil s'il le faut, jouées dans l'ordre du
        // tableau. Jusqu'à EndRenderPass, cette liste n'enregistre rien d'autre ; chaque liste secondaire pose son
        // viewport, son scissor, son pipeline et ses groupes (rien n'est hérité), sans BeginRenderPass, UseTextures,
        // Fork ni Dispatch, et elles sont toutes remplies avant EndRenderPass. Vide (passe commencée comme par
        // BeginRenderPass, à remplir dans cette liste) : image de la swapchain, au-delà de MaxSecondaryCommandLists,
        // ou dans une liste secondaire.
        virtual std::span<CommandList* const> BeginParallelRenderPass(const RenderPassDesc& desc, std::uint32_t count) = 0;
        virtual void EndRenderPass() = 0;
        virtual void SetViewport(const Viewport& viewport) = 0;
        virtual void SetScissor(const Rect& rect) = 0;
        // Dans une passe : remplit `rect` de la profondeur `depth` (passe à profondeur gardée, RenderPassDesc::loadDepth).
        virtual void ClearDepth(const Rect& rect, float depth) = 0;
        virtual void BindPipeline(const Pipeline& pipeline) = 0;
        // Pipeline lié avant : ses layouts disent où vont les groupes. Un groupe reste lié d'un pipeline à un autre
        // si les deux déclarent les mêmes layouts jusqu'à cet indice.
        virtual void SetBindGroup(std::uint32_t index, const BindGroup& group) = 0;
        virtual void BindVertexBuffers(std::uint32_t first, std::span<const Buffer* const> buffers) = 0;
        virtual void BindIndexBuffer(const Buffer& buffer) = 0;
        virtual void PushConstants(const void* data, std::uint32_t size) = 0;
        virtual void Draw(std::uint32_t vertexCount, std::uint32_t firstVertex) = 0;
        // Une instance, d'indice `instance` (SV_VulkanInstanceID).
        virtual void DrawIndexed(std::uint32_t indexCount, std::uint32_t firstIndex, std::uint32_t instance = 0) = 0;
        // `count` commandes DrawIndexedCommand lues dans `buffer` (BufferUsage::Indirect) à partir de `offset` : un
        // seul appel pour beaucoup de maillages qui partagent pipeline, sommets et indices.
        virtual void DrawIndexedIndirect(const Buffer& buffer, std::uint64_t offset, std::uint32_t count) = 0;
        // Pipeline à mesh shaders lié : `groups` groupes de l'étape de tâche (SV_GroupID.x de 0 à groups - 1).
        virtual void DrawMeshTasks(std::uint32_t groups) = 0;
        // Compute shader du pipeline lié (CreateComputePipeline), en groupes de threads ; hors d'une passe.
        virtual void Dispatch(std::uint32_t groupsX, std::uint32_t groupsY = 1, std::uint32_t groupsZ = 1) = 0;
        // Les buffers écrits par les Dispatch précédents deviennent lisibles par la suite : sommets, index, shaders,
        // et CPU (Readback). Hors d'une passe.
        virtual void ComputeBarrier() = 0;
        // Chaque niveau de mip de la texture recalculé par réduction de moitié du précédent, à partir du niveau 0 (écrit
        // par un compute shader, par exemple) ; la texture finit lue par les shaders (Sampled). Hors d'une passe.
        virtual void GenerateMipmaps(const Texture& texture) = 0;
        // Copie `size` octets d'un buffer à l'autre (GpuOnly ou Readback), après tout ce qui précède dans la liste ;
        // le résultat est lisible par la suite et par le CPU (Readback, après Device::Submit). Hors d'une passe.
        virtual void CopyBuffer(const Buffer& source, std::uint64_t sourceOffset, Buffer& target, std::uint64_t targetOffset,
                                std::uint64_t size) = 0;
        // Ray tracing (Device::HasRayTracing), hors d'une passe. Construit les structures basses, toutes en même temps :
        // chacune prend sa place de travail à la suite dans `scratch` (BufferUsage::AccelerationScratch, au moins la
        // somme de leurs GetScratchSize), qu'aucune autre commande en vol n'utilise. Elles sont ensuite lisibles par
        // BuildTopLevel. Une structure basse ne se construit qu'une fois.
        virtual void BuildBottomLevels(std::span<AccelerationStructure* const> structures, const Buffer& scratch) = 0;
        // Reconstruit la structure haute avec ces instances (au plus GetMaxInstances), écrites par le CPU dans sa
        // mémoire : aucune image en vol ne doit la lire. Lisible ensuite par les shaders.
        virtual void BuildTopLevel(AccelerationStructure& structure, std::span<const AccelerationInstance> instances,
                                   const Buffer& scratch) = 0;
        // Note l'heure du GPU quand tout ce qui précède est fini, dans la case `slot` (< MaxTimestamps) de l'image en
        // cours ; relue deux images plus tard par Device::GetTimestamps. Sans effet si le GPU ne sait pas le faire.
        virtual void WriteTimestamp(std::uint32_t slot) = 0;
        // Diagnostic de plantage : les commandes qui suivent forment l'étape `name` (texte constant, vivant jusqu'à la
        // fin du programme), jusqu'à l'étape suivante ou la fin de l'image ; MaxSteps par image au plus. Si le GPU se
        // bloque, le journal et le fichier plantage-gpu.txt disent quelles étapes avaient commencé ou fini. Sans effet
        // sans l'extension qui le permet (AMD) ni hors d'une image.
        virtual void BeginStep(const char* name) = 0;
    };
}
