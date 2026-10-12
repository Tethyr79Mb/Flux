#include "Flux/Core/Engine.hpp"

#include "Flux/Animation/Animator.hpp"
#include "Flux/Audio/AudioSystem.hpp"
#include "Flux/Core/Log.hpp"
#include "Flux/Core/Version.hpp"
#include "Flux/Graphics/Planet.hpp"
#include "Flux/Input/InputActions.hpp"
#include "Flux/Interface/Interface.hpp"
#include "Flux/Physics/PhysicsContext.hpp"
#include "Flux/Physics/PhysicsWorld.hpp"
#include "Flux/Platform/Console.hpp"
#include "Flux/Platform/Dialogs.hpp"
#include "Flux/Platform/Filesystem.hpp"
#include "Flux/Platform/Input.hpp"
#include "Flux/Platform/Memory.hpp"
#include "Flux/Platform/Window.hpp"
#include "Flux/Renderer/Renderer.hpp"
#include "Flux/Scene/SceneChange.hpp"
#include "Flux/Scene/SceneSerializer.hpp"
#include "Flux/Scene/Seats.hpp"
#include "Flux/Scene/ScreenUi.hpp"
#include "Flux/UI/Context.hpp"
#include "Flux/UI/DrawList.hpp"
#include "Interface/InterfaceLibrary.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <format>
#include <fstream>
#include <iterator>
#include <numbers>
#include <string>
#include <utility>

namespace Flux {
    namespace {
        // Liaison d'un texte ou d'un bouton aux touches d'une action d'entrée : « Touche : Avancer ».
        constexpr std::string_view KeyBindingPrefix = "Touche : ";

        std::filesystem::path InputActionsFile() {
            return Platform::GetExecutableDirectory() / "Touches.json";
        }
        using Clock = std::chrono::steady_clock;

        // Caméra qui suit le personnage : un peu moins que la verticale, au-dessus comme en dessous.
        constexpr float FollowMaxPitch = 1.3f;

        double Milliseconds(Clock::duration duration) {
            return std::chrono::duration<double, std::milli>(duration).count();
        }

        // Où passe le temps d'une image (millisecondes) : ce que montre le panneau « Moteur ».
        enum Cost : std::size_t {
            UpdateCost,       // CPU : fenêtre, interface, caméra, jeu, éditeur (avant le rendu)
            GameCost,         // CPU, compris dans le précédent : systèmes du jeu (physique, sièges, animation)
            PrepareCost,      // CPU : préparation du rendu
            RecordCost,       // CPU : commandes de dessin
            WaitGpuCost,      // CPU arrêté : attente du GPU
            WaitImageCost,    // CPU arrêté : attente d'une image libre et présentation (V-Sync, plafond)
            GpuShadowsCost,   // GPU
            GpuSceneCost,
            GpuObjectsCost,   // GPU, compris dans la scène
            GpuGroundCost,
            GpuSkyCost,
            GpuOceanCost,
            GpuTransparentCost,
            GpuInterfaceCost,
            CostCount,
        };

        // Moyenne sur une fenêtre glissante d'une demi-seconde : lisible, sans chiffres qui clignotent. Un temps
        // inconnu (négatif : heures du GPU absentes) n'entre pas dans sa moyenne.
        class FrameStats {
            public:
            void Tick(Clock::time_point now) {
                ++m_frames;
                const std::chrono::duration<double> elapsed = now - m_windowStart;
                if (elapsed.count() >= 0.5) {
                    m_fps = m_frames / elapsed.count();
                    m_frameMilliseconds = 1000.0 * elapsed.count() / m_frames;
                    for (std::size_t i = 0; i < CostCount; ++i) {
                        m_averages[i] = m_counts[i] > 0 ? m_sums[i] / m_counts[i] : -1.0;
                    }
                    m_sums = {};
                    m_counts = {};
                    m_frames = 0;
                    m_windowStart = now;
                }
            }

            void Add(Cost cost, double milliseconds) {
                if (milliseconds >= 0.0) {
                    m_sums[cost] += milliseconds;
                    ++m_counts[cost];
                }
            }

            double GetFps() const { return m_fps; }
            double GetFrameMilliseconds() const { return m_frameMilliseconds; }
            double Get(Cost cost) const { return m_averages[cost]; }

            private:
            Clock::time_point m_windowStart = Clock::now();
            int m_frames = 0;
            double m_fps = 0.0;
            double m_frameMilliseconds = 0.0;
            std::array<double, CostCount> m_sums{};
            std::array<int, CostCount> m_counts{};
            std::array<double, CostCount> m_averages{};
        };

        std::string FormatCost(double milliseconds) {
            return milliseconds < 0.0 ? std::string("?") : std::format("{:.2f} ms", milliseconds);
        }

        std::string FormatMegabytes(std::uint64_t bytes) {
            return std::format("{:.0f} Mo", static_cast<double>(bytes) / (1024.0 * 1024.0));
        }

