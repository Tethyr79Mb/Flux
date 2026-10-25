#include "Flux/Core/GameSettings.hpp"

#include "Flux/Core/Log.hpp"
#include "Flux/Platform/Filesystem.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <fstream>
#include <sstream>

namespace Flux::GameSettingsFile {
    namespace {
        constexpr std::string_view FormatName = "flux.game-settings";
        constexpr int FormatVersion = 1;
        // Bornes de la fenêtre : assez petite pour l'interface, pas plus grande qu'un écran 8K.
        constexpr int MinWindowSize = 320;
        constexpr int MaxWindowSize = 7680;

        using Json = nlohmann::json;

        // Lit une valeur si elle a le bon type ; sinon garde la valeur par défaut et le note.
        template <typename T, typename Check>
        void Read(const Json& object, const char* key, T& value, Check isValid, std::vector<std::string>* warnings) {
            const auto found = object.find(key);
            if (found == object.end()) {
                return;
            }
            if (isValid(*found)) {
                value = found->get<T>();
            } else if (warnings) {
                warnings->push_back(std::format("« {} » ignoré (valeur non valide)", key));
            }
        }

        bool IsBool(const Json& value) { return value.is_boolean(); }
        bool IsNumber(const Json& value) { return value.is_number(); }
        bool IsInteger(const Json& value) { return value.is_number_integer(); }
    }

    std::expected<GameSettings, std::string> Parse(std::string_view text, std::vector<std::string>* warnings) {
        const Json document = Json::parse(text, nullptr, false);
        if (document.is_discarded() || !document.is_object()) {
            return std::unexpected("ce n'est pas du JSON");
        }
        if (document.value("format", std::string{}) != FormatName) {
            return std::unexpected(std::format("ce ne sont pas des réglages de jeu (« format » différent de « {} »)", FormatName));
        }
        if (const auto version = document.find("version");
            version == document.end() || !version->is_number_integer() || version->get<int>() < 1 || version->get<int>() > FormatVersion) {
            return std::unexpected(std::format("version non gérée (ce moteur lit jusqu'à la version {})", FormatVersion));
        }

        GameSettings settings;
        if (const auto window = document.find("window"); window != document.end() && window->is_object()) {
            Read(*window, "width", settings.windowSize.x, IsInteger, warnings);
            Read(*window, "height", settings.windowSize.y, IsInteger, warnings);
            settings.windowSize.x = std::clamp(settings.windowSize.x, MinWindowSize, MaxWindowSize);
            settings.windowSize.y = std::clamp(settings.windowSize.y, MinWindowSize, MaxWindowSize);
            Read(*window, "maximized", settings.maximized, IsBool, warnings);
            Read(*window, "fullscreen", settings.fullscreen, IsBool, warnings);
        }
        Read(document, "vsync", settings.vsync, IsBool, warnings);
        Read(document, "temporalAA", settings.temporalAA, IsBool, warnings);
        if (const auto detail = document.find("groundDetail"); detail != document.end()) {
            const auto found = detail->is_string() ? std::ranges::find(GroundDetailKeys, detail->get<std::string>()) : GroundDetailKeys.end();
            if (found != GroundDetailKeys.end()) {
                settings.groundDetail = static_cast<GroundDetail>(found - GroundDetailKeys.begin());
            } else if (warnings) {
                warnings->push_back("« groundDetail » ignoré (valeur non valide)");
            }
        }
        if (const auto quality = document.find("cloudQuality"); quality != document.end()) {
            const auto found = quality->is_string() ? std::ranges::find(CloudQualityKeys, quality->get<std::string>()) : CloudQualityKeys.end();
            if (found != CloudQualityKeys.end()) {
                settings.cloudQuality = static_cast<CloudQuality>(found - CloudQualityKeys.begin());
            } else if (warnings) {
                warnings->push_back("« cloudQuality » ignoré (valeur non valide)");
            }
        }
        Read(document, "volume", settings.volume, IsNumber, warnings);
        settings.volume = std::clamp(settings.volume, 0.0f, 1.0f);
        Read(document, "engineStats", settings.showEngineStats, IsBool, warnings);
        return settings;
    }

    std::string ToString(const GameSettings& settings) {
        nlohmann::ordered_json document;
        document["format"] = FormatName;
        document["version"] = FormatVersion;
        document["window"] = {{"width", settings.windowSize.x}, {"height", settings.windowSize.y}, {"maximized", settings.maximized},
                              {"fullscreen", settings.fullscreen}};
        document["vsync"] = settings.vsync;
        document["temporalAA"] = settings.temporalAA;
        document["groundDetail"] = GroundDetailKeys[static_cast<std::size_t>(settings.groundDetail)];
        document["cloudQuality"] = CloudQualityKeys[static_cast<std::size_t>(settings.cloudQuality)];
        document["volume"] = std::round(settings.volume * 100.0) / 100.0; // 0.4 plutôt que 0.4000000059604645
        document["engineStats"] = settings.showEngineStats;
        return document.dump(2) + "\n";
    }

    std::filesystem::path GetPath() {
        return Platform::GetExecutableDirectory() / "Reglages.json";
    }

    std::expected<GameSettings, std::string> Load(const std::filesystem::path& path) {
        std::ifstream stream(path, std::ios::binary);
        if (!stream) {
            return GameSettings{}; // premier lancement
        }
        std::ostringstream text;
        text << stream.rdbuf();
        std::vector<std::string> warnings;
        auto settings = Parse(text.str(), &warnings);
        if (!settings) {
            return std::unexpected(Platform::ToUtf8(path.filename()) + " : " + settings.error());
        }
        for (const std::string& warning : warnings) {
            Log::Warning("{} : {}", Platform::ToUtf8(path.filename()), warning);
        }
        return settings;
    }

    std::expected<void, std::string> Save(const GameSettings& settings, const std::filesystem::path& path) {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream << ToString(settings);
        if (!stream) {
            return std::unexpected("impossible d'écrire " + Platform::ToUtf8(path));
        }
        return {};
    }
}
