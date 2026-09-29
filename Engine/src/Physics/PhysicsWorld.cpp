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
#include <memory>
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
        // Jolt exige une rotation normalisée ; une rotation nulle (fichier abîmé) devient l'identité.
        JPH::Quat ToJoltRotation(const Math::Quaternionf& q) {
            const JPH::Quat rotation = ToJolt(q);
            return rotation.LengthSq() > 1.0e-12f ? rotation.Normalized() : JPH::Quat::sIdentity();
        }
        Math::Vector3f FromJolt(JPH::Vec3Arg v) { return {v.GetX(), v.GetY(), v.GetZ()}; }
        Math::Quaternionf FromJolt(JPH::QuatArg q) { return {q.GetX(), q.GetY(), q.GetZ(), q.GetW()}; }

        float MaxComponent(const Math::Vector3f& v) {
            return std::max({std::abs(v.x), std::abs(v.y), std::abs(v.z)});
        }

        // Forme de base de la collision (sans le décalage du centre), à l'échelle de l'entité.
        JPH::ShapeSettings::ShapeResult CreateBaseShape(const ColliderComponent& collider, const Math::Vector3f& absScale,
                                                        bool simulated) {
            // Une forme plate ou nulle n'a pas de volume : Jolt la refuserait. Un objet simulé minuscule (quelques
            // milligrammes, inertie quasi nulle) rend la simulation instable jusqu'aux NaN : 5 cm au moins.
            const float MinSize = simulated ? 0.05f : 1.0e-3f;
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
        JPH::RefConst<JPH::Shape> CreateShape(const ColliderComponent& collider, const Math::Vector3f& scale, bool simulated) {
            const Math::Vector3f absScale{std::abs(scale.x), std::abs(scale.y), std::abs(scale.z)};
            JPH::ShapeSettings::ShapeResult result = CreateBaseShape(collider, absScale, simulated);
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
            float groundSpeed = 0.0f; // vitesse horizontale au dernier pas, pour les animations
            float airTime = 0.0f;     // secondes sans toucher un sol où tenir debout (saut, chute, pente trop raide)
        };
    }

    namespace {
        // Un monde Jolt : celui de la scène, ou celui d'un vaisseau (ShipComponent), dont les corps sont dans son
        // repère.
        struct Grid {
            Entity frame = NullEntity; // vaisseau ; NullEntity : le monde, autour de l'ancre
            std::unique_ptr<JPH::PhysicsSystem> system;
            std::unordered_map<Entity, JPH::BodyID> bodies;
            std::unordered_map<Entity, Character> characters;
            // Repère du vaisseau par rapport à l'ancre, pour cette mise à jour : les corps n'y voient ni sa position
            // ni sa rotation, seulement son échelle (celle de ses enfants dans le monde).
            Math::Vector3d framePosition;
            Math::Quaternionf frameRotation;
            Math::Vector3f frameScale{1.0f, 1.0f, 1.0f};

            // Transformation relative à l'ancre ramenée dans le repère de ce monde.
            RelativeTransform InFrame(const RelativeTransform& transform) const {
                if (frame == NullEntity) {
                    return transform;
                }
                const Math::Quaternionf inverse = frameRotation.Conjugate();
                return {transform.entity, inverse.Rotate(transform.position - framePosition),
                        Math::Normalize(inverse * transform.rotation), transform.scale};
            }
        };

        // Vaisseau le plus proche au-dessus de l'entité (elle-même exclue) : le monde physique où elle vit.
        Entity FrameOf(const Scene& scene, Entity entity) {
            for (Entity parent = scene.GetParent(entity); parent != NullEntity; parent = scene.GetParent(parent)) {
                if (scene.TryGet<ShipComponent>(parent)) {
                    return parent;
                }
            }
            return NullEntity;
        }
    }

    struct PhysicsWorld::Impl {
        BroadPhaseLayers broadPhaseLayers;
        ObjectVsBroadPhase objectVsBroadPhase;
        ObjectPairs objectPairs;
        std::unique_ptr<JPH::TempAllocatorImpl> tempAllocator;
        std::unique_ptr<JPH::JobSystemThreadPool> jobSystem;
        std::vector<std::unique_ptr<Grid>> grids; // [0] : le monde ; puis un par vaisseau

        bool anchored = false;
        Math::WorldPosition anchor; // origine du monde de la scène
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
            grids.push_back(CreateGrid(NullEntity));
        }

        ~Impl() {
            for (const std::unique_ptr<Grid>& grid : grids) {
                DestroyGrid(*grid);
            }
            grids.clear();
            jobSystem.reset();
            tempAllocator.reset();
            ReleaseJolt();
        }

        std::unique_ptr<Grid> CreateGrid(Entity frame) {
            auto grid = std::make_unique<Grid>();
            grid->frame = frame;
            grid->system = std::make_unique<JPH::PhysicsSystem>();
            // Le monde de la scène peut être grand ; un vaisseau, beaucoup moins.
            const bool world = frame == NullEntity;
            const JPH::uint maxBodies = world ? 65536 : 8192;
            constexpr JPH::uint BodyMutexes = 0; // 0 : valeur par défaut de Jolt
            const JPH::uint maxBodyPairs = world ? 65536 : 8192;
            const JPH::uint maxContacts = world ? 10240 : 4096;
            grid->system->Init(maxBodies, BodyMutexes, maxBodyPairs, maxContacts, broadPhaseLayers, objectVsBroadPhase, objectPairs);
            return grid;
        }

        static void DestroyGrid(Grid& grid) {
            grid.characters.clear(); // leurs corps internes vivent dans le système
            JPH::BodyInterface& bodyInterface = grid.system->GetBodyInterface();
            for (const auto& [entity, id] : grid.bodies) {
                if (!id.IsInvalid()) {
                    bodyInterface.RemoveBody(id);
                    bodyInterface.DestroyBody(id);
                }
            }
            grid.bodies.clear();
            grid.system.reset();
        }

        Grid* FindGrid(Entity frame) {
            const auto found = std::ranges::find(grids, frame, [](const std::unique_ptr<Grid>& grid) { return grid->frame; });
            return found == grids.end() ? nullptr : found->get();
        }

        // Transformation d'une entité par rapport à l'ancre, d'après la liste calculée pour cette mise à jour.
        const RelativeTransform* FindTransform(Entity entity) const {
            const auto found = std::ranges::find(transforms, entity, &RelativeTransform::entity);
            return found == transforms.end() ? nullptr : &*found;
        }

        void ChooseAnchor(const Scene& scene) {
            // Le secteur du premier objet physique : la simulation se passe autour de lui.
            const entt::registry& registry = scene.GetRegistry();
            const auto first = [&](auto view) {
                for (const Entity entity : view) {
                    anchor = {scene.GetWorldPosition(entity).sector, {}};
                    anchored = true;
                    return;
                }
            };
            first(registry.view<const ColliderComponent>());
            if (!anchored) {
                first(registry.view<const CharacterComponent>());
            }
            if (!anchored) {
                first(registry.view<const ShipComponent>());
            }
        }

        // Mouvement d'essai des vaisseaux (ShipComponent::velocity, angularVelocity), dans leurs propres axes.
        static void MoveShips(Scene& scene, double deltaSeconds) {
            for (const auto [entity, ship] : scene.GetRegistry().view<const ShipComponent>().each()) {
                if (ship.velocity == Math::Vector3f{} && ship.angularVelocity == Math::Vector3f{}) {
                    continue;
                }
                TransformComponent& transform = scene.Get<TransformComponent>(entity);
                const Math::Vector3d move = Math::Vector3d(transform.rotation.Rotate(ship.velocity)) * deltaSeconds;
                constexpr float RadiansPerDegree = JPH::JPH_PI / 180.0f;
                const float speed = Math::Length(ship.angularVelocity) * RadiansPerDegree;
                if (speed > 0.0f) {
                    const Math::Quaternionf turn = Math::Quaternionf::FromAxisAngle(ship.angularVelocity, speed * static_cast<float>(deltaSeconds));
                    transform.rotation = Math::Normalize(transform.rotation * turn);
                }
                if (scene.GetParent(entity) != NullEntity) {
                    transform.position += move; // dans l'espace de son parent (vaisseau dans un vaisseau)
                    continue;
                }
                // Racine : son secteur change quand il en franchit la limite.
                const SectorComponent* sector = scene.TryGet<SectorComponent>(entity);
                Math::WorldPosition world{sector ? sector->sector : Math::Vector3i64{}, transform.position};
                world += move;
                transform.position = world.offset;
                if (world.sector != Math::Vector3i64{}) {
                    scene.Add<SectorComponent>(entity, world.sector);
                } else if (sector) {
                    scene.Remove<SectorComponent>(entity);
                }
            }
        }

        void CreateBody(const Scene& scene, Grid& grid, Entity entity, const ColliderComponent& collider,
                        const RelativeTransform& transform) {
            // Coque d'un vaisseau : elle bouge avec lui, les objets de son parent s'y cognent.
            const bool hull = scene.TryGet<ShipComponent>(entity) != nullptr;
            bool simulated = collider.simulatePhysics && !hull;
            if (simulated && scene.GetParent(entity) != grid.frame) {
                // La physique réécrirait la position locale comme si elle était celle de son repère.
                if (!warnedChild) {
                    std::fprintf(stderr, "Flux : un objet simulé doit être une racine de la hiérarchie ou un enfant direct "
                                         "d'un vaisseau : « %s » est traité comme immobile\n",
                                 scene.Get<NameComponent>(entity).name.c_str());
                    warnedChild = true;
                }
                simulated = false;
            }
            const JPH::RefConst<JPH::Shape> shape = CreateShape(collider, transform.scale, simulated);
            if (!shape) {
                grid.bodies.emplace(entity, JPH::BodyID{}); // pas de nouvel essai à chaque image
                return;
            }
            const JPH::EMotionType motion = hull ? JPH::EMotionType::Kinematic
                                          : simulated ? JPH::EMotionType::Dynamic
                                                      : JPH::EMotionType::Static;
            const bool moving = motion != JPH::EMotionType::Static;
            JPH::BodyCreationSettings settings(shape, JPH::RVec3(ToJolt(transform.position)), ToJoltRotation(transform.rotation),
                                               motion, moving ? Layers::Moving : Layers::NonMoving);
            settings.mUserData = static_cast<JPH::uint64>(entt::to_integral(entity));
            settings.mFriction = collider.friction;
            settings.mRestitution = collider.restitution;
            settings.mGravityFactor = collider.gravityFactor;
            if (simulated && collider.mass > 0.0f) {
                settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
                settings.mMassPropertiesOverride.mMass = collider.mass;
            }
            const JPH::BodyID id = grid.system->GetBodyInterface().CreateAndAddBody(
                settings, moving ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
            grid.bodies.emplace(entity, id);
        }

        void CreateCharacter(Grid& grid, Entity entity, const CharacterComponent& component, const RelativeTransform& transform) {
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
            // Corps interne : les objets dynamiques et les rayons voient le personnage. Un peu plus mince que la
            // capsule (comme l'exemple de Jolt) : il est téléporté à chaque pas, et s'il touchait ce que la capsule
            // touche, il éjecterait les caisses en les repoussant.
            constexpr float InnerFraction = 0.9f;
            settings->mInnerBodyShape = JPH::RotatedTranslatedShapeSettings(
                JPH::Vec3(0.0f, halfCylinder + radius, 0.0f), JPH::Quat::sIdentity(),
                new JPH::CapsuleShape(halfCylinder + radius * (1.0f - InnerFraction), radius * InnerFraction)).Create().Get();
            settings->mInnerBodyLayer = Layers::Moving;

            Character character;
            // Seul le cap (autour de y) compte : un personnage reste debout.
            const Math::Vector3f forward = transform.rotation.Rotate(Math::Vector3f{0.0f, 0.0f, 1.0f});
            character.yaw = std::atan2(forward.x, forward.z);
            character.character = new JPH::CharacterVirtual(settings, JPH::RVec3(ToJolt(transform.position)),
                                                            JPH::Quat::sRotation(JPH::Vec3::sAxisY(), character.yaw),
                                                            static_cast<JPH::uint64>(entt::to_integral(entity)), grid.system.get());
            grid.characters.emplace(entity, std::move(character));
        }

        // Vaisseaux apparus ou disparus, repère de chacun pour cette mise à jour.
        void SynchronizeGrids(const Scene& scene) {
            std::erase_if(grids, [&](const std::unique_ptr<Grid>& grid) {
                if (grid->frame == NullEntity || (scene.IsValid(grid->frame) && scene.TryGet<ShipComponent>(grid->frame))) {
                    return false;
                }
                DestroyGrid(*grid);
                return true;
            });
            for (const auto [entity, ship] : scene.GetRegistry().view<const ShipComponent>().each()) {
                Grid* grid = FindGrid(entity);
                if (!grid) {
                    grids.push_back(CreateGrid(entity));
                    grid = grids.back().get();
                }
                if (const RelativeTransform* transform = FindTransform(entity)) {
                    grid->framePosition = transform->position;
                    grid->frameRotation = transform->rotation;
                    grid->frameScale = transform->scale;
                }
                grid->system->SetGravity(JPH::Vec3(0.0f, -ship.gravity, 0.0f)); // le sol du pont
            }
        }

        // Nouvelles entités, entités détruites, composants ajoutés ou retirés, entités passées d'un repère à l'autre.
        void Synchronize(Scene& scene) {
            SynchronizeGrids(scene);
            for (const std::unique_ptr<Grid>& grid : grids) {
                JPH::BodyInterface& bodyInterface = grid->system->GetBodyInterface();
                std::erase_if(grid->bodies, [&](const auto& entry) {
                    const auto [entity, id] = entry;
                    if (scene.IsValid(entity) && scene.TryGet<ColliderComponent>(entity) && !scene.TryGet<CharacterComponent>(entity)
                        && FrameOf(scene, entity) == grid->frame) {
                        return false;
                    }
                    if (!id.IsInvalid()) {
                        bodyInterface.RemoveBody(id);
                        bodyInterface.DestroyBody(id);
                    }
                    return true;
                });
                std::erase_if(grid->characters, [&](const auto& entry) {
                    const Entity entity = entry.first;
                    return !scene.IsValid(entity) || !scene.TryGet<CharacterComponent>(entity) || scene.GetParent(entity) != grid->frame;
                });
            }

            entt::registry& registry = scene.GetRegistry();
            player = NullEntity;
            for (const auto [entity, component] : registry.view<const CharacterComponent>().each()) {
                if (component.player && player == NullEntity) {
                    player = entity;
                }
                // Racine, ou enfant direct d'un vaisseau : sinon il suivrait deux maîtres (ignoré).
                const Entity frame = FrameOf(scene, entity);
                if (scene.GetParent(entity) != frame) {
                    continue;
                }
                Grid* grid = FindGrid(frame);
                const RelativeTransform* transform = FindTransform(entity);
                if (grid && transform && !grid->characters.contains(entity)) {
                    CreateCharacter(*grid, entity, component, grid->InFrame(*transform));
                }
            }
            for (const auto [entity, collider] : registry.view<const ColliderComponent>().each()) {
                if (registry.all_of<CharacterComponent>(entity)) {
                    continue;
                }
                Grid* grid = FindGrid(FrameOf(scene, entity));
                const RelativeTransform* transform = FindTransform(entity);
                if (grid && transform && !grid->bodies.contains(entity)) {
                    CreateBody(scene, *grid, entity, collider, grid->InFrame(*transform));
                }
            }
        }

        // Coques des vaisseaux : amenées là où le vaisseau est maintenant, en steps pas (vitesse comprise : ce qui
        // les touche est poussé, un personnage posé dessus est emporté).
        void MoveHulls(const Scene& scene, int steps) {
            for (const std::unique_ptr<Grid>& grid : grids) {
                JPH::BodyInterface& bodyInterface = grid->system->GetBodyInterface();
                for (const auto& [entity, id] : grid->bodies) {
                    if (id.IsInvalid() || !scene.TryGet<ShipComponent>(entity)) {
                        continue;
                    }
                    if (const RelativeTransform* transform = FindTransform(entity)) {
                        const RelativeTransform target = grid->InFrame(*transform);
                        bodyInterface.MoveKinematic(id, JPH::RVec3(ToJolt(target.position)), ToJoltRotation(target.rotation),
                                                    StepSeconds * static_cast<float>(steps));
                    }
                }
            }
        }

        void UpdateCharacters(Grid& grid, const Scene& scene, const CharacterInput& input) {
            JPH::PhysicsSystem& system = *grid.system;
            const JPH::Vec3 gravity = system.GetGravity();
            const JPH::Vec3 up = -gravity.NormalizedOr(JPH::Vec3(0.0f, -1.0f, 0.0f));
            // Commandes dans le repère du monde, ramenées dans celui du vaisseau.
            const Math::Vector3f move = grid.frameRotation.Conjugate().Rotate(input.move);
            for (auto& [entity, character] : grid.characters) {
                const CharacterComponent& component = scene.Get<CharacterComponent>(entity);
                JPH::CharacterVirtual& body = *character.character;
                body.SetUp(up);
                body.UpdateGroundVelocity();

                JPH::Vec3 wanted = JPH::Vec3::sZero();
                bool jump = false;
                if (component.player && entity == player) {
                    wanted = ToJolt(grid.frame == NullEntity ? input.move : move);
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
                const JPH::RVec3 before = body.GetPosition();
                body.ExtendedUpdate(StepSeconds, gravity, update, system.GetDefaultBroadPhaseLayerFilter(Layers::Moving),
                                    system.GetDefaultLayerFilter(Layers::Moving), {}, {}, *tempAllocator);

                // Vitesse des pas : le chemin vraiment parcouru (nulle contre un mur), par rapport au sol qui peut
                // bouger, dans le plan du sol ; nulle en l'air.
                if (body.GetGroundState() == JPH::CharacterVirtual::EGroundState::OnGround) {
                    const JPH::Vec3 moved = JPH::Vec3(body.GetPosition() - before) / StepSeconds - body.GetGroundVelocity();
                    character.groundSpeed = (moved - up * moved.Dot(up)).Length();
                    character.airTime = 0.0f;
                } else {
                    character.groundSpeed = 0.0f;
                    character.airTime += StepSeconds;
                }
            }
        }

        // Position d'une entité d'après la simulation : secteur et décalage pour une racine du monde, position locale
        // pour un enfant direct d'un vaisseau (dans son espace, à son échelle).
        void Write(Scene& scene, const Grid& grid, Entity entity, JPH::RVec3Arg position, JPH::QuatArg rotation) {
            TransformComponent& transform = scene.Get<TransformComponent>(entity);
            transform.rotation = FromJolt(rotation.Normalized());
            if (grid.frame != NullEntity) {
                const Math::Vector3f& scale = grid.frameScale;
                const auto unscale = [](double value, float factor) { return factor != 0.0f ? value / factor : 0.0; };
                transform.position = {unscale(position.GetX(), scale.x), unscale(position.GetY(), scale.y),
                                      unscale(position.GetZ(), scale.z)};
                return;
            }
            Math::WorldPosition world = anchor;
            world += Math::Vector3d{position.GetX(), position.GetY(), position.GetZ()};
            transform.position = world.offset;
            if (world.sector != Math::Vector3i64{}) {
                scene.Add<SectorComponent>(entity, world.sector);
            } else if (scene.TryGet<SectorComponent>(entity)) {
                scene.Remove<SectorComponent>(entity);
            }
        }

        void WriteBack(Scene& scene, const Grid& grid) {
            JPH::BodyInterface& bodyInterface = grid.system->GetBodyInterface();
            for (const auto& [entity, id] : grid.bodies) {
                if (id.IsInvalid() || bodyInterface.GetMotionType(id) != JPH::EMotionType::Dynamic || !bodyInterface.IsActive(id)) {
                    continue; // immobile, coque (suit son vaisseau) ou endormi : n'a pas bougé de lui-même
                }
                JPH::RVec3 position;
                JPH::Quat rotation;
                bodyInterface.GetPositionAndRotation(id, position, rotation);
                Write(scene, grid, entity, position, rotation);
            }
            for (const auto& [entity, character] : grid.characters) {
                Write(scene, grid, entity, character.character->GetPosition(), character.character->GetRotation());
                CharacterComponent& component = scene.Get<CharacterComponent>(entity);
                component.groundSpeed = character.groundSpeed;
                component.airTime = character.airTime;
            }
        }

        void Update(Scene& scene, double deltaSeconds, const CharacterInput& input) {
            if (!anchored) {
                ChooseAnchor(scene);
                if (!anchored) {
                    return; // rien de physique dans la scène
                }
            }
            MoveShips(scene, deltaSeconds);
            scene.ComputeRelativeTransforms(anchor, transforms);
            Synchronize(scene);

            accumulator = std::min(accumulator + deltaSeconds, static_cast<double>(StepSeconds * MaxStepsPerUpdate));
            const int steps = static_cast<int>(accumulator / StepSeconds);
            if (steps == 0) {
                return;
            }
            accumulator -= steps * static_cast<double>(StepSeconds);
            MoveHulls(scene, steps);
            for (int step = 0; step < steps; ++step) {
                for (const std::unique_ptr<Grid>& grid : grids) {
                    UpdateCharacters(*grid, scene, input);
                    grid->system->Update(StepSeconds, 1, tempAllocator.get(), jobSystem.get());
                }
            }
            for (const std::unique_ptr<Grid>& grid : grids) {
                WriteBack(scene, *grid);
            }
        }
    };

    PhysicsWorld::PhysicsWorld() : m_impl(std::make_unique<Impl>()) {}

    PhysicsWorld::~PhysicsWorld() = default;

    void PhysicsWorld::Update(Scene& scene, double deltaSeconds, const CharacterInput& input) {
        m_impl->Update(scene, deltaSeconds, input);
    }

    void PhysicsWorld::SetGravity(const Math::Vector3f& gravity) {
        m_impl->grids.front()->system->SetGravity(ToJolt(gravity));
    }

    Math::Vector3f PhysicsWorld::GetGravity() const {
        return FromJolt(m_impl->grids.front()->system->GetGravity());
    }

    Entity PhysicsWorld::GetPlayer() const {
        return m_impl->player;
    }

    int PhysicsWorld::GetBodyCount() const {
        int count = 0;
        for (const std::unique_ptr<Grid>& grid : m_impl->grids) {
            count += static_cast<int>(std::ranges::count_if(grid->bodies, [](const auto& entry) { return !entry.second.IsInvalid(); }));
        }
        return count;
    }
}
