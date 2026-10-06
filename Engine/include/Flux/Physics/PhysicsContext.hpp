#pragma once

#include "Flux/Export.hpp"

#include <filesystem>
#include <memory>

namespace Flux {
    // Ce qui sert à tous les mondes physiques et vit aussi longtemps que le moteur, comme le module de physique
    // d'Unreal démarré avant toute scène : Jolt initialisé, ses fils de calcul et sa mémoire de travail, les
    // maillages de collision déjà lus (avec leur enveloppe convexe). Les mondes (PhysicsWorld, un par scène) le
    // partagent : changer de scène ou relancer le jeu dans l'éditeur ne refait ni l'un ni l'autre. Un maillage dont
    // le fichier a changé sur le disque est relu. Aucun type Jolt ici.
    class FLUX_API PhysicsContext {
        public:
        PhysicsContext();
        ~PhysicsContext();

        PhysicsContext(const PhysicsContext&) = delete;
        PhysicsContext& operator=(const PhysicsContext&) = delete;

        // Dossier des assets du projet, d'où sont lus les maillages de collision (ColliderShape::Mesh). Un autre
        // dossier oublie ceux déjà lus. Vide : pas de collision en maillage.
        void SetAssetsDirectory(const std::filesystem::path& directory);
        // Maillages de collision gardés en mémoire, lisibles ou non (tests, panneau Moteur).
        int GetMeshCount() const;

        private:
        friend class PhysicsWorld; // seul à voir le détail (Engine/src/Physics/PhysicsContextImpl.hpp)
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
}
