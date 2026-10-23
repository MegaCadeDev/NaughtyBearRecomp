// Implementations of the single-header libraries mod support uses: cgltf (glTF models),
// stb_image (their textures) and stb_image_write (textures exported for modders).
#define CGLTF_IMPLEMENTATION
#if !defined(__PROSPERO__) // the PS5 version has no mods

#include <cgltf.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_FAILURE_USERMSG
#include <stb_image.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#endif
