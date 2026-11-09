#pragma once

#include "Flux/Core/ByteStream.hpp"
#include "Flux/Core/Uuid.hpp"
#include "Flux/Math/Vector3.hpp"
#include "Flux/Scene/ComponentRegistry.hpp"
#include "Flux/Scene/Scene.hpp"
#include "Flux/SimExport.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <utility>
#include <vector>

// Réplication : le serveur, qui fait autorité, envoie aux clients l'état du monde, champ par champ comme le marque
// ComponentBuilder::Replicated. Rien ici ne dépend du réseau : le jeu met les octets dans ses messages (FluxNet).
//
// Le modèle est celui de Quake 3 : le serveur prend un instantané à chaque envoi et l'écrit en ne gardant que ce qui a
// changé depuis le dernier instantané que le client dit avoir reçu (ou en entier s'il n'en a reçu aucun). Le client le
// reconstruit, le range et affiche le monde un peu dans le passé, entre deux instantanés reçus : une perte de paquet
// ne se voit pas, et rien n'est jamais renvoyé (le suivant porte l'état à jour).
namespace Flux::Replication {
    // Champs répliqués des composants enregistrés, à plat : chaque entité a la même suite d'octets dans un instantané.
    class FLUX_SIM_API Schema {
        public:
        struct Field
        {
            const FieldInfo* info = nullptr;
            std::uint32_t offset = 0; // dans les valeurs d'une entité
            std::uint32_t size = 0;
            std::uint32_t index = 0; // parmi les champs répliqués de son composant (bit du masque)
        };
        struct Component
        {
            const ComponentInfo* info = nullptr;
            std::uint32_t firstField = 0;
            std::uint32_t fieldCount = 0;
        };
        // Bits des masques (composants présents, champs changés) : 64 au plus.
        static constexpr std::size_t MaxComponents = 64;
        static constexpr std::size_t MaxFieldsPerComponent = 64;

        // Relit le registre : après l'enregistrement des composants du jeu, et à chaque rechargement du module.
        void Build(const ComponentRegistry& components);

        std::span<const Component> GetComponents() const { return m_components; }
        std::span<const Field> GetFields() const { return m_fields; }
        std::span<const Field> GetFields(const Component& component) const {
            return std::span(m_fields).subspan(component.firstField, component.fieldCount);
        }
        // Octets des valeurs d'une entité (tous les composants répliqués, présents ou non).
        std::size_t GetStateSize() const { return m_stateSize; }
        // Empreinte des clés, types et tailles : le client et le serveur doivent avoir la même, sinon ils ne se
        // comprennent pas (champ marqué d'un côté seulement, protocole du jeu oublié).
        std::uint64_t GetHash() const { return m_hash; }

        private:
        std::vector<Component> m_components;
        std::vector<Field> m_fields;
        std::size_t m_stateSize = 0;
        std::uint64_t m_hash = 0;
    };

    struct EntityState
    {
        Uuid id;
        Math::Vector3i64 sector; // racine seulement (SectorComponent), 0 sinon
        std::uint64_t components = 0; // bit i : Schema::GetComponents()[i] présent sur l'entité
    };

    // L'état répliqué du monde à un pas de simulation. Gardé d'un instantané à l'autre (anneaux du serveur et du
    // client) : les vecteurs gardent leur mémoire, aucune allocation une fois la scène vue.
    struct FLUX_SIM_API WorldState
    {
        std::uint32_t sequence = 0; // numéro de l'instantané (1, 2...), 0 : vide
        std::uint64_t step = 0;     // pas de simulation du serveur (SimulationClock::GetStep)
        std::vector<EntityState> entities; // triées par identifiant
        std::vector<std::byte> values;     // entities.size() * Schema::GetStateSize(), dans le même ordre

        std::span<const std::byte> GetValues(const Schema& schema, std::size_t entity) const {
            return std::span(values).subspan(entity * schema.GetStateSize(), schema.GetStateSize());
        }
        // Index de l'entité, ou -1.
        std::ptrdiff_t Find(Uuid id) const;
        void Clear();
    };

    // Prend l'état des entités de la scène qui ont un identifiant.
    class FLUX_SIM_API Capturer {
        public:
        void Capture(const Scene& scene, const Schema& schema, WorldState& out);

        private:
        std::vector<std::pair<Uuid, Entity>> m_sorted;
    };

