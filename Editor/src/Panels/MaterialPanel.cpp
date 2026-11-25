#include "Panels/MaterialPanel.hpp"

#include "Panels/AssetPicker.hpp"

#include <Flux/Core/Engine.hpp>
#include <Flux/Platform/Dialogs.hpp>
#include <Flux/Platform/Filesystem.hpp>
#include <Flux/Platform/Window.hpp>
#include <Flux/Scene/SceneSerializer.hpp>
#include <Flux/UI/Context.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <optional>
#include <system_error>
#include <utility>

namespace FluxEditor {
    using namespace Flux;

    namespace {
        constexpr std::string_view Extension = ".fluxmat";
        constexpr std::string_view PanelTitle = "Matériau";
        constexpr auto CheckInterval = std::chrono::seconds(1); // fichier changé hors de l'éditeur

        // `path` est `from` ou dedans (dossier déplacé) : son nouveau chemin sous `to`.
        std::optional<std::filesystem::path> MovedPath(const std::filesystem::path& path, const std::filesystem::path& from,
                                                       const std::filesystem::path& to) {
            const std::filesystem::path relative = path.lexically_normal().lexically_relative(from.lexically_normal());
            if (relative.empty() || *relative.begin() == "..") {
                return std::nullopt;
            }
            return relative == "." ? to : to / relative;
        }
    }

    bool MaterialPanel::IsMaterial(const std::filesystem::path& path) {
        return Platform::ToUtf8(path.filename()).ends_with(Extension);
    }

    std::string MaterialPanel::NewMaterialText() {
        LayeredMaterialLayer layer;
        layer.name = "Aluminium brossé";
        const std::string folder = "Materiaux/Couches/Aluminium-brosse/";
        layer.colorImage = folder + "couleur.jpg";
        layer.normalImage = folder + "normales.jpg";
        layer.roughnessImage = folder + "rugosite.jpg";
        layer.metallicImage = folder + "metal.jpg";
        layer.heightImage = folder + "hauteur.jpg";
        layer.settings.tiling = 30.0f;
        return SerializeLayeredMaterial({{}, {layer}});
    }

    bool MaterialPanel::Read(const std::string& text) {
        auto material = ParseLayeredMaterial(text);
        if (!material) {
            m_error = material.error();
            return false;
        }
        m_material = std::move(*material);
        m_error.clear();
        return true;
    }

    void MaterialPanel::Open(Engine& engine, const std::filesystem::path& path) {
        if (IsOpen() && path.lexically_normal() == m_path.lexically_normal()) {
            m_focus = true;
            return;
        }
        if (!ConfirmDiscard(engine)) {
            return;
        }
        Close(engine);
        if (!engine.GetProject() || !IsMaterial(path)) {
            return;
        }
        m_path = path;
        Load(engine);
        m_focus = true;
    }

    bool MaterialPanel::Load(Engine& engine) {
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
        Preview(engine, m_saved);
        return true;
    }

    bool MaterialPanel::Save(Engine& engine) {
        if (!IsOpen() || !m_error.empty()) {
            return false;
        }
        Commit(engine);
        const std::string text = Write();
        if (auto written = SceneSerializer::WriteFile(m_path, text); !written) {
            Platform::ShowError(engine.GetWindow(), "Matériau", written.error());
            return false;
        }
        std::error_code error;
        m_savedTime = std::filesystem::last_write_time(m_path, error);
        m_saved = text;
        Preview(engine, text); // le fichier dit maintenant la même chose : plus d'aperçu
        return true;
    }

    void MaterialPanel::Close(Engine& engine) {
        if (IsOpen()) {
            engine.PreviewMaterial(m_path, {});
        }
        const bool placed = m_placed;
        *this = MaterialPanel{};
        m_placed = placed;
    }

