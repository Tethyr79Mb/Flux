#pragma once

#include "Flux/Export.hpp"
#include "Flux/Renderer/Renderer.hpp"
#include "Renderer/RHI/RHI.hpp"

#include <cstdint>
#include <expected>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace Flux {
    // Graphe de rendu : l'image décrite comme une suite de passes qui disent ce qu'elles lisent et écrivent, au lieu
    // d'appels enchaînés à la main. Décrit une fois (AddPass, puis Compile qui vérifie les dépendances), joué à chaque
    // image (Execute) : pas d'allocation par image.
    // Une passe est faite d'étapes (Step), chacune nommée : le graphe pose avant chacune le repère de plantage
    // (CommandList::BeginStep) et, sur demande, une heure du GPU ; le temps de chaque étape et de chaque passe en
    // découle (ReadTimings) sans liste d'heures à tenir à part.
    // Étape 1 du render graph (Engine/ROADMAP.md) : l'ordre est celui de la description, les barrières et les cibles
    // restent faites par les passes et le RHI ; les lectures et écritures déclarées servent déjà à vérifier l'ordre.
    class FLUX_API RenderGraph {
        public:
        using ResourceId = std::uint32_t;
        using Record = std::function<void(RHI::CommandList&)>;
        using Condition = std::function<bool()>;

        // Contenu d'une ressource au début de l'image.
        enum class Lifetime : std::uint8_t {
            Frame,    // écrit par une passe de l'image avant d'être lu
            Previous, // gardé de l'image précédente : lisible avant d'être réécrit (historique, pyramide de profondeur)
            External, // rempli hors du graphe (envois du CPU) : toujours lisible
        };

        using Timing = RenderPassTiming;

        // Ce qu'une passe décrit : ses ressources, ses étapes, et sa cible quand elle est une passe de dessin.
        class FLUX_API PassBuilder {
            public:
            PassBuilder& Reads(ResourceId resource);
            PassBuilder& Writes(ResourceId resource);
            // Commence la passe de dessin (BeginRenderPass, viewport…) ; le graphe la termine après la dernière étape.
            PassBuilder& Begin(Record begin);
            // Passe jouée seulement quand `condition` est vraie (vérifiée à chaque image).
            PassBuilder& When(Condition condition);
            // `name` : texte constant, vivant jusqu'à la fin du programme (repère de plantage, panneau Moteur).
            PassBuilder& Step(const char* name, Record record, Condition condition = {});

            private:
            friend class RenderGraph;
            PassBuilder(RenderGraph& graph, std::uint32_t pass) : m_graph(graph), m_pass(pass) {}
            RenderGraph& m_graph;
            std::uint32_t m_pass;
        };

        ResourceId AddResource(const char* name, Lifetime lifetime = Lifetime::Frame);
        PassBuilder AddPass(const char* name);
        // Vérifie la description : chaque lecture suit une écriture de l'image (ou lit l'image précédente, ou une
        // ressource externe), heures du GPU en nombre suffisant. Le message dit quelle passe et quelle ressource.
        std::expected<void, std::string> Compile();

        // Joue les passes dans l'ordre. `timestamps` : une heure du GPU au début puis après chaque étape (même une
        // étape sautée, qui compte alors pour 0) : les cases restent celles de ReadTimings d'une image à l'autre.
        void Execute(RHI::CommandList& commands, bool timestamps) const;
        // Temps des passes et de leurs étapes d'après les heures relues (Device::GetTimestamps), dans l'ordre de
        // la description : une passe, puis ses étapes si elle en a plusieurs.
        std::span<const Timing> ReadTimings(std::span<const double> stamps);
        // Heures du GPU qu'utilise Execute.
        std::uint32_t GetTimestampCount() const { return m_stepCount + 1; }

        private:
        struct Resource
        {
            const char* name;
            Lifetime lifetime;
        };
        struct Step
        {
            const char* name;
            Record record;
            Condition condition;
        };
        struct Pass
        {
            const char* name;
            std::vector<ResourceId> reads;
            std::vector<ResourceId> writes;
            Record begin;
            Condition condition;
            std::vector<Step> steps;
        };

        std::vector<Resource> m_resources;
        std::vector<Pass> m_passes;
        std::vector<Timing> m_timings;
        std::uint32_t m_stepCount = 0;
    };
}