        void DrawEnginePanel(UI::Context& ui, Renderer& renderer, const FrameStats& stats, Camera& camera,
                             FlyCameraController* controller) {
            if (ui.BeginPanel("Moteur", {8.0f, 8.0f}, 360.0f)) {
                ui.LabelText("Images/s", std::format("{:.0f} ({:.2f} ms)", stats.GetFps(), stats.GetFrameMilliseconds()));
                ui.LabelText("GPU", renderer.GetDeviceName());
                bool vsync = renderer.IsVSyncEnabled();
                if (ui.Checkbox("V-Sync", vsync)) {
                    renderer.SetVSync(vsync);
                }
                ui.LabelText("Présentation", renderer.GetPresentModeName());

                // Lecture : si « Attente » domine alors que le GPU travaille peu, la cadence vient de l'extérieur
                // (V-Sync, pilote) ; si le GPU est long, c'est le dessin ; sinon, le CPU.
                ui.Separator();
                ui.LabelText("CPU mise à jour", FormatCost(stats.Get(UpdateCost)));
                ui.LabelText("  dont jeu", FormatCost(stats.Get(GameCost)));
                ui.LabelText("CPU rendu", FormatCost(stats.Get(PrepareCost)) + " + " + FormatCost(stats.Get(RecordCost)));
                ui.LabelText("Attente GPU", FormatCost(stats.Get(WaitGpuCost)));
                ui.LabelText("Attente écran", FormatCost(stats.Get(WaitImageCost)));
                ui.LabelText("GPU ombres", FormatCost(stats.Get(GpuShadowsCost)));
                ui.LabelText("GPU scène", FormatCost(stats.Get(GpuSceneCost)));
                ui.LabelText("  dont objets", FormatCost(stats.Get(GpuObjectsCost)));
                ui.LabelText("  dont sol", FormatCost(stats.Get(GpuGroundCost)));
                ui.LabelText("  dont ciel", FormatCost(stats.Get(GpuSkyCost)));
                ui.LabelText("  dont océan", FormatCost(stats.Get(GpuOceanCost)));
                ui.LabelText("  dont transparents, flammes", FormatCost(stats.Get(GpuTransparentCost)));
                ui.LabelText("GPU interface", FormatCost(stats.Get(GpuInterfaceCost)));
                const RenderTimings& render = renderer.GetTimings();
                ui.LabelText("Tuiles de planète", std::format("{} dont {} eau", render.planetTiles, render.planetOceanTiles));
                ui.LabelText("  en mémoire", std::format("{} / {}", render.planetStoredTiles, render.planetTileCapacity));

                // RAM utilisée : présente en RAM en ce moment, bibliothèques partagées comprises (le Gestionnaire des
                // tâches montre la part privée, plus petite) ; réservée : promise au processus, même pas encore
                // touchée (pilote compris). GPU : ce que le rendu a demandé au pilote, en mémoire vidéo et en RAM.
                ui.Separator();
                const Platform::ProcessMemory memory = Platform::GetProcessMemory();
                const auto [gpuDevice, gpuHost] = renderer.GetGpuMemory();
                ui.LabelText("RAM utilisée", FormatMegabytes(memory.workingSet));
                ui.LabelText("RAM réservée", FormatMegabytes(memory.privateBytes));
                ui.LabelText("Mémoire vidéo", FormatMegabytes(gpuDevice));
                ui.LabelText("GPU en RAM", FormatMegabytes(gpuHost));

                if (controller) {
                    ui.Separator();
                    controller->DrawFields(ui, camera);
                }
            }
            ui.EndPanel();
        }
    }

