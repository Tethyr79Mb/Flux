#pragma once

#include <Flux/Core/Uuid.hpp>

#include <cstddef>
#include <deque>
#include <string>
#include <utility>

namespace Flux {
    class Engine;

    namespace UI {
        class Context;
    }
}

namespace FluxEditor {
    // Annuler / refaire. Chaque pas garde l'instantané d'une seule entité avant et après la modification
    // (SceneSerializer::SaveEntity) : le coût dépend de l'entité touchée, pas de la taille de la scène.
    //
    // Deux façons d'enregistrer un pas :
    // - modifications continues de l'entité sélectionnée (glisser un champ, le gizmo) : BeginFrame avant les
    //   panneaux, NoteModified quand un panneau l'a modifiée, EndFrame après ; tout le geste devient un seul pas ;
    // - actions ponctuelles (créer, supprimer, ajouter un composant) : Capture avant, puis Push.
    class UndoHistory {
        public:
        // Instantané d'une entité (vide si elle n'existe pas), avec ses descendants si withDescendants.
        static std::string Capture(Flux::Engine& engine, Flux::Uuid entity, bool withDescendants = false);

        // Désactivé pendant le jeu : la scène est une copie jetée à l'arrêt, rien n'est enregistré.
        void SetEnabled(bool enabled) { m_enabled = enabled; }
        bool IsEnabled() const { return m_enabled; }

        void BeginFrame(Flux::Engine& engine, Flux::Uuid selection);
        // label : nom du pas dans le menu Édition (« Déplacer »...), pris à la première modification du geste.
        void NoteModified(const char* label = "Modifier") {
            if (!m_frameModified && !m_gestureOpen) {
                m_gestureLabel = label;
            }
            m_frameModified = true;
        }
        // interacting : un widget ou le gizmo est encore tenu ; le pas n'est enregistré qu'une fois lâché.
        void EndFrame(Flux::Engine& engine, bool interacting);
        // Enregistre tout de suite le geste en cours (avant une action ponctuelle, un Annuler...).
        void Commit(Flux::Engine& engine);

        // before, after : instantanés (vide : l'entité n'existe pas). Ignoré s'ils sont identiques.
        void Push(std::string label, Flux::Uuid entity, std::string before, std::string after);

        bool CanUndo() const { return m_position > 0; }
        bool CanRedo() const { return m_position < m_steps.size(); }
        // Libellé du pas annulé ou refait par le menu (« Déplacer », « Supprimer »...).
        const std::string& GetUndoLabel() const { return m_steps[m_position - 1].label; }
        const std::string& GetRedoLabel() const { return m_steps[m_position].label; }
        // Retourne l'entité concernée, à sélectionner (Uuid vide si elle n'existe plus).
        Flux::Uuid Undo(Flux::Engine& engine);
        Flux::Uuid Redo(Flux::Engine& engine);

        // Nouvelle scène ou scène ouverte : l'historique ne la concerne plus.
        void Clear();
        // Vrai si la scène a changé (pas enregistré, annulé ou refait) depuis le dernier appel.
        bool TakeChanged() { return std::exchange(m_changed, false); }

        private:
        struct Step
        {
            std::string label;
            Flux::Uuid entity;
            std::string before;
            std::string after;
        };

        // Remet l'entité dans l'état de l'instantané (le détruit si l'instantané est vide).
        Flux::Uuid Apply(Flux::Engine& engine, Flux::Uuid entity, const std::string& snapshot);

        static constexpr std::size_t MaxSteps = 500;

        std::deque<Step> m_steps;
        std::size_t m_position = 0; // pas déjà faits (annulables) ; ceux d'après se refont
        bool m_enabled = true;
        bool m_changed = false;

        // Geste en cours sur l'entité sélectionnée.
        Flux::Uuid m_gestureEntity;
        std::string m_gestureBefore; // état avant la frame (ou avant le geste, s'il a commencé)
        const char* m_gestureLabel = "Modifier";
        bool m_gestureOpen = false;
        bool m_frameModified = false;
    };
}
