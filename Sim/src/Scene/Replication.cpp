#include "Flux/Scene/Replication.hpp"

#include "Flux/Math/Color.hpp"
#include "Flux/Math/Quaternion.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>
#include <optional>

namespace Flux::Replication {
    namespace {
        // Format des instantanés : à changer (et GameProtocolVersion avec) si Encode change.
        constexpr std::uint64_t FormatVersion = 1;

        enum EntityFlags : std::uint8_t {
            SectorChanged = 1 << 0,
            ComponentsChanged = 1 << 1,
            NewEntity = 1 << 2, // absente de la base : toutes ses valeurs suivent
            ParentChanged = 1 << 3,
        };

        std::uint32_t SizeOf(FieldType type) {
            switch (type) {
                case FieldType::Bool: return sizeof(bool);
                case FieldType::Int: return sizeof(std::int32_t);
                case FieldType::Float: return sizeof(float);
                case FieldType::Double: return sizeof(double);
                case FieldType::Vector3f: return sizeof(Math::Vector3f);
                case FieldType::Vector3d: return sizeof(Math::Vector3d);
                case FieldType::Quaternion: return sizeof(Math::Quaternionf);
                case FieldType::Color: return sizeof(Math::LinearColor);
                case FieldType::String:
                case FieldType::Enum: break;
            }
            return 0;
        }

        // FNV-1a 64 bits.
        void HashBytes(std::uint64_t& hash, const void* data, std::size_t size) {
            const auto* bytes = static_cast<const unsigned char*>(data);
            for (std::size_t i = 0; i < size; ++i) {
                hash = (hash ^ bytes[i]) * 0x100000001B3ull;
            }
        }

        void HashText(std::uint64_t& hash, std::string_view text) {
            HashBytes(hash, text.data(), text.size());
            HashBytes(hash, "", 1); // séparateur : « ab »+« c » diffère de « a »+« bc »
        }

        template <typename T>
        T ReadValue(const std::byte* bytes) {
            T value;
            std::memcpy(&value, bytes, sizeof(T));
            return value;
        }

        template <typename T>
        void WriteValue(void* destination, const T& value) {
            std::memcpy(destination, &value, sizeof(T));
        }

        template <typename T>
        T Mix(T a, T b, float t) {
            return a + (b - a) * static_cast<T>(t);
        }

        // Valeur du champ à t entre from et to ; ce qui ne s'interpole pas (booléen, entier) prend celle de to.
        void Interpolate(FieldType type, const std::byte* from, const std::byte* to, float t, void* destination) {
            switch (type) {
                case FieldType::Float: WriteValue(destination, Mix(ReadValue<float>(from), ReadValue<float>(to), t)); return;
                case FieldType::Double: WriteValue(destination, Mix(ReadValue<double>(from), ReadValue<double>(to), t)); return;
                case FieldType::Vector3f: {
                    const auto a = ReadValue<Math::Vector3f>(from);
                    const auto b = ReadValue<Math::Vector3f>(to);
                    WriteValue(destination, Math::Vector3f{Mix(a.x, b.x, t), Mix(a.y, b.y, t), Mix(a.z, b.z, t)});
                    return;
                }
                case FieldType::Vector3d: {
                    const auto a = ReadValue<Math::Vector3d>(from);
                    const auto b = ReadValue<Math::Vector3d>(to);
                    WriteValue(destination, Math::Vector3d{Mix(a.x, b.x, t), Mix(a.y, b.y, t), Mix(a.z, b.z, t)});
                    return;
                }
                case FieldType::Quaternion:
                    WriteValue(destination, Math::Slerp(ReadValue<Math::Quaternionf>(from), ReadValue<Math::Quaternionf>(to), t));
                    return;
                case FieldType::Color: {
                    const auto a = ReadValue<Math::LinearColor>(from);
                    const auto b = ReadValue<Math::LinearColor>(to);
                    WriteValue(destination, Math::LinearColor{Mix(a.r, b.r, t), Mix(a.g, b.g, t), Mix(a.b, b.b, t), Mix(a.a, b.a, t)});
                    return;
                }
                case FieldType::Bool:
                case FieldType::Int:
                case FieldType::String:
                case FieldType::Enum: break;
            }
            std::memcpy(destination, to, SizeOf(type));
        }

