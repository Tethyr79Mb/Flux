#pragma once

#include <filesystem>
#include <string>
#include <utility>

namespace Flux {
    class Engine;

    namespace UI {
        class Context;
    }
}

namespace FluxEditor {
    // Fichier de la scène éditée : barre de menus « Fichier », raccourcis clavier, titre de la fenêtre,
    // et confirmation avant de perdre des modifications (nouvelle scène, ouverture, fermeture).
    class SceneDocument {
        public:
        // Raccourcis Ctrl+N/O/S ; à appeler à chaque frame, avant les panneaux.
        void HandleShortcuts(Flux::Engine& engine);
        // Éléments du menu « Fichier » (ouvert par l'appelant, qui y ajoute les siens) : scènes, puis Quitter.
        void DrawSceneMenuItems(Flux::UI::Context& ui, Flux::Engine& engine);
        void DrawQuitMenuItem(Flux::UI::Context& ui, Flux::Engine& engine);
        // Titre de la fenêtre : nom de la scène, étoile si modifiée, projet.
        void UpdateTitle(Flux::Engine& engine);
        // Pendant le jeu : ni ouverture, ni enregistrement (la scène est une copie jetable).
        void SetLocked(bool locked) { m_locked = locked; }
        // Vrai si une autre scène a remplacé la scène éditée (nouvelle, ouverte) depuis le dernier appel.
        bool TakeReplaced() { return std::exchange(m_replaced, false); }
        // Une modification faite dans l'éditeur (pas l'animation du jeu) : le titre prend une étoile.
        void MarkModified() { m_modified = true; }
        // Fermeture de la fenêtre ou « Quitter » : false si l'utilisateur annule.
        bool ConfirmClose(Flux::Engine& engine);
        // Ouvre une scène (panneau « Contenu »), après avoir proposé d'enregistrer les modifications.
        void OpenFile(Flux::Engine& engine, const std::filesystem::path& path);
        // Ouvre une scène sans rien demander (démarrage de l'éditeur) ; false et message d'erreur en cas d'échec.
        bool Load(Flux::Engine& engine, const std::filesystem::path& path);
        const std::filesystem::path& GetPath() const { return m_path; }
        // Le fichier de la scène ouverte a été supprimé : la scène reste en mémoire, sans titre et à enregistrer.
        void ForgetFile(const std::filesystem::path& path);
        // Le fichier de la scène ouverte a été renommé : la suite s'enregistre sous le nouveau nom.
        void FileRenamed(const std::filesystem::path& from, const std::filesystem::path& to);

        private:
        void NewScene(Flux::Engine& engine);
        void Open(Flux::Engine& engine);
        bool Save(Flux::Engine& engine);
        bool SaveAs(Flux::Engine& engine);
        bool WriteTo(Flux::Engine& engine, const std::filesystem::path& path);
        // Propose d'enregistrer les modifications ; false si l'utilisateur annule (ou si l'enregistrement échoue).
        bool ConfirmDiscard(Flux::Engine& engine);
        std::filesystem::path DialogDirectory(const Flux::Engine& engine) const;

        std::filesystem::path m_path; // vide : scène jamais enregistrée par l'éditeur
        bool m_modified = false;
        bool m_locked = false;
        bool m_replaced = false;
        std::string m_title;          // dernier titre donné à la fenêtre
    };
}