    bool MaterialPanel::ConfirmDiscard(Engine& engine) {
        if (!IsModified()) {
            return true;
        }
        const Platform::Answer answer =
            Platform::AskYesNoCancel(engine.GetWindow(), "Matériau modifié",
                                     "Enregistrer les modifications de « " + Platform::ToUtf8(m_path.filename()) + " » ?");
        return answer == Platform::Answer::No || (answer == Platform::Answer::Yes && Save(engine));
    }

    bool MaterialPanel::ConfirmClose(Engine& engine) {
        if (!ConfirmDiscard(engine)) {
            return false;
        }
        Close(engine);
        return true;
    }

    void MaterialPanel::Preview(Engine& engine, const std::string& text) {
        if (text == m_previewed) {
            return;
        }
        m_previewed = text;
        // Identique au fichier : les modèles relisent le fichier.
        engine.PreviewMaterial(m_path, text == m_saved ? std::string() : text);
    }

    void MaterialPanel::Commit(Engine& engine) {
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
        Preview(engine, text);
    }

    void MaterialPanel::Undo(Engine& engine, bool redo) {
        Commit(engine);
        if (redo ? m_position + 1 >= m_steps.size() : m_position == 0) {
            return;
        }
        m_position = redo ? m_position + 1 : m_position - 1;
        Read(m_steps[m_position]);
        Preview(engine, m_steps[m_position]);
    }

    bool MaterialPanel::HandleShortcuts(Engine& engine) {
        const Input& input = engine.GetWindow().GetInput();
        if (!IsOpen() || engine.GetUi().WantsKeyboard() || !input.IsControlDown()) {
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
            Undo(engine, input.IsShiftDown());
            return true;
        }
        if (input.WasLetterPressed('Y')) {
            Undo(engine, true);
            return true;
        }
        return false;
    }