    Engine::Engine(const EngineConfig& config)
        : m_window(std::make_unique<Window>()),
          m_showDebugOverlay(config.showDebugOverlay),
          m_drawScreenUi(config.drawScreenUi),
          m_freeCamera(config.freeCamera),
          m_physicsContext(std::make_shared<PhysicsContext>()),
          m_audio(std::make_unique<AudioSystem>()),
          m_interfaces(std::make_unique<InterfaceLibrary>()),
          m_inputActions(std::make_unique<InputActions>()) {
        RegisterEngineComponents(m_components);
        Log::Info("Flux {}", GetFullVersion());
        if (!m_window->Create(config.title, config.windowSize, config.maximized)) {
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
        m_engineValues["V-Sync"] = {.get = [this] { return m_renderer->IsVSyncEnabled() ? 1.0 : 0.0; },
                                    .set = [this](double value) {
                                        if ((value >= 0.5) != m_renderer->IsVSyncEnabled()) {
                                            m_renderer->SetVSync(value >= 0.5); // recrée la chaîne d'images : pas à chaque image
                                        }
                                    }};
        m_engineValues["Volume général"] = {.get = [this] { return static_cast<double>(m_audio->GetMasterVolume()); },
                                             .set = [this](double value) {
                                                 m_audio->SetMasterVolume(static_cast<float>(std::clamp(value, 0.0, 1.0)));
                                             }};
        m_engineValues["Images par seconde"] = {.get = [this] { return std::round(m_fps); }};

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
        Clock::time_point previousFrame = Clock::now();
        while (!m_quitRequested && !Platform::ConsoleCloseRequested() && m_window->IsOpen()) {
            m_window->HandleEvents();
            if (m_window->ConsumeCloseRequest() && (!m_closeRequestHandler || m_closeRequestHandler(*this))) {
                break;
            }
            if (!m_window->IsOpen()) {
                break;
            }

            const Math::Vector2i size = m_window->GetSize();
            if (size.x == 0 || size.y == 0) {
                m_window->WaitEvents(); // fenêtre réduite : on dort jusqu'au prochain événement
                continue;
            }

            const Clock::time_point now = Clock::now();
            // Plafonné : après un blocage (déplacement de fenêtre, point d'arrêt), la caméra ne fait pas un bond.
            const double deltaSeconds = std::min(std::chrono::duration<double>(now - previousFrame).count(), 0.1);
            previousFrame = now;
            stats.Tick(now);
            m_fps = stats.GetFps();
            const RenderTimings& render = m_renderer->GetTimings();
            stats.Add(PrepareCost, render.prepare);
            stats.Add(RecordCost, render.record);
            stats.Add(WaitGpuCost, render.waitGpu);
            stats.Add(WaitImageCost, render.waitImage + render.present);
            stats.Add(GpuShadowsCost, render.gpuShadows);
            stats.Add(GpuSceneCost, render.gpuScene);
            stats.Add(GpuObjectsCost, render.gpuObjects);
            stats.Add(GpuGroundCost, render.gpuGround);
            stats.Add(GpuSkyCost, render.gpuSky);
            stats.Add(GpuOceanCost, render.gpuOcean);
            stats.Add(GpuTransparentCost, render.gpuTransparent);
            stats.Add(GpuInterfaceCost, render.gpuInterface);

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
                RotateFollowCamera();
            } else {
                m_followRotating = false;
                if (m_freeCamera) {
                    m_camera.roll = 0.0f; // vol libre : l'horizon reste droit, même après la vue cockpit
                    m_cameraController.Update(m_camera, *m_window, deltaSeconds,
                                              m_cameraInputEnabled && !m_ui->WantsMouse() && !m_interfaceWantsMouse,
                                              m_cameraInputEnabled && !m_ui->WantsKeyboard());
                } else {
                    m_window->SetCursorLocked(false); // personnage retiré pendant un clic droit
                }
            }
            if (m_gameState != GameState::Stopped) {
                // Pause : celle de l'éditeur (F6), ou un menu ouvert qui la demande.
                const double gameDelta = m_gameState == GameState::Running && !m_interfacePaused ? deltaSeconds : 0.0;
                const Clock::time_point gameStart = Clock::now();
                UpdateGameSystems(gameDelta);
                if (m_gameUpdate) {
                    m_gameUpdate(*this, gameDelta);
                }
                stats.Add(GameCost, Milliseconds(Clock::now() - gameStart));
            }
            // Après la physique et le jeu : la caméra voit le personnage là où il est dessiné.
            m_hiddenPlayer = NullEntity;
            if (const Entity player = GetFollowedPlayer(); player != NullEntity) {
                PlaceFollowCamera(player, deltaSeconds);
            } else {
                m_followShown = -1.0f;
                m_followShip = NullEntity;
                m_followFrameSet = false;
            }
            // Sons des entités, à la caméra de cette image (qui suit le joueur ou vole librement).
            m_audio->SetAssetsDirectory(m_project ? m_project->assetsDirectory : std::filesystem::path{});
            m_audio->Update(m_scene, m_camera, m_gameState != GameState::Stopped,
                            m_gameState == GameState::Paused || m_interfacePaused, deltaSeconds);
            if (m_editorUpdate) {
                m_editorUpdate(*this, deltaSeconds);
            }
            // Après les mises à jour (la scène de cette frame), sous les panneaux (ajoutés par EndFrame).
            if (m_drawScreenUi) {
                ScreenUi::Draw(m_scene, m_renderer->GetOverlay(), {}, {static_cast<float>(size.x), static_cast<float>(size.y)},
                               GetScreenUiOptions());
            }
            if (const Entity player = GetFollowedPlayer(); player != NullEntity && m_drawScreenUi) {
                DrawFlightInfo(player, {static_cast<float>(size.x), static_cast<float>(size.y)}, deltaSeconds);
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
            if (m_showDebugOverlay) {
                DrawEnginePanel(*m_ui, *m_renderer, stats, m_camera, m_freeCamera ? &m_cameraController : nullptr);
            }
            m_ui->EndFrame(m_renderer->GetOverlay());
            m_window->SetMouseCursor(m_ui->GetMouseCursor()); // double flèche au bord d'un panneau
            stats.Add(UpdateCost, Milliseconds(Clock::now() - now));

            m_renderer->SetAssetsDirectory(m_project ? m_project->assetsDirectory : std::filesystem::path{});
            m_renderer->SetThrusterPreview(m_gameState == GameState::Stopped); // éditeur, jeu arrêté
            if (!m_renderer->RenderFrame(m_scene, m_camera, m_hiddenPlayer)) {
                return 1;
            }
            RunInterfaceActions(); // avant le changement de scène : une fonction peut en demander un
            LoadRequestedScene();
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
        SeatInput seatInput;
        seatInput.interact = WasActionPressed("Interagir");
        if (seated) {
            seatInput.thrust = axis("Avancer", "Reculer");
            seatInput.strafe = axis("Aller à droite", "Aller à gauche");
            seatInput.lift = axis("Monter", "Descendre");
            seatInput.roll = axis("Rouler à droite", "Rouler à gauche");
            seatInput.brake = IsActionDown("Freiner");
            seatInput.boost = IsActionDown("Boost");
            seatInput.travel = WasActionPressed("Mode voyage");
            seatInput.mouseYaw = m_steerYaw;
            seatInput.mousePitch = m_steerPitch;
            if (WasActionPressed("Changer de vue")) {
                m_cockpitView = !m_cockpitView;
            }
            // Aide au pilotage : selon la gravité du dehors (planète, sol), rien dans l'espace.
            if (const Entity seat = Seats::GetPilotSeat(m_scene, player); seat != NullEntity) {
                if (const Entity ship = m_scene.GetShip(seat); ship != NullEntity) {
                    ShipComponent& component = m_scene.Get<ShipComponent>(ship);
                    if (WasActionPressed("Stabiliser à plat")) {
                        component.levelAssist = !component.levelAssist;
                        Log::Info("Stabilisation à plat : {}", component.levelAssist ? "oui" : "non");
                    }
                    if (const Math::Vector3f gravity = m_physics->GetOutsideGravity(m_scene, ship); Math::Length(gravity) > 0.01f) {
                        seatInput.up = Math::Normalize(gravity) * -1.0f;
                    }
                    const FlightState flight = GetFlightState(ship);
                    seatInput.air = flight.air;
                    if (flight.nearPlanet) {
                        seatInput.planetUp = Math::Vector3f(flight.up);
                        seatInput.planetDistance = static_cast<float>(flight.boundary);
                        seatInput.planetCenter = static_cast<float>(flight.center);
                    }
                }
            }
        } else {
            // Avant et droite de la caméra, ramenés à l'horizontale de l'endroit (sur une planète : tangente au sol).
            const Math::Vector3f forward = m_followFrame.Rotate(Math::Vector3f{-std::sin(m_followYaw), 0.0f, -std::cos(m_followYaw)});
            const Math::Vector3f right = m_followFrame.Rotate(Math::Vector3f{std::cos(m_followYaw), 0.0f, -std::sin(m_followYaw)});
            character.move = forward * axis("Avancer", "Reculer") + right * axis("Aller à droite", "Aller à gauche");
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
        m_physics->Update(m_scene, deltaSeconds, character);
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
        m_physics.reset(); // les corps de l'ancienne scène
        if (m_gameState != GameState::Stopped) {
            StartPhysics();
        }
        return {};
    }

    void Engine::SetGameState(GameState state) {
        const bool starting = m_gameState == GameState::Stopped && state != GameState::Stopped;
        m_gameState = state;
        if (starting) {
            StartPhysics(); // lancement du jeu (éditeur) : la scène est prête
        }
        if (state == GameState::Stopped) {
            m_physics.reset();
            m_audio->StopAll();
            m_interfaces->ClearInstances(); // cases cochées, fenêtres déplacées : le prochain jeu repart du fichier
            m_interfaceActions.clear();
            m_interfacePaused = m_interfacePausedNow = false;
        }
    }

    Engine::FlightState Engine::GetFlightState(Entity ship) {
        FlightState flight;
        const Math::WorldPosition position = m_scene.GetWorldPosition(ship);
        const PlanetComponent* nearest = nullptr;
        Math::Vector3d offset{};
        Math::Quaternionf rotation;
        for (const auto [entity, planet] : m_scene.GetRegistry().view<const PlanetComponent>().each()) {
            if (m_scene.GetShip(entity) != NullEntity) {
                continue; // décor d'un vaisseau
            }
            const Math::Vector3d candidate = Math::Delta(m_scene.GetWorldPosition(entity), position);
            if (!nearest || Math::Length(candidate) - planet.radius < Math::Length(offset) - nearest->radius) {
                nearest = &planet;
                offset = candidate;
                rotation = m_scene.GetWorldRotation(entity);
            }
        }
        // Dans l'espace (au-delà de deux rayons, là où s'arrête sa gravité, voir PlanetGround) : pas d'altitude.
        if (!nearest || Math::Length(offset) < 1.0 || Math::Length(offset) > 2.0 * nearest->radius) {
            return flight;
        }
        if (!m_flightTerrain || m_flightTerrain->GetRadius() != nearest->radius || m_flightTerrain->GetRelief() != nearest->relief
            || m_flightTerrainSeed != nearest->seed) {
            m_flightTerrain = std::make_shared<const Planet::Terrain>(nearest->radius, nearest->relief, nearest->seed);
            m_flightTerrainSeed = nearest->seed;
        }
        flight.nearPlanet = true;
        flight.up = Math::Normalize(offset);
        flight.center = Math::Length(offset);
        const Math::Vector3d direction = rotation.Conjugate().Rotate(flight.up);
        double ground = m_flightTerrain->GetHeight(direction, m_flightTerrain->GetOctaves(m_flightTerrain->GetMaxLevel()));
        if (nearest->ocean) {
            ground = std::max(ground, nearest->seaLevel * 0.01 * nearest->relief);
        }
        flight.altitude = Math::Length(offset) - nearest->radius - ground;
        // Comme le ciel (Atmosphere.cpp) : densité divisée par e tous les huitièmes de sa hauteur, nulle au-dessus.
        const double height = std::max(nearest->atmosphereHeight, 100.0f);
        const double above = std::max(Math::Length(offset) - nearest->radius, 0.0);
        if (nearest->atmosphere > 0.0f && above < height) {
            flight.air = static_cast<float>(std::min(1.0, nearest->atmosphere * std::exp(-8.0 * above / height)));
        }
        flight.boundary = std::max(nearest->atmosphere > 0.0f ? above - height : flight.altitude, 0.0);
        return flight;
    }

    void Engine::DrawFlightInfo(Entity player, Math::Vector2f screen, double deltaSeconds) {
        const Entity seat = Seats::GetPilotSeat(m_scene, player);
        const Entity ship = seat != NullEntity ? m_scene.GetShip(seat) : NullEntity;
        if (ship == NullEntity) {
            m_flightShip = NullEntity; // remesurée en reprenant les commandes
            return;
        }
        const ShipComponent& component = m_scene.Get<ShipComponent>(ship);
        // Vitesse mesurée sur son déplacement, pas celle que visent ses propulseurs : contre le sol ou un mur, il
        // pousse sans avancer. Lissée sur un quart de seconde ; figée en pause.
        const Math::WorldPosition position = m_scene.GetWorldPosition(ship);
        if (m_flightShip != ship) {
            m_flightShip = ship;
            m_flightVelocity = Math::Vector3d(m_scene.GetWorldRotation(ship).Rotate(component.velocity));
        } else if (deltaSeconds > 0.0) {
            const Math::Vector3d measured = Math::Delta(m_flightPosition, position) / deltaSeconds;
            m_flightVelocity = m_flightVelocity + (measured - m_flightVelocity) * std::min(1.0, deltaSeconds / 0.25);
        }
        m_flightPosition = position;
        const Math::Vector3d velocity = m_flightVelocity;
        // Nombre à la française, en m ou km selon sa taille.
        const auto distance = [](double meters, std::string_view unit) {
            std::string text = std::abs(meters) >= 10000.0 ? std::format("{:.1f} k{}", meters / 1000.0, unit)
                                                            : std::format("{:.0f} {}", meters, unit);
            std::ranges::replace(text, '.', ',');
            return text;
        };
        std::string text = "Vitesse  " + distance(Math::Length(velocity), "m/s");
        if (component.travelLevel > 0.0f) {
            text += component.travelLevel < 1.0f ? std::format("\nMode voyage  {:.0f} %", component.travelLevel * 100.0f) : "\nMode voyage";
        }

        if (const FlightState flight = GetFlightState(ship); flight.nearPlanet) {
            text += "\nVerticale  " + distance(Math::Dot(velocity, flight.up), "m/s");
            text += "\nAltitude  " + distance(flight.altitude, "m");
            if (flight.air > 0.001f) {
                text += std::format("\nAir  {:.0f} %", flight.air * 100.0f);
            }
        }
        const float size = 20.0f;
        UI::DrawList& list = m_renderer->GetOverlay();
        const Math::Vector2f extent = list.MeasureText(text, size);
        const Math::Vector2f topLeft{(screen.x - extent.x) * 0.5f, screen.y - extent.y - 40.0f};
        list.AddRoundedRect(topLeft - Math::Vector2f{14.0f, 10.0f}, topLeft + extent + Math::Vector2f{14.0f, 10.0f}, {0.0f, 0.0f, 0.0f, 0.45f}, 8.0f);
        list.AddText(topLeft, text, size, {0.85f, 0.95f, 1.0f, 1.0f});
    }

    Entity Engine::GetFollowedPlayer() const {
        if (m_gameState == GameState::Stopped || !m_physics) {
            return NullEntity;
        }
        const Entity player = m_physics->GetPlayer();
        return m_scene.IsValid(player) ? player : NullEntity;
    }

    void Engine::RotateFollowCamera() {
        // Comme le vol libre : clic droit maintenu (commencé hors de l'interface) pour tourner autour du personnage.
        const Input& input = m_window->GetInput();
        const bool held = m_window->HasFocus() && input.IsMouseButtonDown(MouseButton::Right);
        const bool started = input.WasMouseButtonPressed(MouseButton::Right) && !m_ui->WantsMouse() && !m_interfaceWantsMouse;
        m_followRotating = held && (m_followRotating || started);
        constexpr float Sensitivity = 0.0025f;
        const Math::Vector2f mouse = input.GetMouseDelta();
        // Aux commandes, sans clic droit : la souris dirige le vaisseau, curseur caché. Alt maintenu le libère (panneaux
        // de l'éditeur), comme un menu qui met le jeu en pause.
        const bool piloting = Seats::GetPilotSeat(m_scene, GetFollowedPlayer()) != NullEntity;
        const bool steering = piloting && !m_followRotating && m_window->HasFocus() && !m_interfacePaused
                           && m_gameState == GameState::Running && !input.IsKeyDown(Key::LeftAlt)
                           && !input.IsKeyDown(Key::RightAlt);
        // La première image où le curseur se cache, son déplacement vient d'avant : ignoré.
        const bool wasLocked = m_window->IsCursorLocked();
        m_window->SetCursorLocked(m_followRotating || steering);
        m_steerYaw = steering && wasLocked ? -mouse.x * Sensitivity : 0.0f;
        m_steerPitch = steering && wasLocked ? -mouse.y * Sensitivity : 0.0f;
        if (m_followRotating) {
            if (Seats::GetPilotSeat(m_scene, GetFollowedPlayer()) != NullEntity) {
                // Aux commandes (cockpit ou vue extérieure) : on tourne la tête, sans perdre le vaisseau de vue.
                m_lookYaw = std::clamp(m_lookYaw - mouse.x * Sensitivity, -2.0f, 2.0f);
                m_lookPitch = std::clamp(m_lookPitch - mouse.y * Sensitivity, -1.2f, 1.2f);
            } else {
                m_followYaw = std::remainder(m_followYaw - mouse.x * Sensitivity, 2.0f * std::numbers::pi_v<float>);
                m_followPitch = std::clamp(m_followPitch - mouse.y * Sensitivity, -FollowMaxPitch, FollowMaxPitch);
            }
        }
        if (const float wheel = input.GetWheelDelta(); wheel != 0.0f && !m_ui->WantsMouse() && !m_interfaceWantsMouse) {
            constexpr float MinDistance = 1.0f;
            constexpr float MinChaseZoom = 0.9f; // en tailles du vaisseau : plus près, on entrerait dans sa coque
            if (Seats::GetPilotSeat(m_scene, GetFollowedPlayer()) != NullEntity) {
                // Aux commandes, comme à pied : en arrière, le cockpit passe à la vue extérieure ; en avant au plus près,
                // retour dans le cockpit.
                if (m_cockpitView) {
                    if (wheel < 0.0f) {
                        m_cockpitView = false;
                        m_chaseZoom = MinChaseZoom;
                        m_chaseShown = -1.0f;
                    }
                } else if (wheel > 0.0f && m_chaseZoom <= MinChaseZoom) {
                    m_cockpitView = true;
                } else {
                    m_chaseZoom = std::clamp(m_chaseZoom * std::pow(0.85f, wheel), MinChaseZoom, 8.0f);
                }
            } else if (m_firstPerson) {
                if (wheel < 0.0f) { // en arrière : la caméra ressort doucement derrière le personnage
                    m_firstPerson = false;
                    m_followDistance = 1.5f;
                    m_followShown = 0.0f;
                }
            } else if (wheel > 0.0f && m_followDistance <= MinDistance) {
                m_firstPerson = true; // déjà au plus près : un cran de plus passe dans ses yeux
            } else {
                m_followDistance = std::clamp(m_followDistance * std::pow(0.85f, wheel), MinDistance, 30.0f);
            }
        }
    }

    Entity Engine::GetCockpitSeat(Entity player) const {
        // Dès qu'il s'assoit (animation comprise) et jusqu'à ce qu'il soit debout : la vue part du siège, dans l'axe
        // du vaisseau, au lieu de rester tournée vers où regardait la caméra quand il est monté.
        if (!m_cockpitView || player == NullEntity) {
            return NullEntity;
        }
        const auto* character = m_scene.TryGet<CharacterComponent>(player);
        if (!character || character->seatState == SeatState::Standing || !m_scene.IsValid(character->seat)) {
            return NullEntity;
        }
        const auto* seat = m_scene.TryGet<SeatComponent>(character->seat);
        return seat && seat->pilot ? character->seat : NullEntity;
    }

    void Engine::TurnWithShip(Entity player) {
        const Math::Vector3f up = m_physics->GetUp(m_scene, player);
        const Math::Vector3f y{0.0f, 1.0f, 0.0f};
        if (!m_followFrameSet) {
            // Début du suivi : la caméra garde sa direction (celle de l'éditeur), dans l'horizontale de l'endroit.
            m_followFrame = Math::Quaternionf::FromTo(y, up);
            m_followFrameSet = true;
            AimFollowCamera();
        }
        // Le vaisseau a tourné depuis la dernière image : la caméra aussi, comme la tête de quelqu'un à bord.
        const Entity ship = m_scene.GetShip(player);
        const Math::Quaternionf rotation = ship != NullEntity ? m_scene.GetWorldRotation(ship) : Math::Quaternionf{};
        if (ship != NullEntity && ship == m_followShip) {
            m_followFrame = Math::Normalize(rotation * m_followShipRotation.Conjugate() * m_followFrame);
        }
        m_followShip = ship;
        m_followShipRotation = rotation;
        // Redressée au plus court sur le haut de l'endroit : en marchant autour d'une planète, l'horizon reste droit.
        m_followFrame = Math::Normalize(Math::Quaternionf::FromTo(m_followFrame.Rotate(y), up) * m_followFrame);
    }

    void Engine::AimFollowCamera() {
        const Math::Vector3f forward = m_followFrame.Conjugate().Rotate(m_camera.GetForward());
        m_followYaw = std::atan2(-forward.x, -forward.z);
        m_followPitch = std::clamp(std::asin(std::clamp(forward.y, -1.0f, 1.0f)), -FollowMaxPitch, FollowMaxPitch);
    }

    void Engine::MeasureShip(Entity ship) {
        // Boîte des modèles du vaisseau dans son repère (sans les personnages à bord) : la caméra vise son centre,
        // à une distance proportionnelle à sa taille.
        m_chaseShip = ship;
        Math::Vector3f min{1.0e9f, 1.0e9f, 1.0e9f};
        Math::Vector3f max = min * -1.0f;
        const auto visit = [&](auto&& self, Entity entity, const Math::Vector3d& position, const Math::Quaternionf& rotation,
                               const Math::Vector3f& scale) -> void {
            if (m_scene.TryGet<CharacterComponent>(entity)) {
                return;
            }
            if (m_scene.TryGet<ModelComponent>(entity) || m_scene.TryGet<MeshRendererComponent>(entity)) {
                if (const std::optional<Math::Box3f> box = GetMeshBounds(entity)) {
                    for (int corner = 0; corner < 8; ++corner) {
                        const Math::Vector3f local{(corner & 1) ? box->max.x : box->min.x, (corner & 2) ? box->max.y : box->min.y,
                                                   (corner & 4) ? box->max.z : box->min.z};
                        const Math::Vector3f point = Math::Vector3f(position)
                                                   + rotation.Rotate(Math::Vector3f{local.x * scale.x, local.y * scale.y, local.z * scale.z});
                        min = {std::min(min.x, point.x), std::min(min.y, point.y), std::min(min.z, point.z)};
                        max = {std::max(max.x, point.x), std::max(max.y, point.y), std::max(max.z, point.z)};
                    }
                }
            }
            m_scene.ForEachChild(entity, [&](Entity child) {
                const TransformComponent& transform = m_scene.Get<TransformComponent>(child);
                const Math::Vector3f offset{static_cast<float>(transform.position.x) * scale.x,
                                            static_cast<float>(transform.position.y) * scale.y,
                                            static_cast<float>(transform.position.z) * scale.z};
                self(self, child, position + Math::Vector3d(rotation.Rotate(offset)), rotation * transform.rotation,
                     Math::Vector3f{scale.x * transform.scale.x, scale.y * transform.scale.y, scale.z * transform.scale.z});
            });
        };
        visit(visit, ship, {}, {}, {1.0f, 1.0f, 1.0f});
        if (min.x > max.x) {
            m_chaseCenter = {};
            m_chaseRadius = 10.0f; // rien d'affiché : une taille de chasseur
            return;
        }
        m_chaseCenter = (min + max) * 0.5f;
        m_chaseRadius = std::max(1.0f, Math::Length(max - min) * 0.5f);
    }

    void Engine::PlaceChaseCamera(Entity seat, double deltaSeconds) {
        const Entity ship = m_scene.GetShip(seat);
        if (ship == NullEntity) {
            return;
        }
        if (ship != m_chaseShip) {
            MeasureShip(ship);
            m_chaseShown = -1.0f;
        }
        // Derrière le vaisseau, un peu au-dessus (on regarde légèrement vers le bas), tournée avec lui, roulis compris ;
        // le clic droit tourne autour.
        constexpr float LookDown = 0.2f;
        const Math::Quaternionf shipRotation = m_scene.GetWorldRotation(ship);
        const Math::Quaternionf rotation = shipRotation * Math::Quaternionf::FromAxisAngle({0.0f, 1.0f, 0.0f}, m_lookYaw)
                                         * Math::Quaternionf::FromAxisAngle({1.0f, 0.0f, 0.0f}, m_lookPitch - LookDown);
        Math::WorldPosition target = m_scene.GetWorldPosition(ship);
        target += Math::Vector3d(shipRotation.Rotate(m_chaseCenter));
        const Math::Vector3f back = rotation.Rotate(Math::Vector3f{0.0f, 0.0f, 1.0f});
        const float distance = m_chaseRadius * m_chaseZoom;
        // Un mur derrière le vaisseau (hangar) : la caméra s'avance, sans que le vaisseau lui-même l'arrête.
        constexpr float CameraRadius = 0.2f;
        const float free = m_physics->CastCamera(m_scene, seat, target, back, distance, CameraRadius, ship);
        if (m_chaseShown < 0.0f || free < m_chaseShown) {
            m_chaseShown = free;
        } else {
            constexpr double ReturnRate = 6.0;
            m_chaseShown += (free - m_chaseShown) * static_cast<float>(1.0 - std::exp(-ReturnRate * deltaSeconds));
        }
        m_camera.position = target;
        m_camera.position += Math::Vector3d(back) * static_cast<double>(m_chaseShown);
        m_camera.SetRotation(rotation);
    }

    void Engine::PlaceFollowCamera(Entity player, double deltaSeconds) {
        TurnWithShip(player);
        if (const Entity pilotSeat = Seats::GetPilotSeat(m_scene, player); pilotSeat != NullEntity && !m_cockpitView) {
            PlaceChaseCamera(pilotSeat, deltaSeconds);
            AimFollowCamera(); // en se relevant, elle regarde où regardait la vue extérieure
            m_followShown = -1.0f; // en se relevant, la caméra se replace d'un coup derrière le personnage
            return;
        }
        m_chaseShown = -1.0f;
        if (const Entity seat = GetCockpitSeat(player); seat != NullEntity) {
            // Vue cockpit : depuis l'œil du pilote, dans l'axe du siège (roulis du vaisseau compris).
            const Math::Quaternionf seatRotation = m_scene.GetWorldRotation(seat);
            m_camera.position = m_scene.GetWorldPosition(seat);
            m_camera.position += Math::Vector3d(seatRotation.Rotate(m_scene.Get<SeatComponent>(seat).eye));
            m_camera.SetRotation(seatRotation * Math::Quaternionf::FromAxisAngle({0.0f, 1.0f, 0.0f}, m_lookYaw)
                                 * Math::Quaternionf::FromAxisAngle({1.0f, 0.0f, 0.0f}, m_lookPitch));
            AimFollowCamera(); // en se relevant, elle regarde où regardait le pilote
            m_followShown = -1.0f; // en ressortant, la caméra se replace d'un coup derrière le personnage
            m_hiddenPlayer = player;
            return;
        }
        m_lookYaw = 0.0f;
        m_lookPitch = 0.0f;
        m_followPitch = std::clamp(m_followPitch, -FollowMaxPitch, FollowMaxPitch);
        m_camera.SetRotation(m_followFrame * Math::Quaternionf::FromYawPitchRoll(m_followYaw, m_followPitch, 0.0f));

        // Derrière le personnage, visant un peu sous le haut de sa tête.
        const auto* character = m_scene.TryGet<CharacterComponent>(player);
        const float eyeHeight = character ? character->height * 0.9f : 1.6f;
        Math::WorldPosition target = m_scene.GetWorldPosition(player);
        target += Math::Vector3d(m_followFrame.Rotate(Math::Vector3f{0.0f, eyeHeight, 0.0f}));

        if (m_firstPerson) {
            // Depuis ses yeux : rien entre la tête et la caméra, pas besoin de chercher les murs. Son ombre reste.
            m_camera.position = target;
            m_followShown = 0.0f;
            m_hiddenPlayer = player;
            return;
        }

        // Un mur entre le personnage et la caméra : elle s'avance devant lui. Sphère un peu plus grosse que ce que
        // le plan proche découpe (0,1 m devant l'œil, écran 16/9 : 0,12 m au coin), pour ne jamais voir à travers.
        constexpr float CameraRadius = 0.2f;
        const Math::Vector3f back = m_camera.GetForward() * -1.0f;
        const float free = m_physics->CastCamera(m_scene, player, target, back, m_followDistance, CameraRadius);
        // Elle rentre d'un coup (sinon on verrait le mur de l'intérieur) et ressort en douceur (pas de bond quand
        // le mur ne cache plus rien, par exemple en passant une porte).
        if (m_followShown < 0.0f || free < m_followShown) {
            m_followShown = free;
        } else {
            constexpr double ReturnRate = 6.0; // par seconde : les deux tiers du chemin en 1/6 s
            m_followShown += (free - m_followShown) * static_cast<float>(1.0 - std::exp(-ReturnRate * deltaSeconds));
        }

        m_camera.position = target;
        m_camera.position += Math::Vector3d(back) * static_cast<double>(m_followShown);
        // Caméra poussée contre la tête (dos au mur, sous une verrière) : on verrait l'intérieur du crâne, on ne
        // dessine plus le personnage (vue à la première personne), comme la vue cockpit.
        constexpr float HideDistance = 0.5f;
        if (m_followShown < HideDistance) {
            m_hiddenPlayer = player;
        }
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
            Log::Error("scène {} : {}", *requested, loaded.error());
            return;
        }
        m_audio->StopAll(); // ses sons aussi ; ceux de la nouvelle partent à la prochaine image
    }

    void Engine::RunInterfaceActions() {
        std::vector<std::pair<Interface::ButtonAction, std::string>> actions = std::exchange(m_interfaceActions, {});
        for (auto& [action, target] : actions) {
            if (m_gameState == GameState::Stopped) {
                return; // « Quitter » a arrêté le jeu (éditeur) : la suite visait le jeu
            }
            switch (action) {
                case Interface::ButtonAction::ChangeScene:
                    if (!target.empty()) {
                        RequestScene(std::move(target));
                    }
                    break;
                case Interface::ButtonAction::Quit:
                    if (m_gameQuitHandler) {
                        m_gameQuitHandler(*this);
                    } else {
                        RequestQuit();
                    }
                    break;
                case Interface::ButtonAction::RebindKey:
                    StartKeyRebinding(std::move(target));
                    break;
                case Interface::ButtonAction::ResetKeys:
                    ResetInputActions();
                    break;
                case Interface::ButtonAction::CallFunction:
                    if (const auto found = m_interfaceFunctions.find(target); found != m_interfaceFunctions.end()) {
                        // Copie : la fonction peut s'enregistrer à nouveau ou se retirer pendant l'appel.
                        const InterfaceFunction function = found->second;
                        function(*this);
                    } else {
                        Log::Warning("bouton d'interface : aucune fonction « {} » enregistrée par le jeu", target);
                    }
                    break;
                default:
                    break;
            }
        }
    }

    void Engine::RegisterInterfaceFunction(std::string name, InterfaceFunction function) {
        m_interfaceFunctions.insert_or_assign(std::move(name), std::move(function));
    }

    void Engine::UnregisterInterfaceFunction(std::string_view name) {
        if (const auto found = m_interfaceFunctions.find(name); found != m_interfaceFunctions.end()) {
            m_interfaceFunctions.erase(found);
        }
    }

    void Engine::ClearInterfaceFunctions() {
        m_interfaceFunctions.clear();
        m_interfaceActions.clear();
        m_interfaceValues.clear();
    }

    void Engine::BindInterfaceValue(std::string name, std::function<double()> get, std::function<void(double)> set) {
        m_interfaceValues.insert_or_assign(std::move(name), InterfaceValue{.get = std::move(get), .set = std::move(set), .getText = {}});
    }

    void Engine::BindInterfaceText(std::string name, std::function<std::string()> get) {
        m_interfaceValues.insert_or_assign(std::move(name), InterfaceValue{.get = {}, .set = {}, .getText = std::move(get)});
    }

    void Engine::UnbindInterfaceValue(std::string_view name) {
        if (const auto found = m_interfaceValues.find(name); found != m_interfaceValues.end()) {
            m_interfaceValues.erase(found);
        }
    }

    std::vector<std::string> Engine::GetInterfaceValueNames() const {
        std::vector<std::string> names;
        for (const auto& [name, value] : m_engineValues) {
            if (!m_interfaceValues.contains(name)) {
                names.push_back(name);
            }
        }
        for (const auto& [name, value] : m_interfaceValues) {
            names.push_back(name);
        }
        for (const InputAction& action : m_inputActions->GetActions()) {
            names.push_back(std::string(KeyBindingPrefix) + action.name);
        }
        return names;
    }

    const Engine::InterfaceValue* Engine::FindInterfaceValue(std::string_view name) const {
        if (const auto found = m_interfaceValues.find(name); found != m_interfaceValues.end()) {
            return &found->second; // celles du jeu d'abord : elles peuvent remplacer celles du moteur
        }
        const auto found = m_engineValues.find(name);
        return found != m_engineValues.end() ? &found->second : nullptr;
    }

    void Engine::SyncInterfaceBindings(std::vector<Interface::Element>& elements, std::uint32_t changed) {
        for (Interface::Element& element : elements) {
            if (element.binding.starts_with(KeyBindingPrefix)) {
                // « Touche : Avancer » : les touches de l'action, sur un texte ou un bouton.
                const std::string_view action = std::string_view(element.binding).substr(KeyBindingPrefix.size());
                element.boundText = m_rebindingAction == action ? std::string("Appuyez sur une touche...")
                                                                : m_inputActions->GetKeysText(action);
            } else if (!element.binding.empty()) {
                if (const InterfaceValue* value = FindInterfaceValue(element.binding)) {
                    const bool touched = element.id == changed && value->set;
                    switch (element.type) {
                        case Interface::ElementType::Checkbox:
                            if (touched) {
                                value->set(element.checked ? 1.0 : 0.0);
                            } else if (value->get) {
                                element.checked = value->get() >= 0.5;
                            }
                            break;
                        case Interface::ElementType::Slider:
                            if (touched) {
                                value->set(element.value);
                            } else if (value->get) {
                                element.value = static_cast<float>(value->get());
                            }
                            break;
                        case Interface::ElementType::ProgressBar:
                            if (value->get) {
                                element.value = static_cast<float>(value->get());
                            }
                            break;
                        case Interface::ElementType::Text:
                        case Interface::ElementType::Button:
                            if (value->getText) {
                                element.boundText = value->getText();
                            } else if (value->get) {
                                // Entier sans décimales (images/s, munitions), sinon deux décimales.
                                const double number = value->get();
                                element.boundText = number == std::round(number) ? std::format("{:.0f}", number)
                                                                                 : std::format("{:.2f}", number);
                            }
                            break;
                        default:
                            break;
                    }
                }
            }
            SyncInterfaceBindings(element.children, changed);
        }
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

    const Interface::Document* Engine::LoadInterface(std::string_view relativePath) {
        if (!m_project || relativePath.empty()) {
            return nullptr;
        }
        return m_interfaces->Get(m_project->assetsDirectory, relativePath);
    }

    ScreenUi::DrawOptions Engine::GetScreenUiOptions() {
        return {.textures = [this](std::string_view path) { return LoadAssetTexture(path); },
                .interfaces = [this](const ScreenUi::InterfaceRequest& request) { return DrawInterface(request); }};
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
                SyncInterfaceBindings(instance->document.roots, interaction.changed);
                m_interfaceMouseNow = m_interfaceMouseNow || interaction.wantsMouse;
                m_interfacePausedNow = m_interfacePausedNow || interaction.pausesGame;
                m_escapeUsed = m_escapeUsed || interaction.escapeUsed;
                if (interaction.action != Interface::ButtonAction::None) {
                    // Pas pendant le dessin : la scène est en cours de parcours.
                    m_interfaceActions.emplace_back(interaction.action, interaction.target);
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

    UI::Context& Engine::GetUi() {
        return *m_ui;
    }
}
