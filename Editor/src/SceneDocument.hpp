#pragma once

#include <filesystem>
#include <string>

namespace Flux {
    class Engine;
}

namespace FluxEditor {
    // Fichier de la scène éditée : barre de menus « Fichier », raccourcis clavier, titre de la fenêtre,
    // et confirmation avant de perdre des modifications (nouvelle scène, ouverture, fermeture).
    class SceneDocument {
        public:
        // Barre de menus et raccourcis ; à appeler à chaque frame, avant les panneaux.
        void Update(Flux::Engine& engine);
        // Une modification faite dans l'éditeur (pas l'animation du jeu) : le titre prend une étoile.
        void MarkModified() { m_modified = true; }
        // Fermeture de la fenêtre ou « Quitter » : false si l'utilisateur annule.
        bool ConfirmClose(Flux::Engine& engine);
        // Ouvre une scène (panneau « Contenu »), après avoir proposé d'enregistrer les modifications.
        void OpenFile(Flux::Engine& engine, const std::filesystem::path& path);
        // Ouvre une scène sans rien demander (démarrage de l'éditeur) ; false et message d'erreur en cas d'échec.
        bool Load(Flux::Engine& engine, const std::filesystem::path& path);
        const std::filesystem::path& GetPath() const { return m_path; }

        private:
        void NewScene(Flux::Engine& engine);
        void Open(Flux::Engine& engine);
        bool Save(Flux::Engine& engine);
        bool SaveAs(Flux::Engine& engine);
        bool WriteTo(Flux::Engine& engine, const std::filesystem::path& path);
        // Propose d'enregistrer les modifications ; false si l'utilisateur annule (ou si l'enregistrement échoue).
        bool ConfirmDiscard(Flux::Engine& engine);
        void UpdateTitle(Flux::Engine& engine);
        std::filesystem::path DialogDirectory(const Flux::Engine& engine) const;

        std::filesystem::path m_path; // vide : scène jamais enregistrée par l'éditeur
        bool m_modified = false;
        std::string m_title;          // dernier titre donné à la fenêtre
    };
}