        void SetSector(Scene& scene, Entity entity, const Math::Vector3i64& sector) {
            entt::registry& registry = scene.GetRegistry();
            if (sector != Math::Vector3i64{}) {
                registry.emplace_or_replace<SectorComponent>(entity, sector);
            } else {
                registry.remove<SectorComponent>(entity);
            }
        }
    }

    void Schema::Build(const ComponentRegistry& components) {
        m_components.clear();
        m_fields.clear();
        m_stateSize = 0;
        m_hash = 0xCBF29CE484222325ull;
        HashBytes(m_hash, &FormatVersion, sizeof(FormatVersion));
        for (std::size_t c = 0; c < components.GetCount(); ++c) {
            const ComponentInfo& info = components.Get(c);
            Component component{.info = &info, .firstField = static_cast<std::uint32_t>(m_fields.size())};
            for (const FieldInfo& field : info.fields) {
                if (!field.replicated) {
                    continue;
                }
                const std::uint32_t size = SizeOf(field.type);
                assert(size > 0 && "champ répliqué de taille fixe (ComponentBuilder::Replicated)");
                assert(component.fieldCount < MaxFieldsPerComponent);
                m_fields.push_back({.info = &field, .offset = static_cast<std::uint32_t>(m_stateSize), .size = size,
                                    .index = component.fieldCount});
                ++component.fieldCount;
                m_stateSize += size;
                HashText(m_hash, info.key);
                HashText(m_hash, field.key);
                HashBytes(m_hash, &field.type, sizeof(field.type));
                HashBytes(m_hash, &size, sizeof(size));
            }
            if (component.fieldCount > 0) {
                assert(m_components.size() < MaxComponents);
                m_components.push_back(component);
            }
        }
    }

    std::ptrdiff_t WorldState::Find(Uuid id) const {
        const auto found = std::ranges::lower_bound(entities, id, {}, &EntityState::id);
        return found != entities.end() && found->id == id ? found - entities.begin() : -1;
    }

    void WorldState::Clear() {
        sequence = 0;
        step = 0;
        entities.clear();
        values.clear();
    }

    void Capturer::Capture(const Scene& scene, const Schema& schema, WorldState& out) {
        const entt::registry& registry = scene.GetRegistry();
        m_sorted.clear();
        for (const auto [entity, id] : registry.view<const IdComponent>().each()) {
            if (id.id) {
                m_sorted.emplace_back(id.id, entity);
            }
        }
        std::ranges::sort(m_sorted, {}, &std::pair<Uuid, Entity>::first);

        const std::size_t stateSize = schema.GetStateSize();
        out.entities.resize(m_sorted.size());
        out.values.assign(m_sorted.size() * stateSize, std::byte{0});
        const std::span<const Schema::Component> components = schema.GetComponents();
        for (std::size_t i = 0; i < m_sorted.size(); ++i) {
            const auto [id, entity] = m_sorted[i];
            EntityState& state = out.entities[i];
            state.id = id;
            const auto* sector = registry.try_get<SectorComponent>(entity);
            state.sector = sector ? sector->sector : Math::Vector3i64{};
            const Entity parent = scene.GetParent(entity);
            state.parent = parent != NullEntity ? scene.GetId(parent) : Uuid{};
            state.components = 0;
            std::byte* values = out.values.data() + i * stateSize;
            for (std::size_t c = 0; c < components.size(); ++c) {
                void* instance = components[c].info->get(registry, entity);
                if (instance == nullptr) {
                    continue;
                }
                state.components |= 1ull << c;
                for (const Schema::Field& field : schema.GetFields(components[c])) {
                    std::memcpy(values + field.offset, field.info->access(instance), field.size);
                }
            }
        }
    }

