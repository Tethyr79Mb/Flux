// Tests de la sauvegarde : aller-retour exact, ordre des enfants, fichiers invalides refusés sans rien casser.
#include "Check.hpp"

#include "Flux/Scene/ComponentRegistry.hpp"
#include "Flux/Scene/Scene.hpp"
#include "Flux/Scene/SceneSerializer.hpp"

#include <string>

using namespace Flux;

namespace {
    // Composant « de jeu » avec un champ de chaque type, décrit comme le ferait un module de jeu.
    enum class Mood : std::uint8_t { Calm, Angry = 7 };

    struct TestComponent
    {
        bool flag = false;
        std::int32_t count = 0;
        float ratio = 0.0f;
        double distance = 0.0;
        Math::Vector3f direction;
        Math::Vector3d target;
        Math::Quaternionf orientation;
        Math::LinearColor tint;
        std::string label;
        Mood mood = Mood::Calm;
    };

    void RegisterTestComponent(ComponentRegistry& components) {
        components.Register<TestComponent>("test", "Test")
            .Field("flag", "Drapeau", &TestComponent::flag)
            .Field("count", "Nombre", &TestComponent::count)
            .Field("ratio", "Rapport", &TestComponent::ratio)
            .Field("distance", "Distance", &TestComponent::distance)
            .Field("direction", "Direction", &TestComponent::direction)
            .Field("target", "Cible", &TestComponent::target)
            .Field("orientation", "Orientation", &TestComponent::orientation)
            .Field("tint", "Teinte", &TestComponent::tint)
            .Field("label", "Libellé", &TestComponent::label)
            .Enum("mood", "Humeur", &TestComponent::mood, {{"calm", "Calme", Mood::Calm}, {"angry", "Fâché", Mood::Angry}});
    }

    bool SameTest(const TestComponent* a, const TestComponent* b) {
        if (!a || !b) {
            return a == b;
        }
        return a->flag == b->flag && a->count == b->count && a->ratio == b->ratio && a->distance == b->distance
            && a->direction == b->direction && a->target == b->target && a->orientation == b->orientation
            && a->tint == b->tint && a->label == b->label && a->mood == b->mood;
    }

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
                && (!ma || (ma->mesh == mb->mesh && ma->color == mb->color))
                && SameTest(a.TryGet<TestComponent>(entity), b.TryGet<TestComponent>(other));
        }
        return same;
    }
}

