#include "Panels/SettingsWindow.hpp"

#include "Search.hpp"

#include <Flux/Audio/AudioSystem.hpp>
#include <Flux/Core/Engine.hpp>
#include <Flux/Core/Log.hpp>
#include <Flux/Core/Version.hpp>
#include <Flux/Input/InputActions.hpp>
#include <Flux/Platform/Filesystem.hpp>
#include <Flux/Platform/Window.hpp>
#include <Flux/Renderer/Renderer.hpp>
#include <Flux/UI/Context.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <string_view>
#include <fstream>

namespace FluxEditor {
    using namespace Flux;
    using Json = nlohmann::json;

    namespace {
        constexpr std::array<float, 8> ScaleSteps{0.8f, 0.9f, 1.0f, 1.1f, 1.25f, 1.5f, 1.75f, 2.0f};
        constexpr std::array<std::string_view, 8> ScaleNames{"80 %", "90 %", "100 %", "110 %", "125 %", "150 %", "175 %", "200 %"};
        constexpr float MinScale = ScaleSteps.front();
        constexpr float MaxScale = ScaleSteps.back();
    }

    void SettingsWindow::Load(Engine& engine) {
        std::ifstream file(FilePath(), std::ios::binary);
        if (!file.is_open()) {
            return; // premier lancement : thème par défaut
        }
        const Json json = Json::parse(file, nullptr, false);
        if (json.is_discarded() || !json.is_object()) {
            return;
        }
        if (const auto theme = json.find("theme"); theme != json.end() && theme->is_string()) {
            const UI::Theme found = UI::ThemeFromName(theme->get<std::string>());
            if (found != UI::Theme::Count) {
                m_theme = found;
            }
        }
        if (const auto scale = json.find("uiScale"); scale != json.end() && scale->is_number()) {
            m_uiScale = std::clamp(scale->get<float>(), MinScale, MaxScale);
        }
        ApplyStyle(engine);
        if (const auto volume = json.find("volume"); volume != json.end() && volume->is_number()) {
            m_volume = std::clamp(volume->get<float>(), 0.0f, 1.0f);
            engine.GetAudio().SetMasterVolume(m_volume);
        }
        if (const auto bloom = json.find("bloom"); bloom != json.end() && bloom->is_number()) {
            m_bloom = std::clamp(bloom->get<float>(), 0.0f, 1.0f);
        }
        if (const auto enabled = json.find("bloomEnabled"); enabled != json.end() && enabled->is_boolean()) {
            m_bloomEnabled = enabled->get<bool>();
        }
        if (const auto temporal = json.find("temporalAA"); temporal != json.end() && temporal->is_boolean()) {
            m_temporalAA = temporal->get<bool>();
        }
        if (const auto upscaling = json.find("upscaling"); upscaling != json.end() && upscaling->is_string()) {
            const auto found = std::ranges::find(UpscalingKeys, upscaling->get<std::string>());
            if (found != UpscalingKeys.end()) {
                m_upscaling = static_cast<Upscaling>(found - UpscalingKeys.begin());
            }
        }
        if (const auto sharpness = json.find("sharpness"); sharpness != json.end() && sharpness->is_number()) {
            m_sharpness = std::clamp(sharpness->get<float>(), 0.0f, 1.0f);
        }
        if (const auto detail = json.find("groundDetail"); detail != json.end() && detail->is_string()) {
            const auto found = std::ranges::find(GroundDetailKeys, detail->get<std::string>());
            if (found != GroundDetailKeys.end()) {
                m_groundDetail = static_cast<GroundDetail>(found - GroundDetailKeys.begin());
            }
        }
        if (const auto quality = json.find("cloudQuality"); quality != json.end() && quality->is_string()) {
            const auto found = std::ranges::find(CloudQualityKeys, quality->get<std::string>());
            if (found != CloudQualityKeys.end()) {
                m_cloudQuality = static_cast<CloudQuality>(found - CloudQualityKeys.begin());
            }
        }
        if (const auto distance = json.find("vegetationDistance"); distance != json.end() && distance->is_string()) {
            const auto found = std::ranges::find(VegetationDistanceKeys, distance->get<std::string>());
            if (found != VegetationDistanceKeys.end()) {
                m_vegetationDistance = static_cast<VegetationDistance>(found - VegetationDistanceKeys.begin());
            }
        }
        ApplyGraphics(engine);
    }

