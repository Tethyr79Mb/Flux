#include "Flux/Scene/SceneMerge.hpp"

#include "Flux/Scene/Scene.hpp"
#include "Flux/Scene/SceneSerializer.hpp"

#include <nlohmann/json.hpp>

#include <format>
#include <functional>
#include <unordered_map>
#include <unordered_set>

namespace Flux::SceneMerge {
    namespace {
        using Json = nlohmann::json;

        // Instantanés (SceneSerializer::SaveEntity) d'une entité seule : tous ses champs, au même format pour les trois
        // versions. Parents avant enfants, l'ordre dans lequel RestoreEntity peut recréer une entité apparue.
        std::vector<Json> Snapshots(const Scene& scene, const ComponentRegistry& components) {
            std::vector<Json> snapshots;
            const std::function<void(Entity)> visit = [&](Entity entity) {
                snapshots.push_back(Json::parse(SceneSerializer::SaveEntity(scene, components, entity, false)));
                scene.ForEachChild(entity, visit);
            };
            scene.ForEachRoot(visit);
            return snapshots;
        }

        Json& EntityOf(Json& snapshot) { return snapshot["entities"][0]; }
        const Json& EntityOf(const Json& snapshot) { return snapshot.at("entities").at(0); }

        Uuid IdOf(const Json& entity) { return Uuid::Parse(entity.at("id").get<std::string>()).value_or(Uuid{}); }

        const Json* Find(const Json& object, const std::string& key) {
            const auto found = object.find(key);
            return found != object.end() ? &*found : nullptr;
        }

        // Absent des deux côtés compte comme égal (secteur nul, composant retiré).
        bool Same(const Json* a, const Json* b) { return a ? b && *a == *b : !b; }

        // Clé par clé (nom, parent, secteur, composants ; champs d'un composant si components), ce qui a changé de base
        // à changed passe dans target, sauf si target l'a changé lui aussi.
        void MergeObject(Json& target, const Json& base, const Json& changed, bool components) {
            std::vector<std::string> keys;
            for (const auto& [key, value] : base.items()) {
                keys.push_back(key);
            }
            for (const auto& [key, value] : changed.items()) {
                if (!base.contains(key)) {
                    keys.push_back(key);
                }
            }
            for (const std::string& key : keys) {
                const Json* before = Find(base, key);
                const Json* after = Find(changed, key);
                if (Same(before, after)) {
                    continue;
                }
                const auto current = target.find(key);
                Json* own = current != target.end() ? &*current : nullptr;
                // Composant présent partout : champ par champ (la partie change la vitesse d'un vaisseau, l'éditeur sa
                // vitesse max).
                if (components && before && after && own && before->is_object() && after->is_object() && own->is_object()) {
                    MergeObject(*own, *before, *after, false);
                    continue;
                }
                if (!Same(own, before)) {
                    continue;
                }
                if (after) {
                    target[key] = *after;
                } else {
                    target.erase(key);
                }
            }
        }
    }

    std::vector<std::string> ApplyChanges(Scene& target, const Scene& base, const Scene& changed,
                                          const ComponentRegistry& components) {
        std::unordered_map<Uuid, Json> original;
        for (Json& snapshot : Snapshots(base, components)) {
            Json entity = std::move(EntityOf(snapshot));
            const Uuid id = IdOf(entity);
            original.emplace(id, std::move(entity));
        }

        std::vector<std::string> problems;
        const auto restore = [&](const Json& snapshot) {
            if (auto restored = SceneSerializer::RestoreEntity(target, components, snapshot.dump()); !restored) {
                problems.push_back(std::format("« {} » : {}", EntityOf(snapshot).value("name", std::string{}), restored.error()));
            }
        };
        std::unordered_set<Uuid> kept;
        for (const Json& snapshot : Snapshots(changed, components)) {
            const Json& entity = EntityOf(snapshot);
            const Uuid id = IdOf(entity);
            kept.insert(id);
            const Entity existing = target.FindById(id);
            const auto before = original.find(id);
            if (before == original.end()) {
                // Apparue pendant la partie (vaisseau d'une mission, caisse lâchée...) : ajoutée à sa place.
                if (existing == NullEntity) {
                    restore(snapshot);
                }
                continue;
            }
            if (existing == NullEntity || before->second == entity) {
                continue; // retirée dans l'éditeur, ou pas touchée par la partie (presque tout le décor)
            }
            Json merged = Json::parse(SceneSerializer::SaveEntity(target, components, existing, false));
            const Json own = EntityOf(merged);
            MergeObject(EntityOf(merged), before->second, entity, true);
            if (EntityOf(merged) != own) {
                restore(merged);
            }
        }
        // Détruites pendant la partie (vaisseau abattu...) : après les autres, qui ont pu quitter leur parent détruit.
        for (const auto& [id, entity] : original) {
            if (const Entity destroyed = kept.contains(id) ? NullEntity : target.FindById(id); destroyed != NullEntity) {
                target.DestroyEntity(destroyed);
            }
        }
        return problems;
    }
}
