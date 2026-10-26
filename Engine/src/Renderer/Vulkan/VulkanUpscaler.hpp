#pragma once

#include "Renderer/RHI/RHI.hpp"

#include <expected>
#include <memory>
#include <string>

namespace Flux::Vk {
    class Device;

    // RHI::Upscaler par FSR 3.1 d'AMD (amd_fidelityfx_vk.dll, à côté de l'exe, chargée ici).
    std::expected<std::unique_ptr<RHI::Upscaler>, std::string> CreateFsrUpscaler(const Device& device, RHI::Extent outputSize);
}
