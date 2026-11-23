#pragma once

#include "Flux/SimExport.hpp"

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace Flux {
    // Graphe d'animation (fichier .fluxanim, JSON) : des paramètres (nombres posés par le moteur et le jeu), et des
    // couches (corps entier, puis par exemple le haut du corps qui vise) faites d'états qui jouent une animation ou un
    // mélange de plusieurs, et de transitions de l'un à l'autre quand leurs conditions sont vraies. Le graphe décrit le comportement ; sa lecture pour une entité est dans AnimatorComponent
    // (Animator::Update).
    struct AnimationGraph
    {
        // Assez pour un personnage armé ; l'animateur garde leurs valeurs dans un tableau fixe (pas d'allocation).
        static constexpr std::size_t MaxParameters = 16;

        struct Parameter
        {
            std::string name;
            float value = 0.0f; // au début du jeu, tant que personne ne l'a posé
        };

        // Animation d'un état, placée à value sur l'axe de son paramètre de mélange.
        struct BlendPoint
        {
            std::string animation; // nom dans le fichier du modèle
            float value = 0.0f;
            // Décalage de son cycle (0 à 1) : cale les pas d'une animation sur ceux d'une autre (même pied au même instant).
            float phase = 0.0f;
        };

        struct State
        {
            std::string name;
            // Lu par le moteur ou le jeu pour trouver un état sans dépendre de son nom (les sièges : SitDownTag, StandUpTag).
            std::string tag;
            // Une animation, ou plusieurs mélangées selon le paramètre blendParameter (valeurs croissantes) : à
            // mi-chemin entre deux points, moitié de chacune, cycles calés l'un sur l'autre. Hors des points, la première
            // ou la dernière, jouée plus lentement ou plus vite en proportion (valeur / celle du point).
            std::vector<BlendPoint> points;
            int blendParameter = -1;
            float speed = 1.0f; // vitesse de lecture
            bool loop = true;   // sinon jouée une fois, finie sur sa dernière image
        };

        enum class Compare : std::uint8_t { Less, LessEqual, Greater, GreaterEqual, Equal, NotEqual };

        struct Condition
        {
            int parameter = 0;
            Compare compare = Compare::Greater;
            float value = 0.0f;
        };

        // De from (-1 : depuis n'importe quel autre état) vers to quand toutes ses conditions sont vraies ; avec atEnd,
        // seulement une fois l'animation (non bouclée) de from finie. Essayées dans l'ordre du fichier, la première
        // valable l'emporte. Fondu de duration secondes.
        struct Transition
        {
            int from = -1;
            int to = 0;
            std::vector<Condition> conditions;
            float duration = 0.25f;
            bool atEnd = false;
        };

        // États et transitions d'une couche.
        struct StateMachine
        {
            std::vector<State> states; // au moins un
            std::vector<Transition> transitions;
            int start = 0; // état du début

            // Indice de l'état de ce nom ou de cette étiquette, -1 s'il n'y en a pas.
            FLUX_SIM_API int FindState(std::string_view name) const;
            FLUX_SIM_API int FindTag(std::string_view tag) const;
        };

        // Couche : sa machine à états joue par-dessus les couches d'avant, sur les os de son masque (l'os mask et ses
        // descendants ; vide : tout le corps), avec la part weight (paramètre weightParameter, borné à 0..1, ou 1),
        // atteinte en douceur en fade secondes. La première couche est le corps entier, sans masque ni poids.
        struct Layer : StateMachine
        {
            std::string name;
            std::string mask;
            int weightParameter = -1;
            float weight = 1.0f; // sans paramètre
            float fade = 0.25f;
            // Rotation du premier os du masque prise dans le repère du modèle et non par rapport à son parent : le buste
            // garde l'orientation du clip même quand les hanches de la couche du dessous regardent ailleurs (clip de
            // tir de profil, hanches tournées, joué sur la marche).
            bool modelSpace = false;
        };

        static constexpr std::size_t MaxLayers = 4;

        std::vector<Parameter> parameters;
        std::vector<Layer> layers; // au moins une : le corps entier
        // Fichiers dont les animations s'ajoutent à celles du modèle (chemins relatifs aux assets), par exemple une
        // animation Mixamo par fichier : même squelette, os reconnus par leur nom. L'animation seule dans son fichier
        // prend le nom du fichier (Viser.glb : « Viser »). Un nom déjà pris est celui du modèle, puis du premier
        // fichier qui l'a.
        std::vector<std::string> files;

        // Indice du paramètre de ce nom, -1 s'il n'y en a pas.
        FLUX_SIM_API int FindParameter(std::string_view name) const;
    };

    // Paramètres que le moteur pose lui-même sur un personnage (CharacterComponent), s'ils sont déclarés par son graphe.
    namespace AnimationParameters {
        inline constexpr std::string_view Speed = "speed";     // vitesse au sol, m/s
        // Allure : 0 à l'arrêt, 1 à sa vitesse de marche, 2 à sa vitesse de course (entre les deux en proportion,
        // au-delà de 2 en proportion de la course). Points de mélange à 1 et 2 : chaque animation jouée à sa juste vitesse.
        inline constexpr std::string_view Gait = "gait";
        inline constexpr std::string_view AirTime = "airTime"; // secondes passées en l'air, 0 au sol
        inline constexpr std::string_view Seat = "seat";       // SeatState : 0 debout, 1 s'assoit, 2 assis, 3 se lève
    }

    // Étiquettes des états que lisent les sièges : la durée de leur animation est celle du geste.
    inline constexpr std::string_view SitDownTag = "sitDown";
    inline constexpr std::string_view StandUpTag = "standUp";

    // Lit un graphe ; l'erreur dit quoi et où en français.
    FLUX_SIM_API std::expected<AnimationGraph, std::string> ParseAnimationGraph(std::string_view text);
}
