#include "Interface/InterfaceLibrary.hpp"

#include "Flux/Platform/Filesystem.hpp"

#include <cstdio>
#include <system_error>

namespace Flux {
    namespace {
        // Un fichier n'est pas réinterrogé plus souvent : quelques interfaces dessinées à chaque image ne coûtent
        // pas un accès disque chacune.
        constexpr std::chrono::milliseconds CheckInterval{500};
    }

    InterfaceLibrary::Entry& InterfaceLibrary::Refresh(const std::filesystem::path& assets, std::string_view file) {
        auto [found, inserted] = m_files.try_emplace(std::string(file));
        Entry& entry = found->second;
        const Clock::time_point now = Clock::now();
        if (!inserted && now - entry.checked < CheckInterval) {
            return entry;
        }
        entry.checked = now;
        const std::filesystem::path path = assets / Platform::PathFromUtf8(file);
        std::error_code error;
        const std::filesystem::file_time_type time = std::filesystem::last_write_time(path, error);
        if (error) {
            if (entry.valid || inserted) {
                std::fprintf(stderr, "Flux : interface introuvable : %s\n", std::string(file).c_str());
            }
            entry.valid = false;
            entry.time = {};
            return entry;
        }
        if (!inserted && time == entry.time) {
            return entry;
        }
        entry.time = time;
        if (auto loaded = Interface::LoadFile(path)) {
            entry.document = std::move(*loaded);
            entry.valid = true;
            entry.version = m_nextVersion++;
        } else {
            // Fichier en cours d'écriture ou abîmé : on garde la version d'avant s'il y en a une.
            std::fprintf(stderr, "Flux : interface %s\n", loaded.error().c_str());
        }
        return entry;
    }

    const Interface::Document* InterfaceLibrary::Get(const std::filesystem::path& assets, std::string_view file) {
        const Entry& entry = Refresh(assets, file);
        return entry.valid ? &entry.document : nullptr;
    }

    InterfaceLibrary::Instance* InterfaceLibrary::GetInstance(const std::filesystem::path& assets, Entity entity,
                                                              std::string_view file, std::uint64_t frame) {
        const Entry& entry = Refresh(assets, file);
        if (!entry.valid) {
            m_instances.erase(entity);
            return nullptr;
        }
        Instance& instance = m_instances[entity];
        if (instance.file != file || instance.version != entry.version) {
            instance.file = file;
            instance.version = entry.version;
            instance.document = entry.document;
            instance.interaction = {};
        }
        instance.lastFrame = frame;
        return &instance;
    }

    void InterfaceLibrary::ForgetInstancesBefore(std::uint64_t frame) {
        std::erase_if(m_instances, [frame](const auto& item) { return item.second.lastFrame < frame; });
    }
}
