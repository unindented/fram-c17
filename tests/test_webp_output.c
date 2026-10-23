#include "test_webp_output.h"

#include <stdint.h>

#define TEST_NO_MAIN
#include <acutest.h>

#include "webp/decode.h"

int test_webp_decode(struct Str webp, struct Image* image_out) {
  *image_out = (struct Image){0};
  int width = 0;
  int height = 0;
  uint8_t* pixels = WebPDecodeRGB((const uint8_t*)webp.data, (size_t)webp.len, &width, &height);
  if (pixels == NULL) {
    TEST_MSG("failed to decode WebP output of %td bytes", webp.len);
    return -1;
  }
  *image_out = (struct Image){.pixels = pixels, .width_px = width, .height_px = height};
  return 0;
}
