#include <stdlib.h>

// Every allocation stb makes goes through `image_alloc` and `image_realloc`, which call `os_oom`
// instead of returning `NULL`, so a failed allocation inside stb ends the build like every other.
// stb's resizer passes a context pointer to its hooks, which these allocations do not need.
#include "formats/image.h"
#define STBIR_MALLOC(size, user_data) ((void)(user_data), image_alloc(size))
#define STBIR_FREE(data, user_data) ((void)(user_data), free(data))
#define STBIW_MALLOC(size) image_alloc(size)
#define STBIW_REALLOC(data, size) image_realloc((data), (size))
#define STBIW_FREE(data) free(data)

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image_resize2.h"

#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
