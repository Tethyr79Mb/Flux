// ModelMesh.slang avec les ombres du soleil en rayons (ray queries) : choisi par MeshRenderer quand le GPU a le ray
// tracing (RHI::Device::HasRayTracing). Compilé à part : la structure haute (sceneStructure) n'existe que dans ce module.
#define FLUX_RAY_QUERY 1
#include "ModelMesh.slang"