    void Encode(const Schema& schema, const WorldState& current, const WorldState* baseline, ByteWriter& out) {
        out.Write(current.sequence);
        out.Write(baseline ? baseline->sequence : std::uint32_t{0});
        out.Write(current.step);
        static const WorldState Empty;
        const WorldState& base = baseline ? *baseline : Empty;

        // Disparues depuis la base.
        std::size_t removed = 0;
        for (const EntityState& entity : base.entities) {
            removed += current.Find(entity.id) < 0 ? 1 : 0;
        }
        out.WriteVarint(removed);
        for (const EntityState& entity : base.entities) {
            if (current.Find(entity.id) < 0) {
                out.Write(entity.id.value);
            }
        }

        // Nouvelles ou changées : le nombre est écrit à sa place une fois connu.
        const std::size_t countAt = out.GetSize();
        out.Write(std::uint32_t{0});
        std::uint32_t changed = 0;
        const std::span<const Schema::Component> components = schema.GetComponents();
        std::size_t b = 0;
        for (std::size_t i = 0; i < current.entities.size(); ++i) {
            const EntityState& entity = current.entities[i];
            while (b < base.entities.size() && base.entities[b].id < entity.id) {
                ++b;
            }
            const bool known = b < base.entities.size() && base.entities[b].id == entity.id;
            const std::span<const std::byte> values = current.GetValues(schema, i);
            const std::span<const std::byte> before = known ? base.GetValues(schema, b) : std::span<const std::byte>{};

            std::uint8_t flags = 0;
            if (!known) {
                // Secteur 0 (presque toutes) : pas écrit, c'est celui d'une entité nouvelle.
                flags = NewEntity | ComponentsChanged | (entity.sector != Math::Vector3i64{} ? SectorChanged : 0)
                    | (entity.parent ? ParentChanged : 0);
            } else {
                flags |= entity.sector != base.entities[b].sector ? SectorChanged : 0;
                flags |= entity.components != base.entities[b].components ? ComponentsChanged : 0;
                flags |= entity.parent != base.entities[b].parent ? ParentChanged : 0;
            }
            // Champs changés, composant par composant (un composant ajouté depuis la base : tous ses champs).
            bool anyField = false;
            std::array<std::uint64_t, Schema::MaxComponents> fieldMasks{};
            for (std::size_t c = 0; c < components.size(); ++c) {
                if ((entity.components & (1ull << c)) == 0) {
                    continue;
                }
                const bool wasPresent = known && (base.entities[b].components & (1ull << c)) != 0;
                for (const Schema::Field& field : schema.GetFields(components[c])) {
                    if (!wasPresent
                        || std::memcmp(values.data() + field.offset, before.data() + field.offset, field.size) != 0) {
                        fieldMasks[c] |= 1ull << field.index;
                        anyField = true;
                    }
                }
            }
            if (flags == 0 && !anyField) {
                continue;
            }
            ++changed;
            out.Write(entity.id.value);
            out.Write(flags);
            if (flags & SectorChanged) {
                out.Write(entity.sector);
            }
            if (flags & ParentChanged) {
                out.Write(entity.parent.value);
            }
            if (flags & ComponentsChanged) {
                out.WriteVarint(entity.components);
            }
            for (std::size_t c = 0; c < components.size(); ++c) {
                if ((entity.components & (1ull << c)) == 0) {
                    continue;
                }
                out.WriteVarint(fieldMasks[c]);
                for (const Schema::Field& field : schema.GetFields(components[c])) {
                    if (fieldMasks[c] & (1ull << field.index)) {
                        out.WriteBytes(values.data() + field.offset, field.size);
                    }
                }
            }
        }
        out.Patch(countAt, changed);
    }

    SnapshotHeader ReadHeader(ByteReader& reader) {
        SnapshotHeader header;
        header.sequence = reader.Read<std::uint32_t>();
        header.baseline = reader.Read<std::uint32_t>();
        header.step = reader.Read<std::uint64_t>();
        return header;
    }

