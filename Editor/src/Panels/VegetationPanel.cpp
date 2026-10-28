#include "Panels/VegetationPanel.hpp"

#include "Panels/AssetPicker.hpp"

#include <Flux/Core/Engine.hpp>
#include <Flux/Platform/Dialogs.hpp>
#include <Flux/Platform/Filesystem.hpp>
#include <Flux/Platform/Window.hpp>
#include <Flux/Scene/SceneSerializer.hpp>
#include <Flux/UI/Context.hpp>

#include <algorithm>
#include <format>
#include <optional>
#include <system_error>

namespace FluxEditor {
    using namespace Flux;
    using Planetology::TreeSpecies;
    using Planetology::VegetationBiome;

    namespace {
        constexpr std::string_view BiomeExtension = ".biome.json";
        constexpr std::string_view SpeciesExtension = ".espece.json";
        constexpr std::string_view PanelTitle = "Végétation";
        // Pendant un glisser, les planètes suivent à ce rythme : chaque aperçu fait resemer les arbres visibles.
        constexpr auto PreviewInterval = std::chrono::milliseconds(100);
        constexpr auto CheckInterval = std::chrono::seconds(1); // fichier changé hors de l'éditeur

        // [minimum, maximum] gardés dans l'ordre : le bord qu'on glisse pousse l'autre.
        bool DrawRange(UI::Context& ui, std::string_view label, float& low, float& high, float speed, double min, double max,
                       int decimals) {
            Math::Vector2f value{low, high};
            if (!ui.DragFloat2(label, value, speed, min, max, decimals)) {
                return false;
            }
            if (value.x > value.y) {
                if (value.x != low) {
                    value.y = value.x;
                } else {
                    value.x = value.y;
                }
            }
            low = value.x;
            high = value.y;
            return true;
        }

        // `value` désigne `from` ou un fichier sous lui (dossier déplacé) : réécrit vers `to`.
        bool ReplacePath(std::string& value, const std::string& from, const std::string& to) {
            if (value == from) {
                value = to;
                return true;
            }
            if (value.starts_with(from + "/")) {
                value = to + value.substr(from.size());
                return true;
            }
            return false;
        }

        std::optional<std::filesystem::path> MovedPath(const std::filesystem::path& path, const std::filesystem::path& from,
                                                       const std::filesystem::path& to) {
            const std::filesystem::path relative = path.lexically_normal().lexically_relative(from.lexically_normal());
            if (relative.empty() || *relative.begin() == "..") {
                return std::nullopt;
            }
            return relative == "." ? to : to / relative;
        }
    }

    bool VegetationPanel::IsBiome(const std::filesystem::path& path) {
        return Platform::ToUtf8(path.filename()).ends_with(BiomeExtension);
    }

    bool VegetationPanel::IsSpecies(const std::filesystem::path& path) {
        return Platform::ToUtf8(path.filename()).ends_with(SpeciesExtension);
    }

    std::string VegetationPanel::Write() const {
        return m_kind == Kind::Biome ? Planetology::WriteVegetationBiome(m_biome) : Planetology::WriteTreeSpecies(m_species);
    }

    bool VegetationPanel::Read(const std::string& text) {
        if (m_kind == Kind::Biome) {
            auto biome = Planetology::ParseVegetationBiome(text);
            if (!biome) {
                m_error = biome.error();
                return false;
            }
            m_biome = std::move(*biome);
        } else {
            auto species = Planetology::ParseTreeSpecies(text);
            if (!species) {
                m_error = species.error();
                return false;
            }
            m_species = std::move(*species);
        }
        m_error.clear();
        return true;
    }

    void VegetationPanel::Open(Engine& engine, const std::filesystem::path& path) {
        if (m_kind != Kind::None && path.lexically_normal() == m_path.lexically_normal()) {
            m_focus = true;
            return;
        }
        if (!ConfirmDiscard(engine)) {
            return;
        }
        Close();
        const Project* project = engine.GetProject();
        if (!project || (!IsBiome(path) && !IsSpecies(path))) {
            return;
        }
        m_kind = IsBiome(path) ? Kind::Biome : Kind::Species;
        m_path = path;
        m_reference = Platform::ToGenericUtf8(path.lexically_normal().lexically_relative(project->assetsDirectory.lexically_normal()));
        Load();
        m_focus = true;
    }

