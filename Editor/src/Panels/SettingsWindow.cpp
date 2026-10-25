#include "Panels/SettingsWindow.hpp"

#include <Flux/Audio/AudioSystem.hpp>
#include <Flux/Core/Engine.hpp>
#include <Flux/Core/Log.hpp>
#include <Flux/Input/InputActions.hpp>
#include <Flux/Platform/Filesystem.hpp>
#include <Flux/Platform/Window.hpp>
#include <Flux/Renderer/Renderer.hpp>
#include <Flux/UI/Context.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
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
        engine.GetRenderer().SetBloom(m_bloomEnabled ? m_bloom : 0.0f);
        if (const auto temporal = json.find("temporalAA"); temporal != json.end() && temporal->is_boolean()) {
            m_temporalAA = temporal->get<bool>();
        }
        engine.GetRenderer().SetTemporalAA(m_temporalAA);
        if (const auto detail = json.find("groundDetail"); detail != json.end() && detail->is_string()) {
            const auto found = std::ranges::find(GroundDetailKeys, detail->get<std::string>());
            if (found != GroundDetailKeys.end()) {
                m_groundDetail = static_cast<GroundDetail>(found - GroundDetailKeys.begin());
            }
        }
        engine.GetRenderer().SetGroundDetail(m_groundDetail);
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
                        {"groundDetail", GroundDetailKeys[static_cast<std::size_t>(m_groundDetail)]}};
        std::ofstream file(FilePath(), std::ios::binary | std::ios::trunc);
        file << json.dump(2);
        if (!file) {
            Log::Warning("paramètres de l'éditeur non enregistrés");
        }
    }

    void SettingsWindow::ApplyStyle(Engine& engine) const {
        engine.GetUi().SetStyle(UI::Style::FromTheme(m_theme).Scaled(m_uiScale)); // visible dès la frame suivante
    }

    std::filesystem::path SettingsWindow::FilePath() {
        return Platform::GetExecutableDirectory() / "FluxEditor.settings.json";
    }

    void SettingsWindow::Draw(Engine& engine) {
        if (!m_open) {
            return;
        }
        UI::Context& ui = engine.GetUi();
        const Math::Vector2i screen = engine.GetWindow().GetSize();
        const float width = 420.0f;
        // Au milieu de l'écran à la première ouverture ; ensuite là où on l'a laissée.
        if (ui.BeginPanel("Paramètres", {(static_cast<float>(screen.x) - width) * 0.5f, static_cast<float>(screen.y) * 0.25f},
                          width, 0.0f, &m_open)) {
            if (ui.Tab("Graphiques", m_tab == Tab::Graphics)) {
                m_tab = Tab::Graphics;
            }
            ui.SameLine();
            if (ui.Tab("Son", m_tab == Tab::Sound)) {
                m_tab = Tab::Sound;
            }
            ui.SameLine();
            if (ui.Tab("Touches", m_tab == Tab::Keys)) {
                m_tab = Tab::Keys;
            }
            ui.SameLine();
            if (ui.Tab("Interface", m_tab == Tab::Interface)) {
                m_tab = Tab::Interface;
            }
            ui.Separator();

            if (m_tab == Tab::Graphics) {
                Renderer& renderer = engine.GetRenderer();
                ui.LabelText("GPU", renderer.GetDeviceName());
                ui.LabelText("Vulkan", renderer.GetApiVersion());
                bool vsync = renderer.IsVSyncEnabled();
                if (ui.Checkbox("V-Sync", vsync)) {
                    renderer.SetVSync(vsync);
                }
                ui.LabelText("Présentation", renderer.GetPresentModeName());
                ui.Separator();
                // Lisse les bords en escalier et le grain des nuages en mélangeant les images successives.
                if (ui.Checkbox("Anticrénelage (TAA)", m_temporalAA)) {
                    renderer.SetTemporalAA(m_temporalAA);
                    Save();
                }
                // Distance où le sol des planètes devient moins fin : plus élevé, plus de tuiles à calculer et dessiner.
                int detail = static_cast<int>(m_groundDetail);
                if (ui.Choice("Détail du sol", detail, GroundDetailLabels)) {
                    m_groundDetail = static_cast<GroundDetail>(detail);
                    renderer.SetGroundDetail(m_groundDetail);
                    Save();
                }
                // Le bloom fait déborder la lumière des zones très lumineuses (soleil, flammes, émissifs).
                if (ui.Checkbox("Bloom", m_bloomEnabled)) {
                    renderer.SetBloom(m_bloomEnabled ? m_bloom : 0.0f);
                    Save();
                }
                if (m_bloomEnabled) {
                    float percent = m_bloom * 100.0f;
                    if (ui.DragFloat("Intensité du bloom (%)", percent, 0.05f, 0.0, 30.0, 1)) {
                        m_bloom = percent / 100.0f;
                        renderer.SetBloom(m_bloom);
                        m_bloomChanged = true;
                    }
                    // Enregistré au lâcher, pas à chaque pixel glissé.
                    if (m_bloomChanged && !ui.IsItemActive()) {
                        m_bloomChanged = false;
                        Save();
                    }
                }
            } else if (m_tab == Tab::Sound) {
                AudioSystem& audio = engine.GetAudio();
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
                if (!audio.IsAvailable()) {
                    ui.TextDisabled("Aucune sortie son : le jeu reste muet.");
                }
                ui.TextDisabled("Les sons (composant Son) jouent en mode Jouer.");
            } else if (m_tab == Tab::Keys) {
                // Actions d'entrée du moteur et du jeu chargé : clic sur la touche, puis la nouvelle touche (Échap annule).
                // Gardées dans Touches.json, à côté de l'exécutable : les mêmes pour l'éditeur et le jeu.
                const InputActions& actions = engine.GetInputActions();
                std::string_view category;
                std::uint32_t row = 0;
                for (const InputAction& action : actions.GetActions()) {
                    if (action.category != category) {
                        category = action.category;
                        ui.TextDisabled(category);
                    }
                    ui.PushId(++row);
                    const bool waiting = engine.GetKeyRebinding() == action.name;
                    ui.LabelText(action.name, waiting ? std::string("Appuyez sur une touche...") : actions.GetKeysText(action.name));
                    ui.SameLine();
                    if (ui.Button("Changer")) {
                        engine.StartKeyRebinding(action.name);
                    }
                    ui.PopId();
                }
                ui.Separator();
                if (ui.Button("Touches par défaut")) {
                    engine.ResetInputActions();
                }
                ui.TextDisabled("La touche choisie remplace la principale ; la seconde reste.");
            } else {
                int theme = static_cast<int>(m_theme);
                if (ui.Choice("Thème", theme, UI::GetThemeNames())) {
                    m_theme = static_cast<UI::Theme>(theme);
                    ApplyStyle(engine);
                    Save();
                }
                // Une liste plutôt qu'un curseur : tout se remet en page à chaque valeur, le champ glissé fuirait
                // sous la souris.
                int scale = static_cast<int>(std::ranges::min_element(ScaleSteps, {}, [this](float step) {
                                                 return std::abs(step - m_uiScale);
                                             }) - ScaleSteps.begin());
                if (ui.Choice("Taille de l'interface", scale, ScaleNames)) {
                    m_uiScale = ScaleSteps[static_cast<std::size_t>(scale)];
                    ApplyStyle(engine);
                    Save();
                }
                ui.SetItemTooltip("Texte, marges et boutons. La largeur des panneaux se règle à la souris.");
            }
        }
        ui.EndPanel();
    }
}
