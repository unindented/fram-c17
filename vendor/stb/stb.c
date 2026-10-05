#define STBI_ONLY_JPEG
#define STBI_NO_LINEAR
#define STBI_NO_STDIO
#define STBI_NO_THREAD_LOCALS
#define STBI_THREAD_LOCAL _Thread_local
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image_resize2.h"

#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
