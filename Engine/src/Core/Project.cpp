#include "Flux/Core/Project.hpp"

#include "Flux/Platform/Filesystem.hpp"

#include <nlohmann/json.hpp>

#include <format>
#include <fstream>
#include <sstream>
#include <system_error>

namespace Flux::ProjectFile {
    namespace {
        constexpr std::string_view FormatName = "flux.project";

        // Chemin relatif qui reste sous le dossier du projet : ni absolu, ni « .. ».
        std::expected<std::filesystem::path, std::string> ReadRelative(const nlohmann::json& document, const char* key) {
            const auto found = document.find(key);
            if (found == document.end() || !found->is_string() || found->get<std::string>().empty()) {
                return std::unexpected(std::format("« {} » doit être un chemin (texte)", key));
            }
            const std::filesystem::path path = Platform::PathFromUtf8(found->get<std::string>()).lexically_normal();
            if (path.has_root_path() || (!path.empty() && *path.begin() == "..")) {
                return std::unexpected(std::format("« {} » doit être relatif au projet, sans « .. »", key));
            }
            return path;
        }
    }

    std::expected<Project, std::string> Parse(std::string_view json, const std::filesystem::path& file) {
        try {
            const nlohmann::json document = nlohmann::json::parse(json);
            if (!document.is_object() || document.value("format", std::string{}) != FormatName) {
                return std::unexpected(std::format("ce n'est pas un projet Flux (« format » différent de « {} »)", FormatName));
            }
            const int version = document.at("version").get<int>();
            if (version < 1 || version > FormatVersion) {
                return std::unexpected(std::format("version {} non gérée (ce moteur lit jusqu'à la version {})", version, FormatVersion));
            }
            auto assets = ReadRelative(document, "assets");
            if (!assets) {
                return std::unexpected(assets.error());
            }
            auto startupScene = ReadRelative(document, "startupScene");
            if (!startupScene) {
                return std::unexpected(startupScene.error());
            }

            Project project;
            project.name = document.value("name", std::string{});
            if (project.name.empty()) {
                project.name = Platform::ToUtf8(file.stem());
            }
            project.file = std::filesystem::absolute(file).lexically_normal();
            project.assetsDirectory = (project.file.parent_path() / *assets).lexically_normal();
            project.startupScene = *startupScene;
            return project;
        } catch (const std::exception& error) {
            return std::unexpected(std::string(error.what()));
        }
    }

    std::expected<Project, std::string> Load(const std::filesystem::path& file) {
        std::ifstream stream(file, std::ios::binary);
        if (!stream) {
            return std::unexpected("projet introuvable : " + Platform::ToUtf8(file));
        }
        std::ostringstream text;
        text << stream.rdbuf();
        if (auto project = Parse(text.str(), file); !project) {
            return std::unexpected(Platform::ToUtf8(file.filename()) + " : " + project.error());
        } else {
            return project;
        }
    }

    std::string ToString(const Project& project) {
        nlohmann::ordered_json document;
        document["format"] = FormatName;
        document["version"] = FormatVersion;
        document["name"] = project.name;
        document["assets"] = Platform::ToGenericUtf8(project.assetsDirectory.lexically_relative(project.file.parent_path()));
        document["startupScene"] = Platform::ToGenericUtf8(project.startupScene);
        return document.dump(2) + '\n';
    }

    std::expected<void, std::string> Save(const Project& project) {
        const std::string text = ToString(project);
        std::filesystem::path temporary = project.file;
        temporary += ".tmp";
        {
            std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
            if (!stream.write(text.data(), static_cast<std::streamsize>(text.size()))) {
                return std::unexpected("écriture impossible : " + Platform::ToUtf8(temporary));
            }
        }
        std::error_code error;
        std::filesystem::rename(temporary, project.file, error);
        if (error) {
            return std::unexpected(std::format("remplacement de {} : {}", Platform::ToUtf8(project.file), error.message()));
        }
        return {};
    }
}
