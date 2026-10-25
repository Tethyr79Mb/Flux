// PlanetMesh.slang en groupes de 256 threads (8 x 16 mailles par groupe, jupes à part), pour les pilotes qui préfèrent
// des groupes plus grands (AMD : maxPreferredMeshWorkGroupInvocations à 256). MeshRenderer choisit.
#define FLUX_MESH_LARGE 1
#include "PlanetMesh.slang"
