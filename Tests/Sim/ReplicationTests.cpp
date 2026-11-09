// Réplication : champs marqués, instantanés écrits en entier puis en différences, relus à l'identique ; entités
// créées, détruites, changées de secteur ; messages abîmés refusés sans lire hors du tampon ; affichage interpolé
// entre deux instantanés, en retard sur le dernier reçu.
#include "Check.hpp"

#include "Flux/Scene/ComponentRegistry.hpp"
#include "Flux/Scene/Replication.hpp"
#include "Flux/Scene/Scene.hpp"
#include "Flux/Scene/SceneSerializer.hpp"

#include <cmath>
#include <cstring>
#include <numbers>
#include <vector>

using namespace Flux;

namespace {
    // Composant d'essai : un champ répliqué de chaque genre, et un qui ne l'est pas.
    struct Gauge
    {
        float level = 0.0f;
        std::int32_t count = 0;
        bool on = false;
        float local = 0.0f; // pas répliqué
    };

    void RegisterGauge(ComponentRegistry& components) {
        components.Register<Gauge>("gauge", "Jauge")
            .Field("level", "Niveau", &Gauge::level)
            .Replicated()
            .Field("count", "Nombre", &Gauge::count)
            .Replicated()
            .Field("on", "Allumée", &Gauge::on)
            .Replicated()
            .Field("local", "Locale", &Gauge::local);
    }

    bool Same(const Replication::WorldState& a, const Replication::WorldState& b) {
        if (a.entities.size() != b.entities.size() || a.values != b.values) {
            return false;
        }
        for (std::size_t i = 0; i < a.entities.size(); ++i) {
            if (a.entities[i].id != b.entities[i].id || a.entities[i].sector != b.entities[i].sector
                || a.entities[i].components != b.entities[i].components) {
                return false;
            }
        }
        return true;
    }

    // Écrit current (depuis baseline), le relit et le compare. size : taille du message écrit.
    bool RoundTrip(const Replication::Schema& schema, const Replication::WorldState& current,
                   const Replication::WorldState* baseline, std::size_t& size) {
        ByteWriter writer;
        Replication::Encode(schema, current, baseline, writer);
        size = writer.GetSize();
        ByteReader reader(writer.GetBytes());
        const Replication::SnapshotHeader header = Replication::ReadHeader(reader);
        CHECK(header.sequence == current.sequence && header.step == current.step);
        CHECK(header.baseline == (baseline ? baseline->sequence : 0u));
        Replication::WorldState decoded;
        CHECK(Replication::Decode(schema, header, reader, baseline, decoded));
        CHECK(reader.IsAtEnd());
        CHECK(Same(decoded, current));
        return true;
    }

    bool SchemaFromRegistry() {
        ComponentRegistry components;
        RegisterEngineComponents(components);
        Replication::Schema engine;
        engine.Build(components);
        // Le moteur ne marque que la place des entités.
        CHECK(engine.GetComponents().size() == 1 && engine.GetComponents()[0].info->key == "transform");
        CHECK(engine.GetFields().size() == 2);
        CHECK(engine.GetStateSize() == sizeof(Math::Vector3d) + sizeof(Math::Quaternionf));

        RegisterGauge(components);
        Replication::Schema withGame;
        withGame.Build(components);
        CHECK(withGame.GetComponents().size() == 2);
        CHECK(withGame.GetFields(withGame.GetComponents()[1]).size() == 3); // « local » n'y est pas
        // Un champ marqué d'un côté seulement change l'empreinte : client et serveur se refusent.
        CHECK(withGame.GetHash() != engine.GetHash());
        Replication::Schema again;
        again.Build(components);
        CHECK(again.GetHash() == withGame.GetHash());
        return true;
    }

