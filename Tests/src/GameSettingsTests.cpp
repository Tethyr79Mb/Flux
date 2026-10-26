// Tests des réglages du joueur (Reglages.json) : aller-retour, valeurs fausses remplacées sans refuser le fichier.
#include "Check.hpp"

#include "Flux/Core/GameSettings.hpp"

using namespace Flux;

bool RunGameSettingsTests() {
    GameSettings changed;
    changed.windowSize = {1920, 1080};
    changed.maximized = true;
    changed.fullscreen = true;
    changed.vsync = false;
    changed.temporalAA = false;
    changed.groundDetail = GroundDetail::Ultra;
    changed.cloudQuality = CloudQuality::Low;
    changed.upscaling = Upscaling::FsrPerformance;
    changed.sharpness = 0.8f;
    changed.volume = 0.25f;
    changed.showEngineStats = false;
    const auto reread = GameSettingsFile::Parse(GameSettingsFile::ToString(changed));
    CHECK(reread.has_value());
    CHECK(reread->windowSize.x == 1920 && reread->windowSize.y == 1080 && reread->maximized && reread->fullscreen);
    CHECK(!reread->vsync && !reread->temporalAA && reread->volume == 0.25f && !reread->showEngineStats);
    CHECK(reread->groundDetail == GroundDetail::Ultra);
    CHECK(reread->cloudQuality == CloudQuality::Low);
    CHECK(reread->upscaling == Upscaling::FsrPerformance && reread->sharpness == 0.8f);

    // Valeur absente : la valeur par défaut, sans avertissement.
    std::vector<std::string> warnings;
    const auto partial = GameSettingsFile::Parse(R"({"format": "flux.game-settings", "version": 1, "vsync": false})", &warnings);
    CHECK(partial && !partial->vsync && partial->windowSize.x == 1280 && partial->volume == 1.0f && warnings.empty());
    CHECK(partial->upscaling == DefaultUpscaling && partial->sharpness == DefaultSharpness);

    // Valeurs fausses : ignorées (avertissement) ou ramenées dans leurs bornes ; le fichier reste lu.
    warnings.clear();
    const auto wrong = GameSettingsFile::Parse(R"({"format": "flux.game-settings", "version": 1,
        "window": {"width": "grand", "height": 20}, "vsync": 1, "volume": 3, "groundDetail": "extreme",
        "upscaling": "dlss", "sharpness": 4, "cloudQuality": 2})", &warnings);
    CHECK(wrong.has_value());
    CHECK(wrong->windowSize.x == 1280 && wrong->windowSize.y == 320);
    CHECK(wrong->vsync && wrong->volume == 1.0f && wrong->groundDetail == DefaultGroundDetail);
    CHECK(wrong->upscaling == DefaultUpscaling && wrong->sharpness == 1.0f && wrong->cloudQuality == DefaultCloudQuality);
    CHECK(warnings.size() == 5); // « width », « vsync », « groundDetail », « upscaling » et « cloudQuality »

    const char* invalid[] = {
        "pas du json",
        R"({"format": "flux.project", "version": 1})",
        R"({"format": "flux.game-settings", "version": 2})",
        R"({"format": "flux.game-settings"})",
    };
    for (const char* text : invalid) {
        const auto result = GameSettingsFile::Parse(text);
        CHECK(!result.has_value() && !result.error().empty());
    }
    return true;
}
