#include "Flux/Physics/PhysicsWorld.hpp"

#include "Flux/Scene/Scene.hpp"

// Jolt : ses en-têtes seulement ici (et jamais dans un en-tête public).
#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace Flux {
    namespace {
        // Pas de simulation fixe : la même physique à 30 ou 144 images/s.
        constexpr float StepSeconds = 1.0f / 60.0f;
        // Au plus tant de pas par image : après un blocage, la simulation ralentit au lieu de s'emballer.
        constexpr int MaxStepsPerUpdate = 4;

        // Deux calques : les objets immobiles ne se testent pas entre eux.
        namespace Layers {
            constexpr JPH::ObjectLayer NonMoving = 0;
            constexpr JPH::ObjectLayer Moving = 1;
            constexpr JPH::uint Count = 2;
        }

        class BroadPhaseLayers final : public JPH::BroadPhaseLayerInterface {
            public:
            JPH::uint GetNumBroadPhaseLayers() const override { return Layers::Count; }
            JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
                return JPH::BroadPhaseLayer(static_cast<JPH::BroadPhaseLayer::Type>(layer));
            }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
            const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override {
                return static_cast<JPH::BroadPhaseLayer::Type>(layer) == Layers::NonMoving ? "immobile" : "mobile";
            }
#endif
        };

        class ObjectVsBroadPhase final : public JPH::ObjectVsBroadPhaseLayerFilter {
            public:
            bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer broadPhase) const override {
                return layer == Layers::Moving || static_cast<JPH::BroadPhaseLayer::Type>(broadPhase) == Layers::Moving;
            }
        };

        class ObjectPairs final : public JPH::ObjectLayerPairFilter {
            public:
            bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
                return a == Layers::Moving || b == Layers::Moving;
            }
        };

        // Initialisation globale de Jolt (allocateur, fabrique de types) : une fois pour tous les mondes.
        std::mutex g_joltMutex;
        int g_joltUsers = 0;

        void AcquireJolt() {
            const std::scoped_lock lock(g_joltMutex);
            if (g_joltUsers++ == 0) {
                JPH::RegisterDefaultAllocator();
                JPH::Factory::sInstance = new JPH::Factory();
                JPH::RegisterTypes();
            }
        }

        void ReleaseJolt() {
            const std::scoped_lock lock(g_joltMutex);
            if (--g_joltUsers == 0) {
                JPH::UnregisterTypes();
                delete JPH::Factory::sInstance;
                JPH::Factory::sInstance = nullptr;
            }
        }

        JPH::Vec3 ToJolt(const Math::Vector3f& v) { return {v.x, v.y, v.z}; }
        JPH::Vec3 ToJolt(const Math::Vector3d& v) {
            return {static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)};
        }
        JPH::Quat ToJolt(const Math::Quaternionf& q) { return {q.x, q.y, q.z, q.w}; }
        Math::Vector3f FromJolt(JPH::Vec3Arg v) { return {v.GetX(), v.GetY(), v.GetZ()}; }
        Math::Quaternionf FromJolt(JPH::QuatArg q) { return {q.GetX(), q.GetY(), q.GetZ(), q.GetW()}; }

        float MaxComponent(const Math::Vector3f& v) {
            return std::max({std::abs(v.x), std::abs(v.y), std::abs(v.z)});
        }

        // Forme de base de la collision (sans le décalage du centre), à l'échelle de l'entité.
        JPH::ShapeSettings::ShapeResult CreateBaseShape(const ColliderComponent& collider, const Math::Vector3f& absScale) {
            constexpr float MinSize = 1.0e-3f; // une forme plate ou nulle n'a pas de volume : Jolt la refuserait
            if (collider.shape == ColliderShape::Sphere) {
                return JPH::SphereShapeSettings(std::max(collider.radius * MaxComponent(absScale), MinSize)).Create();
            }
            if (collider.shape == ColliderShape::Capsule) {
                const float radius = std::max(collider.radius * std::max(absScale.x, absScale.z), MinSize);
                const float halfCylinder = std::max(collider.height * absScale.y * 0.5f - radius, MinSize);
                return JPH::CapsuleShapeSettings(halfCylinder, radius).Create();
            }
            const JPH::Vec3 half = JPH::Vec3(std::max(collider.size.x * absScale.x, MinSize), std::max(collider.size.y * absScale.y, MinSize),
                                             std::max(collider.size.z * absScale.z, MinSize)) * 0.5f;
            // L'arrondi des arêtes ne peut pas dépasser la moitié du plus petit côté (sol fin).
            const float convexRadius = std::min(JPH::cDefaultConvexRadius, half.ReduceMin() * 0.5f);
            return JPH::BoxShapeSettings(half, convexRadius).Create();
        }

        // Forme de la collision, à l'échelle de l'entité. nullptr (et un message) si Jolt la refuse.
        JPH::RefConst<JPH::Shape> CreateShape(const ColliderComponent& collider, const Math::Vector3f& scale) {
            const Math::Vector3f absScale{std::abs(scale.x), std::abs(scale.y), std::abs(scale.z)};
            JPH::ShapeSettings::ShapeResult result = CreateBaseShape(collider, absScale);
            if (result.HasError()) {
                std::fprintf(stderr, "Flux : forme de collision refusée : %s\n", result.GetError().c_str());
                return nullptr;
            }
            JPH::RefConst<JPH::Shape> shape = result.Get();
            const Math::Vector3f center{collider.center.x * scale.x, collider.center.y * scale.y, collider.center.z * scale.z};
            if (center != Math::Vector3f{}) {
                result = JPH::RotatedTranslatedShapeSettings(ToJolt(center), JPH::Quat::sIdentity(), shape).Create();
                if (result.HasError()) {
                    return nullptr;
                }
                shape = result.Get();
            }
            return shape;
        }

        struct Character {
            JPH::Ref<JPH::CharacterVirtual> character;
            float yaw = 0.0f; // direction du regard (le modèle regarde vers +z en glTF)
        };
    }

    struct PhysicsWorld::Impl {
        BroadPhaseLayers broadPhaseLayers;
        ObjectVsBroadPhase objectVsBroadPhase;
        ObjectPairs objectPairs;
        std::unique_ptr<JPH::TempAllocatorImpl> tempAllocator;
        std::unique_ptr<JPH::JobSystemThreadPool> jobSystem;
        std::unique_ptr<JPH::PhysicsSystem> system;

        bool anchored = false;
        Math::WorldPosition anchor; // origine de la simulation
        std::unordered_map<Entity, JPH::BodyID> bodies;
        std::unordered_map<Entity, Character> characters;
        std::vector<RelativeTransform> transforms;
        double accumulator = 0.0;
        Entity player = NullEntity;
        bool warnedChild = false;

        Impl() {
            AcquireJolt();
            tempAllocator = std::make_unique<JPH::TempAllocatorImpl>(16 * 1024 * 1024);
            // Tous les cœurs sauf un (le thread principal, qui dessine), au moins un.
            const int threads = std::max(1, static_cast<int>(std::thread::hardware_concurrency()) - 1);
            jobSystem = std::make_unique<JPH::JobSystemThreadPool>(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, threads);
            system = std::make_unique<JPH::PhysicsSystem>();
            constexpr JPH::uint MaxBodies = 65536;
            constexpr JPH::uint BodyMutexes = 0; // 0 : valeur par défaut de Jolt
            constexpr JPH::uint MaxBodyPairs = 65536;
            constexpr JPH::uint MaxContacts = 10240;
            system->Init(MaxBodies, BodyMutexes, MaxBodyPairs, MaxContacts, broadPhaseLayers, objectVsBroadPhase, objectPairs);
        }

        ~Impl() {
            characters.clear(); // leurs corps internes vivent dans le système
            JPH::BodyInterface& bodyInterface = system->GetBodyInterface();
            for (const auto& [entity, id] : bodies) {
                bodyInterface.RemoveBody(id);
                bodyInterface.DestroyBody(id);
            }
            system.reset();
            jobSystem.reset();
            tempAllocator.reset();
            ReleaseJolt();
        }

        // Transformation d'une entité par rapport à l'ancre, d'après la liste calculée pour cette mise à jour.
        const RelativeTransform* FindTransform(Entity entity) const {
            const auto found = std::ranges::find(transforms, entity, &RelativeTransform::entity);
            return found == transforms.end() ? nullptr : &*found;
        }

        void ChooseAnchor(const Scene& scene) {
            // Le secteur du premier objet physique : la simulation se passe autour de lui.
            const entt::registry& registry = scene.GetRegistry();
            for (const Entity entity : registry.view<const ColliderComponent>()) {
                anchor = {scene.GetWorldPosition(entity).sector, {}};
                anchored = true;
                return;
            }
            for (const Entity entity : registry.view<const CharacterComponent>()) {
                anchor = {scene.GetWorldPosition(entity).sector, {}};
                anchored = true;
                return;
            }
        }

        void CreateBody(const Scene& scene, Entity entity, const ColliderComponent& collider, const RelativeTransform& transform) {
            const JPH::RefConst<JPH::Shape> shape = CreateShape(collider, transform.scale);
            if (!shape) {
                bodies.emplace(entity, JPH::BodyID{}); // pas de nouvel essai à chaque image
                return;
            }
            const auto* rigidBody = scene.TryGet<RigidBodyComponent>(entity);
            BodyMotion motion = rigidBody ? rigidBody->motion : BodyMotion::Static;
            if (motion != BodyMotion::Static && scene.GetParent(entity) != NullEntity) {
                // La physique réécrirait la position locale comme si elle était celle du monde.
                if (!warnedChild) {
                    std::fprintf(stderr, "Flux : un corps physique mobile doit être une racine de la hiérarchie : "
                                         "« %s » est traité comme immobile\n",
                                 scene.Get<NameComponent>(entity).name.c_str());
                    warnedChild = true;
                }
                motion = BodyMotion::Static;
            }
            const JPH::EMotionType motionType = motion == BodyMotion::Dynamic   ? JPH::EMotionType::Dynamic
                                              : motion == BodyMotion::Kinematic ? JPH::EMotionType::Kinematic
                                                                                : JPH::EMotionType::Static;
            JPH::BodyCreationSettings settings(shape, JPH::RVec3(ToJolt(transform.position)), ToJolt(transform.rotation).Normalized(),
                                               motionType, motion == BodyMotion::Static ? Layers::NonMoving : Layers::Moving);
            settings.mUserData = static_cast<JPH::uint64>(entt::to_integral(entity));
            if (rigidBody) {
                settings.mFriction = rigidBody->friction;
                settings.mRestitution = rigidBody->restitution;
                settings.mGravityFactor = rigidBody->gravityFactor;
                if (rigidBody->mass > 0.0f && motion == BodyMotion::Dynamic) {
                    settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
                    settings.mMassPropertiesOverride.mMass = rigidBody->mass;
                }
            }
            const JPH::BodyID id = system->GetBodyInterface().CreateAndAddBody(
                settings, motion == BodyMotion::Static ? JPH::EActivation::DontActivate : JPH::EActivation::Activate);
            bodies.emplace(entity, id);
        }

        void CreateCharacter(const Scene& scene, Entity entity, const CharacterComponent& component, const RelativeTransform& transform) {
            const float radius = std::max(component.radius, 0.05f);
            const float halfCylinder = std::max(component.height * 0.5f - radius, 0.01f);
            // Capsule posée sur l'origine de l'entité : ses pieds.
            const JPH::RefConst<JPH::Shape> shape = JPH::RotatedTranslatedShapeSettings(
                JPH::Vec3(0.0f, halfCylinder + radius, 0.0f), JPH::Quat::sIdentity(), new JPH::CapsuleShape(halfCylinder, radius)).Create().Get();

            JPH::Ref<JPH::CharacterVirtualSettings> settings = new JPH::CharacterVirtualSettings();
            settings->mShape = shape;
            settings->mMaxSlopeAngle = JPH::DegreesToRadians(50.0f);
            // Seuls les contacts sous le centre de la sphère du bas comptent comme sol.
            settings->mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -radius);
            // Corps interne : les objets dynamiques et les rayons voient le personnage.
            settings->mInnerBodyShape = shape;
            settings->mInnerBodyLayer = Layers::Moving;

            Character character;
            // Seul le cap (autour de y) compte : un personnage reste debout.
            const Math::Vector3f forward = transform.rotation.Rotate(Math::Vector3f{0.0f, 0.0f, 1.0f});
            character.yaw = std::atan2(forward.x, forward.z);
            character.character = new JPH::CharacterVirtual(settings, JPH::RVec3(ToJolt(transform.position)),
                                                            JPH::Quat::sRotation(JPH::Vec3::sAxisY(), character.yaw),
                                                            static_cast<JPH::uint64>(entt::to_integral(entity)), system.get());
            characters.emplace(entity, std::move(character));
            (void)scene;
        }

        // Nouvelles entités, entités détruites, composants ajoutés ou retirés.
        void Synchronize(Scene& scene) {
            JPH::BodyInterface& bodyInterface = system->GetBodyInterface();
            std::erase_if(bodies, [&](const auto& entry) {
                const auto [entity, id] = entry;
                if (scene.IsValid(entity) && scene.TryGet<ColliderComponent>(entity) && !scene.TryGet<CharacterComponent>(entity)) {
                    return false;
                }
                if (!id.IsInvalid()) {
                    bodyInterface.RemoveBody(id);
                    bodyInterface.DestroyBody(id);
                }
                return true;
            });
            std::erase_if(characters, [&](const auto& entry) {
                return !scene.IsValid(entry.first) || !scene.TryGet<CharacterComponent>(entry.first);
            });

            entt::registry& registry = scene.GetRegistry();
            player = NullEntity;
            for (const auto [entity, component] : registry.view<const CharacterComponent>().each()) {
                if (component.player && player == NullEntity) {
                    player = entity;
                }
                if (!characters.contains(entity)) {
                    if (scene.GetParent(entity) != NullEntity) {
                        continue; // un personnage enfant suivrait deux maîtres : ignoré
                    }
                    if (const RelativeTransform* transform = FindTransform(entity)) {
                        CreateCharacter(scene, entity, component, *transform);
                    }
                }
            }
            for (const auto [entity, collider] : registry.view<const ColliderComponent>().each()) {
                if (bodies.contains(entity) || registry.all_of<CharacterComponent>(entity)) {
                    continue;
                }
                if (const RelativeTransform* transform = FindTransform(entity)) {
                    CreateBody(scene, entity, collider, *transform);
                }
            }
        }

        void MoveKinematicBodies(const Scene& scene) {
            JPH::BodyInterface& bodyInterface = system->GetBodyInterface();
            for (const auto& [entity, id] : bodies) {
                if (id.IsInvalid() || bodyInterface.GetMotionType(id) != JPH::EMotionType::Kinematic) {
                    continue;
                }
                if (const RelativeTransform* transform = FindTransform(entity)) {
                    bodyInterface.MoveKinematic(id, JPH::RVec3(ToJolt(transform->position)), ToJolt(transform->rotation).Normalized(), StepSeconds);
                }
                (void)scene;
            }
        }

        void UpdateCharacters(const Scene& scene, const CharacterInput& input) {
            const JPH::Vec3 gravity = system->GetGravity();
            const JPH::Vec3 up = -gravity.NormalizedOr(JPH::Vec3(0.0f, -1.0f, 0.0f));
            for (auto& [entity, character] : characters) {
                const CharacterComponent& component = scene.Get<CharacterComponent>(entity);
                JPH::CharacterVirtual& body = *character.character;
                body.SetUp(up);
                body.UpdateGroundVelocity();

                JPH::Vec3 wanted = JPH::Vec3::sZero();
                bool jump = false;
                if (component.player && entity == player) {
                    wanted = ToJolt(input.move);
                    wanted -= up * wanted.Dot(up); // dans le plan du sol
                    if (wanted.LengthSq() > 1.0f) {
                        wanted = wanted.Normalized();
                    }
                    wanted *= input.run ? component.runSpeed : component.walkSpeed;
                    jump = input.jump;
                }

                // Comme l'exemple CharacterVirtual de Jolt : au sol, on part de la vitesse du sol (plateforme qui
                // bouge) et on peut sauter ; en l'air, la vitesse verticale continue et la gravité s'ajoute.
                const JPH::Vec3 vertical = up * body.GetLinearVelocity().Dot(up);
                const JPH::Vec3 ground = body.GetGroundVelocity();
                JPH::Vec3 velocity;
                const bool onGround = body.GetGroundState() == JPH::CharacterVirtual::EGroundState::OnGround;
                if (onGround && (vertical - ground).Dot(up) < 0.1f) {
                    velocity = ground;
                    if (jump) {
                        velocity += up * component.jumpSpeed;
                    }
                } else {
                    velocity = vertical;
                }
                velocity += gravity * StepSeconds + wanted; // la direction reste pilotable en l'air
                body.SetLinearVelocity(velocity);

                // Le personnage se tourne peu à peu vers où il marche.
                if (wanted.LengthSq() > 1.0e-4f) {
                    const float target = std::atan2(wanted.GetX(), wanted.GetZ());
                    const float difference = std::remainder(target - character.yaw, 2.0f * JPH::JPH_PI);
                    constexpr float TurnSpeed = 12.0f; // rad/s
                    character.yaw += std::clamp(difference, -TurnSpeed * StepSeconds, TurnSpeed * StepSeconds);
                    body.SetRotation(JPH::Quat::sRotation(JPH::Vec3::sAxisY(), character.yaw));
                }

                JPH::CharacterVirtual::ExtendedUpdateSettings update;
                update.mStickToFloorStepDown = -up * 0.5f;
                update.mWalkStairsStepUp = up * 0.4f;
                body.ExtendedUpdate(StepSeconds, gravity, update, system->GetDefaultBroadPhaseLayerFilter(Layers::Moving),
                                    system->GetDefaultLayerFilter(Layers::Moving), {}, {}, *tempAllocator);
            }
        }

        // Position d'une racine d'après la simulation : secteur et décalage.
        void WriteRoot(Scene& scene, Entity entity, JPH::RVec3Arg position, JPH::QuatArg rotation) {
            Math::WorldPosition world = anchor;
            world += Math::Vector3d{position.GetX(), position.GetY(), position.GetZ()};
            TransformComponent& transform = scene.Get<TransformComponent>(entity);
            transform.position = world.offset;
            transform.rotation = FromJolt(rotation.Normalized());
            if (world.sector != Math::Vector3i64{}) {
                scene.Add<SectorComponent>(entity, world.sector);
            } else if (scene.TryGet<SectorComponent>(entity)) {
                scene.Remove<SectorComponent>(entity);
            }
        }

        void WriteBack(Scene& scene) {
            JPH::BodyInterface& bodyInterface = system->GetBodyInterface();
            for (const auto& [entity, id] : bodies) {
                if (id.IsInvalid() || bodyInterface.GetMotionType(id) != JPH::EMotionType::Dynamic || !bodyInterface.IsActive(id)) {
                    continue; // endormi : n'a pas bougé
                }
                JPH::RVec3 position;
                JPH::Quat rotation;
                bodyInterface.GetPositionAndRotation(id, position, rotation);
                WriteRoot(scene, entity, position, rotation);
            }
            for (const auto& [entity, character] : characters) {
                WriteRoot(scene, entity, character.character->GetPosition(), character.character->GetRotation());
            }
        }

        void Update(Scene& scene, double deltaSeconds, const CharacterInput& input) {
            if (!anchored) {
                ChooseAnchor(scene);
                if (!anchored) {
                    return; // rien de physique dans la scène
                }
            }
            scene.ComputeRelativeTransforms(anchor, transforms);
            Synchronize(scene);

            accumulator = std::min(accumulator + deltaSeconds, static_cast<double>(StepSeconds * MaxStepsPerUpdate));
            bool stepped = false;
            while (accumulator >= StepSeconds) {
                accumulator -= StepSeconds;
                MoveKinematicBodies(scene);
                UpdateCharacters(scene, input);
                system->Update(StepSeconds, 1, tempAllocator.get(), jobSystem.get());
                stepped = true;
            }
            if (stepped) {
                WriteBack(scene);
            }
        }
    };

    PhysicsWorld::PhysicsWorld() : m_impl(std::make_unique<Impl>()) {}

    PhysicsWorld::~PhysicsWorld() = default;

    void PhysicsWorld::Update(Scene& scene, double deltaSeconds, const CharacterInput& input) {
        m_impl->Update(scene, deltaSeconds, input);
    }

    void PhysicsWorld::SetGravity(const Math::Vector3f& gravity) {
        m_impl->system->SetGravity(ToJolt(gravity));
    }

    Math::Vector3f PhysicsWorld::GetGravity() const {
        return FromJolt(m_impl->system->GetGravity());
    }

    Entity PhysicsWorld::GetPlayer() const {
        return m_impl->player;
    }

    int PhysicsWorld::GetBodyCount() const {
        return static_cast<int>(std::ranges::count_if(m_impl->bodies, [](const auto& entry) { return !entry.second.IsInvalid(); }));
    }
}
