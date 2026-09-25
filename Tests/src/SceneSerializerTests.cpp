// Tests de la sauvegarde : aller-retour exact, ordre des enfants, fichiers invalides refusés sans rien casser.
#include "Check.hpp"

#include "Flux/Scene/Scene.hpp"
#include "Flux/Scene/SceneSerializer.hpp"

#include <string>

using namespace Flux;

namespace {
    // Même contenu, entité par entité (retrouvées par Uuid).
    bool SameScene(const Scene& a, const Scene& b) {
        if (a.GetEntityCount() != b.GetEntityCount()) {
            return false;
        }
        bool same = true;
        for (const auto [entity, id] : a.GetRegistry().view<const IdComponent>().each()) {
            const Entity other = b.FindById(id.id);
            if (other == NullEntity) {
                return false;
            }
            const auto& ta = a.Get<TransformComponent>(entity);
            const auto& tb = b.Get<TransformComponent>(other);
            const Entity parentA = a.GetParent(entity);
            const Entity parentB = b.GetParent(other);
            const auto* sa = a.TryGet<SectorComponent>(entity);
            const auto* sb = b.TryGet<SectorComponent>(other);
            const auto* ma = a.TryGet<MeshRendererComponent>(entity);
            const auto* mb = b.TryGet<MeshRendererComponent>(other);
            same = same
                && a.Get<NameComponent>(entity).name == b.Get<NameComponent>(other).name
                && ta.position == tb.position && ta.rotation == tb.rotation && ta.scale == tb.scale // égalité exacte
                && (parentA == NullEntity ? parentB == NullEntity : parentB != NullEntity && a.GetId(parentA) == b.GetId(parentB))
                && (sa ? sa->sector : Math::Vector3i64{}) == (sb ? sb->sector : Math::Vector3i64{})
                && (ma == nullptr) == (mb == nullptr)
                && (!ma || (ma->mesh == mb->mesh && ma->color == mb->color));
        }
        return same;
    }
}

bool RunSceneSerializerTests() {
    Scene scene;
    const Entity root = scene.CreateEntity("Système « accentué »");
    scene.Add<SectorComponent>(root, Math::Vector3i64{-9'460'730, 42, 1LL << 40});
    scene.Get<TransformComponent>(root) = {{0.1, 1.0 / 3.0, -123456789.123456789},
                                           Math::Quaternionf::FromYawPitchRoll(0.3f, -0.2f, 0.1f), {1.0f, 2.5f, 0.1f}};
    const Entity first = scene.CreateEntity("Premier", root);
    const Entity second = scene.CreateEntity("Second", root);
    scene.CreateEntity("Petit-enfant", first);
    scene.Add<MeshRendererComponent>(second, MeshType::Cube, Math::LinearColor{0.1f, 0.2f, 0.3f, 0.5f});
    scene.CreateEntity("Autre racine");

    // Aller-retour : tout est identique, au bit près pour les doubles.
    const std::string saved = SceneSerializer::SaveToString(scene);
    Scene loaded;
    CHECK(SceneSerializer::LoadFromString(loaded, saved).has_value());
    CHECK(SameScene(scene, loaded));
    CHECK(SceneSerializer::SaveToString(loaded) == saved); // fichier stable : pas de diff git sans modification

    // L'ordre des enfants est conservé.
    Entity children[2]{};
    int count = 0;
    loaded.ForEachChild(loaded.FindById(scene.GetId(root)), [&](Entity child) { children[count++] = child; });
    CHECK(count == 2 && loaded.GetId(children[0]) == scene.GetId(first) && loaded.GetId(children[1]) == scene.GetId(second));

    // Fichiers invalides : erreur explicite, et la scène déjà chargée reste intacte.
    const char* invalid[] = {
        "pas du json",
        R"({"format": "autre", "version": 1, "entities": []})",
        R"({"format": "flux.scene", "version": 99, "entities": []})",
        R"({"format": "flux.scene", "version": 1, "entities": [{"id": "0000000000000001", "parent": "0000000000000002"}]})",
        R"({"format": "flux.scene", "version": 1, "entities": [{"id": "0000000000000001"}, {"id": "0000000000000001"}]})",
        R"({"format": "flux.scene", "version": 1, "entities": [
            {"id": "0000000000000001", "parent": "0000000000000002"},
            {"id": "0000000000000002", "parent": "0000000000000001"}]})",
        R"({"format": "flux.scene", "version": 1, "entities": [{"id": "0000000000000001", "meshRenderer": {"mesh": "théière"}}]})",
        R"({"format": "flux.scene", "version": 1, "entities": [{"id": "0000000000000001", "transform": {"position": [1, 2]}}]})",
    };
    for (const char* text : invalid) {
        const auto result = SceneSerializer::LoadFromString(loaded, text);
        CHECK(!result.has_value() && !result.error().empty());
        CHECK(SameScene(scene, loaded));
    }

    // Écriture minimale à la main : les champs absents prennent leur valeur par défaut.
    CHECK(SceneSerializer::LoadFromString(loaded, R"({"format": "flux.scene", "version": 1, "entities": [
        {"id": "00000000000000ab", "name": "Cube", "meshRenderer": {}}]})").has_value());
    const Entity cube = loaded.FindById(Uuid{0xab});
    CHECK(cube != NullEntity && loaded.GetEntityCount() == 1);
    CHECK(loaded.Get<TransformComponent>(cube).scale == (Math::Vector3f{1.0f, 1.0f, 1.0f}));
    CHECK(loaded.TryGet<MeshRendererComponent>(cube) != nullptr);
    return true;
}
