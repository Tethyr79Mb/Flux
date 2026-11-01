#pragma once

#include <Flux/Scene/Components.hpp>

#include <cstdint>
#include <string>

namespace Flux {
    class ComponentRegistry;
    class Scene;
}

// Amarrage aux stations (sorti du moteur le 2026-11-01).
namespace Game {
    // Quai d'une station (entité enfant d'un vaisseau immobile) : là où un vaisseau s'amarre. Son origine est celle
    // du vaisseau posé (pour le Chasseur, son plancher de cockpit, 2,3 m au-dessus de la dalle), son -z la direction
    // de la sortie : le vaisseau amarré a son nez tourné vers elle, prêt à repartir. Le pilote automatique arrive par
    // l'avant du quai (approach mètres devant, à hover mètres de haut), s'arrête au-dessus, fait demi-tour et descend.
    struct DockComponent
    {
        float approach = 100.0f;  // m devant le quai : point d'approche, hors de la station
        float hover = 6.0f;       // m au-dessus du quai : hauteur de l'approche et du demi-tour
        float clearance = 150.0f; // m au-dessus du quai : on passe par-dessus la station quand on arrive de derrière
        // Pendant le jeu : vaisseau qui l'a réservé ou qui y est amarré.
        Flux::Entity ship = Flux::NullEntity;
    };

    // Étape du pilote automatique d'un vaisseau (Docking) : vers un quai (par-dessus la station si on arrive par
    // l'arrière ou le côté, ou par-dessous si on est plus bas qu'elle ; puis devant l'entrée, au-dessus du quai,
    // demi-tour, descente), ou en repartant (remonter, sortir).
    enum class AutopilotStage : std::uint8_t { Off, Climb, Over, Under, Approach, Enter, Turn, Descend, Rise, Exit };

    // Pilote automatique d'un vaisseau, ajouté à la première touche L (pendant le jeu, non enregistré) : son étape, et
    // le quai réservé, ou occupé une fois amarré (étape Off, le vaisseau enfant de la station).
    struct AutopilotComponent
    {
        AutopilotStage stage = AutopilotStage::Off;
        Flux::Entity dock = Flux::NullEntity;
    };

    // Enregistre DockComponent (« dock », même nom qu'au temps du moteur : les scènes se relisent).
    void RegisterDock(Flux::ComponentRegistry& registry);

    // Pilote automatique d'amarrage : conduit un vaisseau jusqu'à un quai libre d'une station (DockComponent), l'y pose
    // et en fait un enfant de la station (on se lève et on marche dans la station), puis l'en ressort. Il pilote à la
    // place du joueur en réglant ShipComponent::velocity et angularVelocity (ShipComponent::autopilot levé) ; la physique le déplace comme d'habitude.
    // Le vaisseau heurte la station comme le reste (physique du moteur) : le trajet passe par l'entrée.
    namespace Docking {
        // Distance maximale (m) entre le vaisseau et un quai libre pour lancer le pilote automatique.
        inline constexpr double Range = 5000.0;

        enum class ToggleResult { Docking, Leaving, Cancelled, NoDock };

        // Touche du pilote automatique (L par défaut) : en vol, vers le quai libre le plus proche à moins de Range ;
        // amarré, repartir ; pendant le trajet, le couper (le pilote reprend la main sur place).
        ToggleResult Toggle(Flux::Scene& scene, Flux::Entity ship);

        // Pendant le jeu, après le pilotage (Ships::Update) et avant la physique : les vaisseaux sous pilote automatique
        // suivent leur trajet ; ceux amarrés restent immobiles, quoi que fasse le pilote.
        void Update(Flux::Scene& scene, double deltaSeconds);

        // Amarré : posé sur son quai, enfant de sa station.
        bool IsDocked(const Flux::Scene& scene, Flux::Entity ship);

        // Pose tout de suite le vaisseau sur ce quai (vaisseau garé dessus au début de la partie, code du jeu) : quai
        // réservé, vaisseau amarré et immobile. false si le quai n'est pas libre ou n'appartient à aucune station.
        bool Dock(Flux::Scene& scene, Flux::Entity ship, Flux::Entity dock);

        // Étape en cours pour les infos de vol (« Pilote auto : vers Quai 2 », « Amarré : Quai 2 »), vide sinon.
        std::string Describe(const Flux::Scene& scene, Flux::Entity ship);
    }
}
