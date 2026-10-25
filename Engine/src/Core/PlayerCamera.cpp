#include "Core/PlayerCamera.hpp"

#include "Flux/Core/Engine.hpp"
#include "Flux/Physics/PhysicsWorld.hpp"
#include "Flux/Platform/Input.hpp"
#include "Flux/Platform/Window.hpp"
#include "Flux/Scene/Seats.hpp"
#include "Flux/UI/Context.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace Flux {
    namespace {
        // Caméra qui suit le personnage : un peu moins que la verticale, au-dessus comme en dessous.
        constexpr float FollowMaxPitch = 1.3f;
    }

    void PlayerCamera::HandleInput(Engine& engine, Entity player) {
        Window& window = engine.GetWindow();
        const UI::Context& ui = engine.GetUi();
        Scene& scene = engine.GetScene();
        // Comme le vol libre : clic droit maintenu (commencé hors de l'interface) pour tourner autour du personnage.
        const Input& input = window.GetInput();
        const bool held = window.HasFocus() && input.IsMouseButtonDown(MouseButton::Right);
        const bool started = input.WasMouseButtonPressed(MouseButton::Right) && !ui.WantsMouse() && !engine.InterfaceWantsMouse();
        m_followRotating = held && (m_followRotating || started);
        constexpr float Sensitivity = 0.0025f;
        const Math::Vector2f mouse = input.GetMouseDelta();
        // Aux commandes, sans clic droit : la souris dirige le vaisseau, curseur caché. Alt maintenu le libère (panneaux
        // de l'éditeur), comme un menu qui met le jeu en pause.
        const bool piloting = Seats::GetPilotSeat(scene, player) != NullEntity;
        const bool steering = piloting && !m_followRotating && window.HasFocus() && !engine.IsInterfacePausing()
                           && engine.GetGameState() == GameState::Running && !input.IsKeyDown(Key::LeftAlt)
                           && !input.IsKeyDown(Key::RightAlt);
        // La première image où le curseur se cache, son déplacement vient d'avant : ignoré.
        const bool wasLocked = window.IsCursorLocked();
        window.SetCursorLocked(m_followRotating || steering);
        m_steerYaw = steering && wasLocked ? -mouse.x * Sensitivity : 0.0f;
        m_steerPitch = steering && wasLocked ? -mouse.y * Sensitivity : 0.0f;
        if (m_followRotating) {
            if (Seats::GetPilotSeat(scene, player) != NullEntity) {
                // Aux commandes (cockpit ou vue extérieure) : on tourne la tête, sans perdre le vaisseau de vue.
                m_lookYaw = std::clamp(m_lookYaw - mouse.x * Sensitivity, -2.0f, 2.0f);
                m_lookPitch = std::clamp(m_lookPitch - mouse.y * Sensitivity, -1.2f, 1.2f);
            } else {
                m_followYaw = std::remainder(m_followYaw - mouse.x * Sensitivity, 2.0f * std::numbers::pi_v<float>);
                m_followPitch = std::clamp(m_followPitch - mouse.y * Sensitivity, -FollowMaxPitch, FollowMaxPitch);
            }
        }
        if (const float wheel = input.GetWheelDelta(); wheel != 0.0f && !ui.WantsMouse() && !engine.InterfaceWantsMouse()) {
            constexpr float MinDistance = 1.0f;
            constexpr float MinChaseZoom = 0.9f; // en tailles du vaisseau : plus près, on entrerait dans sa coque
            if (Seats::GetPilotSeat(scene, player) != NullEntity) {
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

    Entity PlayerCamera::GetCockpitSeat(const Scene& scene, Entity player) const {
        // Dès qu'il s'assoit (animation comprise) et jusqu'à ce qu'il soit debout : la vue part du siège, dans l'axe
        // du vaisseau, au lieu de rester tournée vers où regardait la caméra quand il est monté.
        if (!m_cockpitView || player == NullEntity) {
            return NullEntity;
        }
        const auto* character = scene.TryGet<CharacterComponent>(player);
        if (!character || character->seatState == SeatState::Standing || !scene.IsValid(character->seat)) {
            return NullEntity;
        }
        const auto* seat = scene.TryGet<SeatComponent>(character->seat);
        return seat && seat->pilot ? character->seat : NullEntity;
    }

    void PlayerCamera::TurnWithShip(Engine& engine, Entity player) {
        Scene& scene = engine.GetScene();
        const Math::Vector3f up = engine.GetPhysics()->GetUp(scene, player);
        const Math::Vector3f y{0.0f, 1.0f, 0.0f};
        if (!m_followFrameSet) {
            // Début du suivi : la caméra garde sa direction (celle de l'éditeur), dans l'horizontale de l'endroit.
            m_followFrame = Math::Quaternionf::FromTo(y, up);
            m_followFrameSet = true;
            Aim(engine.GetCamera());
        }
        // Le vaisseau a tourné depuis la dernière image : la caméra aussi, comme la tête de quelqu'un à bord.
        const Entity ship = scene.GetShip(player);
        const Math::Quaternionf rotation = ship != NullEntity ? scene.GetWorldRotation(ship) : Math::Quaternionf{};
        if (ship != NullEntity && ship == m_followShip) {
            m_followFrame = Math::Normalize(rotation * m_followShipRotation.Conjugate() * m_followFrame);
        }
        m_followShip = ship;
        m_followShipRotation = rotation;
        // Redressée au plus court sur le haut de l'endroit : en marchant autour d'une planète, l'horizon reste droit.
        m_followFrame = Math::Normalize(Math::Quaternionf::FromTo(m_followFrame.Rotate(y), up) * m_followFrame);
    }

    void PlayerCamera::Aim(const Camera& camera) {
        const Math::Vector3f forward = m_followFrame.Conjugate().Rotate(camera.GetForward());
        m_followYaw = std::atan2(-forward.x, -forward.z);
        m_followPitch = std::clamp(std::asin(std::clamp(forward.y, -1.0f, 1.0f)), -FollowMaxPitch, FollowMaxPitch);
    }

    void PlayerCamera::MeasureShip(Engine& engine, Entity ship) {
        Scene& scene = engine.GetScene();
        // Boîte des modèles du vaisseau dans son repère (sans les personnages à bord) : la caméra vise son centre,
        // à une distance proportionnelle à sa taille.
        m_chaseShip = ship;
        Math::Vector3f min{1.0e9f, 1.0e9f, 1.0e9f};
        Math::Vector3f max = min * -1.0f;
        const auto visit = [&](auto&& self, Entity entity, const Math::Vector3d& position, const Math::Quaternionf& rotation,
                               const Math::Vector3f& scale) -> void {
            if (scene.TryGet<CharacterComponent>(entity)) {
                return;
            }
            if (scene.TryGet<ModelComponent>(entity) || scene.TryGet<MeshRendererComponent>(entity)) {
                if (const std::optional<Math::Box3f> box = engine.GetMeshBounds(entity)) {
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
            scene.ForEachChild(entity, [&](Entity child) {
                const TransformComponent& transform = scene.Get<TransformComponent>(child);
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

    void PlayerCamera::PlaceChase(Engine& engine, Entity seat, double deltaSeconds) {
        Scene& scene = engine.GetScene();
        Camera& camera = engine.GetCamera();
        const Entity ship = scene.GetShip(seat);
        if (ship == NullEntity) {
            return;
        }
        if (ship != m_chaseShip) {
            MeasureShip(engine, ship);
            m_chaseShown = -1.0f;
        }
        // Derrière le vaisseau, un peu au-dessus (on regarde légèrement vers le bas), tournée avec lui, roulis compris ;
        // le clic droit tourne autour.
        constexpr float LookDown = 0.2f;
        const Math::Quaternionf shipRotation = scene.GetWorldRotation(ship);
        const Math::Quaternionf rotation = shipRotation * Math::Quaternionf::FromAxisAngle({0.0f, 1.0f, 0.0f}, m_lookYaw)
                                         * Math::Quaternionf::FromAxisAngle({1.0f, 0.0f, 0.0f}, m_lookPitch - LookDown);
        Math::WorldPosition target = scene.GetWorldPosition(ship);
        target += Math::Vector3d(shipRotation.Rotate(m_chaseCenter));
        const Math::Vector3f back = rotation.Rotate(Math::Vector3f{0.0f, 0.0f, 1.0f});
        const float distance = m_chaseRadius * m_chaseZoom;
        // Un mur derrière le vaisseau (hangar) : la caméra s'avance, sans que le vaisseau lui-même l'arrête.
        constexpr float CameraRadius = 0.2f;
        const float free = engine.GetPhysics()->CastCamera(scene, seat, target, back, distance, CameraRadius, ship);
        if (m_chaseShown < 0.0f || free < m_chaseShown) {
            m_chaseShown = free;
        } else {
            constexpr double ReturnRate = 6.0;
            m_chaseShown += (free - m_chaseShown) * static_cast<float>(1.0 - std::exp(-ReturnRate * deltaSeconds));
        }
        camera.position = target;
        camera.position += Math::Vector3d(back) * static_cast<double>(m_chaseShown);
        camera.SetRotation(rotation);
    }

    void PlayerCamera::Place(Engine& engine, Entity player, double deltaSeconds) {
        Scene& scene = engine.GetScene();
        Camera& camera = engine.GetCamera();
        m_hiddenPlayer = NullEntity;
        TurnWithShip(engine, player);
        if (const Entity pilotSeat = Seats::GetPilotSeat(scene, player); pilotSeat != NullEntity && !m_cockpitView) {
            PlaceChase(engine, pilotSeat, deltaSeconds);
            Aim(camera); // en se relevant, elle regarde où regardait la vue extérieure
            m_followShown = -1.0f; // en se relevant, la caméra se replace d'un coup derrière le personnage
            return;
        }
        m_chaseShown = -1.0f;
        if (const Entity seat = GetCockpitSeat(scene, player); seat != NullEntity) {
            // Vue cockpit : depuis l'œil du pilote, dans l'axe du siège (roulis du vaisseau compris).
            const Math::Quaternionf seatRotation = scene.GetWorldRotation(seat);
            camera.position = scene.GetWorldPosition(seat);
            camera.position += Math::Vector3d(seatRotation.Rotate(scene.Get<SeatComponent>(seat).eye));
            camera.SetRotation(seatRotation * Math::Quaternionf::FromAxisAngle({0.0f, 1.0f, 0.0f}, m_lookYaw)
                                 * Math::Quaternionf::FromAxisAngle({1.0f, 0.0f, 0.0f}, m_lookPitch));
            Aim(camera); // en se relevant, elle regarde où regardait le pilote
            m_followShown = -1.0f; // en ressortant, la caméra se replace d'un coup derrière le personnage
            m_hiddenPlayer = player;
            return;
        }
        m_lookYaw = 0.0f;
        m_lookPitch = 0.0f;
        m_followPitch = std::clamp(m_followPitch, -FollowMaxPitch, FollowMaxPitch);
        camera.SetRotation(m_followFrame * Math::Quaternionf::FromYawPitchRoll(m_followYaw, m_followPitch, 0.0f));

        // Derrière le personnage, visant un peu sous le haut de sa tête.
        const auto* character = scene.TryGet<CharacterComponent>(player);
        const float eyeHeight = character ? character->height * 0.9f : 1.6f;
        Math::WorldPosition target = scene.GetWorldPosition(player);
        target += Math::Vector3d(m_followFrame.Rotate(Math::Vector3f{0.0f, eyeHeight, 0.0f}));

        if (m_firstPerson) {
            // Depuis ses yeux : rien entre la tête et la caméra, pas besoin de chercher les murs. Son ombre reste.
            camera.position = target;
            m_followShown = 0.0f;
            m_hiddenPlayer = player;
            return;
        }

        // Un mur entre le personnage et la caméra : elle s'avance devant lui. Sphère un peu plus grosse que ce que
        // le plan proche découpe (0,1 m devant l'œil, écran 16/9 : 0,12 m au coin), pour ne jamais voir à travers.
        constexpr float CameraRadius = 0.2f;
        const Math::Vector3f back = camera.GetForward() * -1.0f;
        const float free = engine.GetPhysics()->CastCamera(scene, player, target, back, m_followDistance, CameraRadius);
        // Elle rentre d'un coup (sinon on verrait le mur de l'intérieur) et ressort en douceur (pas de bond quand
        // le mur ne cache plus rien, par exemple en passant une porte).
        if (m_followShown < 0.0f || free < m_followShown) {
            m_followShown = free;
        } else {
            constexpr double ReturnRate = 6.0; // par seconde : les deux tiers du chemin en 1/6 s
            m_followShown += (free - m_followShown) * static_cast<float>(1.0 - std::exp(-ReturnRate * deltaSeconds));
        }

        camera.position = target;
        camera.position += Math::Vector3d(back) * static_cast<double>(m_followShown);
        // Caméra poussée contre la tête (dos au mur, sous une verrière) : on verrait l'intérieur du crâne, on ne
        // dessine plus le personnage (vue à la première personne), comme la vue cockpit.
        constexpr float HideDistance = 0.5f;
        if (m_followShown < HideDistance) {
            m_hiddenPlayer = player;
        }
    }

    void PlayerCamera::Reset() {
        m_followShown = -1.0f;
        m_followShip = NullEntity;
        m_followFrameSet = false;
        m_hiddenPlayer = NullEntity;
    }

    Math::Vector3f PlayerCamera::GetWalkForward() const {
        return m_followFrame.Rotate(Math::Vector3f{-std::sin(m_followYaw), 0.0f, -std::cos(m_followYaw)});
    }

    Math::Vector3f PlayerCamera::GetWalkRight() const {
        return m_followFrame.Rotate(Math::Vector3f{std::cos(m_followYaw), 0.0f, -std::sin(m_followYaw)});
    }
}
