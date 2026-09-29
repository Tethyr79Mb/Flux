// Tests de la sélection dans la vue 3D : rayons, projection à l'écran, cubes touchés, axe du gizmo.
#include "Check.hpp"

#include "Flux/Graphics/Camera.hpp"
#include "Flux/Scene/Picking.hpp"
#include "Flux/Scene/Scene.hpp"

#include <cmath>
#include <numbers>
#include <vector>

using namespace Flux;

namespace {
    bool Near(double a, double b, double eps = 1e-4) { return std::abs(a - b) < eps; }
    bool Near(Math::Vector3d a, Math::Vector3d b, double eps = 1e-6) { return Math::Length(a - b) < eps; }
    bool Near(Math::Vector2f a, Math::Vector2f b, float eps = 0.01f) {
        return std::abs(a.x - b.x) < eps && std::abs(a.y - b.y) < eps;
    }
}

bool RunPickingTests() {
    // Caméra par défaut : regarde vers -z. Vue de 200 x 100 pixels décalée de (100, 50).
    Camera camera;
    const ScreenProjection projection(camera, {100.0f, 50.0f}, {300.0f, 150.0f});
    const Ray center = projection.GetRay({200.0f, 100.0f});
    CHECK(Near(center.direction, {0.0, 0.0, -1.0}));

    // Aller-retour écran -> rayon -> écran, y compris hors de l'axe (en haut à droite).
    const Ray corner = projection.GetRay({290.0f, 60.0f});
    CHECK(corner.direction.x > 0.0 && corner.direction.y > 0.0);
    const auto back = projection.ToScreen(corner.direction * 25.0);
    CHECK(back && Near(*back, {290.0f, 60.0f}));
    // Bord haut de la vue : demi-champ vertical de 30°.
    const Ray top = projection.GetRay({200.0f, 50.0f});
    CHECK(Near(std::atan2(top.direction.y, -top.direction.z), std::numbers::pi / 6.0));
    CHECK(!projection.ToScreen({0.0, 0.0, 5.0})); // derrière la caméra
    CHECK(Near(projection.PixelsToMeters(50.0, 10.0), 10.0 * std::tan(std::numbers::pi / 6.0)));

    // Segment traversant le plan proche : coupé, sa partie visible reste projetée.
    Math::Vector2f a;
    Math::Vector2f b;
    CHECK(projection.ProjectSegment({1.0, 0.0, -10.0}, {1.0, 0.0, 10.0}, a, b));
    CHECK(Near(a, *projection.ToScreen({1.0, 0.0, -10.0})) && b.x > a.x);
    CHECK(!projection.ProjectSegment({0.0, 0.0, 1.0}, {1.0, 0.0, 2.0}, a, b));

    // Cube de 2 m à 10 m devant, tourné de 45° autour de y : son arête avant est à 10 - sqrt(2).
    RelativeTransform cube{NullEntity, {0.0, 0.0, -10.0}, Math::Quaternionf::FromAxisAngle({0, 1, 0}, std::numbers::pi_v<float> / 4),
                           {2.0f, 2.0f, 2.0f}};
    const auto hit = IntersectBox(center, cube);
    CHECK(hit && Near(*hit, 10.0 - std::sqrt(2.0)));
    CHECK(!IntersectBox(projection.GetRay({290.0f, 60.0f}), cube));
    CHECK(!IntersectBox({{}, {0.0, 0.0, 1.0}}, cube)); // derrière le rayon
    cube.position = {0.0, 0.0, 0.5};
    CHECK(IntersectBox(center, cube) == 0.0); // la caméra est dedans
    cube.scale = {0.0f, 1.0f, 1.0f};
    CHECK(!IntersectBox(center, cube));

    // Boîte d'un modèle, décentrée (un personnage : pieds à l'origine), 1 m devant la caméra, à l'échelle 2.
    const RelativeTransform figure{NullEntity, {0.0, -1.0, -5.0}, {}, {2.0f, 2.0f, 2.0f}};
    const Math::Box3f standing{{-0.25f, 0.0f, -0.1f}, {0.25f, 1.8f, 0.1f}};
    const auto body = IntersectBox(center, figure, standing); // le rayon passe à 1 m au-dessus des pieds
    CHECK(body && Near(*body, 5.0 - 0.2));
    const RelativeTransform lying{NullEntity, {0.0, 1.0, -5.0}, {}, {2.0f, 2.0f, 2.0f}};
    CHECK(!IntersectBox(center, lying, standing)); // pieds 1 m au-dessus du rayon : manqué

    // Scène : le plus proche des cubes visibles est choisi ; une entité sans maillage est ignorée.
    Scene scene;
    const Entity farCube = scene.CreateEntity("Loin");
    scene.Add<MeshRendererComponent>(farCube);
    scene.Get<TransformComponent>(farCube).position = {0.0, 0.0, -20.0};
    const Entity nearCube = scene.CreateEntity("Près");
    scene.Add<MeshRendererComponent>(nearCube);
    scene.Get<TransformComponent>(nearCube).position = {0.0, 0.0, -5.0};
    const Entity empty = scene.CreateEntity("Vide");
    scene.Get<TransformComponent>(empty).position = {0.0, 0.0, -2.0};
    scene.Get<TransformComponent>(empty).scale = {10.0f, 10.0f, 10.0f};
    std::vector<RelativeTransform> transforms;
    scene.ComputeRelativeTransforms(camera.position, transforms);
    const MeshBoundsFunction cubes = [&scene](Entity entity) -> std::optional<Math::Box3f> {
        if (scene.TryGet<MeshRendererComponent>(entity)) {
            return Math::Box3f{};
        }
        return std::nullopt;
    };
    CHECK(PickEntity(transforms, center, cubes) == nearCube);
    CHECK(PickEntity(transforms, projection.GetRay({290.0f, 60.0f}), cubes) == NullEntity);
    // Loin de l'origine (un secteur plus loin) : tout reste relatif à la caméra.
    camera.position.sector = {5, 0, 0};
    scene.Add<SectorComponent>(farCube, Math::Vector3i64{5, 0, 0});
    scene.ComputeRelativeTransforms(camera.position, transforms);
    CHECK(PickEntity(transforms, center, cubes) == farCube);

    // Axe du gizmo : droite x passant par (0, 0, -10) ; la souris vise le point x = 3.
    const Ray aim{{}, Math::Normalize(Math::Vector3d{3.0, 0.0, -10.0})};
    const auto t = ClosestPointOnLine(aim, {0.0, 0.0, -10.0}, {1.0, 0.0, 0.0});
    CHECK(t && Near(*t, 3.0));
    CHECK(!ClosestPointOnLine(center, {0.0, 0.0, -10.0}, {0.0, 0.0, 1.0})); // axe dans l'axe de visée
    return true;
}
