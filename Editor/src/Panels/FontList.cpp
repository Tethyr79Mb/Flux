#include "Panels/FontList.hpp"

#include "Panels/AssetPicker.hpp"

#include <Flux/Core/Engine.hpp>
#include <Flux/Platform/Filesystem.hpp>
#include <Flux/UI/Context.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <optional>
#include <system_error>

namespace FluxEditor {
    using namespace Flux;
    namespace fs = std::filesystem;

    namespace {
        // Entre deux lectures du dossier : la liste est demandée à chaque image tant qu'un texte est inspecté.
        constexpr std::chrono::milliseconds RescanInterval{1000};

        bool IsFontFile(const fs::path& path) {
            std::string extension = Platform::ToUtf8(path.extension());
            std::ranges::transform(extension, extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return extension == ".ttf" || extension == ".otf";
        }
    }

    const std::vector<std::string>& GetProjectFonts(const Engine& engine) {
        static std::vector<std::string> fonts;
        static fs::path scanned;
        static std::chrono::steady_clock::time_point checked;

        const Project* project = engine.GetProject();
        const fs::path folder = project ? project->assetsDirectory / FontsFolder : fs::path{};
        const auto now = std::chrono::steady_clock::now();
        if (folder == scanned && now - checked < RescanInterval) {
            return fonts;
        }
        scanned = folder;
        checked = now;
        fonts.clear();
        std::error_code error;
        if (folder.empty() || !fs::is_directory(folder, error)) {
            return fonts;
        }
        for (fs::recursive_directory_iterator it(folder, error), end; !error && it != end; it.increment(error)) {
            if (it->is_regular_file(error) && IsFontFile(it->path())) {
                fonts.push_back(Platform::ToGenericUtf8(it->path().lexically_relative(project->assetsDirectory)));
            }
        }
        std::ranges::sort(fonts);
        return fonts;
    }

    std::string GetFontLabel(std::string_view font) {
        if (font.empty()) {
            return "Arimo";
        }
        std::string stem = Platform::ToUtf8(Platform::PathFromUtf8(font).stem());
        if (const std::size_t axes = stem.find('['); axes != std::string::npos && axes > 0) {
            stem.resize(axes);
        }
        return stem;
    }

    bool FontChoice(Engine& engine, std::string_view label, std::string& font) {
        UI::Context& ui = engine.GetUi();
        const std::vector<std::string>& fonts = GetProjectFonts(engine);
        std::vector<std::string> labels;
        labels.reserve(fonts.size() + 3);
        labels.push_back("Arimo (par défaut)");
        for (const std::string& file : fonts) {
            labels.push_back(GetFontLabel(file));
        }
        const auto found = std::ranges::find(fonts, font);
        const bool known = font.empty() || found != fonts.end();
        if (!known) {
            labels.push_back(GetFontLabel(font) + " (introuvable)"); // fichier déplacé ou supprimé : on le voit
        }
        labels.push_back("Ajouter une police...");
        int index = font.empty() ? 0 : known ? 1 + static_cast<int>(found - fonts.begin()) : static_cast<int>(labels.size()) - 2;
        const int current = index;

        std::vector<std::string_view> names(labels.begin(), labels.end());
        if (!ui.Choice(label, index, names) || index == current) {
            return false;
        }
        if (index == static_cast<int>(labels.size()) - 1) {
            // Boîte de dialogue modale : la frame attend le choix.
            std::optional<std::string> chosen = ChooseAsset(engine, AssetType::Font, font);
            if (!chosen || *chosen == font) {
                return false;
            }
            font = std::move(*chosen);
            return true;
        }
        if (!known && index == static_cast<int>(labels.size()) - 2) {
            return false;
        }
        font = index == 0 ? std::string{} : fonts[static_cast<std::size_t>(index - 1)];
        return true;
    }
}
