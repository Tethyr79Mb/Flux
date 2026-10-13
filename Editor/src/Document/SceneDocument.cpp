#include "Document/SceneDocument.hpp"

#include <Flux/Core/Engine.hpp>
#include <Flux/Core/Log.hpp>
#include <Flux/Core/Version.hpp>
#include <Flux/Platform/Dialogs.hpp>
#include <Flux/Platform/Filesystem.hpp>
#include <Flux/Platform/Input.hpp>
#include <Flux/Platform/Window.hpp>
#include <Flux/Scene/Scene.hpp>
#include <Flux/Scene/SceneSerializer.hpp>
#include <Flux/UI/Context.hpp>

#include <format>
#include <system_error>

namespace FluxEditor {
    using namespace Flux;

    namespace {
        const Platform::FileType SceneFileType{"Scènes Flux", ".scene.json"};
        const Platform::FileType PrefabFileType{"Prefabs Flux", ".prefab.json"};

        std::string FileName(const std::filesystem::path& path) {
            return Platform::ToUtf8(path.filename());
        }

        bool IsPrefabFile(const std::filesystem::path& path) {
            return Platform::ToUtf8(path.filename()).ends_with(".prefab.json");
        }

        // Un prefab ouvert s'enregistre comme prefab : sa seule racine et ses descendants, identifiants gardés pour
        // que les exemplaires des scènes retrouvent leurs pièces.
        std::expected<void, std::string> WritePrefab(Engine& engine, const std::filesystem::path& path) {
            const Scene& scene = engine.GetScene();
            Entity root = NullEntity;
            int roots = 0;
            scene.ForEachRoot([&](Entity entity) {
                root = entity;
                ++roots;
            });
            if (roots != 1) {
                return std::unexpected(std::format("Un prefab a un seul objet racine (ici {}) : rangez les autres "
                                                   "dessous, ou enregistrez une scène.", roots));
            }
            const auto text = SceneSerializer::SavePrefab(scene, engine.GetComponents(), root, SceneSerializer::PrefabIds::Keep);
            if (!text) {
                return std::unexpected(text.error());
            }
            return SceneSerializer::WriteFile(path, *text);
        }
    }

    void SceneDocument::HandleShortcuts(Engine& engine) {
        // Raccourcis lus par la lettre de la touche (disposition du clavier), comme dans toutes les applications.
        // Muets pendant une saisie : Ctrl+S dans un champ texte ne doit pas enregistrer à moitié.
        const Input& input = engine.GetWindow().GetInput();
        if (!m_locked && !engine.GetUi().WantsKeyboard() && input.IsControlDown()) {
            if (input.WasLetterPressed('N')) {
                NewScene(engine);
            } else if (input.WasLetterPressed('O')) {
                Open(engine);
            } else if (input.WasLetterPressed('S')) {
                input.IsShiftDown() ? SaveAs(engine) : Save(engine);
            }
        }

    }

    void SceneDocument::DrawSceneMenuItems(UI::Context& ui, Engine& engine) {
        // Pendant le jeu, la scène est une copie jetable : rien ne s'ouvre ni ne s'enregistre.
        const bool enabled = !m_locked;
        if (ui.MenuItem("Nouvelle scène", "Ctrl+N", enabled)) {
            NewScene(engine);
        }
        if (ui.MenuItem("Ouvrir...", "Ctrl+O", enabled)) {
            Open(engine);
        }
        if (ui.MenuItem("Enregistrer", "Ctrl+S", enabled)) {
            Save(engine);
        }
        if (ui.MenuItem("Enregistrer sous...", "Ctrl+Maj+S", enabled)) {
            SaveAs(engine);
        }
    }

    void SceneDocument::DrawToolBarItems(UI::Context& ui, Engine& engine) {
        ui.BeginDisabled(m_locked);
        if (ui.MenuBarButton("##nouvelle scène", false, UI::Icon::Scene)) {
            NewScene(engine);
        }
        ui.SetItemTooltip("Nouvelle scène (Ctrl+N)");
        if (ui.MenuBarButton("##ouvrir", false, UI::Icon::Folder)) {
            Open(engine);
        }
        ui.SetItemTooltip("Ouvrir une scène (Ctrl+O)");
        if (ui.MenuBarButton("##enregistrer", false, UI::Icon::Save)) {
            Save(engine);
        }
        ui.SetItemTooltip(m_modified ? "Enregistrer la scène (Ctrl+S) : modifiée" : "Enregistrer la scène (Ctrl+S)");
        ui.EndDisabled();
    }

    void SceneDocument::DrawQuitMenuItem(UI::Context& ui, Engine& engine) {
        if (ui.MenuItem("Quitter", "Alt+F4", !m_locked) && ConfirmClose(engine)) {
            engine.RequestQuit();
        }
    }

    bool SceneDocument::ConfirmClose(Engine& engine) {
        return ConfirmDiscard(engine);
    }

