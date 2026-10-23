#include <stdlib.h>

// Every allocation stb's resizer makes goes through `image_alloc`, which calls `os_oom` instead of
// returning `NULL`, so a failed allocation inside stb ends the build like every other. The resizer
// passes a context pointer to its hooks, which these allocations do not need.
#include "formats/image.h"
#define STBIR_MALLOC(size, user_data) ((void)(user_data), image_alloc(size))
#define STBIR_FREE(data, user_data) ((void)(user_data), free(data))

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image_resize2.h"
