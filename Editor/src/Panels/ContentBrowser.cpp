#include "Panels/ContentBrowser.hpp"

#include "Document/SceneDocument.hpp"

#include <Flux/Core/Engine.hpp>
#include <Flux/Platform/Dialogs.hpp>
#include <Flux/Platform/Filesystem.hpp>
#include <Flux/Platform/Window.hpp>
#include <Flux/UI/Context.hpp>

#include <algorithm>
#include <system_error>
#include <utility>

namespace FluxEditor {
    using namespace Flux;

    namespace {
        constexpr double ScanInterval = 2.0; // secondes : quelques dizaines de fichiers, lecture négligeable

        constexpr std::string_view SceneExtension = ".scene.json";

        bool IsScene(const std::filesystem::path& path) {
            return Platform::ToUtf8(path.filename()).ends_with(SceneExtension);
        }
    }

    void ContentBrowser::Update(Engine& engine, SceneDocument& document, double deltaSeconds) {
        const Project* project = engine.GetProject();
        const std::filesystem::path root = project ? project->assetsDirectory : std::filesystem::path{};
        m_sinceScan += deltaSeconds;
        if (root != m_root || m_sinceScan >= ScanInterval) {
            m_root = root;
            m_sinceScan = 0.0;
            Scan(root);
        }

        UI::Context& ui = engine.GetUi();
        const Math::Vector2i screen = engine.GetWindow().GetSize();
        // Position sans ancrage (l'éditeur l'ancre en bas) : à droite, sous le panneau « Démo » du jeu.
        const float top = 150.0f + ui.GetMenuBarHeight();
        if (ui.BeginPanel("Contenu", {std::max(316.0f, static_cast<float>(screen.x) - 368.0f), top}, 360.0f,
                          std::max(160.0f, static_cast<float>(screen.y) - top - 8.0f))) {
            if (!project) {
                ui.TextDisabled("Aucun projet ouvert.");
            } else if (m_entries.empty()) {
                ui.TextDisabled((m_scenes.empty() ? "Dossier des assets vide ou introuvable :\n" : "Aucun asset hors des scènes dans :\n")
                                + Platform::ToUtf8(root));
            } else {
                ui.TextDisabled("Clic droit pour les actions. Les scènes sont dans l'onglet Scènes.");
                ui.Separator();
                for (const Entry& entry : m_entries) {
                    DrawEntry(ui, engine, document, entry);
                }
            }
        }
        ui.EndPanel();
        DrawRenameWindow(engine, document);
    }

    void ContentBrowser::Scan(const std::filesystem::path& root) {
        // Récursif ; erreurs ignorées (fichier supprimé pendant la lecture, droits) : l'entrée manque, c'est tout.
        // Les scènes vont dans m_scenes, pas dans l'arbre ; un dossier qui ne contenait que des scènes disparaît.
        m_scenes.clear();
        const auto read = [this](const auto& self, const std::filesystem::path& directory) -> std::vector<Entry> {
            std::vector<Entry> entries;
            std::error_code error;
            for (auto it = std::filesystem::directory_iterator(directory, error);
                 !error && it != std::filesystem::directory_iterator(); it.increment(error)) {
                Entry entry;
                entry.path = it->path();
                entry.name = Platform::ToUtf8(entry.path.filename());
                entry.directory = it->is_directory(error);
                if (entry.directory) {
                    const std::size_t scenes = m_scenes.size();
                    entry.children = self(self, entry.path);
                    if (entry.children.empty() && m_scenes.size() > scenes) {
                        continue;
                    }
                } else if (IsScene(entry.path)) {
                    m_scenes.push_back(entry.path);
                    continue;
                }
                entries.push_back(std::move(entry));
            }
            // Dossiers d'abord, puis ordre alphabétique : le même d'une lecture à l'autre.
            std::ranges::sort(entries, [](const Entry& a, const Entry& b) {
                return a.directory != b.directory ? a.directory : a.name < b.name;
            });
            return entries;
        };
        m_entries = root.empty() ? std::vector<Entry>{} : read(read, root);
        std::ranges::sort(m_scenes, [](const std::filesystem::path& a, const std::filesystem::path& b) {
            return a.parent_path() != b.parent_path() ? a.parent_path() < b.parent_path() : a.filename() < b.filename();
        });
    }

    void ContentBrowser::DrawEntry(UI::Context& ui, Engine& engine, SceneDocument& document, const Entry& entry) {
        ui.PushId(entry.name);
        if (entry.directory) {
            const bool open = ui.TreeNode(entry.name + "/", false, entry.children.empty(), true).open;
            ShowContextMenu(ui, engine, document, entry);
            if (open) {
                for (const Entry& child : entry.children) {
                    DrawEntry(ui, engine, document, child);
                }
                ui.TreePop();
            }
        } else {
            ui.TreeNode(entry.name, false, true);
            ShowContextMenu(ui, engine, document, entry);
        }
        ui.PopId();
    }

    void ContentBrowser::ShowContextMenu(UI::Context& ui, Engine& engine, SceneDocument& document, const Entry& entry) {
        if (!ui.BeginContextMenu()) {
            return;
        }
        if (ui.MenuItem("Afficher dans l'explorateur")) {
            Platform::ShowInExplorer(entry.path);
        }
        // Les dossiers se suppriment dans l'explorateur : trop facile de perdre beaucoup d'un coup ici.
        if (!entry.directory && ui.MenuItem("Supprimer")) {
            Delete(engine, document, entry.path);
        }
        ui.EndMenu();
    }