    bool VegetationPanel::Load() {
        std::vector<std::uint8_t> bytes;
        std::error_code error;
        m_savedTime = std::filesystem::last_write_time(m_path, error);
        m_steps.clear();
        m_position = 0;
        m_pending = false;
        if (!Platform::ReadFileBytes(m_path, bytes)) {
            m_error = "fichier introuvable";
            return false;
        }
        if (!Read(std::string(bytes.begin(), bytes.end()))) {
            return false;
        }
        m_saved = Write(); // mis en forme comme l'éditeur l'écrira : comparable à Write() pour savoir s'il est modifié
        m_steps.push_back(m_saved);
        Preview(m_saved);
        return true;
    }

    bool VegetationPanel::Save(Engine& engine) {
        if (m_kind == Kind::None || !m_error.empty()) {
            return false;
        }
        Commit();
        const std::string text = Write();
        if (auto written = SceneSerializer::WriteFile(m_path, text); !written) {
            Platform::ShowError(engine.GetWindow(), "Végétation", written.error());
            return false;
        }
        std::error_code error;
        m_savedTime = std::filesystem::last_write_time(m_path, error);
        m_saved = text;
        Preview(text); // le fichier dit maintenant la même chose : plus d'aperçu
        return true;
    }

    void VegetationPanel::Close() {
        if (!m_reference.empty()) {
            Planetology::SetVegetationPreview(m_reference, {});
        }
        const bool placed = m_placed;
        *this = VegetationPanel{};
        m_placed = placed;
    }

    bool VegetationPanel::ConfirmDiscard(Engine& engine) {
        if (!IsModified()) {
            return true;
        }
        const Platform::Answer answer =
            Platform::AskYesNoCancel(engine.GetWindow(), "Végétation modifiée",
                                     "Enregistrer les modifications de « " + Platform::ToUtf8(m_path.filename()) + " » ?");
        return answer == Platform::Answer::No || (answer == Platform::Answer::Yes && Save(engine));
    }

    bool VegetationPanel::ConfirmClose(Engine& engine) {
        if (!ConfirmDiscard(engine)) {
            return false;
        }
        Close();
        return true;
    }

    void VegetationPanel::Preview(const std::string& text) {
        if (text == m_previewed) {
            return;
        }
        m_previewed = text;
        m_lastPreview = std::chrono::steady_clock::now();
        // Identique au fichier : les planètes relisent le fichier.
        Planetology::SetVegetationPreview(m_reference, text == m_saved ? std::string() : text);
    }

    void VegetationPanel::Commit() {
        if (!m_pending) {
            return;
        }
        m_pending = false;
        std::string text = Write();
        if (text != m_steps[m_position]) {
            m_steps.resize(m_position + 1);
            m_steps.push_back(text);
            ++m_position;
        }
        Preview(text);
    }

    void VegetationPanel::Undo(bool redo) {
        Commit();
        if (redo ? m_position + 1 >= m_steps.size() : m_position == 0) {
            return;
        }
        m_position = redo ? m_position + 1 : m_position - 1;
        Read(m_steps[m_position]);
        Preview(m_steps[m_position]);
    }

    bool VegetationPanel::HandleShortcuts(Engine& engine) {
        const Input& input = engine.GetWindow().GetInput();
        if (m_kind == Kind::None || engine.GetUi().WantsKeyboard() || !input.IsControlDown()) {
            return false;
        }
        // Ctrl+S enregistre tout ce qui est ouvert (la scène aussi, de son côté).
        if (input.WasLetterPressed('S') && !input.IsShiftDown()) {
            if (IsModified()) {
                Save(engine);
            }
            return false;
        }
        if (!m_hovered || !m_error.empty()) {
            return false;
        }
        if (input.WasLetterPressed('Z')) {
            Undo(input.IsShiftDown());
            return true;
        }
        if (input.WasLetterPressed('Y')) {
            Undo(true);
            return true;
        }
        return false;
    }

