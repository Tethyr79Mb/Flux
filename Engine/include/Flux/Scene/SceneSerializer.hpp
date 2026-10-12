#pragma once

#include "Flux/Export.hpp"

#include "Flux/Scene/Components.hpp"

#include <expected>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Flux {
    class ComponentRegistry;
    class Scene;

    // Scènes au format JSON (.scene.json), lisibles et modifiables à la main.
    // Les entités sont écrites parents d'abord, dans l'ordre de la hiérarchie ; leurs liens passent par leur Uuid.
    // Chaque composant enregistré dans components est un objet de ses champs, sous sa clé ("transform": {...}).
    // Un composant de type inconnu (module de jeu absent) est conservé tel quel et réécrit à la sauvegarde.
    // Un exemplaire de prefab (PrefabInstanceComponent) est écrit comme sa racine, plus le chemin du prefab et ce qui
    // diffère du prefab sur chacune de ses pièces ("prefab", "overrides") ; ses pièces sont refaites depuis le fichier
    // du prefab au chargement. Les entités ajoutées sous ses pièces sont écrites à la suite, comme les autres.
    // Un prefab (.prefab.json, format flux.prefab) a la forme d'une scène : une seule racine et ses descendants.
    namespace SceneSerializer {
        // À incrémenter à chaque changement du format ; Load refuse un fichier plus récent que le moteur.
        // 2 : exemplaires de prefabs.
        inline constexpr int FormatVersion = 2;

        // Texte d'un prefab, d'après son chemin relatif aux assets ; une erreur s'il est introuvable.
        using PrefabReader = std::function<std::expected<std::string, std::string>(std::string_view path)>;
        // Lit les prefabs dans ce dossier d'assets.
        FLUX_API PrefabReader ReadPrefabsFrom(std::filesystem::path assetsDirectory);

        // Sans lecteur de prefabs (ou prefab illisible), un exemplaire est écrit avec tous les réglages de ses pièces :
        // le fichier reste juste, juste plus gros.
        FLUX_API std::string SaveToString(const Scene& scene, const ComponentRegistry& components,
                                          const PrefabReader& prefabs = {});
        // Remplace tout le contenu de scene. En cas d'erreur, scene n'est pas modifiée. Lit aussi un prefab (ses
        // entités, à ouvrir pour le modifier). Un exemplaire de prefab sans lecteur, ou dont le prefab est
        // introuvable, est une erreur.
        FLUX_API std::expected<void, std::string> LoadFromString(Scene& scene, const ComponentRegistry& components,
                                                                 std::string_view json, const PrefabReader& prefabs = {});

        // Instantané d'une entité (annuler / refaire) : ses données au format du fichier, sa place parmi ses frères,
        // et celles de tous ses descendants si withDescendants. Les pièces d'un prefab y sont écrites en entier, avec
        // leur lien au prefab.
        FLUX_API std::string SaveEntity(const Scene& scene, const ComponentRegistry& components, Entity entity,
                                        bool withDescendants);
        // Remet les entités d'un instantané dans leur état : recréées (même identifiant, même place) si elles
        // n'existent plus, sinon nom, parent, secteur et composants remplacés. Le reste de la scène n'est pas touché.
        // En cas d'erreur (parent disparu...), scene n'est pas modifiée.
        FLUX_API std::expected<void, std::string> RestoreEntity(Scene& scene, const ComponentRegistry& components,
                                                                std::string_view snapshot);

        // Identifiants des entités d'un prefab écrit par SavePrefab.
        enum class PrefabIds : std::uint8_t {
            ForInstance, // tirés de ceux de la scène : `root` deviendra l'exemplaire (LinkPrefab) sans les changer
            Keep,        // ceux de la scène : le prefab lui-même, ouvert pour le modifier (les exemplaires s'y retrouvent)
        };
        // Prefab fait de `root` et de ses descendants (format flux.prefab), racine ramenée à l'origine. Erreur si l'un
        // d'eux est déjà un exemplaire de prefab (un prefab dans un prefab n'est pas encore géré).
        FLUX_API std::expected<std::string, std::string> SavePrefab(const Scene& scene, const ComponentRegistry& components,
                                                                    Entity root, PrefabIds ids);
        // `root` (que SavePrefab vient d'écrire dans `path`) devient un exemplaire de ce prefab, sans rien changer
        // d'autre : ses pièces gardent leurs identifiants et leurs réglages.
        FLUX_API void LinkPrefab(Scene& scene, Entity root, std::string_view path);
        // Nouvel exemplaire du prefab `path`, racine sous `parent` (aucun : à la racine de la scène), placé comme le
        // prefab. Rend sa racine.
        FLUX_API std::expected<Entity, std::string> Instantiate(Scene& scene, const ComponentRegistry& components,
                                                                std::string_view path, const PrefabReader& prefabs,
                                                                Entity parent = NullEntity);
        // Ce qu'une pièce d'exemplaire (PrefabLinkComponent) change de son prefab, pour l'inspecteur.
        struct PrefabOverrides
        {
            bool known = false; // pièce d'un exemplaire dont le prefab a pu être lu
            bool name = false;
            std::vector<std::pair<std::string, std::string>> fields; // (composant, champ) changés
            std::vector<std::string> added;   // composants absents du prefab (clés)
            std::vector<std::string> removed; // composants du prefab retirés de la pièce (clés)
        };
        FLUX_API PrefabOverrides GetPrefabOverrides(const Scene& scene, const ComponentRegistry& components, Entity entity,
                                                    const PrefabReader& prefabs);
        // Remet le composant `key` d'une pièce comme dans son prefab : ses champs, ou retiré s'il n'y est pas, ou
        // remis s'il en avait été retiré. false si la pièce ou son prefab est introuvable.
        FLUX_API bool RevertToPrefab(Scene& scene, const ComponentRegistry& components, Entity entity, std::string_view key,
                                     const PrefabReader& prefabs);
        // L'exemplaire dont `root` est la racine redevient des entités ordinaires, réglages gardés (plus suivi par le
        // prefab).
        FLUX_API void UnlinkPrefab(Scene& scene, Entity root);

        // Écrit d'abord un fichier temporaire : une coupure pendant la sauvegarde n'abîme pas l'ancien fichier.
        FLUX_API std::expected<void, std::string> Save(const Scene& scene, const ComponentRegistry& components,
                                                       const std::filesystem::path& path, const PrefabReader& prefabs = {});
        FLUX_API std::expected<void, std::string> Load(Scene& scene, const ComponentRegistry& components,
                                                       const std::filesystem::path& path, const PrefabReader& prefabs = {});
        // Écrit un texte déjà prêt (SavePrefab), de la même façon que Save.
        FLUX_API std::expected<void, std::string> WriteFile(const std::filesystem::path& path, std::string_view text);
    }
}