    void SceneDocument::NewScene(Engine& engine) {
        if (!ConfirmDiscard(engine)) {
            return;
        }
        engine.GetScene().Clear();
        m_path.clear();
        m_modified = false;
        m_replaced = true;
    }

    void SceneDocument::Open(Engine& engine) {
        if (!ConfirmDiscard(engine)) {
            return;
        }
        if (const auto path = Platform::ShowOpenFileDialog(engine.GetWindow(), "Ouvrir une scène", SceneFileType,
                                                           DialogDirectory(engine))) {
            Load(engine, *path);
        }
    }

    void SceneDocument::OpenFile(Engine& engine, const std::filesystem::path& path) {
        if (!m_locked && ConfirmDiscard(engine)) {
            Load(engine, path);
        }
    }

    bool SceneDocument::Load(Engine& engine, const std::filesystem::path& path) {
        // En cas d'erreur, la scène en cours reste intacte (le chargement vérifie tout avant de la remplacer).
        if (auto loaded = SceneSerializer::Load(engine.GetScene(), engine.GetComponents(), path, engine.GetPrefabReader());
            !loaded) {
            Platform::ShowError(engine.GetWindow(), "Ouvrir une scène", loaded.error());
            return false;
        }
        m_path = path;
        m_modified = false;
        m_replaced = true;
        Log::Info("scène ouverte : {}", FileName(path));
        return true;
    }

    void SceneDocument::ForgetFile(const std::filesystem::path& path) {
        if (!m_path.empty() && m_path.lexically_normal() == path.lexically_normal()) {
            m_path.clear();
            m_modified = true;
        }
    }

    void SceneDocument::FileRenamed(const std::filesystem::path& from, const std::filesystem::path& to) {
        if (!m_path.empty() && m_path.lexically_normal() == from.lexically_normal()) {
            m_path = to;
        }
    }

    bool SceneDocument::Save(Engine& engine) {
        return m_path.empty() ? SaveAs(engine) : WriteTo(engine, m_path);
    }

    bool SceneDocument::SaveAs(Engine& engine) {
        const std::string name = m_path.empty() ? "Nouvelle.scene.json" : FileName(m_path);
        // Un prefab ouvert s'enregistre sous un autre nom de prefab, pas en scène.
        const bool prefab = IsPrefabFile(m_path);
        const auto path = Platform::ShowSaveFileDialog(engine.GetWindow(), prefab ? "Enregistrer le prefab" : "Enregistrer la scène",
                                                       prefab ? PrefabFileType : SceneFileType, DialogDirectory(engine), name);
        return path && WriteTo(engine, *path);
    }

    bool SceneDocument::WriteTo(Engine& engine, const std::filesystem::path& path) {
        const auto saved = IsPrefabFile(path) ? WritePrefab(engine, path)
                                              : SceneSerializer::Save(engine.GetScene(), engine.GetComponents(), path,
                                                                      engine.GetPrefabReader());
        if (!saved) {
            Platform::ShowError(engine.GetWindow(), "Enregistrer la scène", saved.error());
            return false;
        }
        m_path = path;
        m_modified = false;
        Log::Info("scène enregistrée : {}", FileName(path));
        return true;
    }

    bool SceneDocument::ConfirmDiscard(Engine& engine) {
        if (!m_modified) {
            return true;
        }
        const std::string name = m_path.empty() ? "la scène sans titre" : "« " + FileName(m_path) + " »";
        switch (Platform::AskYesNoCancel(engine.GetWindow(), "Flux Editor",
                                         "Enregistrer les modifications de " + name + " ?")) {
            case Platform::Answer::Yes:
                return Save(engine);
            case Platform::Answer::No:
                return true;
            case Platform::Answer::Cancel:
                break;
        }
        return false;
    }

    void SceneDocument::UpdateTitle(Engine& engine) {
        std::string title = m_path.empty() ? "Sans titre" : FileName(m_path);
        if (m_modified) {
            title += " *";
        }
        if (m_locked) {
            title += " [en jeu]";
        }
        if (const Project* project = engine.GetProject()) {
            title += " - " + project->name;
        }
        title += std::format(" - Flux Editor {}", GetFullVersion());
        if (title != m_title) {
            engine.GetWindow().SetTitle(title);
            m_title = std::move(title);
        }
    }

    std::filesystem::path SceneDocument::DialogDirectory(const Engine& engine) const {
        if (!m_path.empty()) {
            return m_path.parent_path();
        }
        // Les scènes du projet, sinon celles copiées à côté de l'exe.
        std::error_code error;
        if (const Project* project = engine.GetProject();
            project && std::filesystem::is_directory(project->assetsDirectory / "scenes", error)) {
            return project->assetsDirectory / "scenes";
        }
        return Platform::GetExecutableDirectory() / "assets" / "scenes";
    }
}