    void SettingsWindow::Save() const {
        const Json json{{"format", "flux.editor-settings"},
                        {"version", 1},
                        {"theme", UI::GetThemeNames()[static_cast<std::size_t>(m_theme)]},
                        {"uiScale", m_uiScale},
                        {"volume", m_volume},
                        {"bloomEnabled", m_bloomEnabled},
                        {"bloom", m_bloom},
                        {"temporalAA", m_temporalAA},
                        {"upscaling", UpscalingKeys[static_cast<std::size_t>(m_upscaling)]},
                        {"sharpness", m_sharpness},
                        {"groundDetail", GroundDetailKeys[static_cast<std::size_t>(m_groundDetail)]},
                        {"cloudQuality", CloudQualityKeys[static_cast<std::size_t>(m_cloudQuality)]},
                        {"vegetationDistance", VegetationDistanceKeys[static_cast<std::size_t>(m_vegetationDistance)]}};
        std::ofstream file(FilePath(), std::ios::binary | std::ios::trunc);
        file << json.dump(2);
        if (!file) {
            Log::Warning("paramètres de l'éditeur non enregistrés");
        }
    }

    void SettingsWindow::ApplyGraphics(Engine& engine) const {
        Renderer& renderer = engine.GetRenderer();
        renderer.SetBloom(m_bloomEnabled ? m_bloom : 0.0f);
        renderer.SetTemporalAA(m_temporalAA);
        renderer.SetUpscaling(m_upscaling);
        renderer.SetSharpness(m_sharpness);
        renderer.SetGroundDetail(m_groundDetail);
        renderer.SetCloudQuality(m_cloudQuality);
        renderer.SetVegetationDistance(m_vegetationDistance);
    }

    void SettingsWindow::ApplyStyle(Engine& engine) const {
        engine.GetUi().SetStyle(UI::Style::FromTheme(m_theme).Scaled(m_uiScale)); // visible dès la frame suivante
    }

    std::filesystem::path SettingsWindow::FilePath() {
        return Platform::GetExecutableDirectory() / "FluxEditor.settings.json";
    }


    namespace {
        // Préréglages des graphismes : de quoi partir vite, chaque réglage reste modifiable ensuite. Élevé = valeurs
        // par défaut. Ultra garde la pleine définition (FSR Natif AA lisse sans réduire l'image).
        struct GraphicsPreset
        {
            std::string_view name;
            Upscaling upscaling;
            GroundDetail groundDetail;
            CloudQuality cloudQuality;
            VegetationDistance vegetationDistance;
        };
        constexpr std::array<GraphicsPreset, 4> GraphicsPresets{{
            {"Bas", Upscaling::FsrPerformance, GroundDetail::Low, CloudQuality::Low, VegetationDistance::Low},
            {"Moyen", Upscaling::FsrBalanced, GroundDetail::Medium, CloudQuality::Medium, VegetationDistance::Medium},
            {"Élevé", DefaultUpscaling, DefaultGroundDetail, DefaultCloudQuality, DefaultVegetationDistance},
            {"Ultra", Upscaling::FsrNativeAA, GroundDetail::Ultra, CloudQuality::High, VegetationDistance::Ultra},
        }};

        struct Category
        {
            std::string_view name;
            UI::Icon icon;
            std::string_view summary; // sous le titre de la page
        };
        constexpr std::array<Category, 5> Categories{{
            {"Graphiques", UI::Icon::Monitor, "Qualité de l'image et coût pour la carte graphique."},
            {"Son", UI::Icon::Sound, "Volume de l'éditeur et du mode Jouer."},
            {"Touches", UI::Icon::Keyboard, "Les mêmes pour l'éditeur et le jeu (Touches.json, à côté de l'exécutable)."},
            {"Interface", UI::Icon::Window, "Apparence de l'éditeur."},
            {"Système", UI::Icon::Info, "Carte graphique, version et fichiers de réglages."},
        }};
    }