    void VegetationPanel::Update(Engine& engine) {
        m_hovered = false;
        if (m_kind == Kind::None) {
            return;
        }
        UI::Context& ui = engine.GetUi();
        const auto now = std::chrono::steady_clock::now();
        // Fichier réécrit hors de l'éditeur (ou par le Contenu) : relu, s'il n'y a rien à perdre ici.
        if (now - m_lastCheck >= CheckInterval) {
            m_lastCheck = now;
            std::error_code error;
            const auto time = std::filesystem::last_write_time(m_path, error);
            if (!error && time != m_savedTime && !m_pending && !IsModified()) {
                Load();
            }
        }
        // À côté de l'inspecteur, en onglet, la première fois ; devant à chaque fichier ouvert.
        if (!m_placed) {
            ui.DockPanelNextTo(PanelTitle, "Inspecteur");
            m_placed = true;
        }
        if (std::exchange(m_focus, false)) {
            ui.FocusPanel(PanelTitle);
        }
        bool open = true;
        bool changed = false;
        const float screenWidth = static_cast<float>(engine.GetWindow().GetSize().x);
        ui.SetNextPanelIcon(m_kind == Kind::Biome ? UI::Icon::Biome : UI::Icon::Tree);
        if (ui.BeginPanel(PanelTitle, {std::max(316.0f, screenWidth - 752.0f), 8.0f}, 368.0f, 0.0f, &open)) {
            m_hovered = ui.IsPanelHovered();
            const bool modified = IsModified();
            ui.Text((m_kind == Kind::Biome ? "Biome : " : "Espèce : ") + Platform::ToUtf8(m_path.filename()) + (modified ? " *" : ""));
            ui.BeginDisabled(!modified);
            if (ui.Button("##enregistrer", UI::Icon::Save)) {
                Save(engine);
            }
            ui.SetItemTooltip("Enregistrer (Ctrl+S)");
            ui.EndDisabled();
            ui.SameLine();
            ui.BeginDisabled(m_position == 0 && !m_pending);
            if (ui.Button("##annuler", UI::Icon::Undo)) {
                Undo(false);
            }
            ui.SetItemTooltip("Annuler (Ctrl+Z, souris sur ce panneau)");
            ui.EndDisabled();
            ui.SameLine();
            ui.BeginDisabled(m_position + 1 >= m_steps.size());
            if (ui.Button("##rétablir", UI::Icon::Redo)) {
                Undo(true);
            }
            ui.SetItemTooltip("Rétablir (Ctrl+Y)");
            ui.EndDisabled();
            ui.Separator();
            if (!m_error.empty()) {
                ui.TextDisabled("Fichier illisible : " + m_error + "\nLe corriger dans un éditeur de texte : il sera relu.");
            } else {
                changed = m_kind == Kind::Biome ? DrawBiome(engine, ui) : DrawSpecies(engine, ui);
            }
        }
        ui.EndPanel();

        if (changed) {
            m_pending = true;
        }
        // Tout le geste (un glisser) fait un seul pas d'annulation ; les planètes le suivent pendant ce temps.
        if (m_pending) {
            if (!ui.IsInteracting()) {
                Commit();
            } else if (now - m_lastPreview >= PreviewInterval) {
                Preview(Write());
            }
        }
        if (!open && ConfirmDiscard(engine)) {
            Close();
            return;
        }
        if (!m_openNext.empty()) {
            const std::filesystem::path next = std::exchange(m_openNext, {});
            Open(engine, next);
        }
    }

    bool VegetationPanel::DrawAsset(Engine& engine, UI::Context& ui, const char* label, AssetType type, std::string& value) {
        std::string text = value;
        bool browse = false;
        bool changed = ui.InputTextBrowse(label, text, browse);
        if (browse) {
            // Boîte de dialogue modale : la frame attend le choix.
            if (std::optional<std::string> chosen = ChooseAsset(engine, type, text)) {
                text = std::move(*chosen);
                changed = true;
            }
        }
        // Vide : le fichier ne se relirait plus (un modèle, une espèce sont obligatoires).
        if (!changed || text.empty() || text == value) {
            return false;
        }
        value = std::move(text);
        return true;
    }

