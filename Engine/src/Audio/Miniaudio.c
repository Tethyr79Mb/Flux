/* Implémentation de miniaudio (bibliothèque « miniaudio » de cmake/FluxDependencies.cmake), compilée en C.
   stb_vorbis d'abord en déclarations seules, puis en entier après miniaudio : c'est ainsi que miniaudio lit les .ogg. */
#define STB_VORBIS_HEADER_ONLY
#include "extras/stb_vorbis.c"

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#undef STB_VORBIS_HEADER_ONLY
#include "extras/stb_vorbis.c"
