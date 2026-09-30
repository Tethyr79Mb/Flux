#include "Flux/Core/Engine.hpp"

#include "Flux/Animation/Animator.hpp"
#include "Flux/Physics/PhysicsWorld.hpp"
#include "Flux/Platform/Dialogs.hpp"
#include "Flux/Platform/Filesystem.hpp"
#include "Flux/Platform/Input.hpp"
#include "Flux/Platform/Window.hpp"
#include "Flux/Renderer/Renderer.hpp"
#include "Flux/Scene/SceneChange.hpp"
#include "Flux/Scene/SceneSerializer.hpp"
#include "Flux/Scene/Seats.hpp"
#include "Flux/Scene/ScreenUi.hpp"
#include "Flux/UI/Context.hpp"
#include "Flux/UI/DrawList.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <format>
#include <numbers>
#include <string>
#include <utility>

namespace Flux {
    namespace {
        using Clock = std::chrono::steady_clock;

        // Caméra qui suit le personnage : un peu moins que la verticale, au-dessus comme en dessous.
        constexpr float FollowMaxPitch = 1.3f;

        // Moyenne sur une fenêtre glissante d'une demi-seconde : lisible, sans chiffres qui clignotent.
        class FrameStats {
            public:
            void Tick(Clock::time_point now) {
                ++m_frames;
                const std::chrono::duration<double> elapsed = now - m_windowStart;
                if (elapsed.count() >= 0.5) {
                    m_fps = m_frames / elapsed.count();
                    m_frameMilliseconds = 1000.0 * elapsed.count() / m_frames;
                    m_frames = 0;
                    m_windowStart = now;
                }
            }

            double GetFps() const { return m_fps; }
            double GetFrameMilliseconds() const { return m_frameMilliseconds; }

            private:
            Clock::time_point m_windowStart = Clock::now();
            int m_frames = 0;
            double m_fps = 0.0;
            double m_frameMilliseconds = 0.0;
        };

        void DrawEnginePanel(UI::Context& ui, Renderer& renderer, const FrameStats& stats, Camera& camera,
                             FlyCameraController& controller) {
            if (ui.BeginPanel("Moteur", {8.0f, 8.0f}, 360.0f)) {
                ui.LabelText("Images/s", std::format("{:.0f} ({:.2f} ms)", stats.GetFps(), stats.GetFrameMilliseconds()));
                ui.LabelText("GPU", renderer.GetDeviceName());
                bool vsync = renderer.IsVSyncEnabled();
                if (ui.Checkbox("V-Sync", vsync)) {
                    renderer.SetVSync(vsync);
                }
                ui.LabelText("Présentation", renderer.GetPresentModeName());

                ui.Separator();
                controller.DrawFields(ui, camera);
            }
            ui.EndPanel();
        }
    }

    Engine::Engine(const EngineConfig& config)
        : m_window(std::make_unique<Window>()),
          m_showDebugOverlay(config.showDebugOverlay),
          m_drawScreenUi(config.drawScreenUi) {
        RegisterEngineComponents(m_components);
        if (!m_window->Create(config.title, config.windowSize, config.maximized)) {
            std::fputs("Flux : impossible de créer la fenêtre\n", stderr);
            m_quitRequested = true;
            return;
        }

        auto renderer = Renderer::Create(*m_window);
        if (!renderer) {
            std::fprintf(stderr, "Flux : %s\n", renderer.error().c_str());
            m_quitRequested = true;
            return;
        }
        m_renderer = std::move(renderer.value());
        m_ui = std::make_unique<UI::Context>(m_renderer->GetOverlay().GetFont());
        m_ui->SetColorPicker([window = m_window.get()](Math::LinearColor& color) {
            const std::optional<Math::LinearColor> picked = Platform::ShowColorDialog(*window, color);
            if (picked) {
                color = *picked;
            }
            return picked.has_value();
        });

        // Un peu en arrière et au-dessus de la grille de test, légèrement vers le bas.
        m_camera.position.offset = {0.0, 6.0, 30.0};
        m_camera.pitch = -0.2f;
    }

    // Défini ici, où Renderer est complet, pour que unique_ptr<Renderer> puisse le détruire.
    Engine::~Engine() = default;

