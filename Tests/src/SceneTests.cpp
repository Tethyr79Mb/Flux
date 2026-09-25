// Tests de la scène : hiérarchie, transformations composées, secteurs.
#include "Check.hpp"

#include "Flux/Scene/Scene.hpp"

#include <cmath>
#include <numbers>
#include <vector>

using namespace Flux;

namespace {
    bool Near(Math::Vector3d a, Math::Vector3d b, double eps = 1e-6) { return Math::Length(a - b) < eps; }
}

bool RunSceneTests() {
    Scene scene;
    const Entity a = scene.CreateEntity("A");
    const Entity b = scene.CreateEntity("B", a);
    const Entity c = scene.CreateEntity("C", a);
    const Entity d = scene.CreateEntity("D", b);
    int count = 0; Entity order[2];
    scene.ForEachChild(a, [&](Entity e) { order[count++] = e; });
    CHECK(count == 2 && order[0] == b && order[1] == c);
    CHECK(!scene.SetParent(a, d));           // cycle refusé
    CHECK(!scene.SetParent(a, a));
    CHECK(scene.SetParent(c, b));             // C passe sous B
    count = 0; scene.ForEachChild(a, [&](Entity) { ++count; }); CHECK(count == 1);
    count = 0; scene.ForEachChild(b, [&](Entity) { ++count; }); CHECK(count == 2);
    CHECK(scene.SetParent(d, NullEntity));    // D devient racine
    int roots = 0; scene.ForEachRoot([&](Entity) { ++roots; }); CHECK(roots == 2);

    // Transformations : A tourne de 90° autour de y, B à (10,0,0) sous A => B en (0,0,-10)
    scene.Get<TransformComponent>(a).rotation = Math::Quaternionf::FromAxisAngle({0, 1, 0}, std::numbers::pi_v<float> / 2);
    scene.Get<TransformComponent>(a).scale = {2, 2, 2};
    scene.Get<TransformComponent>(b).position = {10, 0, 0};
    scene.Get<TransformComponent>(c).position = {1, 0, 0};
    std::vector<RelativeTransform> out;
    scene.ComputeRelativeTransforms({}, out);
    CHECK(out.size() == 4);
    for (const auto& t : out) {
        if (t.entity == b) CHECK(Near(t.position, {0, 0, -20}, 1e-4));    // échelle 2 du parent
        if (t.entity == c) CHECK(Near(t.position, {0, 0, -22}, 1e-4));    // C sous B : +1 en x local, tourné et mis à l'échelle
    }
    const Math::WorldPosition wc = scene.GetWorldPosition(c);
    CHECK(Near(wc.offset, {0, 0, -22}, 1e-4) && wc.sector == Math::Vector3i64{});

    // Secteurs : racine à une année-lumière, caméra dans le même secteur, 1 mm d'écart
    const Entity far = scene.CreateEntity("Loin");
    scene.Add<SectorComponent>(far, Math::Vector3i64{0, 0, -9'460'730});
    scene.Get<TransformComponent>(far).position = {0.001, 0, 0};
    Math::WorldPosition camera; camera.sector = {0, 0, -9'460'730};
    scene.ComputeRelativeTransforms(camera, out);
    for (const auto& t : out) {
        if (t.entity == far) {
            CHECK(std::abs(t.position.x - 0.001) < 1e-9); // 1 mm à une année-lumière, au nanomètre près
        }
    }

    // WorldPosition : normalisation et déplacement à travers une frontière de secteur
    Math::WorldPosition p; p += {6.0e8, 0, -1.5e9};
    CHECK(p.sector == (Math::Vector3i64{1, 0, -2}) && Near(p.offset, {-4.0e8, 0, 5.0e8}));
    CHECK(Near(Math::Delta({}, p), {6.0e8, 0, -1.5e9}));

    // Destruction récursive
    scene.DestroyEntity(a);
    CHECK(!scene.IsValid(a) && !scene.IsValid(b) && !scene.IsValid(c) && scene.IsValid(d));
    roots = 0; scene.ForEachRoot([&](Entity) { ++roots; }); CHECK(roots == 2);

    // Ordre des racines et ajout en fin de liste après suppressions (lastChild tenu à jour).
    const Entity e = scene.CreateEntity("E");
    std::vector<Entity> rootOrder;
    scene.ForEachRoot([&](Entity root) { rootOrder.push_back(root); });
    CHECK((rootOrder == std::vector<Entity>{d, far, e}));
    scene.DestroyEntity(e);
    const Entity f = scene.CreateEntity("F");
    const Entity g = scene.CreateEntity("G", f);
    scene.DestroyEntity(g);
    const Entity h = scene.CreateEntity("H", f);
    rootOrder.clear();
    scene.ForEachRoot([&](Entity root) { rootOrder.push_back(root); });
    CHECK((rootOrder == std::vector<Entity>{d, far, f}));
    std::vector<Entity> childOrder;
    scene.ForEachChild(f, [&](Entity child) { childOrder.push_back(child); });
    CHECK((childOrder == std::vector<Entity>{h}));
    scene.Clear();
    CHECK(scene.GetEntityCount() == 0);
    return true;
}
