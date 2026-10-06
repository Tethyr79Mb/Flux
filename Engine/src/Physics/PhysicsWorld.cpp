#include "Flux/Physics/PhysicsWorld.hpp"

#include "PhysicsContextImpl.hpp"
#include "PlanetGround.hpp"

#include "Flux/Core/Log.hpp"
#include "Flux/Scene/Scene.hpp"
#include "Flux/Scene/Seats.hpp"

// Jolt : ses en-têtes seulement ici (et jamais dans un en-tête public).
#include <Jolt/Jolt.h>

#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/ScaledShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Flux {
    namespace {
        // Pas de simulation fixe : la même physique à 30 ou 144 images/s.
        constexpr float StepSeconds = 1.0f / 60.0f;
        // Au plus tant de pas par image : après un blocage, la simulation ralentit au lieu de s'emballer.
        constexpr int MaxStepsPerUpdate = 4;

        // Calques : les objets immobiles ne se testent pas entre eux ; ceux qui n'arrêtent que la caméra (verrière)
        // ne touchent rien, seul le lancer de la caméra les voit.
        namespace Layers {
            constexpr JPH::ObjectLayer NonMoving = 0;
            constexpr JPH::ObjectLayer Moving = 1;
            constexpr JPH::ObjectLayer CameraOnly = 2;
            constexpr JPH::uint Count = 3;
        }

        class BroadPhaseLayers final : public JPH::BroadPhaseLayerInterface {
            public:
            JPH::uint GetNumBroadPhaseLayers() const override { return Layers::Count; }
            JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
                return JPH::BroadPhaseLayer(static_cast<JPH::BroadPhaseLayer::Type>(layer));
            }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
            const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override {
                switch (static_cast<JPH::BroadPhaseLayer::Type>(layer)) {
                    case Layers::NonMoving: return "immobile";
                    case Layers::Moving: return "mobile";
                    default: return "caméra";
                }
            }
#endif
        };

        class ObjectVsBroadPhase final : public JPH::ObjectVsBroadPhaseLayerFilter {
            public:
            bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer broadPhase) const override {
                const auto other = static_cast<JPH::BroadPhaseLayer::Type>(broadPhase);
                if (layer == Layers::CameraOnly || other == Layers::CameraOnly) {
                    return false;
                }
                return layer == Layers::Moving || other == Layers::Moving;
            }
        };

        class ObjectPairs final : public JPH::ObjectLayerPairFilter {
            public:
            bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
                if (a == Layers::CameraOnly || b == Layers::CameraOnly) {
                    return false;
                }
                return a == Layers::Moving || b == Layers::Moving;
            }
        };

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

        // Haut d'après la gravité ; sans gravité, le y du repère.
        JPH::Vec3 UpFrom(JPH::Vec3Arg gravity) {
            return -gravity.NormalizedOr(JPH::Vec3(0.0f, -1.0f, 0.0f));
        }
        // La même rotation, redressée au plus court pour que son y soit `up` (debout sur une planète).
        JPH::Quat Upright(JPH::QuatArg rotation, JPH::Vec3Arg up) {
            return (JPH::Quat::sFromTo(rotation.RotateAxisY(), up) * rotation).Normalized();
        }
        // Debout selon `up`, le z (l'avant du modèle glTF) vers `forward` ramené dans le plan du sol.
        JPH::Quat Facing(JPH::Vec3Arg forward, JPH::Vec3Arg up) {
            const JPH::Vec3 z = (forward - up * forward.Dot(up)).NormalizedOr(up.GetNormalizedPerpendicular());
            const JPH::Vec3 x = up.Cross(z);
            return JPH::Mat44(JPH::Vec4(x, 0.0f), JPH::Vec4(up, 0.0f), JPH::Vec4(z, 0.0f), JPH::Vec4(0.0f, 0.0f, 0.0f, 1.0f))
                .GetQuaternion()
                .Normalized();
        }

        float MaxComponent(const Math::Vector3f& v) {
            return std::max({std::abs(v.x), std::abs(v.y), std::abs(v.z)});
        }

        // Forme de base de la collision (sans le décalage du centre), à l'échelle de l'entité. mesh : triangles déjà
        // prêts (ColliderShape::Mesh), dans le repère du modèle.
        JPH::ShapeSettings::ShapeResult CreateBaseShape(const ColliderComponent& collider, const Math::Vector3f& scale,
                                                        bool simulated, const JPH::Shape* mesh) {
            const Math::Vector3f absScale{std::abs(scale.x), std::abs(scale.y), std::abs(scale.z)};
            // Une forme plate ou nulle n'a pas de volume : Jolt la refuserait. Un objet simulé minuscule (quelques
            // milligrammes, inertie quasi nulle) rend la simulation instable jusqu'aux NaN : 5 cm au moins.
            const float MinSize = simulated ? 0.05f : 1.0e-3f;
            if (collider.shape == ColliderShape::Mesh) {
                // Échelle de l'entité (miroir compris) ; une échelle nulle écraserait tout en un plan.
                const auto axis = [](float value) { return std::copysign(std::max(std::abs(value), 1.0e-3f), value); };
                return JPH::ScaledShapeSettings(mesh, JPH::Vec3(axis(scale.x), axis(scale.y), axis(scale.z))).Create();
            }
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
        JPH::RefConst<JPH::Shape> CreateShape(const ColliderComponent& collider, const Math::Vector3f& scale, bool simulated,
                                              const JPH::Shape* mesh) {
            JPH::RefConst<JPH::Shape> shape;
            JPH::ShapeSettings::ShapeResult result;
            if (collider.shape == ColliderShape::Mesh && scale == Math::Vector3f{1.0f, 1.0f, 1.0f}) {
                shape = mesh; // partagé par toutes les entités du même fichier
            } else {
                result = CreateBaseShape(collider, scale, simulated, mesh);
                if (result.HasError()) {
                    Log::Warning("forme de collision refusée : {}", result.GetError().c_str());
                    return nullptr;
                }
                shape = result.Get();
            }
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

        // Triangles d'un fichier glTF pour une collision (ColliderShape::Mesh). nullptr (et un message) s'il est
        // illisible ou animé : ses sommets ne sont pas dans la pose affichée.
        struct Character {
            JPH::Ref<JPH::CharacterVirtual> character; // son z : la direction du regard (le modèle regarde vers +z en glTF)
            float groundSpeed = 0.0f; // vitesse horizontale au dernier pas, pour les animations
            float airTime = 0.0f;     // secondes sans toucher un sol où tenir debout (saut, chute, pente trop raide)
            Entity seat = NullEntity; // siège où il était posé au dernier pas
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
            // Décors des vaisseaux posés dans ce monde (enfants immobiles d'un vaisseau qui y vit) : une copie
            // cinématique qui le suit, vue du dehors. On s'y cogne, on y monte par la rampe. Clé : l'entité copiée ;
            // donnée du corps : le vaisseau, comme sa coque.
            std::unordered_map<Entity, JPH::BodyID> mirrors;
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

    }

    struct PhysicsWorld::Impl {
        BroadPhaseLayers broadPhaseLayers;
        ObjectVsBroadPhase objectVsBroadPhase;
        ObjectPairs objectPairs;
        std::shared_ptr<PhysicsContext> context; // partagé avec les autres mondes ; le sien si on n'en donne pas
        PhysicsContext::Impl& shared;
        std::vector<std::unique_ptr<Grid>> grids; // [0] : le monde ; puis un par vaisseau

        bool anchored = false;
        Math::WorldPosition anchor; // origine du monde de la scène
        std::vector<RelativeTransform> transforms;
        double accumulator = 0.0;
        double time = 0.0; // simulé, en secondes
        // Dernier passage de chaque personnage d'un monde à l'autre : pas de nouveau passage juste après (au bas de
        // la rampe, il toucherait les deux sols à la fois).
        std::unordered_map<Entity, double> transfers;
        Entity player = NullEntity;
        bool warnedChild = false;
        bool warnedMeshSimulated = false;
        // Enveloppe convexe de chaque vaisseau (ses collisions immobiles, dans son repère), pour qu'il bute sur le
        // décor ; nullptr : rien de solide, il passe partout.
        std::unordered_map<Entity, JPH::RefConst<JPH::Shape>> shipShapes;
        // Sol et gravité des planètes, dans le monde de la scène.
        PlanetGround planetGround{Layers::NonMoving};
        std::vector<PlanetGround::Probe> probes;

        Impl(std::shared_ptr<PhysicsContext> owner, PhysicsContext::Impl& details)
            : context(std::move(owner)), shared(details) {
            grids.push_back(CreateGrid(NullEntity));
        }

        ~Impl() {
            planetGround.Clear(*grids.front()->system);
            for (const std::unique_ptr<Grid>& grid : grids) {
                DestroyGrid(*grid);
            }
            grids.clear();
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
            for (auto* map : {&grid.bodies, &grid.mirrors}) {
                for (const auto& [entity, id] : *map) {
                    if (!id.IsInvalid()) {
                        bodyInterface.RemoveBody(id);
                        bodyInterface.DestroyBody(id);
                    }
                }
                map->clear();
            }
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

        // Gravité en ce point d'un monde (dans son repère) : celle du vaisseau à bord, celle des planètes ou du monde dehors.
        JPH::Vec3 GravityAt(const Grid& grid, JPH::RVec3Arg position) const {
            const JPH::Vec3 gravity = grid.system->GetGravity();
            if (grid.frame != NullEntity || !planetGround.HasPlanets()) {
                return gravity;
            }
            const Math::Vector3d at{position.GetX(), position.GetY(), position.GetZ()};
            return ToJolt(planetGround.GetGravity(at, FromJolt(gravity)));
        }

        // Ancre trop loin du joueur : les floats perdraient le millimètre (un personnage à 100 km de l'ancre
        // tremblerait sur le sol d'une planète). Le monde de la scène est décalé pour que l'ancre soit sur lui.
        void Recenter(const Scene& scene) {
            constexpr double RecenterDistance = 1000.0; // m : précision d'un dixième de millimètre
            if (player == NullEntity || !scene.IsValid(player)) {
                return;
            }
            const Math::WorldPosition focus = scene.GetWorldPosition(player);
            const Math::Vector3d offset = Math::Delta(anchor, focus);
            if (Math::Length(offset) < RecenterDistance) {
                return;
            }
            Grid& world = *grids.front();
            JPH::BodyInterface& bodyInterface = world.system->GetBodyInterface();
            const JPH::Vec3 shift = -ToJolt(offset);
            for (auto* map : {&world.bodies, &world.mirrors}) {
                for (const auto& [entity, id] : *map) {
                    if (!id.IsInvalid()) {
                        bodyInterface.SetPosition(id, bodyInterface.GetPosition(id) + shift, JPH::EActivation::DontActivate);
                    }
                }
            }
            for (const auto& [entity, character] : world.characters) {
                character.character->SetPosition(character.character->GetPosition() + shift);
            }
            planetGround.Shift(*world.system, offset * -1.0);
            anchor = focus;
        }

        // Ce qui peut toucher le sol d'une planète : personnages, vaisseaux à la racine, objets simulés du monde.
        void CollectProbes(const Scene& scene) {
            probes.clear();
            const auto speedOf = [&](Entity entity) {
                if (const Grid* grid = FindGrid(scene.GetShip(entity))) {
                    if (const auto found = grid->characters.find(entity); found != grid->characters.end()) {
                        return static_cast<double>(found->second.character->GetLinearVelocity().Length());
                    }
                }
                return 0.0;
            };
            for (const Entity entity : scene.GetRegistry().view<const CharacterComponent>()) {
                const double speed = speedOf(entity);
                probes.push_back({Math::Delta(anchor, scene.GetWorldPosition(entity)), 20.0 + speed, speed});
            }
            for (const auto [entity, ship] : scene.GetRegistry().view<const ShipComponent>().each()) {
                if (scene.GetParent(entity) != NullEntity) {
                    continue;
                }
                const JPH::Shape* shape = GetShipShape(scene, entity);
                const double size = shape ? shape->GetLocalBounds().GetExtent().Length() : 10.0;
                const double speed = Math::Length(ship.velocity);
                probes.push_back({Math::Delta(anchor, scene.GetWorldPosition(entity)), size + 20.0 + speed, speed});
            }
            const Grid& world = *grids.front();
            const JPH::BodyInterface& bodyInterface = world.system->GetBodyInterface();
            for (const auto& [entity, id] : world.bodies) {
                if (id.IsInvalid() || bodyInterface.GetMotionType(id) != JPH::EMotionType::Dynamic) {
                    continue;
                }
                // Endormi aussi : son sol doit rester là pour quand on le réveillera.
                const JPH::RVec3 position = bodyInterface.GetCenterOfMassPosition(id);
                const double speed = bodyInterface.IsActive(id) ? bodyInterface.GetLinearVelocity(id).Length() : 0.0;
                const double size = bodyInterface.GetShape(id)->GetLocalBounds().GetExtent().Length();
                probes.push_back({{position.GetX(), position.GetY(), position.GetZ()}, size + 5.0 + speed, speed});
            }
        }

        // Objets simulés du monde près d'une planète : Jolt les tire vers le bas du monde, on corrige vers son centre.
        void ApplyPlanetGravity(const Grid& grid) {
            if (grid.frame != NullEntity || !planetGround.HasPlanets()) {
                return;
            }
            JPH::BodyInterface& bodyInterface = grid.system->GetBodyInterface();
            const JPH::Vec3 base = grid.system->GetGravity();
            for (const auto& [entity, id] : grid.bodies) {
                if (id.IsInvalid() || bodyInterface.GetMotionType(id) != JPH::EMotionType::Dynamic || !bodyInterface.IsActive(id)) {
                    continue;
                }
                const JPH::Vec3 difference = GravityAt(grid, bodyInterface.GetCenterOfMassPosition(id)) - base;
                if (difference.LengthSq() > 1.0e-8f) {
                    bodyInterface.AddLinearVelocity(id, difference * (bodyInterface.GetGravityFactor(id) * StepSeconds));
                }
            }
        }

        void ChooseAnchor(const Scene& scene) {
            // Le secteur du premier objet physique : la simulation se passe autour de lui.
            const entt::registry& registry = scene.GetRegistry();
            const auto first = [&](auto view) {
                if (view.begin() != view.end()) {
                    anchor = {scene.GetWorldPosition(*view.begin()).sector, {}};
                    anchored = true;
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

        // Points de l'enveloppe d'un vaisseau : ses collisions immobiles (maillages : leurs sommets ; formes simples :
        // les coins de leur boîte), dans son repère ; ni les objets simulés, ni la verrière « caméra seulement », ni
        // les personnages, ni les vaisseaux qu'il transporte.
        void CollectShipPoints(const Scene& scene, Entity entity, const Math::Vector3f& position, const Math::Quaternionf& rotation,
                               const Math::Vector3f& scale, std::vector<Math::Vector3f>& points) {
            if (scene.TryGet<CharacterComponent>(entity)) {
                return;
            }
            const auto add = [&](const Math::Vector3f& local) {
                points.push_back(position + rotation.Rotate(Math::Vector3f{local.x * scale.x, local.y * scale.y, local.z * scale.z}));
            };
            if (const auto* collider = scene.TryGet<ColliderComponent>(entity);
                collider && !collider->simulatePhysics && !collider->cameraOnly) {
                if (collider->shape == ColliderShape::Mesh) {
                    if (const CollisionMesh* mesh = FindMesh(scene, entity, *collider); mesh && mesh->shape) {
                        // Enveloppe du maillage, calculée une fois à sa lecture (quelques centaines de points).
                        for (const Math::Vector3f& point : mesh->hull) {
                            add(point);
                        }
                    }
                } else {
                    Math::Vector3f half = collider->size * 0.5f;
                    if (collider->shape == ColliderShape::Sphere) {
                        half = {collider->radius, collider->radius, collider->radius};
                    } else if (collider->shape == ColliderShape::Capsule) {
                        half = {collider->radius, collider->height * 0.5f, collider->radius};
                    }
                    for (int corner = 0; corner < 8; ++corner) {
                        add(collider->center + Math::Vector3f{(corner & 1) ? half.x : -half.x, (corner & 2) ? half.y : -half.y,
                                                              (corner & 4) ? half.z : -half.z});
                    }
                }
            }
            scene.ForEachChild(entity, [&](Entity child) {
                if (scene.TryGet<ShipComponent>(child)) {
                    return; // il bouge de lui-même
                }
                const TransformComponent& transform = scene.Get<TransformComponent>(child);
                const Math::Vector3f offset{static_cast<float>(transform.position.x) * scale.x,
                                            static_cast<float>(transform.position.y) * scale.y,
                                            static_cast<float>(transform.position.z) * scale.z};
                CollectShipPoints(scene, child, position + rotation.Rotate(offset), Math::Normalize(rotation * transform.rotation),
                                  {scale.x * transform.scale.x, scale.y * transform.scale.y, scale.z * transform.scale.z}, points);
            });
        }

        // Enveloppe convexe du vaisseau, calculée une fois : elle passe sous les ailes et entre les dérives (pas de
        // vol sous un surplomb), mais suffit pour se poser et buter contre le sol ou un mur.
        const JPH::Shape* GetShipShape(const Scene& scene, Entity ship) {
            if (const auto found = shipShapes.find(ship); found != shipShapes.end()) {
                return found->second.GetPtr();
            }
            std::vector<Math::Vector3f> points;
            CollectShipPoints(scene, ship, {}, {}, scene.Get<TransformComponent>(ship).scale, points);
            JPH::RefConst<JPH::Shape> shape;
            if (points.size() >= 4) {
                JPH::Array<JPH::Vec3> hull;
                hull.reserve(points.size());
                for (const Math::Vector3f& point : points) {
                    hull.push_back(ToJolt(point));
                }
                const JPH::ShapeSettings::ShapeResult result = JPH::ConvexHullShapeSettings(hull, 0.02f).Create();
                if (result.HasError()) {
                    Log::Warning("« {} » : enveloppe de collision du vaisseau impossible ({}), il traversera le décor",
                                 scene.Get<NameComponent>(ship).name, result.GetError().c_str());
                } else {
                    shape = result.Get();
                }
            }
            return shipShapes.emplace(ship, shape).first->second.GetPtr();
        }

        // Le décor immobile du monde (pas la verrière « caméra seulement », ni les caisses, coques et copies qui
        // bougent) : ce sur quoi un vaisseau bute.
        class StaticFilter final : public JPH::BodyFilter {
            public:
            bool ShouldCollideLocked(const JPH::Body& body) const override {
                return body.IsStatic() && body.GetObjectLayer() != Layers::CameraOnly;
            }
        };

        // Enfoncement le plus profond de l'enveloppe dans le décor, à cette place.
        float Penetration(const Grid& grid, const JPH::Shape& shape, JPH::QuatArg rotation, JPH::Vec3Arg position) const {
            JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
            const StaticFilter filter;
            grid.system->GetNarrowPhaseQuery().CollideShape(&shape, JPH::Vec3::sOne(),
                                                            JPH::RMat44::sRotationTranslation(rotation, JPH::RVec3(position)),
                                                            JPH::CollideShapeSettings{}, JPH::RVec3::sZero(), collector, {}, {},
                                                            filter);
            float deepest = 0.0f;
            for (const JPH::CollideShapeResult& hit : collector.mHits) {
                deepest = std::max(deepest, hit.mPenetrationDepth);
            }
            return deepest;
        }

        // Premier obstacle sur le trajet de l'enveloppe : fraction du trajet et normale de la surface (vers le
        // vaisseau). Les surfaces dont on s'éloigne ou qu'on longe (posé au sol, on roule) ne comptent pas.
        bool CastShip(const Grid& grid, const JPH::Shape& shape, JPH::QuatArg rotation, JPH::Vec3Arg position,
                      JPH::Vec3Arg motion, float& fraction, JPH::Vec3& normal) const {
            const JPH::RShapeCast cast(&shape, JPH::Vec3::sOne(), JPH::RMat44::sRotationTranslation(rotation, JPH::RVec3(position)),
                                       motion);
            JPH::ShapeCastSettings settings;
            settings.mReturnDeepestPoint = true; // déjà au contact : un vrai point de contact
            JPH::AllHitCollisionCollector<JPH::CastShapeCollector> collector;
            const StaticFilter filter;
            grid.system->GetNarrowPhaseQuery().CastShape(cast, settings, JPH::RVec3::sZero(), collector, {}, {}, filter);
            bool hit = false;
            fraction = 1.0f;
            for (const JPH::ShapeCastResult& result : collector.mHits) {
                if (result.mFraction >= fraction) {
                    continue;
                }
                // Normale de la face touchée (celle du sol, d'un mur) : plus sûre que l'axe de pénétration, quelconque
                // quand deux faces planes se touchent tout juste.
                JPH::BodyLockRead lock(grid.system->GetBodyLockInterfaceNoLock(), result.mBodyID2);
                if (!lock.Succeeded()) {
                    continue;
                }
                const JPH::Vec3 surface = lock.GetBody().GetWorldSpaceSurfaceNormal(result.mSubShapeID2, result.mContactPointOn2);
                if (surface.Dot(motion) >= -1.0e-6f) {
                    continue;
                }
                fraction = result.mFraction;
                normal = surface;
                hit = true;
            }
            return hit;
        }

        // Mouvement des vaisseaux (ShipComponent::velocity, angularVelocity), dans leurs propres axes. Un vaisseau à la
        // racine bute sur le décor immobile du monde : il s'arrête contre le sol ou un mur et glisse le long, et ne
        // tourne pas s'il devait s'y enfoncer. Posé (le décor sous son ventre, à moins de 5 cm), il ne tourne pas du tout.
        void MoveShips(Scene& scene, double deltaSeconds) {
            const Grid* worldGrid = nullptr;
            for (const std::unique_ptr<Grid>& grid : grids) {
                if (grid->frame == NullEntity) {
                    worldGrid = grid.get();
                }
            }
            for (const auto [entity, ship] : scene.GetRegistry().view<ShipComponent>().each()) {
                TransformComponent& transform = scene.Get<TransformComponent>(entity);
                const bool root = scene.GetParent(entity) == NullEntity;
                const JPH::Shape* shape = root && worldGrid ? GetShipShape(scene, entity) : nullptr;
                if (!shape && ship.velocity == Math::Vector3f{} && ship.angularVelocity == Math::Vector3f{}) {
                    continue;
                }
                ship.grounded = false;
                JPH::Vec3 lift = JPH::Vec3::sZero();
                if (shape) {
                    const JPH::Vec3 start = ToJolt(Math::Delta(anchor, scene.GetWorldPosition(entity)));
                    const JPH::Vec3 up = ToJolt(transform.rotation.Rotate(Math::Vector3f{0.0f, 1.0f, 0.0f}));
                    float fraction = 1.0f;
                    JPH::Vec3 normal;
                    // Stabilisateur d'atterrissage : à moins de 4 m d'un sol (pente < 60°) et lent sur le plan (moins
                    // de 10 m/s), le vaisseau se remet parallèle au sol, à 60 °/s au plus, sans toucher à son cap.
                    const float flatSpeed = std::hypot(ship.velocity.x, ship.velocity.z);
                    if (flatSpeed < 10.0f && CastShip(*worldGrid, *shape, ToJoltRotation(transform.rotation), start, up * -4.0f, fraction, normal)
                        && normal.Dot(up) > 0.5f) {
                        const JPH::Vec3 axis = up.Cross(normal);
                        const float angle = std::asin(std::min(1.0f, axis.Length()));
                        if (angle > 1.0e-3f) {
                            constexpr float LevelRate = JPH::JPH_PI / 3.0f; // 60 °/s
                            const float step = std::min(angle, LevelRate * static_cast<float>(deltaSeconds));
                            const Math::Quaternionf level = Math::Quaternionf::FromAxisAngle(FromJolt(axis), step);
                            const Math::Quaternionf leveled = Math::Normalize(level * transform.rotation);
                            // Posé sur une aile, il se redresse en s'appuyant dessus : il remonte de ce qu'il
                            // s'enfoncerait (le train touche le sol au lieu de passer dessous).
                            const float before = Penetration(*worldGrid, *shape, ToJoltRotation(transform.rotation), start);
                            const float after = Penetration(*worldGrid, *shape, ToJoltRotation(leveled), start);
                            if (after > before) {
                                lift = normal * (after - before + 0.001f);
                            }
                            transform.rotation = leveled;
                        }
                    }
                    // Posé : obstacle à moins de 5 cm vers son bas, face tournée vers son haut (pente < 45°). Il ne
                    // tourne plus de lui-même (le stabilisateur, si : il se met à plat sur ses roues).
                    const JPH::Vec3 down = ToJolt(transform.rotation.Rotate(Math::Vector3f{0.0f, -1.0f, 0.0f}));
                    ship.grounded = CastShip(*worldGrid, *shape, ToJoltRotation(transform.rotation), start, down * 0.05f, fraction, normal)
                                    && normal.Dot(-down) > 0.7f;
                    if (ship.grounded) {
                        ship.angularVelocity = {};
                    }
                }
                const Math::Quaternionf previousRotation = transform.rotation;
                constexpr float RadiansPerDegree = JPH::JPH_PI / 180.0f;
                const float speed = Math::Length(ship.angularVelocity) * RadiansPerDegree;
                if (speed > 0.0f) {
                    const Math::Quaternionf turn = Math::Quaternionf::FromAxisAngle(ship.angularVelocity, speed * static_cast<float>(deltaSeconds));
                    transform.rotation = Math::Normalize(transform.rotation * turn);
                }
                Math::Vector3d move = Math::Vector3d(transform.rotation.Rotate(ship.velocity)) * deltaSeconds;
                move += Math::Vector3d(FromJolt(lift));
                if (shape) {
                    const JPH::Vec3 start = ToJolt(Math::Delta(anchor, scene.GetWorldPosition(entity)));
                    if (speed > 0.0f) {
                        // Une rotation qui l'enfoncerait dans le décor (nez contre le sol) est refusée.
                        const float before = Penetration(*worldGrid, *shape, ToJoltRotation(previousRotation), start);
                        const float after = Penetration(*worldGrid, *shape, ToJoltRotation(transform.rotation), start);
                        if (after > before + 0.01f) {
                            transform.rotation = previousRotation;
                            move = Math::Vector3d(transform.rotation.Rotate(ship.velocity)) * deltaSeconds + Math::Vector3d(FromJolt(lift));
                        }
                    }
                    const JPH::Quat rotation = ToJoltRotation(transform.rotation);
                    JPH::Vec3 position = start;
                    JPH::Vec3 motion = ToJolt(move);
                    // Deux passes : jusqu'à l'obstacle, puis le reste du trajet le long de sa surface.
                    for (int pass = 0; pass < 2 && motion.LengthSq() > 1.0e-12f; ++pass) {
                        float fraction = 1.0f;
                        JPH::Vec3 normal;
                        if (!CastShip(*worldGrid, *shape, rotation, position, motion, fraction, normal)) {
                            position += motion;
                            break;
                        }
                        // Arrêté un peu avant le contact (1 mm), pour ne pas s'y coller.
                        const float length = motion.Length();
                        position += motion * std::max(0.0f, fraction - 0.001f / length);
                        motion *= 1.0f - fraction;
                        motion -= normal * std::min(0.0f, motion.Dot(normal));
                        // Sa vitesse aussi perd ce qui allait dans l'obstacle (posé, il ne « pousse » plus le sol).
                        const JPH::Vec3 velocity = ToJolt(transform.rotation.Rotate(ship.velocity));
                        const JPH::Vec3 kept = velocity - normal * std::min(0.0f, velocity.Dot(normal));
                        ship.velocity = transform.rotation.Conjugate().Rotate(FromJolt(kept));
                    }
                    const JPH::Vec3 moved = position - start;
                    move = {moved.GetX(), moved.GetY(), moved.GetZ()};
                }
                if (!root) {
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
            bool simulated = collider.simulatePhysics && !hull && !collider.cameraOnly;
            if (simulated && scene.GetParent(entity) != grid.frame) {
                // La physique réécrirait la position locale comme si elle était celle de son repère.
                if (!warnedChild) {
                    Log::Warning("un objet simulé doit être une racine de la hiérarchie ou un enfant direct d'un vaisseau : "
                                 "« {} » est traité comme immobile", scene.Get<NameComponent>(entity).name);
                    warnedChild = true;
                }
                simulated = false;
            }
            const JPH::Shape* mesh = nullptr;
            if (collider.shape == ColliderShape::Mesh) {
                // Jolt ne simule pas un maillage (ni masse ni volume) : décor immobile ou coque.
                if (simulated && !warnedMeshSimulated) {
                    Log::Warning("« {} » : une collision en maillage ne peut pas être simulée, elle reste immobile",
                                 scene.Get<NameComponent>(entity).name);
                    warnedMeshSimulated = true;
                }
                simulated = false;
                const CollisionMesh* found = FindMesh(scene, entity, collider);
                mesh = found ? found->shape.GetPtr() : nullptr;
                if (!mesh) {
                    grid.bodies.emplace(entity, JPH::BodyID{}); // pas de nouvel essai à chaque image
                    return;
                }
            }
            const JPH::RefConst<JPH::Shape> shape = CreateShape(collider, transform.scale, simulated, mesh);
            if (!shape) {
                grid.bodies.emplace(entity, JPH::BodyID{}); // pas de nouvel essai à chaque image
                return;
            }
            const JPH::EMotionType motion = hull ? JPH::EMotionType::Kinematic
                                          : simulated ? JPH::EMotionType::Dynamic
                                                      : JPH::EMotionType::Static;
            const bool moving = motion != JPH::EMotionType::Static;
            JPH::BodyCreationSettings settings(shape, JPH::RVec3(ToJolt(transform.position)), ToJoltRotation(transform.rotation),
                                               motion, collider.cameraOnly ? Layers::CameraOnly
                                                       : moving           ? Layers::Moving
                                                                          : Layers::NonMoving);
            settings.mUserData = static_cast<JPH::uint64>(entt::to_integral(entity));
            settings.mFriction = collider.friction;
            settings.mRestitution = collider.restitution;
            settings.mGravityFactor = collider.gravityFactor;
            if (simulated && collider.mass > 0.0f) {
                settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
                settings.mMassPropertiesOverride.mMass = collider.mass;
            } else if (hull && mesh) {
                // Coque en maillage : ni masse ni volume, mais Jolt en exige une pour un corps qui bouge (jamais
                // utilisée : rien ne pousse un corps cinématique).
                settings.mOverrideMassProperties = JPH::EOverrideMassProperties::MassAndInertiaProvided;
                settings.mMassPropertiesOverride.mMass = 1.0f;
                settings.mMassPropertiesOverride.mInertia = JPH::Mat44::sIdentity();
            }
            const JPH::BodyID id = grid.system->GetBodyInterface().CreateAndAddBody(
                settings, moving ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
            grid.bodies.emplace(entity, id);
        }

        // Maillage de collision d'une entité : son fichier, ou à défaut celui de son modèle. Lu une fois par fichier.
        const CollisionMesh* FindMesh(const Scene& scene, Entity entity, const ColliderComponent& collider) {
            std::string_view relative = collider.mesh;
            if (relative.empty()) {
                if (const auto* model = scene.TryGet<ModelComponent>(entity)) {
                    relative = model->model;
                }
            }
            const CollisionMesh* mesh = relative.empty() ? nullptr : shared.FindMesh(relative);
            if (!mesh) {
                Log::Warning("« {} » : collision en maillage sans fichier (ni maillage, ni modèle 3D)",
                             scene.Get<NameComponent>(entity).name);
            }
            return mesh;
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
            // Seul le cap compte : un personnage reste debout (sur une planète, vers le haut de l'endroit).
            const JPH::RVec3 position(ToJolt(transform.position));
            const JPH::Vec3 forward = ToJolt(transform.rotation.Rotate(Math::Vector3f{0.0f, 0.0f, 1.0f}));
            character.character = new JPH::CharacterVirtual(settings, position, Facing(forward, UpFrom(GravityAt(grid, position))),
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
                        && scene.GetShip(entity) == grid->frame) {
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
                const Entity frame = scene.GetShip(entity);
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
                Grid* grid = FindGrid(scene.GetShip(entity));
                const RelativeTransform* transform = FindTransform(entity);
                if (grid && transform && !grid->bodies.contains(entity)) {
                    CreateBody(scene, *grid, entity, collider, grid->InFrame(*transform));
                }
            }
            SynchronizeMirrors(scene);
        }

        // Monde où vit le vaisseau d'un monde de vaisseau (celui de la scène, ou d'un vaisseau plus grand).
        Grid* FindOuterGrid(const Scene& scene, const Grid& grid) {
            return grid.frame == NullEntity ? nullptr : FindGrid(scene.GetShip(grid.frame));
        }

        // Copies, dans le monde qui l'entoure, des décors immobiles de chaque vaisseau : sans elles, on traverserait
        // le vaisseau du dehors et on ne pourrait pas monter à bord.
        void SynchronizeMirrors(const Scene& scene) {
            for (const std::unique_ptr<Grid>& outer : grids) {
                JPH::BodyInterface& outerBodies = outer->system->GetBodyInterface();
                std::erase_if(outer->mirrors, [&](const auto& entry) {
                    const auto [entity, id] = entry;
                    // Gardée tant que l'original existe, dans un vaisseau qui vit dans ce monde.
                    if (scene.IsValid(entity)) {
                        const Grid* inner = FindGrid(scene.GetShip(entity));
                        if (inner && FindOuterGrid(scene, *inner) == outer.get()) {
                            const auto source = inner->bodies.find(entity);
                            if (source != inner->bodies.end() && !source->second.IsInvalid()) {
                                return false;
                            }
                        }
                    }
                    outerBodies.RemoveBody(id);
                    outerBodies.DestroyBody(id);
                    return true;
                });
            }
            for (const std::unique_ptr<Grid>& inner : grids) {
                Grid* outer = FindOuterGrid(scene, *inner);
                if (!outer) {
                    continue;
                }
                const JPH::BodyInterface& innerBodies = inner->system->GetBodyInterface();
                for (const auto& [entity, id] : inner->bodies) {
                    if (id.IsInvalid() || outer->mirrors.contains(entity)
                        || innerBodies.GetMotionType(id) != JPH::EMotionType::Static
                        || innerBodies.GetObjectLayer(id) != Layers::NonMoving) {
                        continue; // caisses (dans le vaisseau seulement), verrière (caméra seulement)
                    }
                    const RelativeTransform* transform = FindTransform(entity);
                    if (!transform) {
                        continue;
                    }
                    const RelativeTransform target = outer->InFrame(*transform);
                    JPH::BodyCreationSettings settings(innerBodies.GetShape(id), JPH::RVec3(ToJolt(target.position)),
                                                       ToJoltRotation(target.rotation), JPH::EMotionType::Kinematic,
                                                       Layers::Moving);
                    settings.mUserData = static_cast<JPH::uint64>(entt::to_integral(inner->frame));
                    settings.mFriction = innerBodies.GetFriction(id);
                    // Un maillage n'a ni masse ni volume : Jolt en exige une pour un corps qui bouge, jamais utilisée
                    // (cinématique : rien ne le pousse).
                    settings.mOverrideMassProperties = JPH::EOverrideMassProperties::MassAndInertiaProvided;
                    settings.mMassPropertiesOverride.mMass = 1.0f;
                    settings.mMassPropertiesOverride.mInertia = JPH::Mat44::sIdentity();
                    outer->mirrors.emplace(entity, outer->system->GetBodyInterface().CreateAndAddBody(settings, JPH::EActivation::Activate));
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
                for (const auto& [entity, id] : grid->mirrors) {
                    if (const RelativeTransform* transform = FindTransform(entity)) {
                        const RelativeTransform target = grid->InFrame(*transform);
                        bodyInterface.MoveKinematic(id, JPH::RVec3(ToJolt(target.position)), ToJoltRotation(target.rotation),
                                                    StepSeconds * static_cast<float>(steps));
                    }
                }
            }
        }

        // Point d'un monde (dans son repère) ramené dans celui d'un autre.
        static Math::Vector3d Convert(const Grid& from, const Grid& to, JPH::RVec3Arg point) {
            Math::Vector3d position{point.GetX(), point.GetY(), point.GetZ()};
            if (from.frame != NullEntity) {
                position = from.framePosition + Math::Vector3d(from.frameRotation.Rotate(Math::Vector3f(position)));
            }
            return to.InFrame({NullEntity, position, {}, {1.0f, 1.0f, 1.0f}}).position;
        }

        // Un sol sous ces pieds (point du monde `to`, dans son repère) qui n'est pas le vaisseau `ignored`.
        bool HasGroundBelow(const Grid& to, const Math::Vector3d& feet, Entity ignored) const {
            class Filter final : public JPH::BodyFilter {
                public:
                Filter(const Grid& grid, Entity ignored) : m_grid(grid), m_ignored(ignored) {}
                bool ShouldCollideLocked(const JPH::Body& body) const override {
                    const auto entity = static_cast<Entity>(static_cast<entt::id_type>(body.GetUserData()));
                    return entity != m_ignored && !m_grid.characters.contains(entity);
                }

                private:
                const Grid& m_grid;
                Entity m_ignored;
            };
            const Filter filter(to, ignored);
            // Du genou (30 cm au-dessus des pieds) à 30 cm sous eux, vers le bas de ce monde.
            const JPH::Vec3 down = -UpFrom(GravityAt(to, JPH::RVec3(ToJolt(feet))));
            constexpr float Reach = 0.3f;
            const JPH::RRayCast ray(JPH::RVec3(ToJolt(feet)) - down * Reach, down * (2.0f * Reach));
            JPH::RayCastResult hit;
            return to.system->GetNarrowPhaseQuery().CastRay(ray, hit, to.system->GetDefaultBroadPhaseLayerFilter(Layers::Moving),
                                                            to.system->GetDefaultLayerFilter(Layers::Moving), filter);
        }

        // Monde où un personnage doit passer, d'après ce qui le porte : monter sur le décor d'un vaisseau (sa copie
        // dans ce monde) le fait passer à bord ; en l'air à bord, avec le sol du dehors sous les pieds, il en sort.
        // nullptr : il reste où il est.
        Grid* FindTransfer(const Scene& scene, const Grid& grid, const Character& character) {
            const JPH::CharacterVirtual& body = *character.character;
            const auto state = body.GetGroundState();
            if (state == JPH::CharacterVirtual::EGroundState::OnGround) {
                const auto ground = static_cast<Entity>(static_cast<entt::id_type>(body.GetGroundUserData()));
                if (ground != grid.frame && scene.IsValid(ground) && scene.TryGet<ShipComponent>(ground)
                    && scene.GetShip(ground) == grid.frame) {
                    return FindGrid(ground);
                }
                return nullptr;
            }
            Grid* outer = FindOuterGrid(scene, grid);
            if (state != JPH::CharacterVirtual::EGroundState::InAir || !outer) {
                return nullptr;
            }
            return HasGroundBelow(*outer, Convert(grid, *outer, body.GetPosition()), grid.frame) ? outer : nullptr;
        }

        // Personnages passés à bord d'un vaisseau ou sortis : ils changent de parent, à la même place dans le monde ;
        // leur corps change de monde à la prochaine mise à jour.
        void TransferCharacters(Scene& scene) {
            constexpr double Cooldown = 0.5; // secondes
            std::erase_if(transfers, [&](const auto& entry) { return time - entry.second >= Cooldown; });
            std::vector<std::pair<Entity, Grid*>> moves;
            for (const std::unique_ptr<Grid>& grid : grids) {
                for (const auto& [entity, character] : grid->characters) {
                    const auto last = transfers.find(entity);
                    if (character.seat != NullEntity || (last != transfers.end() && time - last->second < Cooldown)) {
                        continue;
                    }
                    if (Grid* target = FindTransfer(scene, *grid, character)) {
                        moves.emplace_back(entity, target);
                    }
                }
            }
            for (const auto& [entity, target] : moves) {
                const Grid* from = FindGrid(scene.GetShip(entity));
                const Character& character = from->characters.at(entity);
                const JPH::CharacterVirtual& body = *character.character;
                const Math::Vector3d position = Convert(*from, *target, body.GetPosition());
                // Debout dans le nouveau repère, même cap dans le monde (les repères du monde sont l'identité).
                const Math::Quaternionf rotation = target->frameRotation.Conjugate() * from->frameRotation * FromJolt(body.GetRotation());
                const JPH::RVec3 feet(ToJolt(position));
                scene.SetParent(entity, target->frame);
                Write(scene, *target, entity, feet, Upright(ToJoltRotation(rotation), UpFrom(GravityAt(*target, feet))));
                transfers[entity] = time;
            }
        }

        // Personnage sur un siège : posé dessus, face à son -z, sans physique (true). En se relevant, il est remis
        // debout devant le siège.
        bool Sit(const Scene& scene, const Grid& grid, Character& character, Entity seat, JPH::Vec3Arg up) {
            if (seat == NullEntity && character.seat == NullEntity) {
                return false; // debout, comme d'habitude
            }
            JPH::CharacterVirtual& body = *character.character;
            const Entity previous = std::exchange(character.seat, NullEntity);
            const RelativeTransform* found = FindTransform(seat != NullEntity ? seat : previous);
            if (!found) {
                return false; // pas de siège, ou siège disparu : il reste où il est
            }
            const RelativeTransform transform = grid.InFrame(*found);
            const Math::Vector3f forward = transform.rotation.Rotate(Math::Vector3f{0.0f, 0.0f, -1.0f});
            JPH::Vec3 position = ToJolt(transform.position);
            if (seat == NullEntity) {
                // Vient de se relever : à la sortie du siège, dans son repère.
                const auto* component = scene.IsValid(previous) ? scene.TryGet<SeatComponent>(previous) : nullptr;
                const Math::Vector3f exit = component ? component->exit : SeatComponent{}.exit;
                position += ToJolt(transform.rotation.Rotate(exit));
                body.SetPosition(JPH::RVec3(position));
                body.SetLinearVelocity(JPH::Vec3::sZero());
                return false;
            }
            // Le modèle regarde vers son +z : dans le sens du siège.
            body.SetPosition(JPH::RVec3(position));
            body.SetRotation(Facing(ToJolt(forward), up));
            body.SetLinearVelocity(JPH::Vec3::sZero());
            character.groundSpeed = 0.0f;
            character.airTime = 0.0f;
            character.seat = seat;
            return true;
        }

        void UpdateCharacters(Grid& grid, const Scene& scene, const CharacterInput& input) {
            JPH::PhysicsSystem& system = *grid.system;
            // Commandes dans le repère du monde, ramenées dans celui du vaisseau.
            const Math::Vector3f move = grid.frameRotation.Conjugate().Rotate(input.move);
            for (auto& [entity, character] : grid.characters) {
                const CharacterComponent& component = scene.Get<CharacterComponent>(entity);
                JPH::CharacterVirtual& body = *character.character;
                // Le bas de l'endroit : celui du monde, du vaisseau, ou le centre de la planète.
                const JPH::Vec3 gravity = GravityAt(grid, body.GetPosition());
                const JPH::Vec3 up = UpFrom(gravity);
                body.SetUp(up);
                if (Sit(scene, grid, character, component.seat, up)) {
                    continue;
                }
                JPH::Quat rotation = Upright(body.GetRotation(), up);
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

                // Le personnage se tourne peu à peu vers où il marche, autour du haut de l'endroit.
                if (wanted.LengthSq() > 1.0e-4f) {
                    const JPH::Vec3 forward = rotation.RotateAxisZ();
                    const JPH::Vec3 target = wanted.Normalized();
                    const float difference = std::atan2(forward.Cross(target).Dot(up), forward.Dot(target));
                    constexpr float TurnSpeed = 12.0f; // rad/s
                    const float turn = std::clamp(difference, -TurnSpeed * StepSeconds, TurnSpeed * StepSeconds);
                    rotation = (JPH::Quat::sRotation(up, turn) * rotation).Normalized();
                }
                body.SetRotation(rotation);

                JPH::CharacterVirtual::ExtendedUpdateSettings update;
                update.mStickToFloorStepDown = -up * 0.5f;
                update.mWalkStairsStepUp = up * 0.4f;
                const JPH::RVec3 before = body.GetPosition();
                body.ExtendedUpdate(StepSeconds, gravity, update, system.GetDefaultBroadPhaseLayerFilter(Layers::Moving),
                                    system.GetDefaultLayerFilter(Layers::Moving), {}, {}, *shared.tempAllocator);

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

        // Tous les corps de la scène et l'enveloppe des vaisseaux, sans avancer la simulation.
        void Prepare(Scene& scene) {
            if (!anchored) {
                ChooseAnchor(scene);
                if (!anchored) {
                    return;
                }
            }
            scene.ComputeRelativeTransforms(anchor, transforms);
            Synchronize(scene);
            for (const Entity entity : scene.GetRegistry().view<const ShipComponent>()) {
                if (scene.GetParent(entity) == NullEntity) {
                    GetShipShape(scene, entity); // vaisseau à la racine : il bute sur le décor (MoveShips)
                }
            }
            // Le sol des planètes autour de tout ce qui peut le toucher, sans attendre : rien ne tombe au travers.
            CollectProbes(scene);
            planetGround.Update(scene, anchor, probes, *grids.front()->system, true);
        }

        void Update(Scene& scene, double deltaSeconds, const CharacterInput& input) {
            if (!anchored) {
                ChooseAnchor(scene);
                if (!anchored) {
                    return; // rien de physique dans la scène
                }
            }
            Recenter(scene);
            MoveShips(scene, deltaSeconds);
            scene.ComputeRelativeTransforms(anchor, transforms);
            Synchronize(scene);
            CollectProbes(scene);
            planetGround.Update(scene, anchor, probes, *grids.front()->system, false);

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
                    ApplyPlanetGravity(*grid);
                    grid->system->Update(StepSeconds, 1, shared.tempAllocator.get(), shared.jobSystem.get());
                }
            }
            time += steps * static_cast<double>(StepSeconds);
            for (const std::unique_ptr<Grid>& grid : grids) {
                WriteBack(scene, *grid);
            }
            TransferCharacters(scene);
        }
        // Recul libre d'une sphère (la caméra) : fraction de `sweep` parcourue avant de toucher, dans chaque monde.
        float CastSphere(const Scene& scene, Entity followed, const Math::WorldPosition& from, const Math::Vector3f& sweep,
                         float radius, Entity ignoredShip) const {
            if (!anchored) {
                return 1.0f;
            }
            // Coques des vaisseaux où se trouve le joueur : la caméra est dedans, elles ne l'arrêtent pas.
            std::unordered_set<Entity> aboard;
            for (Entity ship = scene.GetShip(followed); ship != NullEntity; ship = scene.GetShip(ship)) {
                aboard.insert(ship);
            }
            if (ignoredShip != NullEntity) {
                aboard.insert(ignoredShip); // sa coque, et les copies de son décor dans le monde extérieur
            }
            const Math::Vector3d start = Math::Delta(anchor, from);
            const JPH::RefConst<JPH::Shape> sphere = new JPH::SphereShape(radius);
            float fraction = 1.0f;
            for (const std::unique_ptr<Grid>& grid : grids) {
                if (ignoredShip != NullEntity && grid->frame == ignoredShip) {
                    continue; // son décor de bord, dans son propre monde
                }
                // Les objets simulés (caisses) et les personnages ne repoussent pas la caméra : elle sauterait sans
                // cesse. Seuls les décors immobiles et les coques comptent.
                class Filter final : public JPH::BodyFilter {
                    public:
                    Filter(const Grid& grid, const std::unordered_set<Entity>& aboard) : m_grid(grid), m_aboard(aboard) {}
                    bool ShouldCollideLocked(const JPH::Body& body) const override {
                        const auto entity = static_cast<Entity>(static_cast<entt::id_type>(body.GetUserData()));
                        return !body.IsDynamic() && !m_grid.characters.contains(entity) && !m_aboard.contains(entity);
                    }

                    private:
                    const Grid& m_grid;
                    const std::unordered_set<Entity>& m_aboard;
                };
                const Filter filter(*grid, aboard);
                const Math::Quaternionf inverse = grid->frameRotation.Conjugate();
                const bool world = grid->frame == NullEntity;
                const Math::Vector3d origin = world ? start : inverse.Rotate(start - grid->framePosition);
                const Math::Vector3f direction = world ? sweep : inverse.Rotate(sweep);
                const JPH::RShapeCast cast(sphere, JPH::Vec3::sOne(), JPH::RMat44::sTranslation(JPH::RVec3(ToJolt(origin))),
                                           ToJolt(direction));
                JPH::ShapeCastSettings settings;
                // Vue de l'intérieur, une verrière ou une coque en maillage montre l'envers de ses faces.
                settings.mBackFaceModeTriangles = JPH::EBackFaceMode::CollideWithBackFaces;
                JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> collector;
                grid->system->GetNarrowPhaseQuery().CastShape(cast, settings, JPH::RVec3::sZero(), collector, {}, {}, filter);
                if (collector.HadHit()) {
                    fraction = std::min(fraction, collector.mHit.mFraction);
                }
            }
            return fraction;
        }
    };

    PhysicsWorld::PhysicsWorld(std::shared_ptr<PhysicsContext> context) {
        if (!context) {
            context = std::make_shared<PhysicsContext>();
        }
        PhysicsContext::Impl& details = *context->m_impl;
        m_impl = std::make_unique<Impl>(std::move(context), details);
    }

    PhysicsWorld::~PhysicsWorld() = default;

    void PhysicsWorld::Prepare(Scene& scene) {
        m_impl->Prepare(scene);
    }

    void PhysicsWorld::Update(Scene& scene, double deltaSeconds, const CharacterInput& input) {
        m_impl->Update(scene, deltaSeconds, input);
    }

    void PhysicsWorld::SetGravity(const Math::Vector3f& gravity) {
        m_impl->grids.front()->system->SetGravity(ToJolt(gravity));
    }

    void PhysicsWorld::SetAssetsDirectory(const std::filesystem::path& directory) {
        m_impl->context->SetAssetsDirectory(directory);
    }

    Math::Vector3f PhysicsWorld::GetGravity() const {
        return FromJolt(m_impl->grids.front()->system->GetGravity());
    }

    Math::Vector3f PhysicsWorld::GetUp(const Scene& scene, Entity entity) const {
        const Math::Vector3f y{0.0f, 1.0f, 0.0f};
        if (!scene.IsValid(entity)) {
            return y;
        }
        const Impl& impl = *m_impl;
        const Entity ship = scene.GetShip(entity);
        if (ship != NullEntity) {
            // À bord : le haut du pont (sans gravité, celui du vaisseau quand même).
            const auto grid = std::ranges::find(impl.grids, ship, [](const std::unique_ptr<Grid>& candidate) { return candidate->frame; });
            const JPH::Vec3 gravity = grid != impl.grids.end() ? (*grid)->system->GetGravity() : JPH::Vec3::sZero();
            return scene.GetWorldRotation(ship).Rotate(FromJolt(UpFrom(gravity)));
        }
        const JPH::Vec3 base = impl.grids.front()->system->GetGravity();
        if (!impl.anchored) {
            return FromJolt(UpFrom(base));
        }
        const Math::Vector3f gravity = impl.planetGround.GetGravity(Math::Delta(impl.anchor, scene.GetWorldPosition(entity)), FromJolt(base));
        return FromJolt(UpFrom(ToJolt(gravity)));
    }

    Math::Vector3f PhysicsWorld::GetOutsideGravity(const Scene& scene, Entity entity) const {
        const Impl& impl = *m_impl;
        const Math::Vector3f base = FromJolt(impl.grids.front()->system->GetGravity());
        if (!scene.IsValid(entity) || !impl.anchored) {
            return base;
        }
        return impl.planetGround.GetGravity(Math::Delta(impl.anchor, scene.GetWorldPosition(entity)), base);
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

    float PhysicsWorld::CastCamera(const Scene& scene, Entity player, const Math::WorldPosition& from,
                                   const Math::Vector3f& direction, float distance, float radius, Entity ignoredShip) const {
        if (distance <= 0.0f) {
            return 0.0f;
        }
        return distance * m_impl->CastSphere(scene, player, from, direction * distance, radius, ignoredShip);
    }
}