    bool VegetationPanel::DrawBiome(Engine& engine, UI::Context& ui) {
        bool changed = false;
        VegetationBiome& biome = m_biome;
        if (ui.CollapsingHeader("Où il pousse")) {
            ui.TextDisabled("Part prise dans chaque zone du relief (0 : jamais, 1 : partout où le climat convient).");
            changed |= ui.DragFloat("Plaines", biome.plains, 0.01f, 0.0, 1.0, 2);
            changed |= ui.DragFloat("Collines", biome.hills, 0.01f, 0.0, 1.0, 2);
            changed |= ui.DragFloat("Montagnes", biome.mountains, 0.01f, 0.0, 1.0, 2);
            changed |= DrawRange(ui, "Altitude (% du relief)", biome.minAltitude, biome.maxAltitude, 0.5f, -100.0, 100.0, 1);
            changed |= DrawRange(ui, "Température (°C)", biome.minTemperature, biome.maxTemperature, 0.2f, -100.0, 100.0, 1);
            changed |= DrawRange(ui, "Humidité", biome.minHumidity, biome.maxHumidity, 0.01f, -1.0, 2.0, 2);
            ui.TextDisabled("Minimum, maximum. Une limite au bout de l'échelle ne limite rien. Le premier biome de la "
                            "planète qui convient prend le sol.");
        }
        if (ui.CollapsingHeader("Forêt")) {
            changed |= ui.DragFloat("Couverture", biome.cover, 0.01f, 0.0, 1.0, 2);
            ui.SetItemTooltip("Part du sol couverte de forêts");
            changed |= ui.DragFloat("Densité", biome.density, 0.01f, 0.0, 1.0, 2);
            ui.SetItemTooltip("Part des places occupée par un arbre dans une forêt (une place par espacement)");
            changed |= ui.DragFloat("Taille des arbres", biome.scale, 0.01f, 0.05, 20.0, 2);
            ui.SetItemTooltip("x la taille de l'espèce");
            changed |= ui.DragFloat("Espacement des arbres", biome.spacing, 0.01f, 0.25, 8.0, 2);
            ui.SetItemTooltip("x l'espacement de l'espèce");
        }
        if (ui.CollapsingHeader("Espèces")) {
            float total = 0.0f;
            for (const VegetationBiome::Share& share : biome.species) {
                total += share.share;
            }
            std::optional<std::size_t> removed;
            for (std::size_t s = 0; s < biome.species.size(); ++s) {
                VegetationBiome::Share& share = biome.species[s];
                ui.PushId(s);
                changed |= DrawAsset(engine, ui, "Espèce", AssetType::Species, share.species);
                changed |= ui.DragFloat("Part", share.share, 0.5f, 0.0, 1000.0, 0);
                ui.SetItemTooltip(std::format("{:.0f} % des arbres", total > 0.0f ? 100.0f * share.share / total : 0.0f));
                if (ui.Button("Modifier l'espèce")) {
                    if (const Project* project = engine.GetProject()) {
                        m_openNext = project->assetsDirectory / Platform::PathFromUtf8(share.species);
                    }
                }
                ui.SameLine();
                if (ui.Button("Retirer")) {
                    removed = s;
                }
                ui.Separator();
                ui.PopId();
            }
            if (removed) {
                biome.species.erase(biome.species.begin() + static_cast<std::ptrdiff_t>(*removed));
                changed = true;
            }
            if (biome.species.empty()) {
                ui.TextDisabled("Aucune espèce : pas d'arbres.");
            }
            ui.BeginDisabled(biome.species.size() >= VegetationBiome::MaxSpecies);
            if (ui.Button("Ajouter une espèce...", UI::Icon::Plus)) {
                // Choisie d'abord : une espèce sans fichier ne se relirait pas.
                if (std::optional<std::string> chosen = ChooseAsset(engine, AssetType::Species, "")) {
                    biome.species.push_back({std::move(*chosen), 50.0f});
                    changed = true;
                }
            }
            ui.EndDisabled();
            ui.SetItemTooltip(std::format("{} espèces au plus", VegetationBiome::MaxSpecies));
        }
        return changed;
    }