    int Engine::Run() {
        if (m_quitRequested) {
            return 1;
        }

        FrameStats stats;
        Clock::time_point previousFrame = Clock::now();
        while (!m_quitRequested && m_window->IsOpen()) {
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

            // Souris ignorée par l'UI pendant le vol libre : le curseur est caché et immobile.
            m_ui->BeginFrame(m_window->GetInput(), {static_cast<float>(size.x), static_cast<float>(size.y)}, deltaSeconds,
                             !m_window->IsCursorLocked());
            // Pendant le jeu, la caméra suit le personnage du joueur s'il y en a un ; sinon, vol libre.
            if (GetFollowedPlayer() != NullEntity) {
                RotateFollowCamera();
            } else {
                m_followRotating = false;
                m_camera.roll = 0.0f; // vol libre : l'horizon reste droit, même après la vue cockpit
                m_cameraController.Update(m_camera, *m_window, deltaSeconds, !m_ui->WantsMouse(), !m_ui->WantsKeyboard());
            }
            if (m_gameState != GameState::Stopped) {
                const double gameDelta = m_gameState == GameState::Running ? deltaSeconds : 0.0;
                UpdateGameSystems(gameDelta);
                if (m_gameUpdate) {
                    m_gameUpdate(*this, gameDelta);
                }
            }
            // Après la physique et le jeu : la caméra voit le personnage là où il est dessiné.
            if (const Entity player = GetFollowedPlayer(); player != NullEntity) {
                PlaceFollowCamera(player, deltaSeconds);
            } else {
                m_followShown = -1.0f;
                m_followShip = NullEntity;
            }
            if (m_editorUpdate) {
                m_editorUpdate(*this, deltaSeconds);
            }
            // Après les mises à jour (la scène de cette frame), sous les panneaux (ajoutés par EndFrame).
            if (m_drawScreenUi) {
                ScreenUi::Draw(m_scene, m_renderer->GetOverlay(), {}, {static_cast<float>(size.x), static_cast<float>(size.y)},
                               {.textures = [this](std::string_view path) { return LoadAssetTexture(path); }});
            }
            if (m_showDebugOverlay) {
                DrawEnginePanel(*m_ui, *m_renderer, stats, m_camera, m_cameraController);
            }
            m_ui->EndFrame(m_renderer->GetOverlay());

            m_renderer->SetAssetsDirectory(m_project ? m_project->assetsDirectory : std::filesystem::path{});
            if (!m_renderer->RenderFrame(m_scene, m_camera)) {
                return 1;
            }
            LoadRequestedScene();
        }
        return 0;
    }

    void Engine::RequestScene(std::string relativePath) {
        m_requestedScene = std::move(relativePath);
    }

