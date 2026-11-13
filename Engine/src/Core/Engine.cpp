#include "Flux/Core/Engine.hpp"

#include "Flux/Animation/Animator.hpp"
#include "Flux/Audio/AudioSystem.hpp"
#include "Flux/Core/JobSystem.hpp"
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
#include "Flux/Scene/SceneChange.hpp"
#include "Flux/Scene/SceneMerge.hpp"
#include "Flux/Scene/SceneSerializer.hpp"
#include "Flux/Scene/Orbits.hpp"
#include "Flux/Scene/Picking.hpp"
#include "Flux/Scene/Seats.hpp"
#include "Flux/Scene/Ships.hpp"
#include "Flux/Scene/Projectiles.hpp"
#include "Flux/Scene/ScreenUi.hpp"
#include "Flux/UI/Context.hpp"
#include "Flux/UI/DrawList.hpp"
#include "Core/EnginePanel.hpp"
#include "Core/Notice.hpp"
#include "Core/InterfaceBindings.hpp"
#include "Core/LoadingScreen.hpp"
#include "Core/LocalPlayerCommand.hpp"
#include "Core/Measurements.hpp"
#include "Core/PlayerCamera.hpp"
#include "Core/QuickSave.hpp"
#include "Core/TransformInterpolation.hpp"
#include "Interface/InterfaceLibrary.hpp"