    // En-tête d'un instantané écrit par Encode : le client y lit le numéro de la base avant de décoder le reste.
    struct SnapshotHeader
    {
        std::uint32_t sequence = 0;
        std::uint32_t baseline = 0; // instantané d'où partent les différences, 0 : écrit en entier
        std::uint64_t step = 0;
    };

    // Écrit current, en différences depuis baseline (nullptr : en entier). Ajoute au tampon sans le vider.
    FLUX_SIM_API void Encode(const Schema& schema, const WorldState& current, const WorldState* baseline, ByteWriter& out);
    FLUX_SIM_API SnapshotHeader ReadHeader(ByteReader& reader);
    // Après ReadHeader : reconstruit l'instantané depuis baseline (celui de header.baseline, nullptr s'il vaut 0).
    // Erreur pour un message abîmé ou une base qui ne correspond pas ; out n'est alors pas utilisable.
    FLUX_SIM_API std::expected<void, std::string> Decode(const Schema& schema, const SnapshotHeader& header,
                                                         ByteReader& reader, const WorldState* baseline, WorldState& out);

    // Écrit dans la scène l'état à t entre from et to (0 : from, 1 : to) : positions, rotations et nombres
    // interpolés, le reste pris dans to. Une entité inconnue de la scène est passée (créée plus tard par le jeu), une
    // entité qui change de secteur saute à sa place de to. Les composants répliqués absents de to sont retirés.
    FLUX_SIM_API void Apply(Scene& scene, const Schema& schema, const WorldState& from, const WorldState& to, float t);
    // Détruit les entités de la scène que state n'a pas (détruites par le serveur). previous : dernier état appliqué,
    // pour ne regarder que les entités qui ont disparu depuis ; nullptr : toute la scène (premier état, scène
    // rechargée). keep : entités (et leurs descendants) que state ne peut pas encore avoir, créées par le serveur après
    // lui (EntitySpawn arrivé avant l'instantané qui les contient).
    FLUX_SIM_API void DestroyMissing(Scene& scene, const WorldState* previous, const WorldState& state,
                                     std::span<const Uuid> keep = {});

    // Les derniers instantanés du serveur, en anneau : base des différences de chaque client.
    class FLUX_SIM_API SnapshotHistory {
        public:
        static constexpr std::size_t Capacity = 32; // un peu plus d'une seconde à 30 envois par seconde

        // Place du prochain instantané (numéro et pas déjà remplis) ; il remplace le plus ancien.
        WorldState& Next(std::uint64_t step);
        const WorldState* Find(std::uint32_t sequence) const;
        const WorldState* GetLatest() const { return m_latest != 0 ? Find(m_latest) : nullptr; }
        void Clear();

        private:
        std::array<WorldState, Capacity> m_states;
        std::uint32_t m_latest = 0;
    };

    // Côté client : instantanés reçus et instant affiché, un peu en retard sur le dernier reçu.
    class FLUX_SIM_API SnapshotReceiver {
        public:
        // Retard de l'affichage sur le dernier instantané, en pas (6 à 60 Hz : 100 ms). À 30 envois par seconde, un
        // instantané perdu sur deux de suite ne se voit pas.
        static constexpr double DelaySteps = 6.0;

        // Décode un instantané (message sans son type). Rend son numéro, à renvoyer au serveur comme accusé de
        // réception ; 0 s'il est ignoré (plus ancien que le dernier, base inconnue) ; erreur s'il est abîmé.
        std::expected<std::uint32_t, std::string> Receive(const Schema& schema, ByteReader& reader);
        // Un pas de simulation du client : l'instant affiché avance d'un pas, rattrapé doucement vers le dernier
        // instantané moins DelaySteps (d'un coup s'il en est loin : premier instantané, serveur en retard).
        void Advance();
        // Instantanés encadrant l'instant affiché, et la part de l'un à l'autre. false avant le premier.
        bool Sample(const WorldState*& from, const WorldState*& to, float& t) const;
        const WorldState* GetLatest() const;
        const WorldState* Find(std::uint32_t sequence) const;
        double GetDisplayStep() const { return m_displayStep; }
        void Clear();

        private:
        std::array<WorldState, SnapshotHistory::Capacity> m_states; // à la place sequence % Capacity
        std::uint32_t m_latest = 0;
        double m_displayStep = 0.0;
        bool m_started = false;
    };
}