    std::expected<void, std::string> Decode(const Schema& schema, const SnapshotHeader& header, ByteReader& reader,
                                            const WorldState* baseline, WorldState& out) {
        if ((header.baseline != 0) != (baseline != nullptr) || (baseline && baseline->sequence != header.baseline)) {
            return std::unexpected("base de l'instantané inconnue");
        }
        static const WorldState Empty;
        const WorldState& base = baseline ? *baseline : Empty;
        const std::size_t stateSize = schema.GetStateSize();
        const std::span<const Schema::Component> components = schema.GetComponents();
        out.Clear();
        out.sequence = header.sequence;
        out.step = header.step;

        // Disparues : lues au fil de la fusion, depuis une copie du lecteur placée sur leur liste.
        const std::uint64_t removedCount = reader.ReadVarint();
        if (removedCount > reader.GetRemaining() / sizeof(std::uint64_t)) {
            return std::unexpected("instantané abîmé (disparues)");
        }
        ByteReader removed = reader;
        reader.Skip(static_cast<std::size_t>(removedCount) * sizeof(std::uint64_t));
        std::uint64_t removedLeft = removedCount;
        std::optional<Uuid> nextRemoved;
        const auto popRemoved = [&] {
            nextRemoved = removedLeft-- > 0 ? std::optional(Uuid{removed.Read<std::uint64_t>()}) : std::nullopt;
        };
        popRemoved();

        std::uint32_t changedLeft = reader.Read<std::uint32_t>();
        std::size_t b = 0;
        Uuid previous{};
        while (reader.IsValid() && (b < base.entities.size() || changedLeft > 0)) {
            const Uuid baseId = b < base.entities.size() ? base.entities[b].id : Uuid{std::numeric_limits<std::uint64_t>::max()};
            // Prochaine changée : son identifiant est lu sans avancer (il ouvre son entrée).
            Uuid changedId{std::numeric_limits<std::uint64_t>::max()};
            if (changedLeft > 0) {
                ByteReader peek = reader;
                changedId = Uuid{peek.Read<std::uint64_t>()};
            }
            if (changedLeft == 0 || baseId < changedId) {
                // Inchangée depuis la base, sauf si elle a disparu.
                while (nextRemoved && *nextRemoved < baseId) {
                    popRemoved();
                }
                if (nextRemoved && *nextRemoved == baseId) {
                    popRemoved();
                } else {
                    out.entities.push_back(base.entities[b]);
                    const std::span<const std::byte> values = base.GetValues(schema, b);
                    out.values.insert(out.values.end(), values.begin(), values.end());
                }
                ++b;
                continue;
            }
            // Changée ou nouvelle.
            reader.Skip(sizeof(std::uint64_t));
            --changedLeft;
            if (!(previous < changedId) || changedId == Uuid{}) {
                return std::unexpected("instantané abîmé (ordre des entités)");
            }
            previous = changedId;
            const std::uint8_t flags = reader.Read<std::uint8_t>();
            const bool known = baseId == changedId;
            if (known == ((flags & NewEntity) != 0)) {
                return std::unexpected("instantané abîmé (entité nouvelle ou non)");
            }
            EntityState entity = known ? base.entities[b] : EntityState{.id = changedId, .sector = {}, .parent = {}, .components = 0};
            const std::size_t at = out.values.size();
            if (known) {
                const std::span<const std::byte> values = base.GetValues(schema, b);
                out.values.insert(out.values.end(), values.begin(), values.end());
                ++b;
            } else {
                out.values.resize(at + stateSize, std::byte{0});
            }
            if (flags & SectorChanged) {
                entity.sector = reader.Read<Math::Vector3i64>();
            }
            if (flags & ParentChanged) {
                entity.parent = Uuid{reader.Read<std::uint64_t>()};
            }
            const std::uint64_t before = entity.components;
            if (flags & ComponentsChanged) {
                entity.components = reader.ReadVarint();
                if (components.size() < 64 && (entity.components >> components.size()) != 0) {
                    return std::unexpected("instantané abîmé (composants)");
                }
            }
            std::byte* values = out.values.data() + at;
            for (std::size_t c = 0; c < components.size(); ++c) {
                const std::uint64_t bit = 1ull << c;
                if ((entity.components & bit) == 0) {
                    if (before & bit) {
                        // Composant retiré : ses valeurs à 0, comme Capture les écrit.
                        for (const Schema::Field& field : schema.GetFields(components[c])) {
                            std::memset(values + field.offset, 0, field.size);
                        }
                    }
                    continue;
                }
                const std::uint64_t fieldMask = reader.ReadVarint();
                if ((before & bit) == 0) {
                    // Composant ajouté : ce que la base avait de lui ne vaut plus rien.
                    for (const Schema::Field& field : schema.GetFields(components[c])) {
                        std::memset(values + field.offset, 0, field.size);
                    }
                }
                for (const Schema::Field& field : schema.GetFields(components[c])) {
                    if (fieldMask & (1ull << field.index)) {
                        reader.ReadBytes(values + field.offset, field.size);
                    }
                }
            }
            out.entities.push_back(entity);
        }
        if (!reader.IsValid() || !removed.IsValid() || changedLeft > 0) {
            return std::unexpected("instantané tronqué");
        }
        return {};
    }