    void MaterialPanel::Update(Engine& engine) {
        m_hovered = false;
        if (!IsOpen()) {
            return;
        }
        UI::Context& ui = engine.GetUi();
        const auto now = std::chrono::steady_clock::now();
        // Fichier réécrit hors de l'éditeur : relu, s'il n'y a rien à perdre ici.
        if (now - m_lastCheck >= CheckInterval) {
            m_lastCheck = now;
            std::error_code error;
            const auto time = std::filesystem::last_write_time(m_path, error);
            if (!error && time != m_savedTime && !m_pending && !IsModified()) {
                Load(engine);
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
        ui.SetNextPanelIcon(UI::Icon::Image);
        if (ui.BeginPanel(PanelTitle, {std::max(316.0f, screenWidth - 752.0f), 8.0f}, 368.0f, 0.0f, &open)) {
            m_hovered = ui.IsPanelHovered();
            const bool modified = IsModified();
            ui.Text("Matériau : " + Platform::ToUtf8(m_path.filename()) + (modified ? " *" : ""));
            ui.BeginDisabled(!modified);
            if (ui.Button("##enregistrer", UI::Icon::Save)) {
                Save(engine);
            }
            ui.SetItemTooltip("Enregistrer (Ctrl+S)");
            ui.EndDisabled();
            ui.SameLine();
            ui.BeginDisabled(m_position == 0 && !m_pending);
            if (ui.Button("##annuler", UI::Icon::Undo)) {
                Undo(engine, false);
            }
            ui.SetItemTooltip("Annuler (Ctrl+Z, souris sur ce panneau)");
            ui.EndDisabled();
            ui.SameLine();
            ui.BeginDisabled(m_position + 1 >= m_steps.size());
            if (ui.Button("##rétablir", UI::Icon::Redo)) {
                Undo(engine, true);
            }
            ui.SetItemTooltip("Rétablir (Ctrl+Y)");
            ui.EndDisabled();
            ui.Separator();
            if (!m_error.empty()) {
                ui.TextDisabled("Fichier illisible : " + m_error + "\nLe corriger dans un éditeur de texte : il sera relu.");
            } else {
                changed = DrawMaterial(engine, ui);
            }
        }
        ui.EndPanel();

        if (changed) {
            m_pending = true;
        }
        // Tout le geste (un glisser) fait un seul pas d'annulation ; les modèles suivent au relâché (ils sont relus en
        // entier : les suivre pendant le glisser relancerait la lecture sans cesse).
        if (m_pending && !ui.IsInteracting()) {
            Commit(engine);
        }
        if (!open && ConfirmDiscard(engine)) {
            Close(engine);
        }
    }

    bool MaterialPanel::DrawImage(Engine& engine, UI::Context& ui, const char* label, std::string& value) {
        std::string text = value;
        bool browse = false;
        bool changed = ui.InputTextBrowse(label, text, browse);
        if (browse) {
            // Boîte de dialogue modale : la frame attend le choix.
            if (std::optional<std::string> chosen = ChooseAsset(engine, AssetType::Image, text)) {
                text = std::move(*chosen);
                changed = true;
            }
        }
        if (!changed || text == value) {
            return false;
        }
        value = std::move(text);
        return true;
    }

    bool MaterialPanel::DrawMaterial(Engine& engine, UI::Context& ui) {
        bool changed = false;
        LayeredMaterial& material = m_material;
        ui.TextDisabled("Couches posées dans l'ordre sur le matériau du modèle, chacune par-dessus les précédentes. "
                        "Le matériau se branche sur un modèle dans l'inspecteur de son objet (« Matériaux du modèle »).");
        changed |= DrawImage(engine, ui, "Masque", material.maskImage);
        ui.SetItemTooltip("Image dans les UV du modèle, cuite dans Blender : un canal par couche (rouge, vert, bleu, "
                          "alpha). Blanc : couche posée, noir : absente.");
        std::optional<std::size_t> removed;
        std::optional<std::size_t> raised;
        for (std::size_t l = 0; l < material.layers.size(); ++l) {
            LayeredMaterialLayer& layer = material.layers[l];
            ui.PushId(l);
            const std::string title = std::format("{}. {}###couche", l + 1, layer.name.empty() ? std::string("Couche") : layer.name);
            if (ui.CollapsingHeader(title)) {
                changed |= DrawLayer(engine, ui, layer);
                ui.BeginDisabled(l == 0);
                if (ui.Button("Monter")) {
                    raised = l;
                }
                ui.SetItemTooltip("Posée avant la couche du dessus : celle-ci la recouvre");
                ui.EndDisabled();
                ui.SameLine();
                if (ui.Button("Retirer la couche")) {
                    removed = l;
                }
            }
            ui.PopId();
        }
        if (raised) {
            std::swap(material.layers[*raised], material.layers[*raised - 1]);
            changed = true;
        }
        if (removed) {
            material.layers.erase(material.layers.begin() + static_cast<std::ptrdiff_t>(*removed));
            changed = true;
        }
        ui.BeginDisabled(material.layers.size() >= MaxMaterialLayers);
        if (ui.Button("Ajouter une couche", UI::Icon::Plus)) {
            LayeredMaterialLayer layer;
            layer.name = "Couche";
            layer.settings.maskChannel = static_cast<int>(std::min<std::size_t>(material.layers.size(), 3));
            layer.settings.metallic = 0.0f;
            material.layers.push_back(std::move(layer));
            changed = true;
        }
        ui.EndDisabled();
        ui.SetItemTooltip(std::format("{} couches au plus", MaxMaterialLayers));
        return changed;
    }

    bool MaterialPanel::DrawLayer(Engine& engine, UI::Context& ui, LayeredMaterialLayer& layer) {
        bool changed = false;
        ModelMaterialLayer& s = layer.settings;
        changed |= ui.InputText("Nom", layer.name);
        // Canal du masque : « Partout » en premier, puis les quatre canaux.
        constexpr std::array<std::string_view, 5> Channels{"Partout", "Rouge du masque", "Vert du masque", "Bleu du masque",
                                                           "Alpha du masque"};
        int channel = s.maskChannel + 1;
        if (ui.Choice("Où", channel, Channels)) {
            s.maskChannel = channel - 1;
            changed = true;
        }
        ui.SetItemTooltip("Partout : la couche recouvre tout le matériau (avec son opacité)");
        if (ui.CollapsingHeader("Images", false)) {
            ui.TextDisabled("Telles que les donne la bibliothèque (Poly Haven, ambientCG) ; vide : sans.");
            changed |= DrawImage(engine, ui, "Couleur", layer.colorImage);
            changed |= DrawImage(engine, ui, "Normales", layer.normalImage);
            changed |= ui.Checkbox("Normales DirectX", layer.normalDirectX);
            ui.SetItemTooltip("Vert vers le bas (fichiers « NormalDX », « nor_dx ») ; décoché : OpenGL (« NormalGL »)");
            changed |= DrawImage(engine, ui, "Rugosité", layer.roughnessImage);
            changed |= DrawImage(engine, ui, "Métal", layer.metallicImage);
            changed |= DrawImage(engine, ui, "Hauteur", layer.heightImage);
            ui.SetItemTooltip("Relief de la matière (« Displacement ») : le bord de la couche le suit");
        }
        Math::Vector3f tint{s.tint.r, s.tint.g, s.tint.b};
        if (ui.DragFloat3("Teinte", tint, 0.01f, 0.0, 8.0, 2)) {
            s.tint = {tint.x, tint.y, tint.z, 1.0f};
            changed = true;
        }
        ui.SetItemTooltip("Multiplie la couleur (rouge, vert, bleu ; 1 : inchangée, plus : plus clair)");
        changed |= ui.DragFloat("Répétitions", s.tiling, 0.1f, 0.01, 1000.0, 1);
        ui.SetItemTooltip("Fois que l'image se répète sur les UV du modèle (plus : motif plus petit)");
        changed |= ui.DragFloat("Métal", s.metallic, 0.01f, 0.0, 1.0, 2);
        ui.SetItemTooltip("x l'image de métal s'il y en a une");
        changed |= ui.DragFloat("Rugosité", s.roughness, 0.01f, 0.0, 1.0, 2);
        ui.SetItemTooltip("x l'image de rugosité s'il y en a une");
        changed |= ui.DragFloat("Relief", s.normalScale, 0.01f, 0.0, 4.0, 2);
        ui.SetItemTooltip("Force des normales de la couche (0 : lisse)");
        changed |= ui.DragFloat("Opacité", s.opacity, 0.01f, 0.0, 1.0, 2);
        ui.BeginDisabled(s.maskChannel < 0);
        changed |= ui.DragFloat("Netteté du bord", s.sharpness, 0.1f, 1.0, 64.0, 1);
        ui.SetItemTooltip("1 : fondu sur tout le masque ; plus : bord franc");
        changed |= ui.DragFloat("Bord selon le relief", s.heightInfluence, 0.01f, 0.0, 2.0, 2);
        ui.SetItemTooltip("Part de l'image de hauteur dans le bord (la saleté s'accroche d'abord aux creux du grain)");
        ui.EndDisabled();
        return changed;
    }

    void MaterialPanel::FileMoved(Engine& engine, const std::filesystem::path& from, const std::filesystem::path& to) {
        if (!IsOpen()) {
            return;
        }
        if (const std::optional<std::filesystem::path> moved = MovedPath(m_path, from, to)) {
            engine.PreviewMaterial(m_path, {});
            m_path = *moved;
            m_previewed.clear();
            std::error_code error;
            m_savedTime = std::filesystem::last_write_time(m_path, error);
            Preview(engine, Write());
        }
    }

    void MaterialPanel::FileDeleted(Engine& engine, const std::filesystem::path& path) {
        if (IsOpen() && MovedPath(m_path, path, path)) {
            Close(engine);
        }
    }
}