    void ContentBrowser::ShowSceneMenu(UI::Context& ui, Engine& engine, SceneDocument& document,
                                       const std::filesystem::path& path) {
        if (!ui.BeginContextMenu()) {
            return;
        }
        if (ui.MenuItem("Ouvrir", {}, document.GetPath().lexically_normal() != path.lexically_normal())) {
            document.OpenFile(engine, path);
        }
        if (ui.MenuItem("Renommer...")) {
            const std::string name = Platform::ToUtf8(path.filename());
            m_renaming = path;
            m_newName = name.substr(0, name.size() - SceneExtension.size());
        }
        if (ui.MenuItem("Définir comme scène de démarrage", {}, !IsStartupScene(engine, path))) {
            SetStartupScene(engine, path);
        }
        if (ui.MenuItem("Afficher dans l'explorateur")) {
            Platform::ShowInExplorer(path);
        }
        if (ui.MenuItem("Supprimer")) {
            Delete(engine, document, path);
        }
        ui.EndMenu();
    }

    void ContentBrowser::Delete(Engine& engine, SceneDocument& document, const std::filesystem::path& path) {
        const Window& window = engine.GetWindow();
        const std::string name = Platform::ToUtf8(path.filename());
        if (IsStartupScene(engine, path)) {
            Platform::ShowError(window, "Supprimer", "« " + name + " » est la scène de démarrage du projet : "
                                                     "Game.exe ne pourrait plus démarrer.");
            return;
        }
        if (!Platform::AskYesNo(window, "Supprimer", "Envoyer « " + name + " » à la corbeille ?")) {
            return;
        }
        if (auto deleted = Platform::MoveToRecycleBin(path); !deleted) {
            Platform::ShowError(window, "Supprimer", deleted.error());
            return;
        }
        document.ForgetFile(path);
        m_sinceScan = ScanInterval; // relu à la prochaine frame
    }

    bool ContentBrowser::IsStartupScene(const Engine& engine, const std::filesystem::path& path) {
        const Project* project = engine.GetProject();
        return project && project->GetStartupScenePath().lexically_normal() == path.lexically_normal();
    }

    void ContentBrowser::SetStartupScene(Engine& engine, const std::filesystem::path& path) {
        Project* project = engine.GetProject();
        if (!project) {
            return;
        }
        // Chemin relatif au dossier des assets, comme dans le fichier : le projet sert aussi depuis bin/.
        const std::filesystem::path relative = path.lexically_normal().lexically_relative(project->assetsDirectory);
        if (relative.empty() || *relative.begin() == "..") {
            Platform::ShowError(engine.GetWindow(), "Scène de démarrage", "La scène doit être dans le dossier des assets.");
            return;
        }
        const std::filesystem::path previous = std::exchange(project->startupScene, relative);
        if (auto saved = ProjectFile::Save(*project); !saved) {
            project->startupScene = previous; // le fichier n'a pas changé : le projet en mémoire non plus
            Platform::ShowError(engine.GetWindow(), "Scène de démarrage", "Projet non enregistré : " + saved.error());
        }
    }

    void ContentBrowser::DrawRenameWindow(Engine& engine, SceneDocument& document) {
        if (m_renaming.empty()) {
            return;
        }
        UI::Context& ui = engine.GetUi();
        const Math::Vector2i screen = engine.GetWindow().GetSize();
        const float width = 400.0f;
        if (ui.BeginPanel("Renommer la scène", {(static_cast<float>(screen.x) - width) * 0.5f, static_cast<float>(screen.y) * 0.3f},
                          width)) {
            ui.TextDisabled(Platform::ToUtf8(m_renaming.filename()));
            // Entrée dans le champ valide aussi ; l'extension .scene.json est ajoutée d'office.
            const bool entered = ui.InputText("Nouveau nom", m_newName);
            ui.Separator();
            if ((ui.Button("Renommer") || entered) && Rename(engine, document, m_newName)) {
                m_renaming.clear();
            }
            ui.SameLine();
            if (ui.Button("Annuler")) {
                m_renaming.clear();
            }
        }
        ui.EndPanel();
    }

    bool ContentBrowser::Rename(Engine& engine, SceneDocument& document, const std::string& name) {
        const Window& window = engine.GetWindow();
        std::string fileName = name;
        if (fileName.ends_with(SceneExtension)) {
            fileName.resize(fileName.size() - SceneExtension.size());
        }
        // Caractères refusés par Windows dans un nom de fichier.
        if (fileName.empty() || fileName.find_first_of("\\/:*?\"<>|") != std::string::npos || fileName.back() == '.'
            || fileName.back() == ' ') {
            Platform::ShowError(window, "Renommer", "Nom invalide : il ne doit pas être vide ni contenir \\ / : * ? \" < > |");
            return false;
        }
        fileName += SceneExtension;
        const std::filesystem::path from = m_renaming;
        const std::filesystem::path to = from.parent_path() / Platform::PathFromUtf8(fileName);
        if (to.filename() == from.filename()) {
            return true; // rien à faire
        }
        std::error_code error;
        if (std::filesystem::exists(to, error)) {
            Platform::ShowError(window, "Renommer", "« " + fileName + " » existe déjà.");
            return false;
        }
        const bool startup = IsStartupScene(engine, from);
        std::filesystem::rename(from, to, error);
        if (error) {
            Platform::ShowError(window, "Renommer", "Renommage impossible : " + error.message());
            return false;
        }
        document.FileRenamed(from, to);
        if (startup) {
            SetStartupScene(engine, to); // sinon Game.exe chercherait l'ancien nom
        }
        m_sinceScan = ScanInterval; // relu à la prochaine frame
        return true;
    }
}