bool RunSceneSerializerTests() {
    ComponentRegistry components;
    RegisterEngineComponents(components);
    RegisterTestComponent(components);

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
    scene.Add<TestComponent>(first, true, -42, 0.1f, 1.0 / 7.0, Math::Vector3f{0.0f, -1.0f, 0.5f},
                             Math::Vector3d{1e12, -2.5, 3.0}, Math::Quaternionf::FromYawPitchRoll(1.0f, 0.5f, 0.0f),
                             Math::LinearColor{0.25f, 0.5f, 0.75f, 1.0f}, "texte « accentué »", Mood::Angry);

    // Aller-retour : tout est identique, au bit près pour les doubles.
    const std::string saved = SceneSerializer::SaveToString(scene, components);
    Scene loaded;
    CHECK(SceneSerializer::LoadFromString(loaded, components, saved).has_value());
    CHECK(SameScene(scene, loaded));
    CHECK(SceneSerializer::SaveToString(loaded, components) == saved); // fichier stable : pas de diff git sans modification

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
        // Champ inconnu, types incorrects, valeur d'énumération inconnue, composant qui n'est pas un objet.
        R"({"format": "flux.scene", "version": 1, "entities": [{"id": "0000000000000001", "test": {"colour": 1}}]})",
        R"({"format": "flux.scene", "version": 1, "entities": [{"id": "0000000000000001", "test": {"flag": 1}}]})",
        R"({"format": "flux.scene", "version": 1, "entities": [{"id": "0000000000000001", "test": {"count": 1.5}}]})",
        R"({"format": "flux.scene", "version": 1, "entities": [{"id": "0000000000000001", "test": {"count": 3000000000}}]})",
        R"({"format": "flux.scene", "version": 1, "entities": [{"id": "0000000000000001", "test": {"label": 3}}]})",
        R"({"format": "flux.scene", "version": 1, "entities": [{"id": "0000000000000001", "test": {"direction": [1, "2", 3]}}]})",
        R"({"format": "flux.scene", "version": 1, "entities": [{"id": "0000000000000001", "test": {"mood": "joyeux"}}]})",
        R"({"format": "flux.scene", "version": 1, "entities": [{"id": "0000000000000001", "test": 5}]})",
        R"({"format": "flux.scene", "version": 1, "entities": [{"id": "0000000000000001", "meshRenderer": {"mesh": "théière"}}]})",
        R"({"format": "flux.scene", "version": 1, "entities": [{"id": "0000000000000001", "transform": {"position": [1, 2]}}]})",
    };
    for (const char* text : invalid) {
        const auto result = SceneSerializer::LoadFromString(loaded, components, text);
        CHECK(!result.has_value() && !result.error().empty());
        CHECK(SameScene(scene, loaded));
    }

    // Écriture minimale à la main : les champs absents prennent leur valeur par défaut.
    CHECK(SceneSerializer::LoadFromString(loaded, components, R"({"format": "flux.scene", "version": 1, "entities": [
        {"id": "00000000000000ab", "name": "Cube", "meshRenderer": {}}]})").has_value());
    const Entity cube = loaded.FindById(Uuid{0xab});
    CHECK(cube != NullEntity && loaded.GetEntityCount() == 1);
    CHECK(loaded.Get<TransformComponent>(cube).scale == (Math::Vector3f{1.0f, 1.0f, 1.0f}));
    CHECK(loaded.TryGet<MeshRendererComponent>(cube) != nullptr);

    // Le message d'une valeur d'énumération inconnue cite les valeurs possibles.
    const auto badMood = SceneSerializer::LoadFromString(loaded, components, R"({"format": "flux.scene", "version": 1,
        "entities": [{"id": "0000000000000001", "test": {"mood": "joyeux"}}]})");
    CHECK(!badMood && badMood.error().find("« angry »") != std::string::npos);

    // Valeurs hors limites (fichier abîmé ou modifié à la main) : ramenées dans les limites du champ.
    {
        ComponentRegistry engine;
        RegisterEngineComponents(engine);
        CHECK(SceneSerializer::LoadFromString(loaded, engine, R"({"format": "flux.scene", "version": 1, "entities": [
            {"id": "00000000000000ef", "collider": {"size": [0.0, 0.5, -2.0], "radius": 0.0, "restitution": 3.0}}]})").has_value());
        const ColliderComponent& collider = loaded.Get<ColliderComponent>(loaded.FindById(Uuid{0xef}));
        CHECK(collider.size.x > 0.0f && collider.size.y == 0.5f && collider.size.z > 0.0f);
        CHECK(collider.radius > 0.0f && collider.restitution == 1.0f);
    }

    // Composant de type inconnu (module de jeu absent) : chargé sans erreur, conservé et réécrit tel quel.
    const char* withUnknown = R"({"format": "flux.scene", "version": 1, "entities": [
        {"id": "00000000000000cd", "name": "Vaisseau", "shield": {"power": 12, "modes": ["a", "b"]}, "test": {"count": 3}}]})";
    ComponentRegistry engineOnly;
    RegisterEngineComponents(engineOnly);
    CHECK(SceneSerializer::LoadFromString(loaded, engineOnly, withUnknown).has_value());
    const Entity ship = loaded.FindById(Uuid{0xcd});
    CHECK(loaded.TryGet<TestComponent>(ship) == nullptr);
    CHECK(loaded.TryGet<UnknownComponentsComponent>(ship) != nullptr);
    const std::string resaved = SceneSerializer::SaveToString(loaded, engineOnly);
    CHECK(resaved.find(R"("shield": {)") != std::string::npos && resaved.find(R"("modes": [)") != std::string::npos);
    // Relu avec le type enregistré : le composant retrouve sa forme normale.
    CHECK(SceneSerializer::LoadFromString(loaded, components, resaved).has_value());
    CHECK(loaded.TryGet<TestComponent>(loaded.FindById(Uuid{0xcd}))->count == 3);

    // Déchargement d'un module : ses types disparaissent du registre, ceux du moteur restent.
    const std::size_t engineCount = engineOnly.GetCount();
    RegisterTestComponent(engineOnly);
    CHECK(engineOnly.GetCount() == engineCount + 1 && engineOnly.Find("test") != nullptr);
    RegisterTestComponent(engineOnly); // réenregistrement : remplace, n'ajoute pas
    CHECK(engineOnly.GetCount() == engineCount + 1);
    engineOnly.Truncate(engineCount);
    CHECK(engineOnly.Find("test") == nullptr && engineOnly.Find("transform") != nullptr);
    // Instantanés d'entité (annuler / refaire).
    {
        Scene edited;
        const Entity a = edited.CreateEntity("A");
        const Entity b = edited.CreateEntity("B");
        const Entity c = edited.CreateEntity("C");
        const Entity child = edited.CreateEntity("Enfant", b);
        const Entity grandChild = edited.CreateEntity("Petit-enfant", child);
        edited.Add<MeshRendererComponent>(child);
        edited.Get<TransformComponent>(grandChild).position = {1.0, 2.0, 3.0};
        const Uuid bId = edited.GetId(b);
        const Uuid childId = edited.GetId(child);

        // Modification annulée : nom, composants retirés ou ajoutés depuis reviennent à l'état de l'instantané.
        const std::string before = SceneSerializer::SaveEntity(edited, components, child, false);
        edited.Get<NameComponent>(child).name = "Renommé";
        edited.Remove<MeshRendererComponent>(child);
        edited.Add<TestComponent>(child).count = 4;
        edited.Get<TransformComponent>(child).position = {9.0, 9.0, 9.0};
        CHECK(SceneSerializer::RestoreEntity(edited, components, before));
        CHECK(edited.Get<NameComponent>(child).name == "Enfant");
        CHECK(edited.TryGet<MeshRendererComponent>(child) && !edited.TryGet<TestComponent>(child));
        CHECK(edited.Get<TransformComponent>(child).position == Math::Vector3d{});
        CHECK(edited.GetParent(child) == b);

        // Suppression annulée : B revient entre A et C, avec ses descendants et leurs identifiants.
        const std::string subtree = SceneSerializer::SaveEntity(edited, components, b, true);
        edited.DestroyEntity(b);
        CHECK(edited.GetEntityCount() == 2);
        CHECK(SceneSerializer::RestoreEntity(edited, components, subtree));
        CHECK(edited.GetEntityCount() == 5);
        const Entity restoredB = edited.FindById(bId);
        CHECK(restoredB != NullEntity && edited.GetNextSibling(a) == restoredB && edited.GetNextSibling(restoredB) == c);
        const Entity restoredChild = edited.FindById(childId);
        CHECK(restoredChild != NullEntity && edited.GetParent(restoredChild) == restoredB);
        CHECK(edited.TryGet<MeshRendererComponent>(restoredChild));
        const Entity restoredGrandChild = edited.FindByName("Petit-enfant");
        CHECK(edited.GetParent(restoredGrandChild) == restoredChild);
        CHECK(edited.Get<TransformComponent>(restoredGrandChild).position == Math::Vector3d(1.0, 2.0, 3.0));

        // Ordre changé (glisser-déposer dans la hiérarchie) puis annulé : A revient devant B, puis refait.
        const Uuid aId = edited.GetId(a);
        const std::string inPlace = SceneSerializer::SaveEntity(edited, components, a, false);
        edited.SetParent(a, NullEntity); // à la fin des racines : dessiné en dernier
        CHECK(edited.GetNextSibling(c) == a);
        const std::string moved = SceneSerializer::SaveEntity(edited, components, a, false);
        CHECK(SceneSerializer::RestoreEntity(edited, components, inPlace));
        CHECK(edited.GetNextSibling(edited.FindById(aId)) == restoredB && edited.GetNextSibling(c) == NullEntity);
        CHECK(SceneSerializer::RestoreEntity(edited, components, moved));
        CHECK(edited.GetNextSibling(c) == a && edited.GetNextSibling(a) == NullEntity);
        CHECK(SceneSerializer::RestoreEntity(edited, components, inPlace));

        // Parent disparu : refusé sans rien toucher.
        const std::string orphan = SceneSerializer::SaveEntity(edited, components, restoredGrandChild, false);
        edited.DestroyEntity(restoredChild);
        CHECK(!SceneSerializer::RestoreEntity(edited, components, orphan));
        CHECK(edited.GetEntityCount() == 3);
        CHECK(!SceneSerializer::RestoreEntity(edited, components, "{}"));
    }
    return true;
}
