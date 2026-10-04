#pragma once

#include "Flux/Export.hpp"
#include "Flux/Math/WorldPosition.hpp"
#include "Flux/Scene/Scene.hpp"

#include <filesystem>
#include <memory>
#include <string_view>

namespace Flux {
    struct Camera;

    // Son du moteur (miniaudio) : les sons des entités (SoundComponent) pendant le jeu, et ceux que le code du jeu
    // lance lui-même. L'écoute est à la caméra. Sans carte son, tout reste muet sans erreur.
    class FLUX_API AudioSystem {
        public:
        AudioSystem();
        ~AudioSystem();

        AudioSystem(const AudioSystem&) = delete;
        AudioSystem& operator=(const AudioSystem&) = delete;

        // false si aucune sortie son n'a pu être ouverte : les appels ne font alors rien.
        bool IsAvailable() const;

        // Volume général, de 0 (muet) à 1 ; s'applique à tous les sons.
        void SetMasterVolume(float volume);
        float GetMasterVolume() const;

        // Son de l'entité (SoundComponent) : le lance (depuis le début s'il jouait déjà), ou l'arrête. Pendant le jeu
        // seulement ; pour un son qui ne se lance pas tout seul (« Jouer au lancement » décoché).
        void Play(Entity entity);
        void Stop(Entity entity);
        bool IsPlaying(Entity entity) const;
        // Son ponctuel (une explosion, un clic) : joué une fois en entier, relatif aux assets ; à cette position du
        // monde, ou partout pareil sans position. Rien ne l'arrête avant la fin, sauf l'arrêt du jeu et StopAll.
        // Aussi hors du jeu (l'éditeur fait écouter un son du Contenu).
        void PlayOnce(std::string_view file, float volume = 1.0f);
        void PlayOnce(std::string_view file, const Math::WorldPosition& position, float volume = 1.0f);

        // Appelés par le moteur.
        void SetAssetsDirectory(const std::filesystem::path& directory);
        // Chaque image, après la caméra : sons des entités lancés, déplacés, réglés. playing : jeu en cours (sinon
        // les sons des entités se taisent) ; paused : jeu en pause (les sons s'interrompent et reprennent où ils en étaient).
        void Update(const Scene& scene, const Camera& camera, bool playing, bool paused, double deltaSeconds);
        // Coupe tout (arrêt du jeu, changement de scène : les entités ne sont plus les mêmes).
        void StopAll();

        private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
}
