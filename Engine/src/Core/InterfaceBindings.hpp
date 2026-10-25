#pragma once

// Ce qui relie les interfaces du jeu (.fluxui) au code : valeurs liées (champ « Liaison » d'une case, d'un curseur,
// d'un texte), touches des actions (« Touche : Avancer »), fonctions C++ des boutons, et les actions des boutons
// cliqués pendant le dessin, faites à la fin de l'image. Voir Engine::BindInterfaceValue.

#include "Flux/Core/Engine.hpp"
#include "Flux/Interface/Interface.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Flux {
    class InputActions;

    class InterfaceBindings {
        public:
        struct Value
        {
            std::function<double()> get{};
            std::function<void(double)> set{};
            std::function<std::string()> getText{}; // texte : prioritaire sur get pour un élément Texte
            std::vector<std::string> options{};     // valeur à choix : ceux des listes déroulantes liées
            std::string_view category{};            // pour l'éditeur (texte constant)
        };
        // Valeur fournie par le moteur (« V-Sync », « Images par seconde »), rangée dans une catégorie de l'éditeur
        // (« Graphismes »...) ; celles du jeu la remplacent.
        void SetEngineValue(std::string name, std::string_view category, Value value);
        // Valeur du jeu, dans la catégorie « Jeu ».
        void BindValue(std::string name, Value value);
        void UnbindValue(std::string_view name);
        // Valeurs liables (moteur puis jeu, puis les touches des actions), pour l'éditeur.
        std::vector<Engine::InterfaceValueInfo> GetValues(const InputActions& actions) const;

        void RegisterFunction(std::string name, Engine::InterfaceFunction function);
        void UnregisterFunction(std::string_view name);
        // Module de jeu déchargé : ses fonctions, ses valeurs et les actions en attente (leur code est dans sa DLL).
        void ClearGame();

        // Interfaces en jeu : les éléments liés affichent leur valeur, ou la changent (changed : touché par le
        // joueur). rebinding : action dont on attend la nouvelle touche.
        void Sync(std::vector<Interface::Element>& elements, std::uint32_t changed, const InputActions& actions,
                  std::string_view rebinding) const;

        // Bouton cliqué pendant le dessin : pas tout de suite, la scène est en cours de parcours.
        void QueueAction(Interface::ButtonAction action, std::string target) { m_actions.emplace_back(action, std::move(target)); }
        void ClearActions() { m_actions.clear(); }
        // Fin de l'image, avant le changement de scène : une fonction peut en demander un. quit remplace la
        // fermeture de la fenêtre pour « Quitter le jeu » (vide : Engine::RequestQuit).
        void RunActions(Engine& engine, const Engine::InterfaceFunction& quit);

        private:
        const Value* Find(std::string_view name) const;

        std::map<std::string, Engine::InterfaceFunction, std::less<>> m_functions;
        std::map<std::string, Value, std::less<>> m_engineValues; // fournies par le moteur
        std::map<std::string, Value, std::less<>> m_gameValues;   // fournies par le jeu
        std::vector<std::pair<Interface::ButtonAction, std::string>> m_actions;
    };
}
