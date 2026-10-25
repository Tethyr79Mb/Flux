#include "Core/QuickSave.hpp"

#include "Flux/Platform/Filesystem.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>

namespace Flux {
    namespace {
        std::filesystem::path QuickSaveDirectory() {
            return Platform::GetExecutableDirectory() / "Sauvegardes";
        }
        std::filesystem::path QuickSavePlayerFile() {
            return QuickSaveDirectory() / "rapide.joueur.json";
        }
        // Empreinte du contenu d'une scène (FNV-1a 64 bits, en hexadécimal) : la date du fichier ne dit rien, la
        // génération le recopie à chaque fois qu'un asset change.
        std::optional<std::string> FileDigest(const std::filesystem::path& path) {
            std::ifstream file(path, std::ios::binary);
            if (!file) {
                return std::nullopt;
            }
            std::uint64_t hash = 14695981039346656037ull;
            for (std::istreambuf_iterator<char> it(file), end; it != end; ++it) {
                hash = (hash ^ static_cast<std::uint8_t>(*it)) * 1099511628211ull;
            }
            return std::format("{:016x}", hash);
        }
        // Fichier du joueur absent ou illisible : un JSON vide (pas un objet).
        nlohmann::json ReadPlayerFile() {
            nlohmann::json player;
            if (std::ifstream file(QuickSavePlayerFile(), std::ios::binary); file) {
                player = nlohmann::json::parse(file, nullptr, false);
            }
            return player;
        }
    }

    void QuickSave::AddData(std::string name, Engine::SaveFunction save, Engine::LoadFunction load) {
        RemoveData(name);
        m_data.push_back({std::move(name), std::move(save), std::move(load)});
    }

    void QuickSave::RemoveData(std::string_view name) {
        std::erase_if(m_data, [&](const Data& data) { return data.name == name; });
    }

    void QuickSave::RequestLoad(bool resumes) {
        m_requests.load = true;
        m_requests.resumes = m_requests.resumes || resumes;
    }

    std::filesystem::path QuickSave::GetSceneFile() {
        return QuickSaveDirectory() / "rapide.scene.json";
    }

    bool QuickSave::Exists() {
        std::error_code error;
        return std::filesystem::exists(GetSceneFile(), error);
    }

    bool QuickSave::Delete() {
        std::error_code error;
        const bool removed = std::filesystem::remove(GetSceneFile(), error);
        std::filesystem::remove(QuickSavePlayerFile(), error);
        return removed;
    }

    bool QuickSave::MatchesStartupScene(const Project& project) {
        // Une sauvegarde d'avant cette empreinte compte comme changée.
        const std::optional<std::string> scene = FileDigest(project.GetStartupScenePath());
        if (!scene) {
            return true;
        }
        const nlohmann::json player = ReadPlayerFile();
        return player.is_object() && player.contains("startupScene") && player["startupScene"] == *scene;
    }

    std::expected<void, std::string> QuickSave::Write(Engine& engine, Entity player) const {
        Scene& scene = engine.GetScene();
        std::error_code error;
        std::filesystem::create_directories(QuickSaveDirectory(), error);
        if (auto saved = SceneSerializer::Save(scene, engine.GetComponents(), GetSceneFile(), engine.GetPrefabReader()); !saved) {
            return saved;
        }
        // Assis, il le reste au chargement (en vol, sinon il tomberait de son siège). Un siège en train de s'asseoir
        // ou de se lever compte comme assis : l'étape est courte.
        nlohmann::json file = nlohmann::json::object();
        if (const auto* character = scene.IsValid(player) ? scene.TryGet<CharacterComponent>(player) : nullptr;
            character && character->seat != NullEntity && scene.IsValid(character->seat)) {
            file["seat"] = scene.GetId(character->seat).ToString();
        }
        nlohmann::json systems = nlohmann::json::object();
        for (const Data& data : m_data) {
            if (std::string text = data.save ? data.save(engine) : std::string{}; !text.empty()) {
                systems[data.name] = std::move(text);
            }
        }
        if (!systems.empty()) {
            file["systems"] = std::move(systems);
        }
        const Project* project = engine.GetProject();
        if (std::optional<std::string> startup = project ? FileDigest(project->GetStartupScenePath()) : std::nullopt) {
            file["startupScene"] = std::move(*startup); // pour ne pas la reprendre si la scène change (ResumeQuickSave)
        }
        return SceneSerializer::WriteFile(QuickSavePlayerFile(), file.dump(2));
    }

    void QuickSave::Restore(Engine& engine, Entity player) const {
        Scene& scene = engine.GetScene();
        // Fichier du joueur absent ou illisible : il est debout là où il était, rien de plus.
        const nlohmann::json file = ReadPlayerFile();
        auto* character = scene.IsValid(player) ? scene.TryGet<CharacterComponent>(player) : nullptr;
        if (character && file.is_object() && file.contains("seat") && file["seat"].is_string()) {
            const std::optional<Uuid> id = Uuid::Parse(file["seat"].get<std::string>());
            const Entity seat = id ? scene.FindById(*id) : NullEntity;
            if (const auto* component = seat != NullEntity ? scene.TryGet<SeatComponent>(seat) : nullptr) {
                character->seat = seat;
                character->seatState = SeatState::Seated;
                character->seatTime = 0.0f;
                // Aux commandes : il garde la vitesse enregistrée au lieu de freiner (comme en s'asseyant).
                if (const Entity ship = component->pilot ? scene.GetShip(seat) : NullEntity; ship != NullEntity) {
                    ShipComponent& shipComponent = scene.Get<ShipComponent>(ship);
                    shipComponent.cruiseSpeed = -shipComponent.velocity.z;
                }
            }
        }
        // Systèmes du jeu : chacun reprend son état (une copie : un load peut en ajouter ou en retirer un autre).
        if (file.is_object() && file.contains("systems") && file["systems"].is_object()) {
            const std::vector<Data> handlers = m_data;
            const nlohmann::json& systems = file["systems"];
            for (const Data& data : handlers) {
                if (const auto found = systems.find(data.name); found != systems.end() && found->is_string() && data.load) {
                    data.load(engine, found->get_ref<const std::string&>());
                }
            }
        }
    }
}
