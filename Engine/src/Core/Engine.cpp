#include "Flux/Core/Engine.hpp"

#include "Flux/Animation/Animator.hpp"
#include "Flux/Audio/AudioSystem.hpp"
#include "Flux/Core/Log.hpp"
#include "Flux/Core/Version.hpp"
#include "Flux/Input/InputActions.hpp"
#include "Flux/Interface/Interface.hpp"
#include "Flux/Physics/PhysicsContext.hpp"
#include "Flux/Physics/PhysicsWorld.hpp"
#include "Flux/Platform/Console.hpp"
#include "Flux/Platform/Dialogs.hpp"
#include "Flux/Platform/Environment.hpp"
#include "Flux/Platform/Filesystem.hpp"
#include "Flux/Platform/Input.hpp"
#include "Flux/Platform/Memory.hpp"
#include "Flux/Platform/Window.hpp"
#include "Flux/Renderer/Renderer.hpp"
#include "Flux/Scene/Docking.hpp"
#include "Flux/Scene/SceneChange.hpp"
#include "Flux/Scene/SceneMerge.hpp"
#include "Flux/Scene/SceneSerializer.hpp"
#include "Flux/Scene/Orbits.hpp"
#include "Flux/Scene/Picking.hpp"
#include "Flux/Scene/Seats.hpp"
#include "Flux/Scene/Ships.hpp"
#include "Flux/Scene/Warp.hpp"
#include "Flux/Scene/Weapons.hpp"
#include "Flux/Scene/ScreenUi.hpp"
#include "Flux/UI/Context.hpp"
#include "Flux/UI/DrawList.hpp"
#include "Core/EnginePanel.hpp"
#include "Core/FlightHud.hpp"
#include "Core/InterfaceBindings.hpp"
#include "Core/LoadingScreen.hpp"
#include "Core/Measurements.hpp"
#include "Core/PlayerCamera.hpp"
#include "Core/QuickSave.hpp"
#include "Interface/InterfaceLibrary.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Flux {
    namespace {
        std::filesystem::path InputActionsFile() {
            return Platform::GetExecutableDirectory() / "Touches.json";
        }
        // Projectiles réservés d'avance : un combat chargé n'alloue pas en pleine image.
        constexpr std::size_t ReservedProjectiles = 4096;
        // Durée de l'éclat d'un impact (s).
        constexpr float ImpactSeconds = 0.12f;
        // Explosion d'un missile : plus grande et plus longue qu'un impact d'obus (vif, puis pâlit comme lui).
        constexpr float ExplosionSize = 5.0f;
        constexpr float ExplosionSeconds = 0.3f;
    }

    Engine::Engine(const EngineConfig& config)
        : m_window(std::make_unique<Window>()),
          m_showDebugOverlay(config.showDebugOverlay),
          m_drawScreenUi(config.drawScreenUi),
          m_freeCamera(config.freeCamera),
          m_physicsContext(std::make_shared<PhysicsContext>()),
          m_audio(std::make_unique<AudioSystem>()),
          m_interfaces(std::make_unique<InterfaceLibrary>()),
          m_inputActions(std::make_unique<InputActions>()),
          m_interfaceBindings(std::make_unique<InterfaceBindings>()),
          m_playerCamera(std::make_unique<PlayerCamera>()),
          m_flightHud(std::make_unique<FlightHud>()),
          m_loadingScreen(std::make_unique<LoadingScreen>(config.loadingScreen)),
          m_quickSave(std::make_unique<QuickSave>()) {
        RegisterEngineComponents(m_components);
        Log::Info("Flux {}", GetFullVersion());
        if (!m_window->Create(config.title, config.windowSize, config.maximized, config.fullscreen)) {
            std::fputs("Flux : impossible de créer la fenêtre\n", stderr);
            m_quitRequested = true;
            return;
        }

        auto renderer = Renderer::Create(*m_window);
        if (!renderer) {
            Log::Error("{}", renderer.error());
            m_quitRequested = true;
            return;
        }
        m_renderer = std::move(renderer.value());
        m_renderer->SetVSync(config.vsync);
        m_renderer->SetTemporalAA(config.temporalAA);
        m_renderer->SetUpscaling(config.upscaling);
        m_renderer->SetSharpness(config.sharpness);
        m_renderer->SetGroundDetail(config.groundDetail);
        m_renderer->SetCloudQuality(config.cloudQuality);
        m_audio->SetMasterVolume(std::clamp(config.volume, 0.0f, 1.0f));
        m_ui = std::make_unique<UI::Context>(m_renderer->GetOverlay().GetFont());
        m_ui->SetColorPicker([window = m_window.get()](Math::LinearColor& color) {
            const std::optional<Math::LinearColor> picked = Platform::ShowColorDialog(*window, color);
            if (picked) {
                color = *picked;
            }
            return picked.has_value();
        });

        // Valeurs du moteur que les interfaces peuvent lier (menu Options du jeu, affichage des images/s).
        m_interfaceBindings->SetEngineValue("V-Sync", "Graphismes", {.get = [this] { return m_renderer->IsVSyncEnabled() ? 1.0 : 0.0; },
                                    .set = [this](double value) {
                                        if ((value >= 0.5) != m_renderer->IsVSyncEnabled()) {
                                            m_renderer->SetVSync(value >= 0.5); // recrée la chaîne d'images : pas à chaque image
                                        }
                                    }});
        m_interfaceBindings->SetEngineValue("Anticrénelage", "Graphismes", {.get = [this] { return m_renderer->IsTemporalAAEnabled() ? 1.0 : 0.0; },
                                           .set = [this](double value) { m_renderer->SetTemporalAA(value >= 0.5); }});
        // Numéro dans UpscalingLabels (0 Natif à 5 FSR Ultra Performance).
        m_interfaceBindings->SetEngineValue("Mise à l'échelle", "Graphismes",
                                            {.get = [this] { return static_cast<double>(m_renderer->GetUpscaling()); },
                                             .set = [this](double value) {
                                                 m_renderer->SetUpscaling(static_cast<Upscaling>(
                                                     std::clamp(std::lround(value), 0l, static_cast<long>(UpscalingCount) - 1)));
                                             },
                                             .getText = {},
                                             .options = {UpscalingLabels.begin(), UpscalingLabels.end()}});
        m_interfaceBindings->SetEngineValue("Netteté FSR", "Graphismes", {.get = [this] { return static_cast<double>(m_renderer->GetSharpness()); },
                                             .set = [this](double value) { m_renderer->SetSharpness(static_cast<float>(value)); }});
        // Numéro dans GroundDetailLabels (0 Bas à 3 Ultra).
        m_interfaceBindings->SetEngineValue("Détail du sol", "Graphismes",
                                            {.get = [this] { return static_cast<double>(m_renderer->GetGroundDetail()); },
                                             .set = [this](double value) {
                                                 m_renderer->SetGroundDetail(static_cast<GroundDetail>(
                                                     std::clamp(std::lround(value), 0l, static_cast<long>(GroundDetailCount) - 1)));
                                             },
                                             .getText = {},
                                             .options = {GroundDetailLabels.begin(), GroundDetailLabels.end()}});
        // Numéro dans CloudQualityLabels (0 Bas à 2 Élevé).
        m_interfaceBindings->SetEngineValue("Qualité des nuages", "Graphismes",
                                            {.get = [this] { return static_cast<double>(m_renderer->GetCloudQuality()); },
                                             .set = [this](double value) {
                                                 m_renderer->SetCloudQuality(static_cast<CloudQuality>(
                                                     std::clamp(std::lround(value), 0l, static_cast<long>(CloudQualityCount) - 1)));
                                             },
                                             .getText = {},
                                             .options = {CloudQualityLabels.begin(), CloudQualityLabels.end()}});
        m_interfaceBindings->SetEngineValue("Volume général", "Son", {.get = [this] { return static_cast<double>(m_audio->GetMasterVolume()); },
                                             .set = [this](double value) {
                                                 m_audio->SetMasterVolume(static_cast<float>(std::clamp(value, 0.0, 1.0)));
                                             }});
        m_interfaceBindings->SetEngineValue("Images par seconde", "Informations", {.get = [this] { return std::round(m_fps); }});

        // Actions du moteur (personnage, vaisseau), puis les touches choisies par le joueur. ZQSD en AZERTY : les
        // touches sont des positions sur le clavier (Key::W est la touche Z en AZERTY).
        InputActions& actions = *m_inputActions;
        actions.Add("Avancer", "Déplacement", Key::W, Key::Up);
        actions.Add("Reculer", "Déplacement", Key::S, Key::Down);
        actions.Add("Aller à gauche", "Déplacement", Key::A, Key::Left);
        actions.Add("Aller à droite", "Déplacement", Key::D, Key::Right);
        actions.Add("Courir", "Déplacement", Key::LeftShift);
        actions.Add("Sauter", "Déplacement", Key::Space);
        actions.Add("Interagir", "Déplacement", Key::F);
        // Comme Star Citizen : Espace monte, Ctrl descend, X freine ; Maj boost et Tab mode voyage, comme X4.
        actions.Add("Freiner", "Vaisseau", Key::X);
        actions.Add("Boost", "Vaisseau", Key::LeftShift);
        actions.Add("Mode voyage", "Vaisseau", Key::Tab);
        actions.Add("Monter", "Vaisseau", Key::Space);
        actions.Add("Descendre", "Vaisseau", Key::LeftControl);
        actions.Add("Rouler à gauche", "Vaisseau", Key::Q);
        actions.Add("Rouler à droite", "Vaisseau", Key::E);
        actions.Add("Changer de vue", "Vaisseau", Key::V);
        actions.Add("Stabiliser à plat", "Vaisseau", Key::H);
        // Près d'une station : se faire conduire au quai libre le plus proche ; amarré : repartir (choix de Bruno).
        actions.Add("Pilote automatique", "Vaisseau", Key::L);
        // Vers la destination que vise le nez (station, planète, objectif de mission) ; en warp : le couper.
        actions.Add("Warp", "Vaisseau", Key::J);
        // F5 / F6 servent déjà à l'éditeur (jouer, pause).
        actions.Add("Sauvegarde rapide", "Partie", Key::F8);
        actions.Add("Chargement rapide", "Partie", Key::F9);
        // Repartir du début au prochain lancement (la sauvegarde est sinon reprise, voir ResumeQuickSave).
        actions.Add("Effacer la sauvegarde", "Partie", Key::F7);
        m_projectiles.reserve(ReservedProjectiles);
        m_projectileHits.reserve(ReservedProjectiles);
        m_impacts.reserve(ReservedProjectiles);
        if (std::ifstream file(InputActionsFile(), std::ios::binary); file) {
            const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            if (auto loaded = actions.LoadFromString(text); !loaded) {
                Log::Warning("Touches.json : {}", loaded.error());
            }
        }

        // Un peu en arrière et au-dessus de la grille de test, légèrement vers le bas.
        m_camera.position.offset = {0.0, 6.0, 30.0};
        m_camera.pitch = -0.2f;
    }

    bool Engine::IsActionDown(std::string_view name) const {
        return !m_keyCaptured && m_rebindingAction.empty() && !m_ui->WantsKeyboard()
            && m_inputActions->IsDown(m_window->GetInput(), name);
    }

    bool Engine::WasActionPressed(std::string_view name) const {
        return !m_keyCaptured && m_rebindingAction.empty() && !m_ui->WantsKeyboard()
            && m_inputActions->WasPressed(m_window->GetInput(), name);
    }

    void Engine::StartKeyRebinding(std::string action) {
        if (m_inputActions->Find(action)) {
            m_rebindingAction = std::move(action);
        } else {
            Log::Warning("changer une touche : aucune action « {} »", action);
        }
    }

    void Engine::ResetInputActions() {
        m_rebindingAction.clear();
        m_inputActions->ResetToDefaults();
        SaveInputActions();
    }

    void Engine::CaptureKeyRebinding() {
        const Key key = m_window->GetInput().GetFirstPressedKey();
        if (key == Key::Unknown) {
            return;
        }
        if (key != Key::Escape) { // Échap annule
            m_inputActions->SetKey(m_rebindingAction, 0, key);
            SaveInputActions();
        }
        m_rebindingAction.clear();
        m_keyCaptured = true;
        m_escapeUsed = true; // ni le jeu ni les interfaces ne voient cette touche
    }

    void Engine::SaveInputActions() const {
        std::ofstream file(InputActionsFile(), std::ios::binary | std::ios::trunc);
        file << m_inputActions->SaveToString();
        if (!file) {
            Log::Error("Touches.json : impossible d'écrire {}", Platform::ToGenericUtf8(InputActionsFile()));
        }
    }

    // Défini ici, où Renderer est complet, pour que unique_ptr<Renderer> puisse le détruire.
    Engine::~Engine() = default;

    int Engine::Run() {
        if (m_quitRequested) {
            return 1;
        }

        FrameStats stats;
        MeasureRecorder measures;
        // Temps du CPU de l'image précédente pour le relevé (négatif : pas mesuré, jeu arrêté).
        double updateMilliseconds = -1.0;
        double gameMilliseconds = -1.0;
        // FLUX_MESURES=secondes : un relevé dès la fin du chargement (essais automatiques, sans clic).
        double automaticMeasure = 0.0;
        if (const std::optional<std::string> seconds = Platform::ReadEnvironment("FLUX_MESURES")) {
            automaticMeasure = std::max(std::atof(seconds->c_str()), 0.0);
        }
        FrameClock::time_point previousFrame = FrameClock::now();
        while (!m_quitRequested && !Platform::ConsoleCloseRequested() && m_window->IsOpen()) {
            m_window->HandleEvents();
            if (m_window->ConsumeCloseRequest() && (!m_closeRequestHandler || m_closeRequestHandler(*this))) {
                break;
            }
            if (!m_window->IsOpen()) {
                break;
            }
            if (const Input& keys = m_window->GetInput();
                keys.WasKeyPressed(Key::F11)
                || ((keys.WasKeyPressed(Key::Enter) || keys.WasKeyPressed(Key::NumpadEnter))
                    && (keys.IsKeyDown(Key::LeftAlt) || keys.IsKeyDown(Key::RightAlt)))) {
                m_window->SetFullscreen(!m_window->IsFullscreen());
            }

            const Math::Vector2i size = m_window->GetSize();
            if (size.x == 0 || size.y == 0) {
                m_window->WaitEvents(); // fenêtre réduite : on dort jusqu'au prochain événement
                continue;
            }

            const FrameClock::time_point now = FrameClock::now();
            // Plafonné : après un blocage (déplacement de fenêtre, point d'arrêt), la caméra ne fait pas un bond.
            const double frameMilliseconds = Milliseconds(now - previousFrame);
            const double deltaSeconds = std::min(frameMilliseconds * 0.001, 0.1);
            previousFrame = now;
            stats.Tick(now);
            m_fps = stats.GetFps();
            const RenderTimings& render = m_renderer->GetTimings();
            stats.Add(PrepareCost, render.prepare);
            stats.Add(RecordCost, render.record);
            stats.Add(WaitGpuCost, render.waitGpu);
            stats.Add(WaitImageCost, render.waitImage + render.present);
            stats.AddGpu(render.gpu);
            if (automaticMeasure > 0.0 && !m_loadingScreen->IsActive()) {
                measures.Start(std::exchange(automaticMeasure, 0.0));
            }
            if (measures.Add(frameMilliseconds,
                             {updateMilliseconds, gameMilliseconds, render.prepare, render.record, render.waitGpu,
                              render.waitImage + render.present},
                             render)) {
                const Math::Vector2i view = m_window->GetSize();
                const MeasureRecorder::Context context{
                    .device = m_renderer->GetDeviceName(),
                    .camera = std::format("secteur ({}, {}, {}), position ({:.1f}, {:.1f}, {:.1f}), lacet {:.3f}, tangage {:.3f}",
                                          m_camera.position.sector.x, m_camera.position.sector.y, m_camera.position.sector.z,
                                          m_camera.position.offset.x, m_camera.position.offset.y, m_camera.position.offset.z,
                                          m_camera.yaw, m_camera.pitch),
                    .width = static_cast<std::uint32_t>(view.x),
                    .height = static_cast<std::uint32_t>(view.y),
                    .vsync = m_renderer->IsVSyncEnabled(),
                    .temporalAA = m_renderer->IsTemporalAAEnabled(),
                    .upscaling = m_renderer->IsUpscalingAvailable() ? m_renderer->GetUpscaling() : Upscaling::Native,
                    .renderSize = m_renderer->GetRenderSize(),
                    .groundDetail = m_renderer->GetGroundDetail(),
                    .cloudQuality = m_renderer->GetCloudQuality(),
                    .occlusionCulling = m_renderer->IsOcclusionCullingEnabled(),
                    .groundGeometryOnly = m_renderer->IsGroundGeometryOnly(),
                    .groundMeshThreads = render.groundMeshThreads,
                    .modelMeshShaders = render.modelMeshShaders};
                if (auto written = measures.Write(Platform::GetExecutableDirectory() / "Mesures", context)) {
                    Log::Info("Mesures écrites dans {}", Platform::ToGenericUtf8(*written));
                    m_flightHud->ShowNotice("Mesures écrites dans Mesures/" + Platform::ToGenericUtf8(written->filename()));
                } else {
                    Log::Error("Mesures : {}", written.error());
                }
            }
            gameMilliseconds = -1.0;

            // Interfaces du jeu : la souris qu'elles ont retenue à l'image précédente n'ira pas au jeu dans celle-ci.
            ++m_frame;
            m_interfaceWantsMouse = std::exchange(m_interfaceMouseNow, false);
            m_interfacePaused = std::exchange(m_interfacePausedNow, false);
            m_escapeUsed = false;
            m_keyCaptured = false;
            if (!m_rebindingAction.empty()) {
                CaptureKeyRebinding();
            }
            m_interfaces->ForgetInstancesBefore(m_frame - 1);
            // Souris ignorée par l'UI pendant le vol libre : le curseur est caché et immobile.
            m_ui->BeginFrame(m_window->GetInput(), {static_cast<float>(size.x), static_cast<float>(size.y)}, deltaSeconds,
                             !m_window->IsCursorLocked());
            // Pendant le jeu, la caméra suit le personnage du joueur s'il y en a un ; sinon, vol libre (éditeur seulement).
            if (GetFollowedPlayer() != NullEntity) {
                m_playerCamera->HandleInput(*this, GetFollowedPlayer());
            } else {
                m_playerCamera->StopRotating();
                if (m_freeCamera) {
                    m_camera.roll = 0.0f; // vol libre : l'horizon reste droit, même après la vue cockpit
                    m_cameraController.Update(m_camera, *m_window, deltaSeconds,
                                              m_cameraInputEnabled && !m_ui->WantsMouse() && !m_interfaceWantsMouse,
                                              m_cameraInputEnabled && !m_ui->WantsKeyboard());
                } else {
                    m_window->SetCursorLocked(false); // personnage retiré pendant un clic droit
                }
            }
            if (m_gameState != GameState::Stopped && !m_loadingScreen->IsActive()) {
                // Pause : celle de l'éditeur (F6), ou un menu ouvert qui la demande.
                const double gameDelta = m_gameState == GameState::Running && !m_interfacePaused ? deltaSeconds : 0.0;
                const FrameClock::time_point gameStart = FrameClock::now();
                UpdateGameSystems(gameDelta);
                if (m_gameUpdate) {
                    m_gameUpdate(*this, gameDelta);
                }
                gameMilliseconds = Milliseconds(FrameClock::now() - gameStart);
                stats.Add(GameCost, gameMilliseconds);
            }
            // Après la physique et le jeu : la caméra voit le personnage là où il est dessiné.
            if (const Entity player = GetFollowedPlayer(); player != NullEntity) {
                m_playerCamera->Place(*this, player, deltaSeconds);
            } else {
                m_playerCamera->Reset();
            }
            // Sons des entités, à la caméra de cette image (qui suit le joueur ou vole librement).
            m_audio->SetAssetsDirectory(m_project ? m_project->assetsDirectory : std::filesystem::path{});
            m_audio->Update(m_scene, m_camera, m_gameState != GameState::Stopped,
                            m_gameState == GameState::Paused || m_interfacePaused || m_loadingScreen->IsActive(), deltaSeconds);
            if (m_editorUpdate) {
                m_editorUpdate(*this, deltaSeconds);
            }
            Orbits::Update(m_scene); // après l'inspecteur : une orbite changée place la planète dès cette image
            // Après les mises à jour (la scène de cette frame), sous les panneaux (ajoutés par EndFrame).
            if (m_drawScreenUi) {
                ScreenUi::Draw(m_scene, m_renderer->GetOverlay(), {}, {static_cast<float>(size.x), static_cast<float>(size.y)},
                               GetScreenUiOptions());
            }
            if (const Entity player = GetFollowedPlayer(); player != NullEntity && m_drawScreenUi) {
                const Math::Vector2f screen{static_cast<float>(size.x), static_cast<float>(size.y)};
                m_flightHud->DrawFlightInfo(m_scene, m_physics.get(), m_renderer->GetOverlay(), player, screen, deltaSeconds);
                DrawWarpInfo(player, screen);
                if (m_gameState == GameState::Running) {
                    RunGameSystems(GameStage::Hud, deltaSeconds);
                }
            }
            if (m_drawScreenUi) {
                // Version dans le coin bas droit : dit quelle compilation tourne sur une capture d'écran.
                UI::DrawList& list = m_renderer->GetOverlay();
                const std::string version(GetFullVersion());
                constexpr float VersionSize = 13.0f;
                const Math::Vector2f extent = list.MeasureText(version, VersionSize);
                list.AddText({static_cast<float>(size.x) - extent.x - 8.0f, static_cast<float>(size.y) - extent.y - 6.0f}, version,
                             VersionSize, {1.0f, 1.0f, 1.0f, 0.45f});
            }
            if (m_flightHud->HasNotice() && m_drawScreenUi) {
                m_flightHud->DrawNotice(m_renderer->GetOverlay(), {static_cast<float>(size.x), static_cast<float>(size.y)},
                                        deltaSeconds, m_loadingScreen->IsActive());
            }
            if (m_loadingScreen->IsActive()) { // par-dessus la scène
                m_loadingScreen->Draw(m_renderer->GetOverlay(), m_renderer->GetTimings(),
                                      {static_cast<float>(size.x), static_cast<float>(size.y)});
            }
            if (m_showDebugOverlay) {
                DrawEnginePanel(*m_ui, *m_renderer, stats, measures, m_camera, m_freeCamera ? &m_cameraController : nullptr);
            }
            if (measures.IsRecording()) {
                m_renderer->RequestDetailedTimings(); // heures du GPU même panneau fermé
            }
            m_ui->EndFrame(m_renderer->GetOverlay());
            m_window->SetMouseCursor(m_ui->GetMouseCursor()); // double flèche au bord d'un panneau
            updateMilliseconds = Milliseconds(FrameClock::now() - now);
            stats.Add(UpdateCost, updateMilliseconds);

            m_renderer->SetAssetsDirectory(m_project ? m_project->assetsDirectory : std::filesystem::path{});
            m_renderer->SetThrusterPreview(m_gameState == GameState::Stopped); // éditeur, jeu arrêté
            m_renderer->SetProjectiles(m_projectiles, m_impacts);
            if (!m_renderer->RenderFrame(m_scene, m_camera, m_playerCamera->GetHiddenPlayer())) {
                return 1;
            }
            m_loadingScreen->Update(m_renderer->GetTimings(), deltaSeconds);
            // Avant le changement de scène : une fonction peut en demander un.
            m_interfaceBindings->RunActions(*this, m_gameQuitHandler);
            LoadRequestedScene();
            RunQuickSave(); // après le changement de scène : F9 l'emporte sur une scène demandée à la même image
        }
        return 0;
    }

    void Engine::RequestScene(std::string relativePath) {
        m_requestedScene = std::move(relativePath);
    }

    void Engine::UpdateGameSystems(double deltaSeconds) {
        if (m_gameState != GameState::Running || m_interfacePaused) {
            return; // en pause : ni minuterie ni clic
        }
        // Clic ou touche de confirmation, sauf s'ils visent l'interface (panneaux de l'éditeur, champ en saisie).
        const Input& input = m_window->GetInput();
        const bool confirmed = (input.WasMouseButtonPressed(MouseButton::Left) && !m_ui->WantsMouse() && !m_interfaceWantsMouse)
                            || ((input.WasKeyPressed(Key::Enter) || input.WasKeyPressed(Key::NumpadEnter)
                                 || input.WasKeyPressed(Key::Space))
                                && !m_ui->WantsKeyboard());
        if (std::optional<std::string> next = SceneChange::Update(m_scene, deltaSeconds, confirmed)) {
            RequestScene(std::move(*next));
        }
        if (!m_loadingScreen->IsActive()) { // pendant le chargement, la scène n'est pas encore celle que voit le joueur
            if (WasActionPressed("Sauvegarde rapide")) {
                m_quickSave->RequestSave();
            }
            if (WasActionPressed("Chargement rapide")) {
                m_quickSave->RequestLoad(false);
            }
            if (WasActionPressed("Effacer la sauvegarde")) {
                DeleteQuickSave();
            }
        }

        if (!m_physics) {
            StartPhysics(); // scène changée sans LoadScene (par le code du jeu) : préparée maintenant
        }
        const Entity player = m_physics->GetPlayer();
        const auto* playerCharacter = m_scene.IsValid(player) ? m_scene.TryGet<CharacterComponent>(player) : nullptr;
        const bool seated = playerCharacter && playerCharacter->seat != NullEntity;
        const auto axis = [&](std::string_view positive, std::string_view negative) {
            return (IsActionDown(positive) ? 1.0f : 0.0f) - (IsActionDown(negative) ? 1.0f : 0.0f);
        };

        // Personnage du joueur, par rapport à la caméra ; assis, les mêmes actions pilotent le vaisseau (poste de
        // pilotage). Touches : actions d'entrée (ZQSD, Maj, Espace, F par défaut).
        CharacterInput character;
        // Systèmes du jeu d'abord : F près d'un donneur de missions sert à lui parler, pas à s'asseoir.
        m_interactionPending = WasActionPressed("Interagir");
        RunGameSystems(GameStage::Interaction, deltaSeconds);
        SeatInput seatInput{.interact = m_interactionPending};
        m_interactionPending = false;
        ShipInput shipInput;
        if (seated) {
            shipInput.thrust = axis("Avancer", "Reculer");
            shipInput.strafe = axis("Aller à droite", "Aller à gauche");
            shipInput.lift = axis("Monter", "Descendre");
            shipInput.roll = axis("Rouler à droite", "Rouler à gauche");
            shipInput.brake = IsActionDown("Freiner");
            shipInput.boost = IsActionDown("Boost");
            shipInput.travel = WasActionPressed("Mode voyage");
            shipInput.mouseYaw = m_playerCamera->GetSteerYaw();
            shipInput.mousePitch = m_playerCamera->GetSteerPitch();
            if (WasActionPressed("Changer de vue")) {
                m_playerCamera->ToggleCockpitView();
            }
            // Aide au pilotage : selon la gravité du dehors (planète, sol), rien dans l'espace.
            if (const Entity ship = Ships::GetPilotedShip(m_scene, player); ship != NullEntity) {
                ShipComponent& component = m_scene.Get<ShipComponent>(ship);
                if (WasActionPressed("Stabiliser à plat")) {
                    component.levelAssist = !component.levelAssist;
                    Log::Info("Stabilisation à plat : {}", component.levelAssist ? "oui" : "non");
                }
                if (const Math::Vector3f gravity = m_physics->GetOutsideGravity(m_scene, ship); Math::Length(gravity) > 0.01f) {
                    shipInput.up = Math::Normalize(gravity) * -1.0f;
                }
                const FlightHud::FlightState flight = m_flightHud->GetFlightState(m_scene, m_physics.get(), ship);
                shipInput.air = flight.air;
                if (flight.nearPlanet) {
                    shipInput.planetUp = Math::Vector3f(flight.up);
                    shipInput.planetDistance = static_cast<float>(flight.boundary);
                    shipInput.planetCenter = static_cast<float>(flight.center);
                }
            }
        } else {
            // Avant et droite de la caméra, ramenés à l'horizontale de l'endroit (sur une planète : tangente au sol).
            character.move = m_playerCamera->GetWalkForward() * axis("Avancer", "Reculer")
                           + m_playerCamera->GetWalkRight() * axis("Aller à droite", "Aller à gauche");
            character.run = IsActionDown("Courir");
            character.jump = WasActionPressed("Sauter");
        }
        Seats::Update(m_scene, player, deltaSeconds, seatInput, [this](Entity entity) {
            // Durées des animations « s'asseoir » et « se lever » de son modèle, à la vitesse de lecture.
            const auto* animator = m_scene.TryGet<AnimatorComponent>(entity);
            const auto* model = m_scene.TryGet<ModelComponent>(entity);
            if (!m_project || !animator || !model || model->model.empty() || animator->speed <= 0.0f) {
                return Seats::Durations{};
            }
            const std::filesystem::path path = m_project->assetsDirectory / Platform::PathFromUtf8(model->model);
            const auto duration = [&](const std::string& name) {
                return name.empty() ? 0.0f : m_renderer->GetAnimationDuration(path, name) / animator->speed;
            };
            return Seats::Durations{duration(animator->sitDownAnimation), duration(animator->standUpAnimation)};
        });
        const Entity pilotedShip = Ships::GetPilotedShip(m_scene, player);
        if (pilotedShip != NullEntity && WasActionPressed("Pilote automatique")) {
            Warp::Stop(m_scene, pilotedShip); // L en warp : il en sort et cherche un quai
            m_warpName.clear();
            switch (Docking::Toggle(m_scene, pilotedShip)) {
            case Docking::ToggleResult::Docking:
            case Docking::ToggleResult::Leaving: break; // l'étape s'affiche dans les infos de vol
            case Docking::ToggleResult::Cancelled: m_flightHud->ShowFlightMessage("Pilote automatique coupé"); break;
            case Docking::ToggleResult::NoDock:
                m_flightHud->ShowFlightMessage(std::format("Aucun quai libre à moins de {:.0f} km", Docking::Range / 1000.0));
                break;
            }
        }
        Ships::Update(m_scene, pilotedShip, deltaSeconds, shipInput);
        Docking::Update(m_scene, deltaSeconds); // après le pilote : il prend la main sous pilote automatique
        UpdateWarp(pilotedShip, pilotedShip != NullEntity && WasActionPressed("Warp"));
        Warp::Update(m_scene, deltaSeconds); // après le pilote, comme le pilote automatique
        RunGameSystems(GameStage::BeforePhysics, deltaSeconds); // IA de combat du jeu : comme Ships::Update
        m_physics->Update(m_scene, deltaSeconds, character);
        // Projectiles après la physique, avant les armes du jeu (étape AfterPhysics) : les vaisseaux sont à leur place
        // de cette image, les nouveaux tirs partent de la bouche des canons (avant, ils naîtraient en retard d'une
        // image : 16 m à 1 000 m/s). Les anciens avancent d'abord.
        m_projectileHits.clear();
        Weapons::GuideMissiles(m_scene, m_projectiles, deltaSeconds);
        Weapons::UpdateProjectiles(
            m_projectiles, deltaSeconds,
            [this](const Math::WorldPosition& from, const Math::Vector3f& segment, Entity ship) -> std::optional<std::pair<Entity, float>> {
                if (const auto hit = m_physics->CastRay(m_scene, from, segment, ship)) {
                    return std::pair{hit->entity, hit->fraction};
                }
                return std::nullopt;
            },
            &m_projectileHits);
        Weapons::UpdateImpacts(m_impacts, deltaSeconds);
        for (const ProjectileHit& hit : m_projectileHits) {
            m_impacts.push_back(hit.missile ? Impact{hit.position, hit.color, ExplosionSeconds, ExplosionSize}
                                            : Impact{hit.position, hit.color, ImpactSeconds});
        }
        m_destroyedNames.clear();
        Weapons::ApplyHits(m_scene, m_projectileHits, player, &m_destroyedNames);
        for (const std::string& name : m_destroyedNames) {
            Log::Info("« {} » détruit", name);
        }
        RunGameSystems(GameStage::AfterPhysics, deltaSeconds); // armes du jeu (joueur, ennemis)
        Animator::Update(m_scene, deltaSeconds); // après la physique : les pas du personnage de cette image
    }

    void Engine::StartPhysics() {
        m_physicsContext->SetAssetsDirectory(m_project ? m_project->assetsDirectory : std::filesystem::path{});
        m_physics = std::make_unique<PhysicsWorld>(m_physicsContext);
        m_physics->Prepare(m_scene);
    }

    void Engine::SetProject(Project project) {
        m_prefabReader = SceneSerializer::ReadPrefabsFrom(project.assetsDirectory);
        m_project = std::move(project);
    }

    std::expected<void, std::string> Engine::LoadScene(const std::filesystem::path& path) {
        if (auto loaded = SceneSerializer::Load(m_scene, m_components, path, GetPrefabReader()); !loaded) {
            return loaded;
        }
        m_quickSave->SetScene(path);
        SceneLoaded();
        return {};
    }

    void Engine::SceneLoaded() {
        m_physics.reset(); // les corps de l'ancienne scène
        m_projectiles.clear();
        m_impacts.clear();
        ResetGameState();
        if (m_gameState != GameState::Stopped) {
            StartPhysics();
            m_loadingScreen->Begin();
        }
    }

    void Engine::SetGameState(GameState state) {
        const bool starting = m_gameState == GameState::Stopped && state != GameState::Stopped;
        m_gameState = state;
        if (starting) {
            StartPhysics(); // lancement du jeu (éditeur) : la scène est prête
            m_loadingScreen->Begin();
        }
        if (state == GameState::Stopped) {
            m_physics.reset();
            m_projectiles.clear();
            m_impacts.clear();
            ResetGameState();
            m_audio->StopAll();
            m_interfaces->ClearInstances(); // cases cochées, fenêtres déplacées : le prochain jeu repart du fichier
            m_interfaceBindings->ClearActions();
            m_interfacePaused = m_interfacePausedNow = false;
            m_quickSave->SetScene({}, {}); // la scène éditée reprend, sans fichier (pas de fusion pour F8)
        }
    }

    Entity Engine::GetFollowedPlayer() const {
        if (m_gameState == GameState::Stopped || !m_physics) {
            return NullEntity;
        }
        const Entity player = m_physics->GetPlayer();
        return m_scene.IsValid(player) ? player : NullEntity;
    }

    void Engine::LoadRequestedScene() {
        std::optional<std::string> requested = std::exchange(m_requestedScene, std::nullopt);
        if (!requested || m_gameState == GameState::Stopped) {
            return; // demande faite juste avant l'arrêt du jeu (éditeur) : la scène éditée est déjà restaurée
        }
        if (!m_project) {
            Log::Warning("changement de scène sans projet ouvert ({})", *requested);
            return;
        }
        const std::filesystem::path path = m_project->assetsDirectory / Platform::PathFromUtf8(*requested);
        if (auto loaded = LoadScene(path); !loaded) {
            // Le jeu reste sur la scène en cours ; le joueur doit savoir pourquoi il ne change pas de scène.
            Log::Error("scène {} : {}", *requested, loaded.error());
            Platform::ShowError(GetWindow(), *requested, loaded.error());
            return;
        }
        m_audio->StopAll(); // ses sons aussi ; ceux de la nouvelle partent à la prochaine image
    }

    bool Engine::ResumeQuickSave() {
        if (!QuickSave::Exists()) {
            return false;
        }
        m_quickSave->RequestLoad(true); // à la fin de la première image, comme F9
        return true;
    }

    void Engine::DeleteQuickSave() {
        m_flightHud->ShowNotice(QuickSave::Delete() ? "Sauvegarde effacée : le prochain lancement repart du début"
                                                    : "Aucune sauvegarde rapide");
    }

    void Engine::RunQuickSave() {
        const QuickSave::Requests requests = m_quickSave->TakeRequests();
        if (m_gameState == GameState::Stopped) {
            return; // jeu arrêté dans l'éditeur à la même image : la scène éditée est déjà restaurée
        }
        if (requests.save) {
            if (auto saved = m_quickSave->Write(*this, m_physics ? m_physics->GetPlayer() : NullEntity); !saved) {
                Log::Error("sauvegarde rapide : {}", saved.error());
                m_flightHud->ShowNotice("Sauvegarde impossible : " + saved.error());
            } else {
                Log::Info("partie sauvegardée dans {}", Platform::ToGenericUtf8(QuickSave::GetSceneFile()));
                m_flightHud->ShowNotice("Partie sauvegardée");
            }
        }
        if (requests.load) {
            QuickLoad(requests.resumes);
        }
    }

    void Engine::QuickLoad(bool resumes) {
        if (!QuickSave::Exists()) {
            m_flightHud->ShowNotice("Aucune sauvegarde rapide");
            return;
        }
        // Scène modifiée dans l'éditeur depuis la sauvegarde (puis recopiée à côté du jeu par la génération) : la
        // scène actuelle, avec ce que la partie avait changé. La sauvegarde seule cacherait la modification.
        const std::optional<QuickSave::Source> source = QuickSave::ReadSource(GetProject());
        const bool merges = source && source->changed;
        if (auto loaded = merges ? LoadMergedQuickSave(source->path) : LoadScene(QuickSave::GetSceneFile()); !loaded) {
            Log::Error("chargement rapide : {}", loaded.error());
            // Sauvegarde d'une version du moteur qui ne la lit plus, par exemple : la partie continue sur la scène
            // en cours (rien n'a été touché).
            m_flightHud->ShowNotice("Sauvegarde non chargée : " + loaded.error());
            return;
        }
        // Une prochaine sauvegarde part de la même scène que celle-ci (pas du fichier de la sauvegarde).
        if (merges) {
            m_quickSave->SetScene(source->path);
        } else if (source) {
            m_quickSave->SetScene(source->path, source->text);
        } else {
            m_quickSave->SetScene({}, {});
        }
        m_audio->StopAll();
        m_playerCamera->Snap();
        m_quickSave->Restore(*this, m_physics ? m_physics->GetPlayer() : NullEntity);
        m_flightHud->ShowNotice(merges ? "Partie reprise dans la scène modifiée (F7 : effacer la sauvegarde)"
                                : resumes ? "Partie reprise (F7 : effacer la sauvegarde)"
                                          : "Partie chargée");
    }

    std::expected<void, std::string> Engine::LoadMergedQuickSave(const std::filesystem::path& scenePath) {
        // Toutes lues avant la fusion : une erreur laisse la scène en cours intacte.
        Scene base;
        Scene saved;
        for (auto [scene, path] : {std::pair{&base, QuickSave::GetBaseSceneFile()}, std::pair{&saved, QuickSave::GetSceneFile()},
                                   std::pair{&m_scene, scenePath}}) {
            if (auto loaded = SceneSerializer::Load(*scene, m_components, path, GetPrefabReader()); !loaded) {
                return std::unexpected(std::format("{} : {}", Platform::ToGenericUtf8(path.filename()), loaded.error()));
            }
        }
        for (const std::string& problem : SceneMerge::ApplyChanges(m_scene, base, saved, m_components)) {
            Log::Warning("reprise de la partie, non repris : {}", problem);
        }
        SceneLoaded();
        return {};
    }

    void Engine::RegisterInterfaceFunction(std::string name, InterfaceFunction function) {
        m_interfaceBindings->RegisterFunction(std::move(name), std::move(function));
    }

    void Engine::UnregisterInterfaceFunction(std::string_view name) {
        m_interfaceBindings->UnregisterFunction(name);
    }

    void Engine::ClearInterfaceFunctions() {
        m_interfaceBindings->ClearGame();
    }

    void Engine::BindInterfaceValue(std::string name, std::function<double()> get, std::function<void(double)> set) {
        m_interfaceBindings->BindValue(std::move(name), {.get = std::move(get), .set = std::move(set), .getText = {}});
    }

    void Engine::BindInterfaceText(std::string name, std::function<std::string()> get) {
        m_interfaceBindings->BindValue(std::move(name), {.get = {}, .set = {}, .getText = std::move(get)});
    }

    void Engine::BindInterfaceChoice(std::string name, std::vector<std::string> options, std::function<double()> get,
                                     std::function<void(double)> set) {
        m_interfaceBindings->BindValue(std::move(name),
                                       {.get = std::move(get), .set = std::move(set), .getText = {}, .options = std::move(options)});
    }

    void Engine::UnbindInterfaceValue(std::string_view name) {
        m_interfaceBindings->UnbindValue(name);
    }

    std::vector<Engine::InterfaceValueInfo> Engine::GetInterfaceValues() const {
        return m_interfaceBindings->GetValues(*m_inputActions);
    }

    bool Engine::SetInterfaceWindowOpen(std::string_view name, bool open) {
        return m_gameState != GameState::Stopped && m_interfaces->SetWindowOpen(name, open);
    }

    Interface::Element* Engine::FindInterfaceElement(std::string_view name) {
        return m_gameState != GameState::Stopped ? m_interfaces->FindInInstances(name) : nullptr;
    }

    void Engine::SetGameUpdate(UpdateFunction update) {
        m_gameUpdate = std::move(update);
    }

    void Engine::AddGameSystem(GameStage stage, std::string name, UpdateFunction function) {
        RemoveGameSystem(name);
        m_gameSystems.push_back({stage, std::move(name), std::move(function)});
    }

    void Engine::RemoveGameSystem(std::string_view name) {
        std::erase_if(m_gameSystems, [&](const GameSystem& system) { return system.name == name; });
    }

    void Engine::ClearGameSystems() {
        m_gameSystems.clear();
        m_quickSave->ClearData();
    }

    void Engine::AddGameSaveData(std::string name, SaveFunction save, LoadFunction load) {
        m_quickSave->AddData(std::move(name), std::move(save), std::move(load));
    }

    void Engine::RemoveGameSaveData(std::string_view name) {
        m_quickSave->RemoveData(name);
    }

    void Engine::RunGameSystems(GameStage stage, double deltaSeconds) {
        // Par indice : un système peut en ajouter ou en retirer un autre.
        for (std::size_t i = 0; i < m_gameSystems.size(); ++i) {
            if (m_gameSystems[i].stage == stage && m_gameSystems[i].function) {
                const UpdateFunction function = m_gameSystems[i].function;
                function(*this, deltaSeconds);
            }
        }
    }

    bool Engine::ConsumeInteraction() {
        return std::exchange(m_interactionPending, false);
    }

    void Engine::SetWarpObjective(std::string name, std::optional<Math::WorldPosition> position) {
        m_warpObjectiveName = position ? std::move(name) : std::string{};
        m_warpObjective = position;
    }

    void Engine::SetFlightInfoExtra(std::string_view text) {
        m_flightHud->SetExtra(text);
    }

    void Engine::ResetGameState() {
        m_warpDestinations.clear();
        m_warpAim = -1;
        m_warpName.clear();
        m_warpObjective.reset();
        m_warpObjectiveName.clear();
        m_flightHud->ClearExtra();
        RunGameSystems(GameStage::Reset, 0.0);
    }

    void Engine::SetEditorUpdate(UpdateFunction update) {
        m_editorUpdate = std::move(update);
    }

    void Engine::SetCloseRequestHandler(CloseFunction handler) {
        m_closeRequestHandler = std::move(handler);
    }

    void Engine::RequestQuit() {
        m_quitRequested = true;
    }

    Window& Engine::GetWindow() {
        return *m_window;
    }

    Renderer& Engine::GetRenderer() {
        return *m_renderer;
    }

    std::optional<UI::TextureInfo> Engine::LoadAssetTexture(std::string_view relativePath) {
        if (!m_project || relativePath.empty()) {
            return std::nullopt;
        }
        return m_renderer->LoadTexture(m_project->assetsDirectory / Platform::PathFromUtf8(relativePath));
    }

    std::optional<UI::FontRef> Engine::LoadAssetFont(std::string_view relativePath) {
        if (!m_project || relativePath.empty()) {
            return std::nullopt;
        }
        return m_renderer->LoadFont(m_project->assetsDirectory / Platform::PathFromUtf8(relativePath));
    }

    const Interface::Document* Engine::LoadInterface(std::string_view relativePath) {
        if (!m_project || relativePath.empty()) {
            return nullptr;
        }
        return m_interfaces->Get(m_project->assetsDirectory, relativePath);
    }

    ScreenUi::DrawOptions Engine::GetScreenUiOptions() {
        return {.textures = [this](std::string_view path) { return LoadAssetTexture(path); },
                .interfaces = [this](const ScreenUi::InterfaceRequest& request) { return DrawInterface(request); },
                .fonts = [this](std::string_view path) { return LoadAssetFont(path); }};
    }

    std::optional<std::pair<Math::Vector2f, Math::Vector2f>> Engine::DrawInterface(const ScreenUi::InterfaceRequest& request) {
        const std::string& file = request.component.file;
        const Interface::Document* document = nullptr;
        Interface::DrawOptions options{.textures = [this](std::string_view path) { return LoadAssetTexture(path); },
                                       .placeholders = request.placeholders};
        if (m_gameState != GameState::Stopped && m_project && !file.empty()) {
            // En jeu : la copie de l'entité, que la souris modifie.
            if (InterfaceLibrary::Instance* instance =
                    m_interfaces->GetInstance(m_project->assetsDirectory, request.entity, file, m_frame)) {
                const Input& input = m_window->GetInput();
                const Interface::Pointer pointer{.position = input.GetMousePosition(),
                                                 .down = input.IsMouseButtonDown(MouseButton::Left),
                                                 .pressed = input.WasMouseButtonPressed(MouseButton::Left),
                                                 .released = input.WasMouseButtonReleased(MouseButton::Left),
                                                 .enabled = !m_ui->WantsMouse() && !m_window->IsCursorLocked(),
                                                 // Une seule interface par Échap : la première dessinée qui s'en sert.
                                                 .escape = input.WasKeyPressed(Key::Escape) && !m_ui->WantsKeyboard()
                                                        && !m_escapeUsed,
                                                 .wheel = input.GetWheelDelta()};
                Interface::Interaction& interaction = instance->interaction;
                Interface::Update(instance->document, interaction, request.viewMin, request.viewMax, pointer);
                m_interfaceBindings->Sync(instance->document.roots, interaction.changed, *m_inputActions, m_rebindingAction);
                m_interfaceMouseNow = m_interfaceMouseNow || interaction.wantsMouse;
                m_interfacePausedNow = m_interfacePausedNow || interaction.pausesGame;
                m_escapeUsed = m_escapeUsed || interaction.escapeUsed;
                if (interaction.action != Interface::ButtonAction::None) {
                    // Pas pendant le dessin : la scène est en cours de parcours.
                    m_interfaceBindings->QueueAction(interaction.action, interaction.target);
                }
                options.interaction = &instance->interaction;
                document = &instance->document;
            }
        } else {
            document = LoadInterface(file);
        }

        if (!document) {
            if (!request.placeholders) {
                return std::nullopt;
            }
            // Éditeur : un message à la place de l'interface, pour la voir et la sélectionner.
            const std::string label = file.empty() ? "Interface : aucun fichier" : "Interface introuvable : " + file;
            const float pixelSize = std::max(12.0f, 20.0f * Interface::GetScale(request.viewMin, request.viewMax));
            const Math::Vector2f size = request.list.MeasureText(label, pixelSize);
            const Math::Vector2f min = (request.viewMin + request.viewMax) * 0.5f - size * 0.5f;
            request.list.AddRect(min - Math::Vector2f{8.0f, 6.0f}, min + size + Math::Vector2f{8.0f, 6.0f},
                                 Math::LinearColor::FromSrgb8(90, 90, 100, 0.6f));
            request.list.AddText(min, label, pixelSize, Math::LinearColor::FromSrgb8(220, 220, 225));
            return std::pair{min - Math::Vector2f{8.0f, 6.0f}, min + size + Math::Vector2f{8.0f, 6.0f}};
        }
        Interface::Draw(*document, request.list, request.viewMin, request.viewMax, options);
        if (!request.needsRect) {
            return std::nullopt;
        }

        // Rectangle qui englobe les éléments du premier niveau : de quoi cliquer l'interface dans la vue.
        std::optional<std::pair<Math::Vector2f, Math::Vector2f>> bounds;
        for (const Interface::Placed& placed : Interface::Arrange(*document, request.viewMin, request.viewMax)) {
            if (placed.depth != 0) {
                continue;
            }
            if (!bounds) {
                bounds = std::pair{placed.rect.min, placed.rect.max};
            } else {
                bounds->first = {std::min(bounds->first.x, placed.rect.min.x), std::min(bounds->first.y, placed.rect.min.y)};
                bounds->second = {std::max(bounds->second.x, placed.rect.max.x), std::max(bounds->second.y, placed.rect.max.y)};
            }
        }
        return bounds;
    }

    std::vector<std::string> Engine::GetAnimationNames(Entity entity) {
        const auto* model = m_scene.TryGet<ModelComponent>(entity);
        if (!m_project || !model || model->model.empty()) {
            return {};
        }
        return m_renderer->GetAnimationNames(m_project->assetsDirectory / Platform::PathFromUtf8(model->model));
    }

    std::optional<Math::Box3f> Engine::GetMeshBounds(Entity entity) {
        if (m_scene.TryGet<MeshRendererComponent>(entity)) {
            return Math::Box3f{};
        }
        if (const auto* model = m_scene.TryGet<ModelComponent>(entity)) {
            if (m_project && !model->model.empty()) {
                if (auto bounds = m_renderer->LoadModel(m_project->assetsDirectory / Platform::PathFromUtf8(model->model))) {
                    return bounds;
                }
            }
            return Math::Box3f{}; // pas (encore) de fichier : le cube unité, pour pouvoir le sélectionner quand même
        }
        // Invisibles en jeu, mais sélectionnables dans l'éditeur : un personnage sans modèle, un mur invisible.
        if (const auto* character = m_scene.TryGet<CharacterComponent>(entity)) {
            return Math::Box3f{{-character->radius, 0.0f, -character->radius}, {character->radius, character->height, character->radius}};
        }
        if (const auto* collider = m_scene.TryGet<ColliderComponent>(entity)) {
            Math::Vector3f half = collider->size * 0.5f;
            if (collider->shape == ColliderShape::Sphere) {
                half = {collider->radius, collider->radius, collider->radius};
            } else if (collider->shape == ColliderShape::Capsule) {
                half = {collider->radius, collider->height * 0.5f, collider->radius};
            }
            return Math::Box3f{collider->center - half, collider->center + half};
        }
        return std::nullopt;
    }

    bool Engine::IsLoading() const {
        return m_loadingScreen->IsActive();
    }

    UI::Context& Engine::GetUi() {
        return *m_ui;
    }
}
