#include "AssetPicker.hpp"

#include <Flux/Core/Engine.hpp>
#include <Flux/Platform/Dialogs.hpp>
#include <Flux/Platform/Filesystem.hpp>
#include <Flux/Platform/Window.hpp>

#include <filesystem>
#include <system_error>

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

        // Seules les images existent pour l'instant (None n'arrive pas ici : champ texte ordinaire).
        AssetKind GetKind([[maybe_unused]] AssetType type) {
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
        const std::string name = Platform::ToUtf8(chosen->filename());
        const fs::path target = assets / kind.folder / chosen->filename();
        if (!Platform::AskYesNo(window, kind.title,
                                "« " + name + " » n'est pas dans le dossier du projet.\nLe copier dans assets/" + kind.folder + " ?")) {
            return std::nullopt;
        }
        if (fs::exists(target, error)) {
            Platform::ShowError(window, kind.title,
                                "Un fichier « " + name + " » existe déjà dans assets/" + kind.folder
                                    + ".\nRenommez l'image, ou choisissez directement celle du projet.");
            return std::nullopt;
        }
        fs::create_directories(target.parent_path(), error);
        if (!fs::copy_file(*chosen, target, error)) {
            Platform::ShowError(window, kind.title, "Copie impossible : " + error.message());
            return std::nullopt;
        }
        return Platform::ToGenericUtf8(fs::path(kind.folder) / chosen->filename());
    }
}
