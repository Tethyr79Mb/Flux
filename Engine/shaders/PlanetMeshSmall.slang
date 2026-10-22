// PlanetMesh.slang en groupes de 64 threads (paquets dessinés en deux moitiés de 8 x 4 mailles), pour les pilotes qui
// préfèrent des groupes plus petits (maxPreferredMeshWorkGroupInvocations sous 128). MeshRenderer choisit.
#define FLUX_MESH_SMALL 1
#include "PlanetMesh.slang"
