// Tests du fichier de projet : chemins relatifs résolus, fichiers invalides refusés.
#include "Check.hpp"

#include "Flux/Core/Project.hpp"

using namespace Flux;

bool RunProjectTests() {
    const std::filesystem::path file = std::filesystem::path("jeu") / "Game.fluxproject";
    const auto project = ProjectFile::Parse(R"({"format": "flux.project", "version": 1, "name": "Démo",
        "assets": "assets", "startupScene": "scenes/Démarrage.scene.json"})", file);
    CHECK(project.has_value());
    CHECK(project->name == "Démo");
    CHECK(project->file.is_absolute());
    CHECK(project->assetsDirectory == (project->file.parent_path() / "assets").lexically_normal());
    CHECK(project->GetStartupScenePath().filename().u8string() == u8"Démarrage.scene.json");

    // Sans nom : celui du fichier.
    const auto unnamed = ProjectFile::Parse(R"({"format": "flux.project", "version": 1,
        "assets": ".", "startupScene": "a.scene.json"})", file);
    CHECK(unnamed.has_value() && unnamed->name == "Game");

    const char* invalid[] = {
        "pas du json",
        R"({"format": "flux.scene", "version": 1, "assets": "a", "startupScene": "b"})",
        R"({"format": "flux.project", "version": 2, "assets": "a", "startupScene": "b"})",
        R"({"format": "flux.project", "version": 1, "startupScene": "b"})",
        R"({"format": "flux.project", "version": 1, "assets": "a", "startupScene": 3})",
        R"({"format": "flux.project", "version": 1, "assets": "../ailleurs", "startupScene": "b"})",
        R"({"format": "flux.project", "version": 1, "assets": "a", "startupScene": "/b.scene.json"})",
    };
    for (const char* text : invalid) {
        const auto result = ProjectFile::Parse(text, file);
        CHECK(!result.has_value() && !result.error().empty());
    }
    return true;
}
