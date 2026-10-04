#include "Flux/Audio/AudioSystem.hpp"

#include "Flux/Core/Log.hpp"
#include "Flux/Core/Uuid.hpp"
#include "Flux/Graphics/Camera.hpp"
#include "Flux/Platform/Filesystem.hpp"
#include "Flux/Scene/Components.hpp"

#include <miniaudio.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Flux {
    namespace {
        // Au-delà, un fichier est lu au fil de la lecture (musique) au lieu d'être décodé en entier en mémoire.
        constexpr std::uintmax_t StreamAbove = 2 * 1024 * 1024;
        // Vitesse du vaisseau lissée sur environ 1/4 de seconde : le bruit des moteurs ne saute pas.
        constexpr double ShipSmoothing = 4.0;

        // Un son miniaudio : ma_sound ne se déplace pas en mémoire une fois initialisé, d'où le unique_ptr.
        struct Sound
        {
            ma_sound sound{};
            bool initialized = false;

            Sound() = default;
            Sound(const Sound&) = delete;
            Sound& operator=(const Sound&) = delete;
            ~Sound() {
                if (initialized) {
                    ma_sound_uninit(&sound);
                }
            }
        };

        // Son d'une entité.
        struct Voice
        {
            Uuid id;            // l'entité d'origine : après un changement de scène, le même numéro peut revenir
            std::string file;   // relancé si le fichier change
            bool spatial = true; // idem (miniaudio le fixe à la création)
            std::unique_ptr<Sound> sound; // nullptr : fichier illisible (signalé une fois)
            bool resumeAfterPause = false;
            double shipSpeed = 0.0; // 0 à 1, lissée
        };

        // Son ponctuel (PlayOnce), libéré à la fin.
        struct OneShot
        {
            std::unique_ptr<Sound> sound;
            std::optional<Math::WorldPosition> position;
            bool resumeAfterPause = false;
        };
    }

    struct AudioSystem::Impl
    {
        ma_engine engine{};
        bool available = false;
        float masterVolume = 1.0f;
        std::filesystem::path assets;
        std::unordered_map<Entity, Voice> voices;
        std::vector<OneShot> oneShots;
        // Play / Stop demandés par le jeu, faits à la prochaine mise à jour (qui a la scène).
        std::vector<std::pair<Entity, bool>> requests;
        bool paused = false;

        // Charge le fichier ; nullptr (et un message) s'il est illisible.
        std::unique_ptr<Sound> Load(std::string_view file, bool spatial) {
            const std::filesystem::path path = assets / Platform::PathFromUtf8(file);
            std::error_code error;
            const std::uintmax_t size = std::filesystem::file_size(path, error);
            if (error) {
                Log::Error("son {} : fichier introuvable", file);
                return nullptr;
            }
            ma_uint32 flags = size > StreamAbove ? MA_SOUND_FLAG_STREAM : MA_SOUND_FLAG_DECODE;
            if (!spatial) {
                flags |= MA_SOUND_FLAG_NO_SPATIALIZATION;
            }
            auto sound = std::make_unique<Sound>();
#ifdef _WIN32
            const ma_result result = ma_sound_init_from_file_w(&engine, path.c_str(), flags, nullptr, nullptr, &sound->sound);
#else
            const ma_result result = ma_sound_init_from_file(&engine, path.c_str(), flags, nullptr, nullptr, &sound->sound);
#endif
            if (result != MA_SUCCESS) {
                Log::Error("son {} : {}", file, ma_result_description(result));
                return nullptr;
            }
            sound->initialized = true;
            // Le son est déjà relatif à l'écoute (la caméra) : pas d'effet Doppler, qui demanderait les vitesses.
            ma_sound_set_doppler_factor(&sound->sound, 0.0f);
            ma_sound_set_attenuation_model(&sound->sound, ma_attenuation_model_linear);
            return sound;
        }

        // Volume, hauteur, distances et position d'après le composant ; position relative à la caméra.
        void Apply(const Scene& scene, const Camera& camera, Entity entity, const SoundComponent& component, Voice& voice,
                   double deltaSeconds) {
            ma_sound* sound = &voice.sound->sound;
            float volume = component.volume;
            float pitch = component.pitch;
            if (component.followShipSpeed) {
                const Entity ship = scene.TryGet<ShipComponent>(entity) ? entity : scene.GetShip(entity);
                double target = 0.0;
                if (const auto* shipComponent = ship != NullEntity ? scene.TryGet<ShipComponent>(ship) : nullptr) {
                    target = std::clamp(static_cast<double>(Math::Length(shipComponent->velocity))
                                            / std::max(shipComponent->maxSpeed, 0.001f), 0.0, 1.0);
                }
                voice.shipSpeed += (target - voice.shipSpeed) * std::min(1.0, deltaSeconds * ShipSmoothing);
                // À l'arrêt, un ronronnement ; à pleine vitesse, plein volume et une quinte plus haut.
                volume *= static_cast<float>(0.3 + 0.7 * voice.shipSpeed);
                pitch *= static_cast<float>(0.75 + 0.5 * voice.shipSpeed);
            }
            ma_sound_set_volume(sound, volume);
            ma_sound_set_pitch(sound, std::max(pitch, 0.01f));
            ma_sound_set_looping(sound, component.loop ? MA_TRUE : MA_FALSE);
            if (component.spatial) {
                const float minDistance = std::max(component.minDistance, 0.0f);
                ma_sound_set_min_distance(sound, minDistance);
                ma_sound_set_max_distance(sound, std::max(component.maxDistance, minDistance + 0.01f));
                const Math::Vector3d position = Math::Delta(camera.position, scene.GetWorldPosition(entity));
                ma_sound_set_position(sound, static_cast<float>(position.x), static_cast<float>(position.y),
                                      static_cast<float>(position.z));
            }
        }

        void Start(Voice& voice) {
            if (voice.sound) {
                ma_sound_seek_to_pcm_frame(&voice.sound->sound, 0);
                ma_sound_start(&voice.sound->sound);
            }
        }
    };

    AudioSystem::AudioSystem() : m_impl(std::make_unique<Impl>()) {
        ma_engine_config config = ma_engine_config_init();
        if (const ma_result result = ma_engine_init(&config, &m_impl->engine); result != MA_SUCCESS) {
            Log::Warning("son indisponible : {}", ma_result_description(result));
            return;
        }
        m_impl->available = true;
    }

    AudioSystem::~AudioSystem() {
        if (m_impl->available) {
            StopAll(); // les sons avant le moteur qui les joue
            ma_engine_uninit(&m_impl->engine);
        }
    }

    bool AudioSystem::IsAvailable() const { return m_impl->available; }

    void AudioSystem::SetMasterVolume(float volume) {
        m_impl->masterVolume = std::clamp(volume, 0.0f, 1.0f);
        if (m_impl->available) {
            ma_engine_set_volume(&m_impl->engine, m_impl->masterVolume);
        }
    }

    float AudioSystem::GetMasterVolume() const { return m_impl->masterVolume; }

    void AudioSystem::Play(Entity entity) { m_impl->requests.emplace_back(entity, true); }

    void AudioSystem::Stop(Entity entity) { m_impl->requests.emplace_back(entity, false); }

    bool AudioSystem::IsPlaying(Entity entity) const {
        const auto found = m_impl->voices.find(entity);
        return found != m_impl->voices.end() && found->second.sound && ma_sound_is_playing(&found->second.sound->sound);
    }

    void AudioSystem::PlayOnce(std::string_view file, float volume) {
        if (!m_impl->available || file.empty()) {
            return;
        }
        if (std::unique_ptr<Sound> sound = m_impl->Load(file, false)) {
            ma_sound_set_volume(&sound->sound, volume);
            ma_sound_start(&sound->sound);
            m_impl->oneShots.push_back({std::move(sound), std::nullopt});
        }
    }

    void AudioSystem::PlayOnce(std::string_view file, const Math::WorldPosition& position, float volume) {
        if (!m_impl->available || file.empty()) {
            return;
        }
        if (std::unique_ptr<Sound> sound = m_impl->Load(file, true)) {
            ma_sound_set_volume(&sound->sound, volume);
            ma_sound_set_min_distance(&sound->sound, 2.0f);
            ma_sound_set_max_distance(&sound->sound, 60.0f);
            // Placé par la prochaine mise à jour, avant que miniaudio ne joue le premier bloc (quelques ms plus tard).
            ma_sound_start(&sound->sound);
            m_impl->oneShots.push_back({std::move(sound), position});
        }
    }

    void AudioSystem::SetAssetsDirectory(const std::filesystem::path& directory) { m_impl->assets = directory; }

    void AudioSystem::StopAll() {
        m_impl->voices.clear();
        m_impl->oneShots.clear();
        m_impl->requests.clear();
        m_impl->paused = false;
    }

    void AudioSystem::Update(const Scene& scene, const Camera& camera, bool playing, bool paused, double deltaSeconds) {
        Impl& impl = *m_impl;
        if (!impl.available) {
            impl.requests.clear();
            return;
        }
        if (!playing) {
            // Jeu arrêté : les sons des entités se taisent ; un son ponctuel (écouté dans l'éditeur) va jusqu'au bout.
            impl.voices.clear();
            impl.requests.clear();
            impl.paused = false;
            std::erase_if(impl.oneShots, [](const OneShot& oneShot) { return ma_sound_at_end(&oneShot.sound->sound); });
            return;
        }

        // Pause : tout s'interrompt et reprendra au même endroit.
        if (paused != impl.paused) {
            impl.paused = paused;
            const auto toggle = [&](Sound* sound, bool& resume) {
                if (!sound) {
                    return;
                }
                if (paused) {
                    resume = ma_sound_is_playing(&sound->sound);
                    ma_sound_stop(&sound->sound);
                } else if (std::exchange(resume, false)) {
                    ma_sound_start(&sound->sound);
                }
            };
            for (auto& [entity, voice] : impl.voices) {
                toggle(voice.sound.get(), voice.resumeAfterPause);
            }
            for (OneShot& oneShot : impl.oneShots) {
                toggle(oneShot.sound.get(), oneShot.resumeAfterPause);
            }
        }

        // L'écoute est à la caméra (les positions lui sont relatives) et tourne avec elle.
        ma_engine_listener_set_position(&impl.engine, 0, 0.0f, 0.0f, 0.0f);
        const Math::Vector3f forward = camera.GetForward();
        const Math::Vector3f up = camera.GetUp();
        ma_engine_listener_set_direction(&impl.engine, 0, forward.x, forward.y, forward.z);
        ma_engine_listener_set_world_up(&impl.engine, 0, up.x, up.y, up.z);

        // Sons d'entités supprimées, devenues muettes ou remplacées (changement de scène).
        std::erase_if(impl.voices, [&](const auto& item) {
            const auto& [entity, voice] = item;
            if (!scene.IsValid(entity)) {
                return true;
            }
            const auto* component = scene.TryGet<SoundComponent>(entity);
            return !component || component->file != voice.file || component->spatial != voice.spatial
                || scene.GetId(entity) != voice.id;
        });

        const auto voiceOf = [&](Entity entity, const SoundComponent& component) -> Voice& {
            auto [found, created] = impl.voices.try_emplace(entity);
            Voice& voice = found->second;
            if (created) {
                voice.id = scene.GetId(entity);
                voice.file = component.file;
                voice.spatial = component.spatial;
                voice.sound = impl.Load(component.file, component.spatial);
            }
            return voice;
        };

        // Nouveaux sons : lancés tout de suite s'ils jouent au lancement. Un son joué une fois ne revient pas.
        for (const auto [entity, component] : scene.GetRegistry().view<const SoundComponent>().each()) {
            if (component.file.empty() || impl.voices.contains(entity)) {
                continue;
            }
            Voice& voice = voiceOf(entity, component);
            if (voice.sound) {
                impl.Apply(scene, camera, entity, component, voice, 0.0);
                if (component.playOnStart && !impl.paused) {
                    impl.Start(voice);
                } else if (component.playOnStart) {
                    voice.resumeAfterPause = true;
                }
            }
        }

        // Demandes du jeu.
        for (const auto& [entity, play] : std::exchange(impl.requests, {})) {
            const auto* component = scene.IsValid(entity) ? scene.TryGet<SoundComponent>(entity) : nullptr;
            if (!component || component->file.empty()) {
                continue;
            }
            Voice& voice = voiceOf(entity, *component);
            if (!voice.sound) {
                continue;
            }
            if (play) {
                impl.Apply(scene, camera, entity, *component, voice, 0.0);
                impl.Start(voice);
            } else {
                ma_sound_stop(&voice.sound->sound);
                voice.resumeAfterPause = false;
            }
        }

        for (auto& [entity, voice] : impl.voices) {
            if (voice.sound) {
                impl.Apply(scene, camera, entity, scene.Get<SoundComponent>(entity), voice, deltaSeconds);
            }
        }

        // Sons ponctuels : suivis par rapport à la caméra, libérés à la fin.
        std::erase_if(impl.oneShots, [&](const OneShot& oneShot) {
            return !oneShot.resumeAfterPause && ma_sound_at_end(&oneShot.sound->sound);
        });
        for (OneShot& oneShot : impl.oneShots) {
            if (oneShot.position) {
                const Math::Vector3f position(Math::Delta(camera.position, *oneShot.position));
                ma_sound_set_position(&oneShot.sound->sound, position.x, position.y, position.z);
            }
        }
    }
}
