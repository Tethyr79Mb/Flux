#include "Panels/AssetPicker.hpp"

#include <Flux/Core/Engine.hpp>
#include <Flux/Platform/Dialogs.hpp>
#include <Flux/Platform/Filesystem.hpp>
#include <Flux/Platform/Window.hpp>
#include <Flux/Assets/Model.hpp>

#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace FluxEditor {
    using namespace Flux;
    namespace fs = std::filesystem;

    namespace {
        struct AssetKind
        {
            Platform::FileType fileType;
            const char* title;
            const char* folder; // sous-dossier des assets où ranger un fichier venu d'ailleurs
        };

        // None n'arrive pas ici : champ texte ordinaire.
        AssetKind GetKind(AssetType type) {
            if (type == AssetType::Scene) {
                return {{"Scènes Flux", ".scene.json"}, "Choisir une scène", "scenes"};
            }
            if (type == AssetType::Model) {
                return {{"Modèles glTF", ".glb;.gltf"}, "Choisir un modèle 3D", "models"};
            }
            if (type == AssetType::Interface) {
                return {{"Interfaces Flux", ".fluxui"}, "Choisir une interface", "interfaces"};
            }
            if (type == AssetType::Sound) {
                return {{"Sons", ".wav;.ogg;.mp3;.flac"}, "Choisir un son", "Sons"};
            }
            if (type == AssetType::Font) {
                return {{"Polices", ".ttf;.otf"}, "Choisir une police", "Polices"};
            }
            if (type == AssetType::Prefab) {
                return {{"Prefabs Flux", ".prefab.json"}, "Choisir un prefab", "Prefabs"};
            }
            if (type == AssetType::Biome) {
                return {{"Biomes Flux", ".biome.json"}, "Choisir un biome", "Vegetation/Biomes"};
            }
            return {{"Images", ".png;.jpg;.jpeg;.tga;.bmp"}, "Choisir une image", "textures"};
        }

        // Chemin de file relatif à root si file est dedans. Comparaison par std::filesystem::equivalent, pas par le
        // texte : sous Windows, « c:\ » et « C:\ » ou des casses différentes désignent le même dossier.
        std::optional<fs::path> RelativeTo(const fs::path& file, const fs::path& root) {
            std::error_code error;
            for (fs::path folder = file.parent_path(); !folder.empty(); folder = folder.parent_path()) {
                if (fs::equivalent(folder, root, error)) {
                    return file.lexically_relative(folder);
                }
                if (folder == folder.parent_path()) {
                    break; // racine du disque
                }
            }
            return std::nullopt;
        }
    }

    std::optional<std::string> ChooseAsset(Engine& engine, AssetType type, std::string_view current) {
        const Window& window = engine.GetWindow();
        const Project* project = engine.GetProject();
        if (!project) {
            Platform::ShowError(window, "Choisir un fichier", "Aucun projet ouvert : les fichiers sont pris dans son dossier des assets.");
            return std::nullopt;
        }
        const AssetKind kind = GetKind(type);
        const fs::path& assets = project->assetsDirectory;

        // L'explorateur s'ouvre sur le dossier du fichier actuel, sinon sur celui du type (textures/), sinon les assets.
        std::error_code error;
        fs::path initial = assets;
        if (const fs::path folder = (assets / Platform::PathFromUtf8(current)).parent_path();
            !current.empty() && fs::is_directory(folder, error)) {
            initial = folder;
        } else if (fs::is_directory(assets / kind.folder, error)) {
            initial = assets / kind.folder;
        }

        const std::optional<fs::path> chosen = Platform::ShowOpenFileDialog(window, kind.title, kind.fileType, initial);
        if (!chosen) {
            return std::nullopt;
        }
        if (const std::optional<fs::path> relative = RelativeTo(*chosen, assets)) {
            return Platform::ToGenericUtf8(*relative);
        }

        // Hors du projet : le jeu ne le trouverait pas une fois installé. On le copie dans les assets.
        // Un .gltf ne va pas seul : ses .bin et ses images le suivent, dans un dossier à son nom (models/Robot/)
        // pour ne pas mélanger les « scene.bin » de deux modèles.
        const std::string name = Platform::ToUtf8(chosen->filename());
        std::vector<fs::path> companions;
        fs::path folder = fs::path(kind.folder);
        if (type == AssetType::Model && Platform::ToUtf8(chosen->extension()) != ".glb") {
            auto files = ListModelFiles(*chosen);
            if (!files) {
                Platform::ShowError(window, kind.title, files.error());
                return std::nullopt;
            }
            for (const std::string& file : *files) {
                const fs::path relative = Platform::PathFromUtf8(file).lexically_normal();
                if (relative.is_absolute() || relative.empty() || *relative.begin() == "..") {
                    Platform::ShowError(window, kind.title,
                                        "« " + name + " » utilise « " + file + " », hors de son dossier : il ne peut pas être copié.\n"
                                            "Exportez-le en .glb (un seul fichier).");
                    return std::nullopt;
                }
                companions.push_back(relative);
            }
            if (!companions.empty()) {
                folder /= chosen->stem();
            }
        }
        const std::string folderName = Platform::ToGenericUtf8(folder);
        const fs::path target = assets / folder / chosen->filename();
        const std::string with = companions.empty() ? "" : " (avec ses " + std::to_string(companions.size()) + " fichiers)";
        if (!Platform::AskYesNo(window, kind.title,
                                "« " + name + " » n'est pas dans le dossier du projet.\nLe copier" + with + " dans assets/"
                                    + folderName + " ?")) {
            return std::nullopt;
        }
        if (fs::exists(target, error)) {
            Platform::ShowError(window, kind.title,
                                "Un fichier « " + name + " » existe déjà dans assets/" + folderName
                                    + ".\nRenommez-le, ou choisissez directement celui du projet.");
            return std::nullopt;
        }
        fs::create_directories(target.parent_path(), error);
        if (!fs::copy_file(*chosen, target, error)) {
            Platform::ShowError(window, kind.title, "Copie impossible : " + error.message());
            return std::nullopt;
        }
        // Un fichier annexe déjà là (deux modèles qui partagent une texture) est gardé tel quel.
        for (const fs::path& companion : companions) {
            const fs::path destination = target.parent_path() / companion;
            fs::create_directories(destination.parent_path(), error);
            if (!fs::exists(destination, error) && !fs::copy_file(chosen->parent_path() / companion, destination, error)) {
                Platform::ShowError(window, kind.title,
                                    "Copie de « " + Platform::ToGenericUtf8(companion) + " » impossible : " + error.message()
                                        + "\nLe modèle risque de s'afficher incomplet.");
            }
        }
        return Platform::ToGenericUtf8(folder / chosen->filename());
    }
}