    void SettingsWindow::Draw(Engine& engine) {
        if (!m_open) {
            return;
        }
        UI::Context& ui = engine.GetUi();
        const Math::Vector2i screen = engine.GetWindow().GetSize();
        // Tailles suivant celle de l'interface (Interface > Taille), comme le texte.
        const float scale = ui.GetStyle().fontSize / 15.0f;
        const float width = std::round(760.0f * scale);
        const float height = std::round(540.0f * scale);
        // Au milieu de l'écran à la première ouverture ; ensuite là où on l'a laissée.
        ui.SetNextPanelIcon(UI::Icon::Settings);
        if (ui.BeginPanel("Paramètres", {(static_cast<float>(screen.x) - width) * 0.5f,
                                         std::max(0.0f, (static_cast<float>(screen.y) - height) * 0.5f)},
                          width, height, &m_open)) {
            ui.BeginChild("catégories", std::round(180.0f * scale), 0.0f);
            for (std::size_t i = 0; i < Categories.size(); ++i) {
                if (ui.Selectable(Categories[i].name, Categories[i].icon, m_tab == static_cast<Tab>(i))) {
                    m_tab = static_cast<Tab>(i);
                }
            }
            ui.EndChild();
            ui.SameLine();
            ui.BeginChild("page", 0.0f, 0.0f, false);
            const Category& category = Categories[static_cast<std::size_t>(m_tab)];
            ui.Heading(category.name, category.icon);
            ui.TextDisabled(category.summary);
            switch (m_tab) {
                case Tab::Graphics: DrawGraphics(engine); break;
                case Tab::Sound: DrawSound(engine); break;
                case Tab::Keys: DrawKeys(engine); break;
                case Tab::Interface: DrawInterface(engine); break;
                case Tab::System: DrawSystem(engine); break;
            }
            ui.EndChild();
        }
        ui.EndPanel();
    }

