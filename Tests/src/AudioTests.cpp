// Son (AudioSystem, SoundComponent) : lancement au début du jeu, demandes du jeu, pause, arrêt, sauvegarde.
// Sans carte son, miniaudio prend sa sortie « nulle » : les sons « jouent » en silence, les états restent vrais.
#include "Check.hpp"

#include "Flux/Audio/AudioSystem.hpp"
#include "Flux/Graphics/Camera.hpp"
#include "Flux/Scene/ComponentRegistry.hpp"
#include "Flux/Scene/Components.hpp"
#include "Flux/Scene/Scene.hpp"
#include "Flux/Scene/SceneSerializer.hpp"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <string>
#include <vector>

using namespace Flux;

namespace {
    // Une seconde de la note la, 16 bits mono.
    void WriteWave(const std::filesystem::path& path) {
        constexpr std::uint32_t rate = 22050;
        std::vector<std::int16_t> samples(rate);
        for (std::size_t i = 0; i < samples.size(); ++i) {
            samples[i] = static_cast<std::int16_t>(8000.0 * std::sin(2.0 * std::numbers::pi * 440.0 * static_cast<double>(i) / rate));
        }
        const auto bytes = static_cast<std::uint32_t>(samples.size() * 2);
        std::ofstream file(path, std::ios::binary);
        const auto put32 = [&](std::uint32_t v) { file.write(reinterpret_cast<const char*>(&v), 4); };
        const auto put16 = [&](std::uint16_t v) { file.write(reinterpret_cast<const char*>(&v), 2); };
        file.write("RIFF", 4);
        put32(36 + bytes);
        file.write("WAVEfmt ", 8);
        put32(16);
        put16(1); // PCM
        put16(1); // mono
        put32(rate);
        put32(rate * 2);
        put16(2);
        put16(16);
        file.write("data", 4);
        put32(bytes);
        file.write(reinterpret_cast<const char*>(samples.data()), bytes);
    }
}

bool RunAudioTests() {
    // Sauvegarde d'abord : elle ne demande pas de sortie son.
    ComponentRegistry components;
    RegisterEngineComponents(components);
    {
        Scene scene;
        const Entity entity = scene.CreateEntity("Moteurs");
        scene.Add<SoundComponent>(entity, SoundComponent{.file = "Sons/Reacteur.wav", .volume = 0.5f, .loop = true,
                                                         .maxDistance = 150.0f, .followShipSpeed = true});
        Scene loaded;
        CHECK(SceneSerializer::LoadFromString(loaded, components, SceneSerializer::SaveToString(scene, components)).has_value());
        const auto* copy = loaded.TryGet<SoundComponent>(loaded.FindByName("Moteurs"));
        CHECK(copy && copy->file == "Sons/Reacteur.wav" && copy->volume == 0.5f && copy->loop && copy->maxDistance == 150.0f);
        CHECK(copy->followShipSpeed && copy->spatial && copy->playOnStart);
        CHECK(components.Find("sound")->FindField("file")->asset == AssetType::Sound);
    }

    AudioSystem audio;
    if (!audio.IsAvailable()) {
        std::printf("(son indisponible sur cette machine : tests de lecture sautés)\n");
        return true;
    }
    const std::filesystem::path directory = std::filesystem::temp_directory_path() / "flux-tests-son";
    std::filesystem::create_directories(directory);
    WriteWave(directory / "la.wav");
    audio.SetAssetsDirectory(directory);

    Scene scene;
    const Entity loop = scene.CreateEntity("Boucle");
    scene.Add<SoundComponent>(loop, SoundComponent{.file = "la.wav", .loop = true});
    const Entity onDemand = scene.CreateEntity("Sur demande");
    scene.Add<SoundComponent>(onDemand, SoundComponent{.file = "la.wav", .playOnStart = false, .spatial = false});
    const Entity missing = scene.CreateEntity("Absent");
    scene.Add<SoundComponent>(missing, SoundComponent{.file = "absent.wav"});
    const Camera camera;

    // Jeu arrêté (éditeur) : muet.
    audio.Update(scene, camera, false, false, 0.016);
    CHECK(!audio.IsPlaying(loop));
    // Lancement : « Jouer au lancement » part tout seul, l'autre attend ; un fichier absent ne bloque rien.
    audio.Update(scene, camera, true, false, 0.016);
    CHECK(audio.IsPlaying(loop));
    CHECK(!audio.IsPlaying(onDemand));
    CHECK(!audio.IsPlaying(missing));
    // Demandes du jeu, faites à la mise à jour suivante.
    audio.Play(onDemand);
    audio.Stop(loop);
    audio.Update(scene, camera, true, false, 0.016);
    CHECK(audio.IsPlaying(onDemand));
    CHECK(!audio.IsPlaying(loop));
    // Pause puis reprise.
    audio.Update(scene, camera, true, true, 0.0);
    CHECK(!audio.IsPlaying(onDemand));
    audio.Update(scene, camera, true, false, 0.016);
    CHECK(audio.IsPlaying(onDemand));
    // Composant retiré : son coupé.
    scene.Remove<SoundComponent>(onDemand);
    audio.Update(scene, camera, true, false, 0.016);
    CHECK(!audio.IsPlaying(onDemand));
    // Arrêt du jeu : tout se tait, et le prochain lancement repart du début.
    audio.Play(loop);
    audio.Update(scene, camera, true, false, 0.016);
    CHECK(audio.IsPlaying(loop));
    audio.Update(scene, camera, false, false, 0.016);
    CHECK(!audio.IsPlaying(loop));
    audio.Update(scene, camera, true, false, 0.016);
    CHECK(audio.IsPlaying(loop));

    audio.PlayOnce("la.wav", 0.5f);
    audio.PlayOnce("la.wav", Math::WorldPosition{{}, {3.0, 0.0, 0.0}});
    audio.Update(scene, camera, true, false, 0.016);
    audio.StopAll();
    std::filesystem::remove_all(directory);
    return true;
}
