#pragma once

#include <Flux/Assets/Image.hpp>
#include <Flux/UI/Context.hpp>

#include <filesystem>

namespace Flux {
    class Engine;
}

namespace FluxEditor {
    // Rectangle [left, right[ x [top, bottom[ de l'image (borné à elle) ; l'image entière s'il est vide.
    Flux::ImageData CropImage(const Flux::ImageData& source, int left, int top, int right, int bottom);

    // F12 : capture de la vue 3D à la taille de l'écran, enregistrée en PNG dans le dossier Captures/ du projet (à
    // côté du .fluxproject, ignoré par Git). La frame capturée est dessinée sans les aides de l'éditeur (grille,
    // gizmo, sélection, barre d'outils de la vue) ; l'interface du jeu reste en mode Jouer. Une capture à la fois.
    class ViewCapture {
        public:
        // Au début de la frame : F12 demande la capture de l'image de cette frame.
        void HandleShortcuts(Flux::Engine& engine);
        // Vrai à la frame capturée : la vue doit se dessiner sans les aides de l'éditeur.
        bool IsCapturingFrame() const { return m_frameRequested; }
        // Après les panneaux : relit l'image quand le GPU l'a finie (deux ou trois frames plus tard) et l'enregistre.
        // view : rectangle de la vue 3D à la frame capturée.
        void Update(Flux::Engine& engine, const Flux::UI::Rect& view);

        private:
        static std::filesystem::path Directory(const Flux::Engine& engine);

        bool m_frameRequested = false; // capture demandée à cette frame
        bool m_pending = false;        // image en route depuis le GPU
        int m_waitedFrames = 0;
        Flux::UI::Rect m_view{};
    };
}