    bool SnapshotsAndDifferences() {
        ComponentRegistry components;
        RegisterEngineComponents(components);
        RegisterGauge(components);
        Replication::Schema schema;
        schema.Build(components);

        Scene scene;
        const Entity ship = scene.CreateEntity("Vaisseau");
        const Entity seat = scene.CreateEntity("Siège", ship);
        const Entity rock = scene.CreateEntity("Rocher");
        for (int i = 0; i < 20; ++i) {
            scene.Get<TransformComponent>(scene.CreateEntity("Caillou")).position = {double(i), 0.0, 5.0}; // immobiles
        }
        scene.GetRegistry().emplace<Gauge>(ship, Gauge{.level = 0.5f, .count = 3, .on = true});
        scene.Get<TransformComponent>(ship).position = {1.0, 2.0, 3.0};
        scene.Get<TransformComponent>(seat).position = {0.0, 1.0, -2.0};

        Replication::Capturer capturer;
        Replication::SnapshotHistory history;
        Replication::WorldState& first = history.Next(10);
        capturer.Capture(scene, schema, first);
        CHECK(first.sequence == 1 && first.entities.size() == 23);
        std::size_t fullSize = 0;
        CHECK(RoundTrip(schema, first, nullptr, fullSize));

        // Rien n'a bougé : seulement l'en-tête et deux compteurs.
        Replication::WorldState& idle = history.Next(12);
        capturer.Capture(scene, schema, idle);
        std::size_t idleSize = 0;
        CHECK(RoundTrip(schema, idle, history.Find(1), idleSize));
        CHECK(idleSize < 24);

        // Le vaisseau avance, la jauge baisse (un champ sur trois), le rocher passe dans un autre secteur, le siège
        // disparaît, une entité naît.
        scene.Get<TransformComponent>(ship).position = {1.0, 2.0, 13.0};
        scene.GetRegistry().get<Gauge>(ship).level = 0.25f;
        scene.GetRegistry().get<Gauge>(ship).local = 9.0f; // pas répliqué : ne compte pas
        scene.GetRegistry().emplace<SectorComponent>(rock, Math::Vector3i64{1, 0, -2});
        scene.DestroyEntity(seat);
        const Entity drone = scene.CreateEntity("Drone");
        scene.GetRegistry().emplace<Gauge>(drone);
        scene.GetRegistry().emplace<SectorComponent>(drone, Math::Vector3i64{0, 3, 0}); // nouvelle, hors du secteur 0
        Replication::WorldState& moved = history.Next(14);
        capturer.Capture(scene, schema, moved);
        std::size_t movedSize = 0;
        CHECK(RoundTrip(schema, moved, history.Find(2), movedSize));
        CHECK(movedSize < fullSize);
        // Le même, depuis une base plus ancienne, et en entier.
        CHECK(RoundTrip(schema, moved, history.Find(1), movedSize));
        CHECK(RoundTrip(schema, moved, nullptr, movedSize));

        // Jauge retirée : le composant disparaît du masque.
        scene.GetRegistry().remove<Gauge>(ship);
        Replication::WorldState& removed = history.Next(16);
        capturer.Capture(scene, schema, removed);
        CHECK(RoundTrip(schema, removed, history.Find(3), movedSize));

        // Une base qui n'est pas celle annoncée est refusée.
        ByteWriter writer;
        Replication::Encode(schema, removed, history.Find(3), writer);
        ByteReader reader(writer.GetBytes());
        const Replication::SnapshotHeader header = Replication::ReadHeader(reader);
        Replication::WorldState out;
        CHECK(!Replication::Decode(schema, header, reader, history.Find(2), out));
        CHECK(!Replication::Decode(schema, header, reader, nullptr, out));
        return true;
    }

    bool DamagedMessages() {
        ComponentRegistry components;
        RegisterEngineComponents(components);
        Replication::Schema schema;
        schema.Build(components);
        Scene scene;
        for (int i = 0; i < 20; ++i) {
            scene.Get<TransformComponent>(scene.CreateEntity("E")).position = {double(i), 0.0, 0.0};
        }
        Replication::Capturer capturer;
        Replication::WorldState state;
        capturer.Capture(scene, schema, state);
        state.sequence = 1;
        ByteWriter writer;
        Replication::Encode(schema, state, nullptr, writer);
        const std::span<const std::byte> bytes = writer.GetBytes();
        // Tronqué n'importe où : erreur, jamais de lecture hors du tampon.
        for (std::size_t size = 0; size < bytes.size(); ++size) {
            ByteReader reader(bytes.first(size));
            const Replication::SnapshotHeader header = Replication::ReadHeader(reader);
            Replication::WorldState out;
            CHECK(!reader.IsValid() || !Replication::Decode(schema, header, reader, nullptr, out));
        }
        // Octets au hasard après l'en-tête : refusés ou lus, sans planter.
        std::vector<std::byte> noise(bytes.begin(), bytes.end());
        std::uint32_t seed = 12345;
        for (int round = 0; round < 200; ++round) {
            std::vector<std::byte> copy = noise;
            for (int k = 0; k < 4; ++k) {
                seed = seed * 1664525u + 1013904223u;
                copy[16 + seed % (copy.size() - 16)] = std::byte(seed >> 24);
            }
            ByteReader reader(copy);
            const Replication::SnapshotHeader header = Replication::ReadHeader(reader);
            Replication::WorldState out;
            (void)Replication::Decode(schema, header, reader, nullptr, out);
        }
        return true;
    }