    void SettingsWindow::DrawGraphics(Engine& engine) {
        UI::Context& ui = engine.GetUi();
        Renderer& renderer = engine.GetRenderer();

        ui.SectionTitle("Préréglage");
        // Enfoncé quand les réglages en cours sont exactement ceux du préréglage ; aucun : « Personnalisé ».
        bool custom = true;
        for (const GraphicsPreset& preset : GraphicsPresets) {
            bool active = m_upscaling == preset.upscaling && m_groundDetail == preset.groundDetail
                       && m_cloudQuality == preset.cloudQuality && m_vegetationDistance == preset.vegetationDistance;
            custom = custom && !active;
            if (&preset != GraphicsPresets.data()) {
                ui.SameLine();
            }
            if (ui.ToggleButton(preset.name, UI::Icon::None, active)) {
                m_upscaling = preset.upscaling;
                m_groundDetail = preset.groundDetail;
                m_cloudQuality = preset.cloudQuality;
                m_vegetationDistance = preset.vegetationDistance;
                ApplyGraphics(engine);
                Save();
            }
        }
        ui.HelpText(custom ? "Personnalisé : les réglages ne suivent aucun préréglage."
                           : "Règle d'un coup la mise à l'échelle, le détail du sol, les nuages et la végétation.");

        ui.SectionTitle("Affichage");
        bool vsync = renderer.IsVSyncEnabled();
        if (ui.Checkbox("Synchronisation verticale (V-Sync)", vsync)) {
            renderer.SetVSync(vsync);
        }
        ui.HelpText("Cale les images sur l'écran : pas de déchirure, mais pas plus d'images/s que sa fréquence.");
        // Indice 0 : 2 images (Renderer::MinSwapchainImages).
        static constexpr std::array<std::string_view, 2> SwapchainLabels{"2 (moins de retard)", "3 (plus fluide)"};
        int swapchain = static_cast<int>(renderer.GetSwapchainImages() - Renderer::MinSwapchainImages);
        if (ui.Choice("Images d'avance", swapchain, SwapchainLabels)) {
            renderer.SetSwapchainImages(Renderer::MinSwapchainImages + static_cast<std::uint32_t>(swapchain));
        }
        ui.HelpText("Avec la V-Sync, 2 images répondent une image plus tôt (16,7 ms à 60 Hz) tant que la carte graphique "
                    "suit l'écran ; 3 évitent un à-coup quand une image dépasse. Sans V-Sync, garder 3.");
        int upscaling = static_cast<int>(m_upscaling);
        if (ui.Choice("Mise à l'échelle", upscaling, UpscalingLabels)) {
            m_upscaling = static_cast<Upscaling>(upscaling);
            renderer.SetUpscaling(m_upscaling);
            Save();
        }
        ui.HelpText("FSR dessine la scène plus petite puis l'agrandit : plus d'images/s quand la carte graphique limite. "
                    "Il lisse aussi les bords, à la place du TAA.");
        if (IsFsr(m_upscaling)) {
            if (!renderer.IsUpscalingAvailable()) {
                ui.LabelText("FSR", "indisponible (voir la console) : rendu en Natif");
            }
            float percent = m_sharpness * 100.0f;
            if (ui.DragFloat("Netteté FSR (%)", percent, 0.5f, 0.0, 100.0, 0)) {
                m_sharpness = percent / 100.0f;
                renderer.SetSharpness(m_sharpness);
                m_sharpnessChanged = true;
            }
            // Enregistré au lâcher, pas à chaque pixel glissé.
            if (m_sharpnessChanged && !ui.IsItemActive()) {
                m_sharpnessChanged = false;
                Save();
            }
        } else {
            if (ui.Checkbox("Anticrénelage temporel (TAA)", m_temporalAA)) {
                renderer.SetTemporalAA(m_temporalAA);
                Save();
            }
            ui.HelpText("Lisse les bords en escalier et le grain des nuages en mélangeant les images successives.");
        }

        ui.SectionTitle("Qualité");
        int detail = static_cast<int>(m_groundDetail);
        if (ui.Choice("Détail du sol", detail, GroundDetailLabels)) {
            m_groundDetail = static_cast<GroundDetail>(detail);
            renderer.SetGroundDetail(m_groundDetail);
            Save();
        }
        ui.HelpText("Distance où le sol des planètes devient moins fin : plus élevé, plus de tuiles à calculer.");
        int vegetation = static_cast<int>(m_vegetationDistance);
        if (ui.Choice("Distance de la végétation", vegetation, VegetationDistanceLabels)) {
            m_vegetationDistance = static_cast<VegetationDistance>(vegetation);
            renderer.SetVegetationDistance(m_vegetationDistance);
            Save();
        }
        ui.HelpText("Distance d'affichage des arbres : 50 %, 75 %, 100 % ou 150 % de la portée de chaque planète. Ultra "
                    "dessine environ deux fois plus de plaines d'arbres qu'Élevé.");
        int clouds = static_cast<int>(m_cloudQuality);
        if (ui.Choice("Qualité des nuages", clouds, CloudQualityLabels)) {
            m_cloudQuality = static_cast<CloudQuality>(clouds);
            renderer.SetCloudQuality(m_cloudQuality);
            Save();
        }
        ui.HelpText("Pas de calcul dans les nuages : Bas coûte environ 40 % d'Élevé, avec plus de grain.");

        ui.SectionTitle("Effets");
        if (ui.Checkbox("Bloom", m_bloomEnabled)) {
            renderer.SetBloom(m_bloomEnabled ? m_bloom : 0.0f);
            Save();
        }
        ui.HelpText("Fait déborder la lumière des zones très lumineuses : soleil, flammes, émissifs.");
        if (m_bloomEnabled) {
            float percent = m_bloom * 100.0f;
            if (ui.DragFloat("Intensité du bloom (%)", percent, 0.05f, 0.0, 30.0, 1)) {
                m_bloom = percent / 100.0f;
                renderer.SetBloom(m_bloom);
                m_bloomChanged = true;
            }
            if (m_bloomChanged && !ui.IsItemActive()) {
                m_bloomChanged = false;
                Save();
            }
        }

        ui.SectionTitle("");
        if (ui.Button("Rétablir les graphismes par défaut", UI::Icon::Undo)) {
            m_upscaling = DefaultUpscaling;
            m_sharpness = DefaultSharpness;
            m_temporalAA = true;
            m_groundDetail = DefaultGroundDetail;
            m_cloudQuality = DefaultCloudQuality;
            m_vegetationDistance = DefaultVegetationDistance;
            m_bloomEnabled = true;
            m_bloom = Renderer::DefaultBloom;
            ApplyGraphics(engine);
            Save();
        }
    }

    void SettingsWindow::DrawSound(Engine& engine) {
        UI::Context& ui = engine.GetUi();
        AudioSystem& audio = engine.GetAudio();
        ui.SectionTitle("Volume");
        float percent = m_volume * 100.0f;
        if (ui.DragFloat("Volume général (%)", percent, 0.5f, 0.0, 100.0, 0)) {
            m_volume = percent / 100.0f;
            audio.SetMasterVolume(m_volume);
            m_volumeChanged = true;
        }
        // Enregistré au lâcher, pas à chaque pixel glissé.
        if (m_volumeChanged && !ui.IsItemActive()) {
            m_volumeChanged = false;
            Save();
        }
        ui.HelpText("Les sons (composant Son) jouent en mode Jouer.");

        ui.SectionTitle("Sortie");
        ui.LabelText("Sortie son", audio.IsAvailable() ? "active" : "aucune : l'éditeur reste muet");

        ui.SectionTitle("");
        if (ui.Button("Rétablir le son par défaut", UI::Icon::Undo)) {
            m_volume = 1.0f;
            audio.SetMasterVolume(m_volume);
            Save();
        }
    }

