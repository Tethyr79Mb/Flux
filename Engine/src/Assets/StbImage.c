/* Implémentation de stb_image et stb_image_write (bibliothèque « stb_image_impl » de cmake/stb), réduite aux formats utiles. Compilée
   optimisée même en Debug : une vingtaine d'images de 2048² y prennent sinon une dizaine de secondes. */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_TGA
#define STBI_ONLY_BMP
#define STBI_NO_STDIO /* fichiers lus par nous : chemins Unicode sous Windows */
#define STBI_FAILURE_USERMSG
#include <stb_image.h>

/* Écriture en PNG seulement (captures d'écran de l'outil MCP de l'éditeur), en mémoire. */
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include <stb_image_write.h>