    bool InterpolatedDisplay() {
        ComponentRegistry components;
        RegisterEngineComponents(components);
        Replication::Schema schema;
        schema.Build(components);

        // Serveur : une entité qui avance de 1 m par pas, tournant autour de y ; une autre détruite en route.
        Scene server;
        const Entity ship = server.CreateEntity("Vaisseau");
        const Entity wreck = server.CreateEntity("Épave");
        // Client : la même scène (même fichier, mêmes identifiants), plus une entité que le serveur n'a plus.
        Scene client;
        const Entity clientShip = client.CreateEntity("Vaisseau", NullEntity, server.GetId(ship));
        client.CreateEntity("Épave", NullEntity, server.GetId(wreck));
        client.CreateEntity("Oubliée");

        Replication::Capturer capturer;
        Replication::SnapshotHistory history;
        Replication::SnapshotReceiver receiver;
        std::uint32_t acknowledged = 0;
        ByteWriter writer;
        const auto send = [&](std::uint64_t step, bool lost) {
            server.Get<TransformComponent>(ship).position = {double(step), 0.0, 0.0};
            server.Get<TransformComponent>(ship).rotation =
                Math::Quaternionf::FromAxisAngle({0, 1, 0}, 0.01f * static_cast<float>(step));
            Replication::WorldState& state = history.Next(step);
            capturer.Capture(server, schema, state);
            writer.Clear();
            Replication::Encode(schema, state, history.Find(acknowledged), writer);
            if (lost) {
                return true;
            }
            ByteReader reader(writer.GetBytes());
            auto received = receiver.Receive(schema, reader);
            CHECK(received && *received == state.sequence);
            acknowledged = *received;
            return true;
        };

        const Replication::WorldState* from = nullptr;
        const Replication::WorldState* to = nullptr;
        float t = 0.0f;
        CHECK(!receiver.Sample(from, to, t)); // rien reçu
        // 30 instantanés par seconde (un pas sur deux), un sur cinq perdu ; l'épave disparaît au pas 40.
        for (std::uint64_t step = 2; step <= 200; step += 2) {
            if (step == 40) {
                server.DestroyEntity(wreck);
            }
            CHECK(send(step, step % 10 == 0 && step > 2));
            for (int i = 0; i < 2; ++i) {
                receiver.Advance();
            }
        }
        // L'affichage suit le dernier instantané (200) avec DelaySteps de retard, à moins d'un pas près.
        CHECK(std::abs(receiver.GetDisplayStep() - (200.0 - Replication::SnapshotReceiver::DelaySteps)) < 1.0);
        CHECK(receiver.Sample(from, to, t));
        CHECK(from->step <= receiver.GetDisplayStep() && receiver.GetDisplayStep() <= to->step);

        // Créée par le serveur après to (EntitySpawn arrivé avant l'instantané qui la contient) : gardée, ses enfants
        // avec elle.
        const Entity spawned = client.CreateEntity("Arrivée");
        const Entity spawnedChild = client.CreateEntity("Arrivée (enfant)", spawned);
        const Uuid keep[] = {client.GetId(spawned)};
        Replication::DestroyMissing(client, nullptr, *to, keep);
        CHECK(client.IsValid(spawned) && client.IsValid(spawnedChild));
        CHECK(client.FindByName("Oubliée") == NullEntity);
        CHECK(client.FindByName("Épave") == NullEntity);
        CHECK(client.IsValid(clientShip));
        Replication::Apply(client, schema, *from, *to, t);
        // Position et rotation interpolées : celles du pas affiché (le serveur avance de 1 m par pas).
        const TransformComponent& drawn = client.Get<TransformComponent>(clientShip);
        CHECK(std::abs(drawn.position.x - receiver.GetDisplayStep()) < 1e-3);
        const Math::Quaternionf expected =
            Math::Quaternionf::FromAxisAngle({0, 1, 0}, 0.01f * static_cast<float>(receiver.GetDisplayStep()));
        CHECK(std::abs(Math::Dot(drawn.rotation, expected)) > 0.99999f);

        // Un instantané en retard (déjà dépassé) est ignoré.
        ByteWriter late;
        Replication::Encode(schema, *history.Find(acknowledged - 1), nullptr, late);
        ByteReader lateReader(late.GetBytes());
        auto ignored = receiver.Receive(schema, lateReader);
        CHECK(ignored && *ignored == 0u);
        return true;
    }
}

namespace {
    // Entité créée pendant le jeu sur le serveur : envoyée en entier (SaveEntity), recréée chez le client avec les
    // mêmes identifiants (RestoreEntity), puis suivie par les instantanés.
    bool SpawnedEntity() {
        ComponentRegistry components;
        RegisterEngineComponents(components);
        Replication::Schema schema;
        schema.Build(components);
        Scene server;
        Scene client;
        const Entity drone = server.CreateEntity("Drone");
        const Entity gun = server.CreateEntity("Canon", drone);
        server.Get<TransformComponent>(gun).position = {0.0, -1.0, 2.0};
        const std::string text = SceneSerializer::SaveEntity(server, components, drone, true);
        CHECK(SceneSerializer::RestoreEntity(client, components, text));
        const Entity clientDrone = client.FindById(server.GetId(drone));
        const Entity clientGun = client.FindById(server.GetId(gun));
        CHECK(clientDrone != NullEntity && clientGun != NullEntity && client.GetParent(clientGun) == clientDrone);

        server.Get<TransformComponent>(drone).position = {100.0, 0.0, 0.0};
        Replication::Capturer capturer;
        Replication::WorldState state;
        capturer.Capture(server, schema, state);
        Replication::Apply(client, schema, state, state, 1.0f);
        CHECK(client.Get<TransformComponent>(clientDrone).position.x == 100.0);
        CHECK(client.Get<TransformComponent>(clientGun).position.z == 2.0);
        return true;
    }
}

bool RunReplicationTests() {
    return SchemaFromRegistry() && SnapshotsAndDifferences() && DamagedMessages() && InterpolatedDisplay()
        && SpawnedEntity();
}