    void Engine::UpdateGameSystems(double deltaSeconds) {
        if (m_gameState != GameState::Running) {
            return; // en pause : ni minuterie ni clic
        }
        // Clic ou touche de confirmation, sauf s'ils visent l'interface (panneaux de l'éditeur, champ en saisie).
        const Input& input = m_window->GetInput();
        const bool confirmed = (input.WasMouseButtonPressed(MouseButton::Left) && !m_ui->WantsMouse())
                            || ((input.WasKeyPressed(Key::Enter) || input.WasKeyPressed(Key::NumpadEnter)
                                 || input.WasKeyPressed(Key::Space))
                                && !m_ui->WantsKeyboard());
        if (std::optional<std::string> next = SceneChange::Update(m_scene, deltaSeconds, confirmed)) {
            RequestScene(std::move(*next));
        }

        if (!m_physics) {
            m_physics = std::make_unique<PhysicsWorld>();
            m_physics->SetAssetsDirectory(m_project ? m_project->assetsDirectory : std::filesystem::path{});
        }
        const Entity player = m_physics->GetPlayer();
        const auto* playerCharacter = m_scene.IsValid(player) ? m_scene.TryGet<CharacterComponent>(player) : nullptr;
        const bool seated = playerCharacter && playerCharacter->seat != NullEntity;
        const auto axis = [&](Key positive, Key negative) {
            return (input.IsKeyDown(positive) ? 1.0f : 0.0f) - (input.IsKeyDown(negative) ? 1.0f : 0.0f);
        };

        // Personnage du joueur : ZQSD (WASD en QWERTY) par rapport à la caméra, Maj pour courir, Espace pour sauter.
        // Assis : les mêmes touches pilotent le vaisseau (poste de pilotage).
        CharacterInput character;
        SeatInput seatInput;
        if (!m_ui->WantsKeyboard()) {
            seatInput.interact = input.WasKeyPressed(Key::E);
            if (seated) {
                seatInput.thrust = axis(Key::W, Key::S);
                seatInput.yaw = axis(Key::A, Key::D);
                seatInput.pitch = axis(Key::Space, Key::LeftControl);
                if (input.WasKeyPressed(Key::V)) {
                    m_cockpitView = !m_cockpitView;
                }
            } else {
                // Avant et droite de la caméra, ramenés à l'horizontale.
                const Math::Vector3f forward{-std::sin(m_camera.yaw), 0.0f, -std::cos(m_camera.yaw)};
                const Math::Vector3f right{std::cos(m_camera.yaw), 0.0f, -std::sin(m_camera.yaw)};
                character.move = forward * axis(Key::W, Key::S) + right * axis(Key::D, Key::A);
                character.run = input.IsKeyDown(Key::LeftShift);
                character.jump = input.WasKeyPressed(Key::Space);
            }
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

    void Engine::SetGameState(GameState state) {
        m_gameState = state;
        if (state == GameState::Stopped) {
            m_physics.reset();
        }
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
        const bool started = input.WasMouseButtonPressed(MouseButton::Right) && !m_ui->WantsMouse();
        m_followRotating = held && (m_followRotating || started);
        m_window->SetCursorLocked(m_followRotating);
        if (m_followRotating) {
            constexpr float Sensitivity = 0.0025f;
            const Math::Vector2f mouse = input.GetMouseDelta();
            if (GetCockpitSeat(GetFollowedPlayer()) != NullEntity) {
                // Aux commandes : on tourne la tête, sans perdre le vaisseau de vue.
                m_lookYaw = std::clamp(m_lookYaw - mouse.x * Sensitivity, -2.0f, 2.0f);
                m_lookPitch = std::clamp(m_lookPitch - mouse.y * Sensitivity, -1.2f, 1.2f);
            } else {
                m_camera.yaw = std::remainder(m_camera.yaw - mouse.x * Sensitivity, 2.0f * std::numbers::pi_v<float>);
                m_camera.pitch = std::clamp(m_camera.pitch - mouse.y * Sensitivity, -FollowMaxPitch, FollowMaxPitch);
            }
        }
        if (const float wheel = input.GetWheelDelta(); wheel != 0.0f && !m_ui->WantsMouse()) {
            m_followDistance = std::clamp(m_followDistance * std::pow(0.85f, wheel), 1.0f, 30.0f);
        }
    }

    Entity Engine::GetCockpitSeat(Entity player) const {
        return m_cockpitView && player != NullEntity ? Seats::GetPilotSeat(m_scene, player) : NullEntity;
    }

    void Engine::TurnWithShip(Entity player) {
        // Le vaisseau a tourné depuis la dernière image : la caméra aussi, comme la tête de quelqu'un à bord.
        const Entity ship = m_scene.GetShip(player);
        const Math::Quaternionf rotation = ship != NullEntity ? m_scene.GetWorldRotation(ship) : Math::Quaternionf{};
        if (ship != NullEntity && ship == m_followShip) {
            const Math::Quaternionf turn = Math::Normalize(rotation * m_followShipRotation.Conjugate());
            const Math::Vector3f forward = turn.Rotate(m_camera.GetForward());
            m_camera.yaw = std::atan2(-forward.x, -forward.z);
            m_camera.pitch = std::clamp(std::asin(std::clamp(forward.y, -1.0f, 1.0f)), -FollowMaxPitch, FollowMaxPitch);
        }
        m_followShip = ship;
        m_followShipRotation = rotation;
    }

    void Engine::PlaceFollowCamera(Entity player, double deltaSeconds) {
        TurnWithShip(player);
        if (const Entity seat = GetCockpitSeat(player); seat != NullEntity) {
            // Vue cockpit : depuis l'œil du pilote, dans l'axe du siège (roulis du vaisseau compris).
            const Math::Quaternionf seatRotation = m_scene.GetWorldRotation(seat);
            m_camera.position = m_scene.GetWorldPosition(seat);
            m_camera.position += Math::Vector3d(seatRotation.Rotate(m_scene.Get<SeatComponent>(seat).eye));
            m_camera.SetRotation(seatRotation * Math::Quaternionf::FromAxisAngle({0.0f, 1.0f, 0.0f}, m_lookYaw)
                                 * Math::Quaternionf::FromAxisAngle({1.0f, 0.0f, 0.0f}, m_lookPitch));
            m_followShown = -1.0f; // en ressortant, la caméra se replace d'un coup derrière le personnage
            return;
        }
        m_lookYaw = 0.0f;
        m_lookPitch = 0.0f;
        m_camera.roll = 0.0f;
        m_camera.pitch = std::clamp(m_camera.pitch, -FollowMaxPitch, FollowMaxPitch);

        // Derrière le personnage, visant un peu sous le haut de sa tête.
        const auto* character = m_scene.TryGet<CharacterComponent>(player);
        const double eyeHeight = character ? character->height * 0.9 : 1.6;
        Math::WorldPosition target = m_scene.GetWorldPosition(player);
        target += Math::Vector3d{0.0, eyeHeight, 0.0};

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
    }

    void Engine::LoadRequestedScene() {
        std::optional<std::string> requested = std::exchange(m_requestedScene, std::nullopt);
        if (!requested || m_gameState == GameState::Stopped) {
            return; // demande faite juste avant l'arrêt du jeu (éditeur) : la scène éditée est déjà restaurée
        }
        if (!m_project) {
            std::fprintf(stderr, "Flux : changement de scène sans projet ouvert (%s)\n", requested->c_str());
            return;
        }
        const std::filesystem::path path = m_project->assetsDirectory / Platform::PathFromUtf8(*requested);
        if (auto loaded = SceneSerializer::Load(m_scene, m_components, path); !loaded) {
            std::fprintf(stderr, "Flux : scène %s : %s\n", requested->c_str(), loaded.error().c_str());
            return;
        }
        m_physics.reset(); // les corps de l'ancienne scène : recréés d'après la nouvelle à la prochaine image
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