    void Apply(Scene& scene, const Schema& schema, const WorldState& from, const WorldState& to, float t) {
        entt::registry& registry = scene.GetRegistry();
        const std::span<const Schema::Component> components = schema.GetComponents();
        std::size_t f = 0;
        for (std::size_t i = 0; i < to.entities.size(); ++i) {
            const EntityState& state = to.entities[i];
            while (f < from.entities.size() && from.entities[f].id < state.id) {
                ++f;
            }
            const Entity entity = scene.FindById(state.id);
            if (entity == NullEntity) {
                continue;
            }
            const bool hasFrom = f < from.entities.size() && from.entities[f].id == state.id;
            // Un saut de secteur ou un changement de parent ne s'interpole pas (positions de deux repères) : d'un coup.
            const bool interpolate = hasFrom && &from != &to && from.entities[f].sector == state.sector
                && from.entities[f].parent == state.parent;
            // Monté à bord d'un vaisseau ou descendu : sa place locale qui suit est dans le repère du nouveau parent.
            // Un parent pas encore connu du client (créé pendant le jeu, en route) : au prochain état.
            const Entity parent = state.parent ? scene.FindById(state.parent) : NullEntity;
            if ((parent != NullEntity || !state.parent) && scene.GetParent(entity) != parent) {
                scene.SetParent(entity, parent);
            }
            if (scene.GetParent(entity) == NullEntity) {
                const auto* sector = registry.try_get<SectorComponent>(entity);
                if ((sector ? sector->sector : Math::Vector3i64{}) != state.sector) {
                    SetSector(scene, entity, state.sector);
                }
            }
            const std::span<const std::byte> values = to.GetValues(schema, i);
            const std::span<const std::byte> before = hasFrom ? from.GetValues(schema, f) : values;
            for (std::size_t c = 0; c < components.size(); ++c) {
                const ComponentInfo& info = *components[c].info;
                void* instance = info.get(registry, entity);
                const std::uint64_t bit = 1ull << c;
                if ((state.components & bit) == 0) {
                    if (instance != nullptr && info.removable) {
                        info.remove(registry, entity);
                    }
                    continue;
                }
                if (instance == nullptr) {
                    info.add(registry, entity);
                    instance = info.get(registry, entity);
                }
                const bool mix = interpolate && (from.entities[f].components & bit) != 0;
                for (const Schema::Field& field : schema.GetFields(components[c])) {
                    void* destination = field.info->access(instance);
                    if (mix) {
                        Interpolate(field.info->type, before.data() + field.offset, values.data() + field.offset, t, destination);
                    } else {
                        std::memcpy(destination, values.data() + field.offset, field.size);
                    }
                }
            }
        }
    }

    void DestroyMissing(Scene& scene, const WorldState* previous, const WorldState& state, std::span<const Uuid> keep) {
        if (previous != nullptr) {
            for (const EntityState& entity : previous->entities) {
                if (state.Find(entity.id) < 0) {
                    if (const Entity gone = scene.FindById(entity.id); gone != NullEntity) {
                        scene.DestroyEntity(gone); // ses enfants avec elle
                    }
                }
            }
            return;
        }
        const auto kept = [&](Entity entity) {
            for (; entity != NullEntity; entity = scene.GetParent(entity)) {
                if (std::ranges::find(keep, scene.GetId(entity)) != keep.end()) {
                    return true;
                }
            }
            return false;
        };
        std::vector<Uuid> missing;
        for (const auto [entity, id] : scene.GetRegistry().view<const IdComponent>().each()) {
            if (id.id && state.Find(id.id) < 0 && !kept(entity)) {
                missing.push_back(id.id);
            }
        }
        for (const Uuid id : missing) {
            if (const Entity gone = scene.FindById(id); gone != NullEntity) {
                scene.DestroyEntity(gone);
            }
        }
    }

    WorldState& SnapshotHistory::Next(std::uint64_t step) {
        ++m_latest;
        if (m_latest == 0) {
            m_latest = 1; // après 4 milliards d'instantanés (4 ans à 30 par seconde)
        }
        WorldState& state = m_states[m_latest % Capacity];
        state.sequence = m_latest;
        state.step = step;
        return state;
    }