#include <algorithm>
#include <cassert>
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
        // Éclats réservés d'avance : un combat chargé n'alloue pas en pleine image.
        constexpr std::size_t ReservedImpacts = 4096;
        // Durée de l'éclat d'un impact (s).
        constexpr float ImpactSeconds = 0.12f;
        // Explosion d'un missile : plus grande et plus longue qu'un impact d'obus (vif, puis pâlit comme lui).
        constexpr float ExplosionSize = 5.0f;
        constexpr float ExplosionSeconds = 0.3f;
    }

    Engine::Engine(const EngineConfig& config)
        : m_window(std::make_unique<Window>()),
          m_jobs(std::make_unique<JobSystem>()),
          m_showDebugOverlay(config.showDebugOverlay),
          m_drawScreenUi(config.drawScreenUi),
          m_freeCamera(config.freeCamera),
          m_audio(std::make_unique<AudioSystem>()),
          m_interfaces(std::make_unique<InterfaceLibrary>()),
          m_inputActions(std::make_unique<InputActions>(&m_simulation.GetActions())),
          m_interfaceBindings(std::make_unique<InterfaceBindings>()),
          m_playerCamera(std::make_unique<PlayerCamera>()),
          m_notice(std::make_unique<Notice>()),
          m_loadingScreen(std::make_unique<LoadingScreen>(config.loadingScreen)),
          m_quickSave(std::make_unique<QuickSave>()),
          m_frameStats(std::make_unique<FrameStats>()),
          m_stepInput(std::make_unique<Input>()),
          m_interpolation(std::make_unique<TransformInterpolation>()) {
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
        m_renderer->SetJobSystem(m_jobs.get());
        m_renderer->SetVSync(config.vsync);
        m_renderer->SetTemporalAA(config.temporalAA);
        m_renderer->SetUpscaling(config.upscaling);
        m_renderer->SetSharpness(config.sharpness);
        m_renderer->SetGroundDetail(config.groundDetail);
        m_renderer->SetCloudQuality(config.cloudQuality);
        m_renderer->SetVegetationDistance(config.vegetationDistance);
        m_renderer->SetBloom(config.bloom ? Renderer::DefaultBloom : 0.0f);
        // Les modèles sont chargés par le rendu : la simulation y lit la durée des animations des sièges.
        m_simulation.SetAnimationDurations([renderer = m_renderer.get()](const std::filesystem::path& model, const std::string& animation) {
            return renderer->GetAnimationDuration(model, animation);
        });
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
        // Texte lié : en pour cent (« 50 % »), comme dans les Paramètres de l'éditeur.
        m_interfaceBindings->SetEngineValue("Netteté FSR", "Graphismes", {.get = [this] { return static_cast<double>(m_renderer->GetSharpness()); },
                                             .set = [this](double value) { m_renderer->SetSharpness(static_cast<float>(value)); },
                                             .getText = [this] { return std::format("{:.0f} %", m_renderer->GetSharpness() * 100.0f); }});
        // Numéro dans GroundDetailLabels (0 Bas à 3 Ultra).
        m_interfaceBindings->SetEngineValue("Détail du sol", "Graphismes",
                                            {.get = [this] { return static_cast<double>(m_renderer->GetGroundDetail()); },
                                             .set = [this](double value) {
                                                 m_renderer->SetGroundDetail(static_cast<GroundDetail>(
                                                     std::clamp(std::lround(value), 0l, static_cast<long>(GroundDetailCount) - 1)));
                                             },
                                             .getText = {},
                                             .options = {GroundDetailLabels.begin(), GroundDetailLabels.end()}});
        // Numéro dans VegetationDistanceLabels (0 Bas à 3 Ultra).
        m_interfaceBindings->SetEngineValue("Distance de la végétation", "Graphismes",
                                            {.get = [this] { return static_cast<double>(m_renderer->GetVegetationDistance()); },
                                             .set = [this](double value) {
                                                 m_renderer->SetVegetationDistance(static_cast<VegetationDistance>(
                                                     std::clamp(std::lround(value), 0l, static_cast<long>(VegetationDistanceCount) - 1)));
                                             },
                                             .getText = {},
                                             .options = {VegetationDistanceLabels.begin(), VegetationDistanceLabels.end()}});
        // Numéro dans CloudQualityLabels (0 Bas à 2 Élevé).
        m_interfaceBindings->SetEngineValue("Qualité des nuages", "Graphismes",
                                            {.get = [this] { return static_cast<double>(m_renderer->GetCloudQuality()); },
                                             .set = [this](double value) {
                                                 m_renderer->SetCloudQuality(static_cast<CloudQuality>(
                                                     std::clamp(std::lround(value), 0l, static_cast<long>(CloudQualityCount) - 1)));
                                             },
                                             .getText = {},
                                             .options = {CloudQualityLabels.begin(), CloudQualityLabels.end()}});
        // Allumé ou coupé : l'intensité reste celle réglée (éditeur), ou celle par défaut si le bloom était coupé.
        m_interfaceBindings->SetEngineValue("Bloom", "Graphismes", {.get = [this] { return m_renderer->GetBloom() > 0.0f ? 1.0 : 0.0; },
                                    .set = [this](double value) {
                                        if ((value >= 0.5) != (m_renderer->GetBloom() > 0.0f)) {
                                            m_renderer->SetBloom(value >= 0.5 ? Renderer::DefaultBloom : 0.0f);
                                        }
                                    }});
        m_interfaceBindings->SetEngineValue("Volume général", "Son", {.get = [this] { return static_cast<double>(m_audio->GetMasterVolume()); },
                                             .set = [this](double value) {
                                                 m_audio->SetMasterVolume(static_cast<float>(std::clamp(value, 0.0, 1.0)));
                                             },
                                             .getText = [this] { return std::format("{:.0f} %", m_audio->GetMasterVolume() * 100.0f); }});
        m_interfaceBindings->SetEngineValue("Images par seconde", "Informations", {.get = [this] { return std::round(m_fps); }});

        // Actions du moteur (personnage, vaisseau), puis les touches choisies par le joueur. ZQSD en AZERTY : les
        // touches sont des positions sur le clavier (Key::W est la touche Z en AZERTY).
        InputActions& actions = *m_inputActions;
        // Celles du personnage sont déclarées par la simulation (même bit sur le serveur) : ici, leurs touches.
        m_actions.forward = actions.Add(std::string(CharacterActions::Forward), "Déplacement", Key::W, Key::Up);
        m_actions.back = actions.Add(std::string(CharacterActions::Back), "Déplacement", Key::S, Key::Down);
        m_actions.left = actions.Add(std::string(CharacterActions::Left), "Déplacement", Key::A, Key::Left);
        m_actions.right = actions.Add(std::string(CharacterActions::Right), "Déplacement", Key::D, Key::Right);
        actions.Add(std::string(CharacterActions::Run), "Déplacement", Key::LeftShift);
        actions.Add(std::string(CharacterActions::Jump), "Déplacement", Key::Space);
        actions.Add(std::string(CharacterActions::Interact), "Déplacement", Key::F);
        // Vue du cockpit ou de dehors : la caméra est au moteur ; les commandes du vaisseau sont au jeu (Simulation::SetPilotInput).
        m_actions.changeView = actions.Add("Changer de vue", "Vaisseau", Key::V);
        // F5 / F6 servent déjà à l'éditeur (jouer, pause).
        m_actions.quickSave = actions.Add("Sauvegarde rapide", "Partie", Key::F8);
        m_actions.quickLoad = actions.Add("Chargement rapide", "Partie", Key::F9);
        // Repartir du début au prochain lancement (la sauvegarde est sinon reprise, voir ResumeQuickSave).
        m_actions.deleteSave = actions.Add("Effacer la sauvegarde", "Partie", Key::F7);
        m_impacts.reserve(ReservedImpacts);
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
        assert(!m_inStep && "pendant un pas de simulation, lire GetPlayerCommand");
        return IsKeyboardFree() && m_inputActions->IsDown(GetCurrentInput(), name);
    }

    bool Engine::WasActionPressed(std::string_view name) const {
        assert(!m_inStep && "pendant un pas de simulation, lire GetPlayerCommand");
        return IsKeyboardFree() && m_inputActions->WasPressed(GetCurrentInput(), name);
    }

    bool Engine::IsKeyboardFree() const {
        return !m_keyCaptured && m_rebindingAction.empty() && !m_ui->WantsKeyboard() && !m_interfaceWantsKeyboard;
    }

    const PlayerCommand& Engine::GetPlayerCommand(Entity player) const {
        return m_simulation.GetPlayerCommand(player);
    }

    void Engine::SampleLocalCommand() {
        LocalView view;
        if (GetFollowedPlayer() != NullEntity) {
            view.walkForward = m_playerCamera->GetWalkForward();
            view.walkRight = m_playerCamera->GetWalkRight();
            view.stickYaw = m_playerCamera->GetSteerYaw();
            view.stickPitch = m_playerCamera->GetSteerPitch();
        }
        view.position = m_camera.position;
        view.forward = m_camera.GetForward();
        m_simulation.SetLocalCommand(Flux::SampleLocalCommand(*m_stepInput, *m_inputActions, IsKeyboardFree(), view,
                                                              {m_actions.forward, m_actions.back, m_actions.left, m_actions.right},
                                                              m_simulation.GetClock().GetStep()));
    }

    const Input& Engine::GetCurrentInput() const {
        return m_inStep ? *m_stepInput : m_window->GetInput();
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

        FrameStats& stats = *m_frameStats;
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
            stats.Add(PrepareSkyCost, render.prepareSky);
            stats.Add(PrepareEntitiesCost, render.prepareEntities);
            stats.Add(PrepareGroundCost, render.prepareGround);
            stats.Add(PrepareTreesCost, render.prepareTrees);
            stats.Add(PrepareUploadsCost, render.prepareUploads);
            stats.Add(WaitGpuCost, render.waitGpu);
            stats.Add(WaitImageCost, render.waitImage + render.present);
            stats.AddGpu(render.gpu);
            if (automaticMeasure > 0.0 && !m_loadingScreen->IsActive()) {
                measures.Start(std::exchange(automaticMeasure, 0.0));
            }
            if (measures.Add(frameMilliseconds,
                             {updateMilliseconds, gameMilliseconds, render.prepare, render.prepareSky, render.prepareEntities,
                              render.prepareGround, render.prepareTrees, render.prepareUploads, render.record,
                              render.waitGpu, render.waitImage + render.present},
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
                    .vegetationDistance = m_renderer->GetVegetationDistance(),
                    .occlusionCulling = m_renderer->IsOcclusionCullingEnabled(),
                    .groundGeometryOnly = m_renderer->IsGroundGeometryOnly(),
                    .groundMeshThreads = render.groundMeshThreads,
                    .modelMeshShaders = render.modelMeshShaders,
                    .rayTracing = render.rayTracing};
                if (auto written = measures.Write(Platform::GetExecutableDirectory() / "Mesures", context)) {
                    Log::Info("Mesures écrites dans {}", Platform::ToGenericUtf8(*written));
                    m_notice->Show("Mesures écrites dans Mesures/" + Platform::ToGenericUtf8(written->filename()));
                } else {
                    Log::Error("Mesures : {}", written.error());
                }
            }
            gameMilliseconds = -1.0;

            // Interfaces du jeu : la souris qu'elles ont retenue à l'image précédente n'ira pas au jeu dans celle-ci.
            ++m_frame;
            m_interfaceWantsMouse = std::exchange(m_interfaceMouseNow, false);
            m_interfaceWantsKeyboard = std::exchange(m_interfaceKeyboardNow, false);
            m_interfacePaused = std::exchange(m_interfacePausedNow, false);
            m_interfaceCovered = std::exchange(m_interfaceCoversNow, false);
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
            // Pas fixes de simulation (SimulationClock) : 0 à 4 dans cette image selon le temps écoulé. Les touches
            // retenues depuis le dernier pas y servent une seule fois ; ce qui a bougé est ensuite dessiné entre les
            // deux derniers pas.
            m_stepInput->Accumulate(m_window->GetInput());
            if (m_gameState != GameState::Stopped && !m_loadingScreen->IsActive()) {
                // Pause : celle de l'éditeur (F6), ou un menu ouvert qui la demande.
                const bool running = m_gameState == GameState::Running && !m_interfacePaused;
                const FrameClock::time_point gameStart = FrameClock::now();
                m_interpolation->Restore(GetScene());
                const int steps = m_simulation.GetClock().Advance(running ? deltaSeconds : 0.0);
                for (int step = 0; step < steps; ++step) {
                    if (step == steps - 1) {
                        m_interpolation->BeginStep(GetScene());
                    }
                    SampleLocalCommand();
                    m_inStep = true;
                    UpdateGameSystems(SimulationClock::StepSeconds);
                    if (m_gameUpdate) {
                        m_gameUpdate(*this, SimulationClock::StepSeconds);
                    }
                    m_inStep = false;
                    m_stepInput->ClearEdges();
                }
                if (steps > 0) {
                    m_interpolation->EndStep(GetScene());
                }
                m_interpolation->Apply(GetScene(), m_simulation.GetClock().GetAlpha());
                stats.AddSteps(steps);
                if (!running) {
                    m_stepInput->ClearEdges(); // une touche de la pause ne part pas à la reprise
                }
                gameMilliseconds = Milliseconds(FrameClock::now() - gameStart);
                stats.Add(GameCost, gameMilliseconds);
            } else {
                m_stepInput->ClearEdges();
            }
            // Après la physique et le jeu : la caméra voit le personnage là où il est dessiné.
            if (const Entity player = GetFollowedPlayer(); player != NullEntity) {
                m_playerCamera->Place(*this, player, deltaSeconds);
            } else {
                m_playerCamera->Reset();
            }
            // Sons des entités, à la caméra de cette image (qui suit le joueur ou vole librement).
            m_audio->SetAssetsDirectory(GetProject() ? GetProject()->assetsDirectory : std::filesystem::path{});
            m_audio->Update(GetScene(), m_camera, m_gameState != GameState::Stopped,
                            m_gameState == GameState::Paused || m_interfacePaused || m_loadingScreen->IsActive(), deltaSeconds);
            if (m_editorUpdate) {
                m_editorUpdate(*this, deltaSeconds);
            }
            Orbits::Update(GetScene()); // après l'inspecteur : une orbite changée place la planète dès cette image
            // Après les mises à jour (la scène de cette frame), sous les panneaux (ajoutés par EndFrame).
            // HUD sous les interfaces de la scène (menus, inventaire) ; rien sous un menu opaque plein écran, connu à
            // l'image précédente (les interfaces sont dessinées après).
            if (const Entity player = GetFollowedPlayer(); player != NullEntity && m_drawScreenUi && !m_interfaceCovered) {
                // Aussi en pause (infos de vol, destinations), avec le temps du jeu : arrêté, les messages attendent.
                RunHudSystems(m_gameState == GameState::Running && !m_interfacePaused ? deltaSeconds : 0.0);
            }
            if (m_drawScreenUi) {
                ScreenUi::Draw(GetScene(), m_renderer->GetOverlay(), {}, {static_cast<float>(size.x), static_cast<float>(size.y)},
                               GetScreenUiOptions());
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
            if (m_notice->IsShown() && m_drawScreenUi) {
                m_notice->Draw(m_renderer->GetOverlay(), {static_cast<float>(size.x), static_cast<float>(size.y)},
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

            m_renderer->SetAssetsDirectory(GetProject() ? GetProject()->assetsDirectory : std::filesystem::path{});
            m_renderer->SetThrusterPreview(m_gameState == GameState::Stopped); // éditeur, jeu arrêté
            // Menu opaque plein écran (dessiné plus haut, à cette image) : la scène derrière ne coûte plus rien au GPU.
            m_renderer->SetSceneCovered(m_interfaceCoversNow);
            m_renderer->SetProjectiles(m_simulation.GetProjectiles(), m_impacts);
            if (!m_renderer->BeginFrame() || !m_renderer->Render(GetScene(), m_camera, m_playerCamera->GetHiddenPlayer())
                || !m_renderer->EndFrame()) {
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

    std::string Engine::GetStatsJson() const {
        return DescribeEngineStats(*m_renderer, *m_frameStats);
    }

    void Engine::RequestScene(std::string relativePath) {
        m_requestedScene = std::move(relativePath);
    }

    void Engine::UpdateGameSystems(double deltaSeconds) {
        if (m_gameState != GameState::Running || m_interfacePaused) {
            return; // en pause : ni minuterie ni clic
        }
        // Clic ou touche de confirmation, sauf s'ils visent l'interface (panneaux de l'éditeur, champ en saisie).
        const Input& input = GetCurrentInput();
        const bool confirmed = (input.WasMouseButtonPressed(MouseButton::Left) && !m_ui->WantsMouse() && !m_interfaceWantsMouse)
                            || ((input.WasKeyPressed(Key::Enter) || input.WasKeyPressed(Key::NumpadEnter)
                                 || input.WasKeyPressed(Key::Space))
                                && !m_ui->WantsKeyboard());
        if (std::optional<std::string> next = SceneChange::Update(GetScene(), deltaSeconds, confirmed)) {
            RequestScene(std::move(*next));
        }
        // Sauvegarde rapide : celle du joueur de cet écran, même sans personnage.
        if (!m_loadingScreen->IsActive()) { // pendant le chargement, la scène n'est pas encore celle que voit le joueur
            const PlayerCommand& command = m_simulation.GetPlayerCommand(m_simulation.GetPlayer());
            if (command.WasPressed(m_actions.quickSave)) {
                m_quickSave->RequestSave();
            }
            if (command.WasPressed(m_actions.quickLoad)) {
                m_quickSave->RequestLoad(false);
            }
            if (command.WasPressed(m_actions.deleteSave)) {
                DeleteQuickSave();
            }
        }

        // Vue du cockpit ou de dehors : la caméra est au moteur, la commande arrive avec celle du pas.
        const Entity player = m_simulation.GetPlayer();
        const Scene& scene = m_simulation.GetScene();
        if (const auto* character = player != NullEntity ? scene.TryGet<CharacterComponent>(player) : nullptr;
            character && character->seat != NullEntity && m_simulation.GetPlayerCommand(player).WasPressed(m_actions.changeView)) {
            m_playerCamera->ToggleCockpitView();
        }
        m_simulation.Step(deltaSeconds);
        // Éclats des projectiles arrivés pendant le pas : seulement à dessiner.
        Projectiles::UpdateImpacts(m_impacts, deltaSeconds);
        for (const ProjectileHit& hit : m_simulation.GetProjectileHits()) {
            m_impacts.push_back(hit.missile ? Impact{hit.position, hit.color, ExplosionSeconds, ExplosionSize}
                                            : Impact{hit.position, hit.color, ImpactSeconds});
        }
    }

    void Engine::SetProject(Project project) {
        m_simulation.SetProject(std::move(project));
    }

    std::expected<void, std::string> Engine::LoadScene(const std::filesystem::path& path) {
        if (auto loaded = SceneSerializer::Load(GetScene(), GetComponents(), path, GetPrefabReader()); !loaded) {
            return loaded;
        }
        m_quickSave->SetScene(path);
        m_scenePath = path;
        SceneLoaded();
        return {};
    }

    void Engine::SceneLoaded() {
        m_interpolation->Clear(); // entités de l'ancienne scène
        m_impacts.clear();
        m_simulation.SceneReplaced();
        if (m_gameState != GameState::Stopped) {
            m_simulation.Start(); // déjà lancée : SceneReplaced a préparé le nouveau monde physique
            m_loadingScreen->Begin();
        }
    }

    void Engine::SetGameState(GameState state) {
        const bool starting = m_gameState == GameState::Stopped && state != GameState::Stopped;
        m_gameState = state;
        if (starting) {
            m_simulation.Start(); // lancement du jeu (éditeur) : la scène est prête
            m_loadingScreen->Begin();
        }
        if (state == GameState::Stopped) {
            m_interpolation->Clear();
            m_impacts.clear();
            m_simulation.Stop();
            m_audio->StopAll();
            m_interfaces->ClearInstances(); // cases cochées, fenêtres déplacées : le prochain jeu repart du fichier
            m_interfaceBindings->ClearActions();
            m_interfacePaused = m_interfacePausedNow = m_interfaceCovered = m_interfaceCoversNow = false;
            m_quickSave->SetScene({}, {}); // la scène éditée reprend (SetPlayedScene au prochain lancement)
        }
    }

    Entity Engine::GetFollowedPlayer() const {
        return m_gameState != GameState::Stopped ? m_simulation.GetPlayer() : NullEntity;
    }

    void Engine::LoadRequestedScene() {
        std::optional<std::string> requested = std::exchange(m_requestedScene, std::nullopt);
        if (!requested || m_gameState == GameState::Stopped) {
            return; // demande faite juste avant l'arrêt du jeu (éditeur) : la scène éditée est déjà restaurée
        }
        if (!GetProject()) {
            Log::Warning("changement de scène sans projet ouvert ({})", *requested);
            return;
        }
        const std::filesystem::path path = GetProject()->assetsDirectory / Platform::PathFromUtf8(*requested);
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

    void Engine::SetPlayedScene(std::filesystem::path path, std::string text) {
        if (path.empty()) {
            text.clear();
        }
        m_quickSave->SetScene(std::move(path), std::move(text));
    }

    void Engine::DeleteQuickSave() {
        m_notice->Show(QuickSave::Delete() ? "Sauvegarde effacée : le prochain lancement repart du début"
                                                    : "Aucune sauvegarde rapide");
    }

    void Engine::RunQuickSave() {
        const QuickSave::Requests requests = m_quickSave->TakeRequests();
        if (m_gameState == GameState::Stopped) {
            return; // jeu arrêté dans l'éditeur à la même image : la scène éditée est déjà restaurée
        }
        if (requests.save) {
            if (auto saved = m_quickSave->Write(*this, m_simulation.GetPlayer()); !saved) {
                Log::Error("sauvegarde rapide : {}", saved.error());
                m_notice->Show("Sauvegarde impossible : " + saved.error());
            } else {
                Log::Info("partie sauvegardée dans {}", Platform::ToGenericUtf8(QuickSave::GetSceneFile()));
                m_notice->Show("Partie sauvegardée");
            }
        }
        if (requests.load) {
            QuickLoad(requests.resumes);
        }
    }

    void Engine::QuickLoad(bool resumes) {
        if (!QuickSave::Exists()) {
            m_notice->Show("Aucune sauvegarde rapide");
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
            m_notice->Show("Sauvegarde non chargée : " + loaded.error());
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
        m_quickSave->Restore(*this, m_simulation.GetPlayer());
        // Sauvegarde sans sa scène de départ (faite avant la fusion, ou dans une scène jamais enregistrée) : les
        // modifications de l'éditeur ne peuvent pas y entrer ; le dire, sinon elles semblent ignorées.
        m_notice->Show(merges    ? "Partie reprise dans la scène modifiée (F7 : effacer la sauvegarde)"
                                : !source ? "Partie reprise telle que sauvegardée, sans les modifications de l'éditeur "
                                            "(F7 : effacer la sauvegarde)"
                                : resumes ? "Partie reprise (F7 : effacer la sauvegarde)"
                                          : "Partie chargée");
        if (!source) {
            Log::Warning("sauvegarde rapide sans scène de départ : les modifications faites dans l'éditeur n'y sont pas "
                         "reprises (F7 l'efface)");
        }
    }

    std::expected<void, std::string> Engine::LoadMergedQuickSave(const std::filesystem::path& scenePath) {
        // Toutes lues avant la fusion : une erreur laisse la scène en cours intacte.
        Scene base;
        Scene saved;
        for (auto [scene, path] : {std::pair{&base, QuickSave::GetBaseSceneFile()}, std::pair{&saved, QuickSave::GetSceneFile()},
                                   std::pair{&GetScene(), scenePath}}) {
            if (auto loaded = SceneSerializer::Load(*scene, GetComponents(), path, GetPrefabReader()); !loaded) {
                return std::unexpected(std::format("{} : {}", Platform::ToGenericUtf8(path.filename()), loaded.error()));
            }
        }
        for (const std::string& problem : SceneMerge::ApplyChanges(GetScene(), base, saved, GetComponents())) {
            Log::Warning("reprise de la partie, non repris : {}", problem);
        }
        m_scenePath = scenePath;
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

    void Engine::BindInterfaceText(std::string name, std::function<std::string()> get, std::function<void(std::string)> set) {
        m_interfaceBindings->BindValue(std::move(name), {.get = {}, .set = {}, .getText = std::move(get), .setText = std::move(set)});
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
        if (stage == GameStage::Hud) {
            m_hudSystems.push_back({stage, std::move(name), std::move(function)});
            return;
        }
        // Les étapes d'un pas tournent dans la simulation ; le jeu reçoit encore le moteur.
        const Simulation::Stage simulationStage = stage == GameStage::Interaction     ? Simulation::Stage::Interaction
                                                : stage == GameStage::BeforePhysics ? Simulation::Stage::BeforePhysics
                                                : stage == GameStage::AfterPhysics  ? Simulation::Stage::AfterPhysics
                                                : stage == GameStage::Replication   ? Simulation::Stage::Replication
                                                                                    : Simulation::Stage::Reset;
        m_simulation.AddSystem(simulationStage, std::move(name),
                               [this, function = std::move(function)](Simulation&, double deltaSeconds) {
                                   if (function) {
                                       function(*this, deltaSeconds);
                                   }
                               });
    }

    void Engine::RemoveGameSystem(std::string_view name) {
        std::erase_if(m_hudSystems, [&](const GameSystem& system) { return system.name == name; });
        m_simulation.RemoveSystem(name);
    }

    void Engine::ClearGameSystems() {
        m_hudSystems.clear();
        m_simulation.ClearSystems();
        m_quickSave->ClearData();
    }

    void Engine::AddGameSaveData(std::string name, SaveFunction save, LoadFunction load) {
        m_quickSave->AddData(std::move(name), std::move(save), std::move(load));
    }

    void Engine::RemoveGameSaveData(std::string_view name) {
        m_quickSave->RemoveData(name);
    }

    void Engine::RunHudSystems(double deltaSeconds) {
        // Par indice : un système peut en ajouter ou en retirer un autre.
        for (std::size_t i = 0; i < m_hudSystems.size(); ++i) {
            if (m_hudSystems[i].function) {
                const UpdateFunction function = m_hudSystems[i].function;
                function(*this, deltaSeconds);
            }
        }
    }


    Math::Vector2f Engine::GetSteerPointer() const {
        return m_playerCamera->GetSteerPointer();
    }

    FlightState Engine::GetFlightState(Entity ship) {
        return m_simulation.GetFlightState(ship);
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
        if (!GetProject() || relativePath.empty()) {
            return std::nullopt;
        }
        return m_renderer->LoadTexture(GetProject()->assetsDirectory / Platform::PathFromUtf8(relativePath));
    }

    std::optional<UI::FontRef> Engine::LoadAssetFont(std::string_view relativePath) {
        if (!GetProject() || relativePath.empty()) {
            return std::nullopt;
        }
        return m_renderer->LoadFont(GetProject()->assetsDirectory / Platform::PathFromUtf8(relativePath));
    }

    const Interface::Document* Engine::LoadInterface(std::string_view relativePath) {
        if (!GetProject() || relativePath.empty()) {
            return nullptr;
        }
        return m_interfaces->Get(GetProject()->assetsDirectory, relativePath);
    }

    ScreenUi::DrawOptions Engine::GetScreenUiOptions() {
        return {.textures = [this](std::string_view path) { return LoadAssetTexture(path); },
                .interfaces = [this](const ScreenUi::InterfaceRequest& request) { return DrawInterface(request); },
                .fonts = [this](std::string_view path) { return LoadAssetFont(path); }};
    }

    std::optional<std::pair<Math::Vector2f, Math::Vector2f>> Engine::DrawInterface(const ScreenUi::InterfaceRequest& request) {
        return DrawInterfaceFile(request.entity, request.component.file, request.list, request.viewMin, request.viewMax,
                                 request.placeholders, request.needsRect);
    }

    std::optional<std::pair<Math::Vector2f, Math::Vector2f>> Engine::DrawInterfaceFile(Entity owner, const std::string& file,
                                                                                        UI::DrawList& list, Math::Vector2f viewMin,
                                                                                        Math::Vector2f viewMax, bool placeholders,
                                                                                        bool needsRect) {
        const Interface::Document* document = nullptr;
        Interface::DrawOptions options{.textures = [this](std::string_view path) { return LoadAssetTexture(path); },
                                       .placeholders = placeholders};
        if (m_gameState != GameState::Stopped && GetProject() && !file.empty()) {
            // En jeu : la copie de l'entité (NullEntity : celle du menu du projet), que la souris modifie.
            if (InterfaceLibrary::Instance* instance =
                    m_interfaces->GetInstance(GetProject()->assetsDirectory, owner, file, m_frame)) {
                Interface::ToggleWindowsByAction(
                    instance->document, [this](std::string_view action) { return WasActionPressed(action); }, !m_interfacePaused);
                const Input& input = m_window->GetInput();
                const Interface::Pointer pointer{.position = input.GetMousePosition(),
                                                 .down = input.IsMouseButtonDown(MouseButton::Left),
                                                 .pressed = input.WasMouseButtonPressed(MouseButton::Left),
                                                 .released = input.WasMouseButtonReleased(MouseButton::Left),
                                                 .enabled = !m_ui->WantsMouse() && !m_window->IsCursorLocked(),
                                                 // Une seule interface par Échap : la première dessinée qui s'en sert.
                                                 .escape = input.WasKeyPressed(Key::Escape) && !m_ui->WantsKeyboard()
                                                        && !m_escapeUsed,
                                                 .wheel = input.GetWheelDelta(),
                                                 .typed = m_ui->WantsKeyboard() ? std::string_view{} : input.GetTextInput(),
                                                 .backspace = input.WasKeyTyped(Key::Backspace) && !m_ui->WantsKeyboard(),
                                                 .enter = (input.WasKeyPressed(Key::Enter) || input.WasKeyPressed(Key::NumpadEnter))
                                                       && !m_ui->WantsKeyboard()};
                Interface::Interaction& interaction = instance->interaction;
                Interface::Update(instance->document, interaction, viewMin, viewMax, pointer);
                m_interfaceBindings->Sync(instance->document.roots, interaction.changed, *m_inputActions, m_rebindingAction);
                m_interfaceMouseNow = m_interfaceMouseNow || interaction.wantsMouse;
                m_interfaceKeyboardNow = m_interfaceKeyboardNow || interaction.wantsKeyboard;
                m_interfacePausedNow = m_interfacePausedNow || interaction.pausesGame;
                m_interfaceCoversNow = m_interfaceCoversNow || interaction.coversView;
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
            if (!placeholders) {
                return std::nullopt;
            }
            // Éditeur : un message à la place de l'interface, pour la voir et la sélectionner.
            const std::string label = file.empty() ? "Interface : aucun fichier" : "Interface introuvable : " + file;
            const float pixelSize = std::max(12.0f, 20.0f * Interface::GetScale(viewMin, viewMax));
            const Math::Vector2f size = list.MeasureText(label, pixelSize);
            const Math::Vector2f min = (viewMin + viewMax) * 0.5f - size * 0.5f;
            list.AddRect(min - Math::Vector2f{8.0f, 6.0f}, min + size + Math::Vector2f{8.0f, 6.0f},
                                 Math::LinearColor::FromSrgb8(90, 90, 100, 0.6f));
            list.AddText(min, label, pixelSize, Math::LinearColor::FromSrgb8(220, 220, 225));
            return std::pair{min - Math::Vector2f{8.0f, 6.0f}, min + size + Math::Vector2f{8.0f, 6.0f}};
        }
        Interface::Draw(*document, list, viewMin, viewMax, options);
        if (!needsRect) {
            return std::nullopt;
        }

        // Rectangle qui englobe les éléments du premier niveau : de quoi cliquer l'interface dans la vue.
        std::optional<std::pair<Math::Vector2f, Math::Vector2f>> bounds;
        for (const Interface::Placed& placed : Interface::Arrange(*document, viewMin, viewMax)) {
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
        const auto* model = GetScene().TryGet<ModelComponent>(entity);
        if (!GetProject() || !model || model->model.empty()) {
            return {};
        }
        return m_renderer->GetAnimationNames(GetProject()->assetsDirectory / Platform::PathFromUtf8(model->model));
    }

    std::optional<Math::Box3f> Engine::GetMeshBounds(Entity entity) {
        if (GetScene().TryGet<MeshRendererComponent>(entity)) {
            return Math::Box3f{};
        }
        if (const auto* model = GetScene().TryGet<ModelComponent>(entity)) {
            if (GetProject() && !model->model.empty()) {
                if (auto bounds = m_renderer->LoadModel(GetProject()->assetsDirectory / Platform::PathFromUtf8(model->model))) {
                    return bounds;
                }
            }
            return Math::Box3f{}; // pas (encore) de fichier : le cube unité, pour pouvoir le sélectionner quand même
        }
        // Invisibles en jeu, mais sélectionnables dans l'éditeur : un personnage sans modèle, un mur invisible.
        if (const auto* character = GetScene().TryGet<CharacterComponent>(entity)) {
            return Math::Box3f{{-character->radius, 0.0f, -character->radius}, {character->radius, character->height, character->radius}};
        }
        if (const auto* collider = GetScene().TryGet<ColliderComponent>(entity)) {
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
