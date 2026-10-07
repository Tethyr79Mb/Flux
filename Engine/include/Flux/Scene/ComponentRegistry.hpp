#pragma once

#include "Flux/Export.hpp"
#include "Flux/Math/Color.hpp"
#include "Flux/Math/Quaternion.hpp"
#include "Flux/Math/Vector3.hpp"
#include "Flux/Scene/Components.hpp"

#include <entt/entity/registry.hpp>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace Flux {
    // Types de champs que le moteur sait sauvegarder et afficher dans l'inspecteur.
    enum class FieldType : std::uint8_t {
        Bool,
        Int,        // std::int32_t
        Float,
        Double,
        Vector3f,
        Vector3d,
        Quaternion, // affiché en angles (degrés), sauvegardé en quaternion
        Color,      // LinearColor, affichée en sRGB 0-255
        String,
        Enum,       // sauvegardé par nom, voir ComponentBuilder::Enum
    };

    // Fichier du projet désigné par un champ texte : l'inspecteur propose alors de le choisir dans l'explorateur.
    enum class AssetType : std::uint8_t {
        None,
        Image, // PNG, JPEG, TGA, BMP
        Scene, // .scene.json
        Model, // .glb, .gltf
        Interface, // .fluxui
        Sound, // .wav, .ogg, .mp3, .flac
        // Pas un fichier : le nom d'une animation du modèle 3D de l'entité (ModelComponent), choisi dans une liste.
        Animation,
    };

    // Réglages d'édition d'un champ numérique (inspecteur).
    struct FieldOptions
    {
        double speed = 0.01; // variation par pixel glissé
        double min = std::numeric_limits<double>::lowest();
        double max = std::numeric_limits<double>::max();
        int decimals = 3;
        bool alpha = true; // couleur : opacité réglable (4e valeur) ; false pour une lumière
    };

    struct EnumOption
    {
        std::string key;   // dans le fichier
        std::string label; // dans l'inspecteur
        std::int64_t value;
    };

    struct FieldInfo
    {
        std::string key;   // dans le fichier (camelCase, anglais)
        std::string label; // dans l'inspecteur
        FieldType type = FieldType::Float;
        FieldOptions options;
        AssetType asset = AssetType::None; // String seulement
        // Adresse du champ dans une instance du composant.
        std::function<void*(void*)> access;
        // Enum seulement : valeurs possibles, et lecture / écriture de la valeur en entier.
        std::vector<EnumOption> enumOptions;
        std::function<std::int64_t(const void*)> getEnum;
        std::function<void(void*, std::int64_t)> setEnum;
    };

    // Description d'un type de composant : de quoi le sauvegarder, le charger et l'éditer sans connaître son type C++.
    struct ComponentInfo
    {
        std::string key;   // dans le fichier
        std::string label; // dans l'inspecteur
        bool removable = true;
        // Composants avec lesquels il ne peut pas cohabiter sur une entité (clés) : l'inspecteur refuse de les
        // ajouter ensemble. Il suffit de le déclarer d'un côté.
        std::vector<std::string> excludes;
        std::vector<FieldInfo> fields;
        // Champs retirés ou renommés (clés) : ignorés à la lecture, pour que les scènes déjà enregistrées s'ouvrent
        // encore ; un autre champ inconnu reste une erreur (faute de frappe dans un fichier écrit à la main).
        std::vector<std::string> removedFields;

        // L'un des deux déclare l'autre incompatible.
        bool Excludes(const ComponentInfo& other) const {
            return std::ranges::contains(excludes, other.key) || std::ranges::contains(other.excludes, key);
        }

        // Composant de l'entité, ou nullptr. Registre const : la lecture ne crée pas de stockage EnTT pour le type
        // (la sauvegarde lit une scène const) ; l'appelant qui modifie le composant doit tenir la scène en non-const.
        std::function<void*(const entt::registry&, Entity)> get;
        std::function<void(entt::registry&, Entity)> add; // valeurs par défaut
        std::function<void(entt::registry&, Entity)> remove;
        // Instance hors scène (chargement : tout est lu et vérifié avant de toucher à la scène), puis transfert.
        std::function<std::shared_ptr<void>()> create;
        std::function<void(entt::registry&, Entity, void*)> emplace;

        const FieldInfo* FindField(std::string_view fieldKey) const {
            for (const FieldInfo& field : fields) {
                if (field.key == fieldKey) {
                    return &field;
                }
            }
            return nullptr;
        }
    };

    namespace Detail {
        template <typename T> struct FieldTypeOf;
        template <> struct FieldTypeOf<bool> { static constexpr FieldType Value = FieldType::Bool; };
        template <> struct FieldTypeOf<std::int32_t> { static constexpr FieldType Value = FieldType::Int; };
        template <> struct FieldTypeOf<float> { static constexpr FieldType Value = FieldType::Float; };
        template <> struct FieldTypeOf<double> { static constexpr FieldType Value = FieldType::Double; };
        template <> struct FieldTypeOf<Math::Vector3f> { static constexpr FieldType Value = FieldType::Vector3f; };
        template <> struct FieldTypeOf<Math::Vector3d> { static constexpr FieldType Value = FieldType::Vector3d; };
        template <> struct FieldTypeOf<Math::Quaternionf> { static constexpr FieldType Value = FieldType::Quaternion; };
        template <> struct FieldTypeOf<Math::LinearColor> { static constexpr FieldType Value = FieldType::Color; };
        template <> struct FieldTypeOf<std::string> { static constexpr FieldType Value = FieldType::String; };
    }

    // Valeur d'une énumération C++ avec ses noms (fichier, inspecteur).
    template <typename E>
    struct EnumValue
    {
        const char* key;
        const char* label;
        E value;
    };

    // Décrit les champs d'un composant, à la suite de ComponentRegistry::Register :
    //     registry.Register<SpinComponent>("spin", "Rotation continue")
    //         .Field("axis", "Axe", &SpinComponent::axis)
    //         .Field("speed", "Vitesse (rad/s)", &SpinComponent::speed, {.speed = 0.01, .decimals = 2});
    // Un champ non décrit n'est ni sauvegardé ni affiché.
    template <typename C>
    class ComponentBuilder {
        public:
        explicit ComponentBuilder(ComponentInfo& info) : m_info(&info) {}

        template <typename T>
        ComponentBuilder& Field(std::string key, std::string label, T C::* member, const FieldOptions& options = {}) {
            static_assert(requires { Detail::FieldTypeOf<T>::Value; },
                          "type de champ non géré (bool, int32, float, double, Vector3f/d, Quaternionf, LinearColor, string)");
            FieldInfo field;
            field.key = std::move(key);
            field.label = std::move(label);
            field.type = Detail::FieldTypeOf<T>::Value;
            field.options = options;
            field.access = [member](void* instance) -> void* { return &(static_cast<C*>(instance)->*member); };
            m_info->fields.push_back(std::move(field));
            return *this;
        }

        // Chemin d'un fichier du projet, relatif au dossier des assets (texte avec des « / ») : choisi dans l'explorateur
        // depuis l'inspecteur.
        ComponentBuilder& Asset(std::string key, std::string label, std::string C::* member, AssetType asset) {
            Field(std::move(key), std::move(label), member);
            m_info->fields.back().asset = asset;
            return *this;
        }

        template <typename E>
            requires std::is_enum_v<E>
        ComponentBuilder& Enum(std::string key, std::string label, E C::* member, std::initializer_list<EnumValue<E>> values) {
            FieldInfo field;
            field.key = std::move(key);
            field.label = std::move(label);
            field.type = FieldType::Enum;
            field.access = [member](void* instance) -> void* { return &(static_cast<C*>(instance)->*member); };
            for (const EnumValue<E>& value : values) {
                field.enumOptions.push_back({value.key, value.label, static_cast<std::int64_t>(value.value)});
            }
            field.getEnum = [](const void* value) { return static_cast<std::int64_t>(*static_cast<const E*>(value)); };
            field.setEnum = [](void* value, std::int64_t number) { *static_cast<E*>(value) = static_cast<E>(number); };
            m_info->fields.push_back(std::move(field));
            return *this;
        }

        // Composant présent sur toutes les entités (transformation) : ni ajouté ni retiré depuis l'inspecteur.
        ComponentBuilder& NotRemovable() {
            m_info->removable = false;
            return *this;
        }

        ComponentBuilder& Excludes(std::string key) {
            m_info->excludes.push_back(std::move(key));
            return *this;
        }

        ComponentBuilder& RemovedField(std::string key) {
            m_info->removedFields.push_back(std::move(key));
            return *this;
        }

        private:
        ComponentInfo* m_info;
    };

    // Composants connus du moteur et du jeu, dans l'ordre d'enregistrement (ordre du fichier et de l'inspecteur).
    // Les composants d'un module de jeu sont retirés automatiquement quand le module est déchargé (GameModule).
    class FLUX_API ComponentRegistry {
        public:
        ComponentRegistry() = default;
        ComponentRegistry(const ComponentRegistry&) = delete;
        ComponentRegistry& operator=(const ComponentRegistry&) = delete;

        template <typename C>
        ComponentBuilder<C> Register(std::string key, std::string label) {
            static_assert(std::is_default_constructible_v<C> && std::is_move_constructible_v<C>);
            Unregister(key); // réenregistrement (rechargement du jeu) : la nouvelle description remplace l'ancienne
            auto info = std::make_unique<ComponentInfo>();
            info->key = std::move(key);
            info->label = std::move(label);
            info->get = [](const entt::registry& registry, Entity entity) -> void* {
                return const_cast<C*>(registry.try_get<C>(entity));
            };
            info->add = [](entt::registry& registry, Entity entity) { registry.emplace_or_replace<C>(entity); };
            info->remove = [](entt::registry& registry, Entity entity) { registry.remove<C>(entity); };
            info->create = [] { return std::shared_ptr<void>(std::make_shared<C>()); };
            info->emplace = [](entt::registry& registry, Entity entity, void* instance) {
                registry.emplace_or_replace<C>(entity, std::move(*static_cast<C*>(instance)));
            };
            m_components.push_back(std::move(info));
            return ComponentBuilder<C>(*m_components.back());
        }

        void Unregister(std::string_view key);
        const ComponentInfo* Find(std::string_view key) const;

        std::size_t GetCount() const { return m_components.size(); }
        const ComponentInfo& Get(std::size_t index) const { return *m_components[index]; }
        // Retire tout ce qui a été enregistré après les count premiers (déchargement d'un module de jeu).
        void Truncate(std::size_t count);

        private:
        std::vector<std::unique_ptr<ComponentInfo>> m_components;
    };

    // Transformation et rendu : les composants du moteur, enregistrés par Engine au démarrage.
    FLUX_API void RegisterEngineComponents(ComponentRegistry& registry);
}
