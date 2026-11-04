#pragma once

#include "Flux/Export.hpp"
#include "Flux/Renderer/Renderer.hpp"
#include "Flux/Renderer/RHI/RHI.hpp"

#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Flux {
    class JobSystem;

    // Graphe de rendu : l'image décrite comme une suite de passes qui disent ce qu'elles lisent et écrivent, au lieu
    // d'appels enchaînés à la main. Décrit une fois (AddPass, puis Compile qui vérifie les dépendances), joué à chaque
    // image (Execute) : pas d'allocation par image.
    // Une passe est faite d'étapes (Step), chacune nommée : le graphe pose avant chacune le repère de plantage
    // (CommandList::BeginStep) et, sur demande, une heure du GPU ; le temps de chaque étape et de chaque passe en
    // découle (ReadTimings) sans liste d'heures à tenir à part.
    // Barrières : une ressource qui est une texture (AddTexture) est préparée par le graphe avant chaque passe qui la
    // déclare : lue (Sampled), ou écrite (cible de couleur, ou de profondeur selon son format). Les passes n'en posent
    // plus, sauf pour leurs textures internes (niveaux du bloom). Les buffers gardent leurs barrières de calcul
    // (CommandList::ComputeBarrier). L'ordre est celui de la description (Engine/ROADMAP.md, étapes suivantes).
    // Enregistrement en parallèle (SetParallelism) : chaque image se joue en deux temps. D'abord, sur le fil qui
    // appelle Execute et dans l'ordre, les conditions, les Prepare des passes, les barrières prévues
    // (CommandList::PlanTextures) et les échanges d'historiques. Puis les passes, coupées en groupes qui se suivent,
    // chacun enregistré par un fil dans sa liste (CommandList::Fork), équilibrés sur le temps CPU de chaque passe aux
    // images précédentes. Les Record ne changent donc aucun état qu'une autre passe lit : ce changement va dans Prepare.
    // Textures de travail (CreateTexture) : créées par le graphe à la taille de la vue 3D ou de la fenêtre, recréées
    // quand elle change (ResizeTextures). Un historique (Lifetime::Previous) en a deux exemplaires : une passe lit le
    // dernier écrit et écrit l'autre, qui devient le dernier écrit ; rien à échanger à la main.
    class FLUX_API RenderGraph {
        public:
        using ResourceId = std::uint32_t;
        using Record = std::function<void(RHI::CommandList&)>;
        using Condition = std::function<bool()>;
        // Texture d'une ressource, demandée à chaque image (elle change avec la taille de la vue, ou d'une image à
        // l'autre pour un historique) ; nullptr : pas utilisée à cette image.
        using TextureSource = std::function<const RHI::Texture*()>;

        // Contenu d'une ressource au début de l'image.
        enum class Lifetime : std::uint8_t {
            Frame,    // écrit par une passe de l'image avant d'être lu
            Previous, // gardé de l'image précédente : lisible avant d'être réécrit (historique, pyramide de profondeur)
            External, // rempli hors du graphe (envois du CPU) : toujours lisible
        };

        using Timing = RenderPassTiming;

        // Taille d'une texture de travail.
        enum class TextureSize : std::uint8_t {
            Render, // la scène dessinée (couleur, mouvement, profondeur) : la vue 3D, ou moins quand FSR l'agrandit
            View,   // la vue 3D (historique, résultat de FSR)
            Window, // la fenêtre (profondeur de l'image finale)
            HalfView, // la moitié de la taille de rendu, arrondie au-dessus (nuages)
        };
        struct TextureDesc
        {
            RHI::Format format = RHI::Format::Undefined;
            TextureSize size = TextureSize::View;
            bool sampled = true;  // lue par des shaders, pas seulement cible
            bool storage = false; // aussi écrite par les compute shaders (Writes avec TextureAccess::Storage)
        };

        RenderGraph() = default;
        RenderGraph(const RenderGraph&) = delete; // possède ses textures
        RenderGraph& operator=(const RenderGraph&) = delete;

        // Ce qu'une passe décrit : ses ressources, ses étapes, et sa cible quand elle est une passe de dessin.
        class FLUX_API PassBuilder {
            public:
            PassBuilder& Reads(ResourceId resource);
            PassBuilder& Writes(ResourceId resource);
            // Écriture d'une texture autrement que comme cible de la passe (Storage : par les compute shaders).
            PassBuilder& Writes(ResourceId resource, RHI::TextureAccess access);
            // Commence la passe de dessin (BeginRenderPass, viewport…) ; le graphe la termine après la dernière étape.
            PassBuilder& Begin(Record begin);
            // Passe de dessin dont les étapes s'enregistrent en même temps, chacune dans sa liste secondaire
            // (CommandList::BeginParallelRenderPass), jouées dans l'ordre. `setup` (viewport, scissor) est appelé au
            // début de chaque liste, rien n'étant hérité. Sans fils (Execute sans JobSystem) : toutes dans la liste.
            // Au lieu de Begin ; pour les passes lourdes, chaque liste coûtant un peu.
            PassBuilder& BeginParallel(std::function<RHI::RenderPassDesc()> desc, Record setup = {});
            // Passe jouée seulement quand `condition` est vraie (vérifiée à chaque image).
            PassBuilder& When(Condition condition);
            // Appelé à chaque image où la passe joue, sur le fil d'Execute, avant tout enregistrement et dans l'ordre
            // des passes : l'état que lisent les passes suivantes (exemplaire d'historique écrit, sortie choisie).
            PassBuilder& Prepare(std::function<void()> prepare);
            // `name` : texte constant, vivant jusqu'à la fin du programme (repère de plantage, panneau Moteur).
            PassBuilder& Step(const char* name, Record record, Condition condition = {});

            private:
            friend class RenderGraph;
            PassBuilder(RenderGraph& graph, std::uint32_t pass) : m_graph(graph), m_pass(pass) {}
            RenderGraph& m_graph;
            std::uint32_t m_pass;
        };

        // Ressource que le graphe ordonne seulement (buffers, envois du CPU).
        ResourceId AddResource(const char* name, Lifetime lifetime = Lifetime::Frame);
        // Texture d'ailleurs (ciel, carte d'ombres) dont le graphe pose les barrières. Lifetime::Frame : son contenu
        // d'avant est jeté à sa première écriture de l'image.
        ResourceId AddTexture(const char* name, Lifetime lifetime, TextureSource texture);
        // Texture de travail créée par le graphe (Frame ou Previous ; Previous : deux exemplaires).
        ResourceId CreateTexture(const char* name, Lifetime lifetime, TextureDesc desc);
        // Crée ou recrée à `extent` les textures de travail de cette taille, le GPU attendu avant. true : recréées,
        // les groupes qui les désignent (BindGroup) sont à refaire. Hors de Execute.
        std::expected<bool, std::string> ResizeTextures(RHI::Device& device, TextureSize size, RHI::Extent extent);
        // Texture de travail telle qu'une passe la lit (le dernier exemplaire écrit) ; nullptr avant ResizeTextures.
        const RHI::Texture* GetTexture(ResourceId resource) const;
        // Exemplaire `copy` (0 ou 1) d'un historique, et celui que sa prochaine écriture remplira.
        const RHI::Texture* GetCopy(ResourceId resource, std::uint32_t copy) const;
        std::uint32_t GetWriteCopy(ResourceId resource) const;
        PassBuilder AddPass(const char* name);
        // Vérifie la description : chaque lecture suit une écriture de l'image (ou lit l'image précédente, ou une
        // ressource externe), pas de texture lue et écrite par la même passe (sauf un historique, qui a deux
        // exemplaires), heures du GPU en nombre suffisant. Le message dit quelle passe et quelle ressource.
        std::expected<void, std::string> Compile();

        // Joue les passes dans l'ordre, chacune précédée des barrières de ses textures. `timestamps` : une heure du GPU au début puis après chaque étape (même une
        // étape sautée, qui compte alors pour 0) : les cases restent celles de ReadTimings d'une image à l'autre.
        // jobs : fils qui enregistrent les groupes de passes (SetParallelism) ; nul : tout dans `commands`.
        void Execute(RHI::CommandList& commands, bool timestamps, JobSystem* jobs = nullptr);
        // Au plus `maxLists` listes par image (0 ou 1 : une seule), une par tranche de `listMicroseconds` de temps
        // CPU d'enregistrement : couper coûte une liste de plus, inutile pour des passes qui s'enregistrent vite.
        void SetParallelism(std::uint32_t maxLists, double listMicroseconds);
        // Listes remplies à la dernière image (1 : pas de parallèle).
        std::uint32_t GetListCount() const { return m_listCount; }
        // Temps des passes et de leurs étapes d'après les heures relues (Device::GetTimestamps), dans l'ordre de
        // la description : une passe, puis ses étapes si elle en a plusieurs. Le temps CPU d'enregistrement de chaque
        // passe aussi (moyenne glissante), sur sa ligne.
        std::span<const Timing> ReadTimings(std::span<const double> stamps);
        // Heures du GPU qu'utilise Execute.
        std::uint32_t GetTimestampCount() const { return m_stepCount + 1; }

        private:
        struct Resource
        {
            const char* name;
            Lifetime lifetime;
            TextureSource texture;                          // texture d'ailleurs
            std::optional<TextureDesc> desc;                // texture de travail
            std::array<std::unique_ptr<RHI::Texture>, 2> owned;
            RHI::Extent extent{};
            std::uint32_t written = 0;                      // exemplaire lu (le dernier écrit)

            bool IsTexture() const { return texture || desc; }
            bool IsHistory() const { return desc && lifetime == Lifetime::Previous; }
        };
        struct Step
        {
            const char* name;
            Record record;
            Condition condition;
            bool active = false; // à cette image
        };
        struct Pass
        {
            const char* name;
            std::vector<ResourceId> reads;
            std::vector<ResourceId> writes;
            std::vector<RHI::TextureAccess> writeAccess; // None : cible de couleur ou de profondeur selon le format
            // Textures de l'image déjà écrites par une passe d'avant (Compile) : gardées, la passe continue leur dessin.
            std::vector<ResourceId> continued;
            Record begin;
            std::function<RHI::RenderPassDesc()> parallelDesc; // BeginParallel
            Record setup;
            Condition condition;
            std::function<void()> prepare;
            std::vector<Step> steps;
            // Rangé par Compile : ses barrières dans m_planned, ses heures du GPU.
            std::uint32_t firstUse = 0;
            std::uint32_t firstSlot = 0;
            // À chaque image : jouée, barrières prévues, temps CPU d'enregistrement (moyenne glissante).
            bool active = false;
            std::uint32_t useCount = 0;
            double cpuMicroseconds = 0.0;
        };

        static constexpr std::uint32_t MaxLists = 16;

        // Premier temps d'Execute : conditions, Prepare, barrières prévues, historiques échangés.
        void Plan(RHI::CommandList& commands);
        // Barrières de la passe, dans `uses` ; leur nombre.
        std::uint32_t CollectUses(const Pass& pass, RHI::TextureUse* uses) const;
        // Groupes de passes à enregistrer chacun dans sa liste (bornes dans m_bounds) ; 1 : tout dans une liste.
        std::uint32_t ChooseGroups();
        void RecordPasses(RHI::CommandList& commands, std::uint32_t first, std::uint32_t last, bool timestamps);
        // Passe de BeginParallel : ses étapes dans des listes secondaires remplies par m_jobs (sinon dans `commands`).
        void RecordParallelSteps(RHI::CommandList& commands, const Pass& pass, bool timestamps);
        static void RecordStep(RHI::CommandList& commands, const Step& step, std::uint32_t slot, bool timestamps);
        const RHI::Texture* ReadTexture(const Resource& resource) const;
        const RHI::Texture* WriteTexture(const Resource& resource) const;

        std::vector<Resource> m_resources;
        std::vector<Pass> m_passes;
        std::vector<Timing> m_timings;
        std::vector<RHI::TextureUse> m_planned; // barrières de toutes les passes, de taille fixée par Compile
        std::uint32_t m_stepCount = 0;
        std::uint32_t m_maxLists = 1;
        double m_listMicroseconds = 100.0;
        std::array<std::uint32_t, MaxLists + 1> m_bounds{};
        std::uint32_t m_listCount = 1;
        JobSystem* m_jobs = nullptr; // de l'Execute en cours
    };
}
