// Implementations of the single-header libraries mod support uses: cgltf (glTF models)
// and stb_image (their textures).
#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_FAILURE_USERMSG
#include <stb_image.h>