    bool VegetationPanel::DrawSpecies(Engine& engine, UI::Context& ui) {
        bool changed = false;
        TreeSpecies& species = m_species;
        if (ui.CollapsingHeader("Variantes")) {
            ui.TextDisabled("Chaque arbre prend une variante au hasard. Une planète en montre 8 au plus, tous biomes réunis.");
            std::optional<std::size_t> removed;
            for (std::size_t v = 0; v < species.variants.size(); ++v) {
                TreeSpecies::Variant& variant = species.variants[v];
                ui.PushId(v);
                ui.Text(std::format("Variante {}", v + 1));
                changed |= DrawAsset(engine, ui, "Près", AssetType::Model, variant.model);
                changed |= DrawAsset(engine, ui, "Simplifié", AssetType::Model, variant.lodModel);
                changed |= DrawAsset(engine, ui, "Loin", AssetType::Model, variant.farModel);
                ui.BeginDisabled(species.variants.size() == 1);
                if (ui.Button("Retirer la variante")) {
                    removed = v;
                }
                ui.EndDisabled();
                ui.Separator();
                ui.PopId();
            }
            if (removed) {
                species.variants.erase(species.variants.begin() + static_cast<std::ptrdiff_t>(*removed));
                changed = true;
            }
            if (ui.Button("Ajouter une variante", UI::Icon::Plus)) {
                species.variants.push_back(species.variants.back()); // ses modèles se changent ensuite
                changed = true;
            }
        }
        if (ui.CollapsingHeader("Forme")) {
            changed |= DrawRange(ui, "Taille (x le modèle)", species.minScale, species.maxScale, 0.01f, 0.05, 20.0, 2);
            ui.SetItemTooltip("Minimum, maximum : chaque arbre prend une taille entre les deux");
            changed |= ui.DragFloat("Pente maximale (°)", species.maxSlope, 0.5f, 0.0, 90.0, 1);
            changed |= ui.DragFloat("Espacement (m)", species.spacing, 0.1f, 1.0, 100.0, 1);
            ui.SetItemTooltip("Distance voulue entre deux arbres, arrondie à la case du semis (environ 6 m) ; deux "
                              "voisins restent à 40 % de l'espacement au moins");
            changed |= ui.DragFloat("Écart de teinte", species.tint, 0.005f, 0.0, 0.5, 3);
            ui.SetItemTooltip("D'un arbre à l'autre (0 : tous pareils)");
            changed |= ui.ColorEdit3("Couleur des cimes", species.canopyColor);
            ui.SetItemTooltip("Couleur des forêts vues de loin, sur le sol");
        }
        if (ui.CollapsingHeader("Collision")) {
            changed |= ui.DragFloat("Rayon du tronc (m)", species.trunkRadius, 0.01f, 0.0, 10.0, 2);
            ui.SetItemTooltip("À la taille du modèle (0 : pas de collision)");
            changed |= ui.DragFloat("Hauteur du tronc (m)", species.trunkHeight, 0.1f, 0.0, 200.0, 1);
            ui.SetItemTooltip("Du pied jusqu'où le tronc arrête un vaisseau ou un personnage, à la taille du modèle");
        }
        return changed;
    }

    void VegetationPanel::FileMoved(const std::filesystem::path& from, const std::filesystem::path& to,
                                    const std::string& oldReference, const std::string& newReference) {
        if (m_kind == Kind::None) {
            return;
        }
        if (const std::optional<std::filesystem::path> moved = MovedPath(m_path, from, to)) {
            Planetology::SetVegetationPreview(m_reference, {});
            m_path = *moved;
            ReplacePath(m_reference, oldReference, newReference);
            m_previewed.clear();
        }
        // Espèces désignées par le biome ouvert : comme le Contenu l'a fait dans le fichier, dans chaque pas.
        if (m_kind == Kind::Biome && m_error.empty()) {
            const VegetationBiome shown = m_biome;
            const auto replace = [&](std::string& text) {
                if (Read(text)) {
                    for (VegetationBiome::Share& share : m_biome.species) {
                        ReplacePath(share.species, oldReference, newReference);
                    }
                    text = Write();
                }
            };
            for (std::string& step : m_steps) {
                replace(step);
            }
            replace(m_saved);
            m_biome = shown;
            for (VegetationBiome::Share& share : m_biome.species) {
                ReplacePath(share.species, oldReference, newReference);
            }
        }
        std::error_code error;
        m_savedTime = std::filesystem::last_write_time(m_path, error);
        Preview(Write());
    }

    void VegetationPanel::FileDeleted(const std::filesystem::path& path) {
        if (m_kind != Kind::None && MovedPath(m_path, path, path)) {
            Close();
        }
    }
}
