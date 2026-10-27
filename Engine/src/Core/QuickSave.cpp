#include "Core/QuickSave.hpp"

#include "Flux/Platform/Filesystem.hpp"

#include <nlohmann/json.hpp>

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
        std::optional<std::string> ReadText(const std::filesystem::path& path) {
            std::ifstream file(path, std::ios::binary);
            if (!file) {
                return std::nullopt;
            }
            return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
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

    std::filesystem::path QuickSave::GetBaseSceneFile() {
        return QuickSaveDirectory() / "rapide.depart.json";
    }

    void QuickSave::SetScene(std::filesystem::path path, std::string text) {
        m_scenePath = std::move(path);
        m_sceneText = std::move(text);
    }

    void QuickSave::SetScene(const std::filesystem::path& path) {
        std::optional<std::string> text = ReadText(path);
        SetScene(text ? path : std::filesystem::path{}, text ? std::move(*text) : std::string{});
    }

    std::optional<QuickSave::Source> QuickSave::ReadSource(const Project* project) {
        const nlohmann::json player = ReadPlayerFile();
        if (!player.is_object() || !player.contains("scene") || !player["scene"].is_string()) {
            return std::nullopt; // sauvegarde du mode Jouer de l'éditeur, ou d'avant la fusion
        }
        std::filesystem::path path = Platform::PathFromUtf8(player["scene"].get<std::string>());
        if (path.is_relative() && project) {
            path = project->assetsDirectory / path;
        }
        std::optional<std::string> text = ReadText(GetBaseSceneFile());
        if (!text) {
            return std::nullopt;
        }
        // Scène disparue depuis : la sauvegarde seule, comme avant.
        const std::optional<std::string> current = ReadText(path);
        const bool changed = current && *current != *text;
        return Source{.path = std::move(path), .text = std::move(*text), .changed = changed};
    }

    bool QuickSave::Exists() {
        std::error_code error;
        return std::filesystem::exists(GetSceneFile(), error);
    }

    bool QuickSave::Delete() {
        std::error_code error;
        const bool removed = std::filesystem::remove(GetSceneFile(), error);
        std::filesystem::remove(QuickSavePlayerFile(), error);
        std::filesystem::remove(GetBaseSceneFile(), error);
        return removed;
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
        // Scène de départ telle qu'au chargement (même si l'éditeur l'a modifiée depuis) : la fusion compare la partie
        // à elle. Chemin relatif aux assets : le dossier du jeu peut être déplacé.
        if (!m_scenePath.empty()) {
            if (auto copied = SceneSerializer::WriteFile(GetBaseSceneFile(), m_sceneText); !copied) {
                return copied;
            }
            const Project* project = engine.GetProject();
            const std::filesystem::path relative = project ? m_scenePath.lexically_relative(project->assetsDirectory)
                                                           : std::filesystem::path{};
            const bool inAssets = !relative.empty() && *relative.begin() != "..";
            file["scene"] = Platform::ToGenericUtf8(inAssets ? relative : m_scenePath);
        } else {
            std::filesystem::remove(GetBaseSceneFile(), error);
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
