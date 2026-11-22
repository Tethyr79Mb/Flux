#pragma once

#include "Flux/Animation/AnimationGraph.hpp"
#include "Flux/Assets/Model.hpp"
#include "Flux/SimExport.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

namespace Flux {
    // Squelettes (os et animations des modèles) et graphes d'animation du projet, lus une fois à la première demande
    // pour la simulation : le serveur, sans rendu, connaît ainsi les animations comme le client. Chemins relatifs aux
    // assets, comme dans les composants.
    // Coût : la première demande d'un modèle le lit en entier (sommets compris) sur le fil qui demande, au
    // démarrage du jeu ou à l'arrivée d'un nouveau modèle animé.
    class FLUX_SIM_API AnimationLibrary {
        public:
        AnimationLibrary();
        ~AnimationLibrary();
        AnimationLibrary(const AnimationLibrary&) = delete;
        AnimationLibrary& operator=(const AnimationLibrary&) = delete;

        // Oublie ce qui a été lu : un fichier modifié est relu à sa prochaine demande.
        void SetAssetsDirectory(const std::filesystem::path& directory);
        void Clear();

        // nullptr si le fichier manque, est illisible ou n'a pas de squelette (dit une fois au journal).
        const ModelSkeleton* GetSkeleton(std::string_view model);
        const AnimationGraph* GetGraph(std::string_view graph);

        // Graphe donné par le code (tests, aperçus) : remplace celui du fichier de ce nom.
        void SetGraph(std::string_view name, AnimationGraph graph);
        void SetSkeleton(std::string_view name, ModelSkeleton skeleton);

        private:
        // Recherche par string_view sans créer de std::string (appelée à chaque pas pour chaque animateur).
        struct Hash
        {
            using is_transparent = void;
            std::size_t operator()(std::string_view text) const { return std::hash<std::string_view>{}(text); }
        };
        template <typename T>
        using Map = std::unordered_map<std::string, std::unique_ptr<T>, Hash, std::equal_to<>>; // nullptr : illisible

        std::filesystem::path m_directory;
        Map<ModelSkeleton> m_skeletons;
        Map<AnimationGraph> m_graphs;
    };
}