    void SettingsWindow::DrawKeys(Engine& engine) {
        UI::Context& ui = engine.GetUi();
        ui.SectionTitle("Actions");
        ui.SearchField("recherche", m_keySearch, "Rechercher une action...");
        ui.HelpText("Clic sur une touche, puis la nouvelle touche (Échap annule). Elle remplace la principale ; la seconde "
                    "reste.");
        // Actions du moteur et du jeu chargé, rangées par catégorie ; une catégorie sans action trouvée disparaît.
        const SearchQuery query(m_keySearch);
        const InputActions& actions = engine.GetInputActions();
        std::string_view category;
        std::uint32_t row = 0;
        bool found = false;
        for (const InputAction& action : actions.GetActions()) {
            ++row;
            if (!query.Matches(action.name) && !query.Matches(action.category)) {
                continue;
            }
            found = true;
            if (action.category != category) {
                category = action.category;
                ui.SectionTitle(category);
            }
            ui.PushId(row);
            const bool waiting = engine.GetKeyRebinding() == action.name;
            if (ui.LabelButton(action.name, waiting ? std::string("Appuyez sur une touche...") : actions.GetKeysText(action.name),
                               waiting)) {
                engine.StartKeyRebinding(action.name);
            }
            ui.PopId();
        }
        if (!found) {
            ui.TextDisabled("Aucune action ne correspond.");
        }
        ui.SectionTitle("");
        if (ui.Button("Touches par défaut", UI::Icon::Undo)) {
            engine.ResetInputActions();
        }
    }

    void SettingsWindow::DrawInterface(Engine& engine) {
        UI::Context& ui = engine.GetUi();
        ui.SectionTitle("Apparence");
        int theme = static_cast<int>(m_theme);
        if (ui.Choice("Thème", theme, UI::GetThemeNames())) {
            m_theme = static_cast<UI::Theme>(theme);
            ApplyStyle(engine);
            Save();
        }
        ui.HelpText("Couleurs des panneaux, des champs et de l'accent.");
        // Une liste plutôt qu'un curseur : tout se remet en page à chaque valeur, le champ glissé fuirait sous la souris.
        int scale = static_cast<int>(std::ranges::min_element(ScaleSteps, {}, [this](float step) {
                                         return std::abs(step - m_uiScale);
                                     }) - ScaleSteps.begin());
        if (ui.Choice("Taille de l'interface", scale, ScaleNames)) {
            m_uiScale = ScaleSteps[static_cast<std::size_t>(scale)];
            ApplyStyle(engine);
            Save();
        }
        ui.HelpText("Texte, marges et boutons. La largeur des panneaux se règle à la souris.");

        ui.SectionTitle("");
        if (ui.Button("Rétablir l'interface par défaut", UI::Icon::Undo)) {
            m_theme = UI::Theme::Dark;
            m_uiScale = 1.0f;
            ApplyStyle(engine);
            Save();
        }
    }

    void SettingsWindow::DrawSystem(Engine& engine) {
        UI::Context& ui = engine.GetUi();
        const Renderer& renderer = engine.GetRenderer();
        ui.SectionTitle("Carte graphique");
        ui.LabelText("Modèle", renderer.GetDeviceName());
        ui.LabelText("Vulkan", renderer.GetApiVersion());
        ui.LabelText("Présentation", std::format("{}, {} images", renderer.GetPresentModeName(), renderer.GetSwapchainImageCount()));
        const Math::Vector2i size = renderer.GetRenderSize();
        ui.LabelText("Scène dessinée en", std::format("{} x {}", size.x, size.y));

        ui.SectionTitle("FluxEngine");
        ui.LabelText("Version", GetFullVersion());
        ui.LabelText("Réglages", "FluxEditor.settings.json");
        ui.HelpText("À côté de FluxEditor.exe ; supprimer ce fichier rend tous les réglages par défaut.");
    }
}