    const WorldState* SnapshotHistory::Find(std::uint32_t sequence) const {
        const WorldState& state = m_states[sequence % Capacity];
        return sequence != 0 && state.sequence == sequence ? &state : nullptr;
    }

    void SnapshotHistory::Clear() {
        for (WorldState& state : m_states) {
            state.Clear();
        }
        m_latest = 0;
    }

    std::expected<std::uint32_t, std::string> SnapshotReceiver::Receive(const Schema& schema, ByteReader& reader) {
        const SnapshotHeader header = ReadHeader(reader);
        if (!reader.IsValid() || header.sequence == 0) {
            return std::unexpected("instantané abîmé (en-tête)");
        }
        if (header.sequence <= m_latest || (header.baseline != 0 && header.sequence - header.baseline >= SnapshotHistory::Capacity)) {
            return 0u; // arrivé après un plus récent : celui-là porte déjà tout
        }
        const WorldState* baseline = nullptr;
        if (header.baseline != 0) {
            const WorldState& candidate = m_states[header.baseline % SnapshotHistory::Capacity];
            if (candidate.sequence != header.baseline) {
                return 0u; // base déjà remplacée ici : le serveur repartira d'un accusé plus récent
            }
            baseline = &candidate;
        }
        WorldState& out = m_states[header.sequence % SnapshotHistory::Capacity];
        if (auto decoded = Decode(schema, header, reader, baseline, out); !decoded) {
            out.Clear();
            return std::unexpected(decoded.error());
        }
        m_latest = header.sequence;
        return header.sequence;
    }

    void SnapshotReceiver::Advance() {
        const WorldState* latest = GetLatest();
        if (latest == nullptr) {
            return;
        }
        const double target = static_cast<double>(latest->step) - DelaySteps;
        if (!m_started || std::abs(target - m_displayStep) > 60.0) {
            m_displayStep = target; // premier instantané, ou trop loin (serveur arrêté un moment) : d'un coup
            m_started = true;
            return;
        }
        // Un pas, corrigé d'un dixième de l'écart : l'horloge du serveur et la nôtre dérivent peu, les arrivées
        // irrégulières se lissent sans à-coup.
        m_displayStep += 1.0 + (target - m_displayStep) * 0.1;
    }

    bool SnapshotReceiver::Sample(const WorldState*& from, const WorldState*& to, float& t) const {
        from = nullptr;
        to = nullptr;
        const WorldState* oldest = nullptr;
        for (const WorldState& state : m_states) {
            if (state.sequence == 0) {
                continue;
            }
            const double step = static_cast<double>(state.step);
            if (step <= m_displayStep && (from == nullptr || state.step > from->step)) {
                from = &state;
            }
            if (step > m_displayStep && (to == nullptr || state.step < to->step)) {
                to = &state;
            }
            if (oldest == nullptr || state.step < oldest->step) {
                oldest = &state;
            }
        }
        if (from == nullptr && to == nullptr) {
            return false;
        }
        if (from == nullptr) {
            from = to = oldest; // affiché avant le plus ancien reçu : on l'attend
            t = 1.0f;
            return true;
        }
        if (to == nullptr) {
            to = from; // au-delà du dernier : on n'extrapole pas
            t = 1.0f;
            return true;
        }
        t = static_cast<float>((m_displayStep - static_cast<double>(from->step))
                               / static_cast<double>(to->step - from->step));
        return true;
    }

    const WorldState* SnapshotReceiver::GetLatest() const {
        const WorldState& state = m_states[m_latest % SnapshotHistory::Capacity];
        return m_latest != 0 && state.sequence == m_latest ? &state : nullptr;
    }

    const WorldState* SnapshotReceiver::Find(std::uint32_t sequence) const {
        const WorldState& state = m_states[sequence % SnapshotHistory::Capacity];
        return sequence != 0 && state.sequence == sequence ? &state : nullptr;
    }

    void SnapshotReceiver::Clear() {
        for (WorldState& state : m_states) {
            state.Clear();
        }
        m_latest = 0;
        m_displayStep = 0.0;
        m_started = false;
    }
}
