#pragma once

// RHI (Render Hardware Interface) : ce que le renderer demande au GPU, sans aucun type d'une API graphique. Vulkan
// l'implémente (Renderer/RHI/Vulkan) ; un autre backend (DX12) n'aurait qu'à implémenter ces interfaces.
//
// Portée actuelle, celle dont le renderer a besoin : une fenêtre (sa swapchain est tenue par le Device), des
// buffers, des textures 2D, cube et tableaux, des groupes de ressources (textures, buffers de stockage) liés aux shaders, des pipelines
// graphiques et de calcul (compute), des structures d'accélération du ray tracing (facultatives), et une liste de
// commandes par image. Une passe dessine dans l'image de la swapchain
// ou dans une texture.
//
// Durées de vie : un objet créé par le Device doit être détruit avant lui ; un BindGroup avant son BindGroupLayout.
// Un objet ne se détruit ni ne se modifie pendant que le GPU l'utilise : après Device::WaitIdle, une fois BeginFrame
// revenu sur l'image qui s'en servait, ou confié à Device::Retire (détruit quand le GPU a fini, sans attendre).
//
// Un en-tête par interface (Buffer, AccelerationStructure, Texture, BindGroup, Pipeline, CommandList, Upscaler, Device), types communs dans
// RHITypes.hpp ; celui-ci les inclut tous.

#include "Flux/Renderer/RHI/RHITypes.hpp"
#include "Flux/Renderer/RHI/Buffer.hpp"
#include "Flux/Renderer/RHI/AccelerationStructure.hpp"
#include "Flux/Renderer/RHI/Texture.hpp"
#include "Flux/Renderer/RHI/BindGroup.hpp"
#include "Flux/Renderer/RHI/Pipeline.hpp"
#include "Flux/Renderer/RHI/CommandList.hpp"
#include "Flux/Renderer/RHI/Upscaler.hpp"
#include "Flux/Renderer/RHI/Device.hpp"
