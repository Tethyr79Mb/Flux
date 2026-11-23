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
#include <vector>

namespace Flux {
    // Animations que joue un modèle avec un graphe : les siennes, puis celles des fichiers du graphe
    // (AnimationGraph::files), recâblées sur les os du modèle. Les clips des animateurs (AnimationClipWeight::animation)
    // sont des indices dans animations.
    struct AnimationSet
    {
        const ModelSkeleton* skeleton = nullptr; // celui du modèle
        std::vector<const ModelAnimation*> animations;
        std::vector<ModelAnimation> borrowed; // celles des fichiers, à rebours de leurs pointeurs dans animations

        // Indice de l'animation de ce nom (vide : la première), -1 s'il n'y en a pas.
        FLUX_SIM_API int Find(std::string_view name) const;
    };

    // Recopie les animations de source sur les nœuds de target de même nom (« mixamorig:Hips » vaut « Hips ») ; les
    // courbes d'un nœud absent de target sont laissées. Les valeurs ne sont pas converties : même squelette attendu
    // (mêmes longueurs d'os et même échelle de l'armature), sinon les translations animées (hanches) seront fausses.
    FLUX_SIM_API std::vector<ModelAnimation> RetargetAnimations(const ModelSkeleton& source, const ModelSkeleton& target);

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
        // Animations du modèle avec celles des fichiers du graphe (graph vide : les siennes seules) ; nullptr sans
        // squelette. Un fichier manquant ou sans animation est dit au journal et laissé.
        const AnimationSet* GetAnimationSet(std::string_view model, std::string_view graph);
        // Sans rien lire : nullptr si GetAnimationSet n'a pas encore été appelé pour eux (le rendu, qui suit la simulation).
        const AnimationSet* FindAnimationSet(std::string_view model, std::string_view graph) const;

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
        using SetMap = std::unordered_map<std::string, Map<AnimationSet>, Hash, std::equal_to<>>; // modèle, puis graphe

        std::filesystem::path m_directory;
        Map<ModelSkeleton> m_skeletons;
        Map<AnimationGraph> m_graphs;
        SetMap m_sets;
    };
}
