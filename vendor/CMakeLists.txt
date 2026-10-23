# This file isolates third-party sources from first-party warnings and analyzers.

add_library(fram_vendor_acutest INTERFACE)
target_include_directories(
  fram_vendor_acutest SYSTEM INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/acutest>"
)

add_library(fram_vendor_copt INTERFACE)
target_include_directories(
  fram_vendor_copt SYSTEM INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/copt>"
)

# libjpeg-turbo's library sources, as its own build lists them. The files in `src/wrapper/` compile
# one precision-generic source once for each sample precision it supports.
add_library(
  fram_vendor_libjpeg_turbo STATIC
  libjpeg-turbo/src/jaricom.c
  libjpeg-turbo/src/jcapimin.c
  libjpeg-turbo/src/jcarith.c
  libjpeg-turbo/src/jchuff.c
  libjpeg-turbo/src/jcicc.c
  libjpeg-turbo/src/jcinit.c
  libjpeg-turbo/src/jclhuff.c
  libjpeg-turbo/src/jcmarker.c
  libjpeg-turbo/src/jcmaster.c
  libjpeg-turbo/src/jcomapi.c
  libjpeg-turbo/src/jcparam.c
  libjpeg-turbo/src/jcphuff.c
  libjpeg-turbo/src/jctrans.c
  libjpeg-turbo/src/jdapimin.c
  libjpeg-turbo/src/jdarith.c
  libjpeg-turbo/src/jdatadst.c
  libjpeg-turbo/src/jdatasrc.c
  libjpeg-turbo/src/jdhuff.c
  libjpeg-turbo/src/jdicc.c
  libjpeg-turbo/src/jdinput.c
  libjpeg-turbo/src/jdlhuff.c
  libjpeg-turbo/src/jdmarker.c
  libjpeg-turbo/src/jdmaster.c
  libjpeg-turbo/src/jdphuff.c
  libjpeg-turbo/src/jdtrans.c
  libjpeg-turbo/src/jerror.c
  libjpeg-turbo/src/jfdctflt.c
  libjpeg-turbo/src/jmemmgr.c
  libjpeg-turbo/src/jmemnobs.c
  libjpeg-turbo/src/jpeg_nbits.c
  libjpeg-turbo/src/wrapper/jcapistd-8.c
  libjpeg-turbo/src/wrapper/jcapistd-12.c
  libjpeg-turbo/src/wrapper/jcapistd-16.c
  libjpeg-turbo/src/wrapper/jccoefct-8.c
  libjpeg-turbo/src/wrapper/jccoefct-12.c
  libjpeg-turbo/src/wrapper/jccolor-8.c
  libjpeg-turbo/src/wrapper/jccolor-12.c
  libjpeg-turbo/src/wrapper/jccolor-16.c
  libjpeg-turbo/src/wrapper/jcdctmgr-8.c
  libjpeg-turbo/src/wrapper/jcdctmgr-12.c
  libjpeg-turbo/src/wrapper/jcdiffct-8.c
  libjpeg-turbo/src/wrapper/jcdiffct-12.c
  libjpeg-turbo/src/wrapper/jcdiffct-16.c
  libjpeg-turbo/src/wrapper/jclossls-8.c
  libjpeg-turbo/src/wrapper/jclossls-12.c
  libjpeg-turbo/src/wrapper/jclossls-16.c
  libjpeg-turbo/src/wrapper/jcmainct-8.c
  libjpeg-turbo/src/wrapper/jcmainct-12.c
  libjpeg-turbo/src/wrapper/jcmainct-16.c
  libjpeg-turbo/src/wrapper/jcprepct-8.c
  libjpeg-turbo/src/wrapper/jcprepct-12.c
  libjpeg-turbo/src/wrapper/jcprepct-16.c
  libjpeg-turbo/src/wrapper/jcsample-8.c
  libjpeg-turbo/src/wrapper/jcsample-12.c
  libjpeg-turbo/src/wrapper/jcsample-16.c
  libjpeg-turbo/src/wrapper/jdapistd-8.c
  libjpeg-turbo/src/wrapper/jdapistd-12.c
  libjpeg-turbo/src/wrapper/jdapistd-16.c
  libjpeg-turbo/src/wrapper/jdcoefct-8.c
  libjpeg-turbo/src/wrapper/jdcoefct-12.c
  libjpeg-turbo/src/wrapper/jdcolor-8.c
  libjpeg-turbo/src/wrapper/jdcolor-12.c
  libjpeg-turbo/src/wrapper/jdcolor-16.c
  libjpeg-turbo/src/wrapper/jddctmgr-8.c
  libjpeg-turbo/src/wrapper/jddctmgr-12.c
  libjpeg-turbo/src/wrapper/jddiffct-8.c
  libjpeg-turbo/src/wrapper/jddiffct-12.c
  libjpeg-turbo/src/wrapper/jddiffct-16.c
  libjpeg-turbo/src/wrapper/jdlossls-8.c
  libjpeg-turbo/src/wrapper/jdlossls-12.c
  libjpeg-turbo/src/wrapper/jdlossls-16.c
  libjpeg-turbo/src/wrapper/jdmainct-8.c
  libjpeg-turbo/src/wrapper/jdmainct-12.c
  libjpeg-turbo/src/wrapper/jdmainct-16.c
  libjpeg-turbo/src/wrapper/jdmerge-8.c
  libjpeg-turbo/src/wrapper/jdmerge-12.c
  libjpeg-turbo/src/wrapper/jdpostct-8.c
  libjpeg-turbo/src/wrapper/jdpostct-12.c
  libjpeg-turbo/src/wrapper/jdpostct-16.c
  libjpeg-turbo/src/wrapper/jdsample-8.c
  libjpeg-turbo/src/wrapper/jdsample-12.c
  libjpeg-turbo/src/wrapper/jdsample-16.c
  libjpeg-turbo/src/wrapper/jfdctfst-8.c
  libjpeg-turbo/src/wrapper/jfdctfst-12.c
  libjpeg-turbo/src/wrapper/jfdctint-8.c
  libjpeg-turbo/src/wrapper/jfdctint-12.c
  libjpeg-turbo/src/wrapper/jidctflt-8.c
  libjpeg-turbo/src/wrapper/jidctflt-12.c
  libjpeg-turbo/src/wrapper/jidctfst-8.c
  libjpeg-turbo/src/wrapper/jidctfst-12.c
  libjpeg-turbo/src/wrapper/jidctint-8.c
  libjpeg-turbo/src/wrapper/jidctint-12.c
  libjpeg-turbo/src/wrapper/jidctred-8.c
  libjpeg-turbo/src/wrapper/jidctred-12.c
  libjpeg-turbo/src/wrapper/jquant1-8.c
  libjpeg-turbo/src/wrapper/jquant1-12.c
  libjpeg-turbo/src/wrapper/jquant2-8.c
  libjpeg-turbo/src/wrapper/jquant2-12.c
  libjpeg-turbo/src/wrapper/jutils-8.c
  libjpeg-turbo/src/wrapper/jutils-12.c
  libjpeg-turbo/src/wrapper/jutils-16.c
)
target_include_directories(
  fram_vendor_libjpeg_turbo SYSTEM
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/libjpeg-turbo/src>"
         "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/libjpeg-turbo/config>"
)

# libwebp's encoder and decoder: their sources, the DSP and utility sources they use, and
# libsharpyuv, without the demuxer, the muxer or the programs. Only the tests decode WebP, so the
# decoder's objects stay out of `fram`, which never refers to them. `WEBP_USE_THREAD` makes the
# one-time DSP setup safe when several workers encode at once. It also allows threads inside an
# encode, which the encoder's configuration leaves off.
add_library(
  fram_vendor_libwebp STATIC
  libwebp/sharpyuv/sharpyuv.c
  libwebp/sharpyuv/sharpyuv_cpu.c
  libwebp/sharpyuv/sharpyuv_csp.c
  libwebp/sharpyuv/sharpyuv_dsp.c
  libwebp/sharpyuv/sharpyuv_gamma.c
  libwebp/sharpyuv/sharpyuv_neon.c
  libwebp/sharpyuv/sharpyuv_sse2.c
  libwebp/src/dsp/alpha_processing_neon.c
  libwebp/src/dsp/alpha_processing_sse2.c
  libwebp/src/dsp/alpha_processing_sse41.c
  libwebp/src/dec/alpha_dec.c
  libwebp/src/dec/buffer_dec.c
  libwebp/src/dec/frame_dec.c
  libwebp/src/dec/idec_dec.c
  libwebp/src/dec/io_dec.c
  libwebp/src/dec/quant_dec.c
  libwebp/src/dec/tree_dec.c
  libwebp/src/dec/vp8_dec.c
  libwebp/src/dec/vp8l_dec.c
  libwebp/src/dec/webp_dec.c
  libwebp/src/dsp/alpha_processing.c
  libwebp/src/dsp/cost_neon.c
  libwebp/src/dsp/cost_sse2.c
  libwebp/src/dsp/cost.c
  libwebp/src/dsp/cpu.c
  libwebp/src/dsp/dec_clip_tables.c
  libwebp/src/dsp/dec_neon.c
  libwebp/src/dsp/dec_sse2.c
  libwebp/src/dsp/dec_sse41.c
  libwebp/src/dsp/dec.c
  libwebp/src/dsp/enc_neon.c
  libwebp/src/dsp/enc_sse2.c
  libwebp/src/dsp/enc_sse41.c
  libwebp/src/dsp/enc.c
  libwebp/src/dsp/filters_neon.c
  libwebp/src/dsp/filters_sse2.c
  libwebp/src/dsp/filters.c
  libwebp/src/dsp/lossless_avx2.c
  libwebp/src/dsp/lossless_enc_avx2.c
  libwebp/src/dsp/lossless_enc_neon.c
  libwebp/src/dsp/lossless_enc_sse2.c
  libwebp/src/dsp/lossless_enc_sse41.c
  libwebp/src/dsp/lossless_enc.c
  libwebp/src/dsp/lossless_neon.c
  libwebp/src/dsp/lossless_sse2.c
  libwebp/src/dsp/lossless_sse41.c
  libwebp/src/dsp/lossless.c
  libwebp/src/dsp/rescaler_neon.c
  libwebp/src/dsp/rescaler_sse2.c
  libwebp/src/dsp/rescaler.c
  libwebp/src/dsp/ssim_sse2.c
  libwebp/src/dsp/ssim.c
  libwebp/src/dsp/upsampling_neon.c
  libwebp/src/dsp/upsampling_sse2.c
  libwebp/src/dsp/upsampling_sse41.c
  libwebp/src/dsp/upsampling.c
  libwebp/src/dsp/yuv_neon.c
  libwebp/src/dsp/yuv_sse2.c
  libwebp/src/dsp/yuv_sse41.c
  libwebp/src/dsp/yuv.c
  libwebp/src/enc/alpha_enc.c
  libwebp/src/enc/analysis_enc.c
  libwebp/src/enc/backward_references_cost_enc.c
  libwebp/src/enc/backward_references_enc.c
  libwebp/src/enc/config_enc.c
  libwebp/src/enc/cost_enc.c
  libwebp/src/enc/filter_enc.c
  libwebp/src/enc/frame_enc.c
  libwebp/src/enc/histogram_enc.c
  libwebp/src/enc/iterator_enc.c
  libwebp/src/enc/near_lossless_enc.c
  libwebp/src/enc/picture_csp_enc.c
  libwebp/src/enc/picture_enc.c
  libwebp/src/enc/picture_psnr_enc.c
  libwebp/src/enc/picture_rescale_enc.c
  libwebp/src/enc/picture_tools_enc.c
  libwebp/src/enc/predictor_enc.c
  libwebp/src/enc/quant_enc.c
  libwebp/src/enc/syntax_enc.c
  libwebp/src/enc/token_enc.c
  libwebp/src/enc/tree_enc.c
  libwebp/src/enc/vp8l_enc.c
  libwebp/src/enc/webp_enc.c
  libwebp/src/utils/bit_reader_utils.c
  libwebp/src/utils/bit_writer_utils.c
  libwebp/src/utils/color_cache_utils.c
  libwebp/src/utils/filters_utils.c
  libwebp/src/utils/huffman_encode_utils.c
  libwebp/src/utils/huffman_utils.c
  libwebp/src/utils/palette.c
  libwebp/src/utils/quant_levels_dec_utils.c
  libwebp/src/utils/quant_levels_utils.c
  libwebp/src/utils/random_utils.c
  libwebp/src/utils/rescaler_utils.c
  libwebp/src/utils/thread_utils.c
  libwebp/src/utils/utils.c
)
# A user includes the public headers as `webp/encode.h`, as libwebp installs them. libwebp's own
# sources include every header from the library's root, as `src/dsp/dsp.h`.
target_include_directories(
  fram_vendor_libwebp SYSTEM
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/libwebp/src>"
  PRIVATE "${CMAKE_CURRENT_LIST_DIR}/libwebp"
)
target_compile_definitions(fram_vendor_libwebp PRIVATE WEBP_USE_THREAD)
target_link_libraries(fram_vendor_libwebp PRIVATE Threads::Threads)
# libwebp calls the math library, which glibc ships apart from the C library. macOS and musl keep
# it in the C library, where a call to `pow` links on its own and there is no `libm` to find.
include(CheckCSourceCompiles)
check_c_source_compiles(
  "#include <math.h>
int main(int argc, char** argv) { (void)argv; return (int)pow((double)argc, 0.5); }"
  FRAM_HAVE_MATH_IN_C_LIBRARY
)
if(NOT FRAM_HAVE_MATH_IN_C_LIBRARY)
  find_library(FRAM_MATH_LIBRARY m REQUIRED)
  target_link_libraries(fram_vendor_libwebp PRIVATE "${FRAM_MATH_LIBRARY}")
endif()

# The SIMD routines exist for x86-64 and AArch64, and any other target uses the C routines.
# libjpeg-turbo's are NASM sources on x86-64 and Neon intrinsics on AArch64. libwebp chooses its
# routines at run time: its SSE2 and Neon routines need nothing beyond what the compiler enables,
# and its SSE4.1 and AVX2 routines need their instruction set enabled for their own sources only,
# while the `WEBP_HAVE_*` definitions tell the dispatchers in every other source that they exist.
# A macOS build names its architecture in `CMAKE_OSX_ARCHITECTURES`. A build for more than one uses
# libjpeg-turbo's C routines, and only the SSE2 or Neon ones of libwebp, which each architecture's
# compiler enables by default.
set(fram_vendor_processor "${CMAKE_SYSTEM_PROCESSOR}")
if(APPLE AND CMAKE_OSX_ARCHITECTURES)
  set(fram_vendor_processor "${CMAKE_OSX_ARCHITECTURES}")
endif()
if(fram_vendor_processor STREQUAL "x86_64")
  if(APPLE)
    set(CMAKE_ASM_NASM_OBJECT_FORMAT macho64)
  else()
    set(CMAKE_ASM_NASM_OBJECT_FORMAT elf64)
  endif()
  include(CheckLanguage)
  check_language(ASM_NASM)
  if(NOT CMAKE_ASM_NASM_COMPILER)
    message(FATAL_ERROR "x86-64 builds need NASM for libjpeg-turbo's SIMD routines; install 'nasm'")
  endif()
  enable_language(ASM_NASM)
  # The NASM sources are a library of their own, because the vendored-code profile and the warning
  # switch below are C compiler options that NASM does not accept.
  add_library(
    fram_vendor_libjpeg_turbo_simd OBJECT
    libjpeg-turbo/simd/x86_64/jsimdcpu.asm
    libjpeg-turbo/simd/x86_64/jfdctflt-sse.asm
    libjpeg-turbo/simd/x86_64/jccolor-sse2.asm
    libjpeg-turbo/simd/x86_64/jcgray-sse2.asm
    libjpeg-turbo/simd/x86_64/jchuff-sse2.asm
    libjpeg-turbo/simd/x86_64/jcphuff-sse2.asm
    libjpeg-turbo/simd/x86_64/jcsample-sse2.asm
    libjpeg-turbo/simd/x86_64/jdcolor-sse2.asm
    libjpeg-turbo/simd/x86_64/jdmerge-sse2.asm
    libjpeg-turbo/simd/x86_64/jdsample-sse2.asm
    libjpeg-turbo/simd/x86_64/jfdctfst-sse2.asm
    libjpeg-turbo/simd/x86_64/jfdctint-sse2.asm
    libjpeg-turbo/simd/x86_64/jidctflt-sse2.asm
    libjpeg-turbo/simd/x86_64/jidctfst-sse2.asm
    libjpeg-turbo/simd/x86_64/jidctint-sse2.asm
    libjpeg-turbo/simd/x86_64/jidctred-sse2.asm
    libjpeg-turbo/simd/x86_64/jquantf-sse2.asm
    libjpeg-turbo/simd/x86_64/jquanti-sse2.asm
    libjpeg-turbo/simd/x86_64/jccolor-avx2.asm
    libjpeg-turbo/simd/x86_64/jcgray-avx2.asm
    libjpeg-turbo/simd/x86_64/jcsample-avx2.asm
    libjpeg-turbo/simd/x86_64/jdcolor-avx2.asm
    libjpeg-turbo/simd/x86_64/jdmerge-avx2.asm
    libjpeg-turbo/simd/x86_64/jdsample-avx2.asm
    libjpeg-turbo/simd/x86_64/jfdctint-avx2.asm
    libjpeg-turbo/simd/x86_64/jidctint-avx2.asm
    libjpeg-turbo/simd/x86_64/jquanti-avx2.asm
  )
  # The object format selects the matching NASM macros. `__CET__` adds the indirect-branch markers
  # that a control-flow-protected ELF link expects, and they are no-ops elsewhere. `PIC` keeps every
  # data reference position-independent, as the executable is.
  if(APPLE)
    set(fram_jpeg_nasm_format MACHO)
  else()
    set(fram_jpeg_nasm_format ELF __CET__)
  endif()
  target_compile_definitions(
    fram_vendor_libjpeg_turbo_simd PRIVATE ${fram_jpeg_nasm_format} __x86_64__ PIC
  )
  target_include_directories(
    fram_vendor_libjpeg_turbo_simd PRIVATE "${CMAKE_CURRENT_LIST_DIR}/libjpeg-turbo/simd/nasm/"
                                           "${CMAKE_CURRENT_LIST_DIR}/libjpeg-turbo/simd/x86_64/"
  )
  target_sources(
    fram_vendor_libjpeg_turbo PRIVATE libjpeg-turbo/simd/jsimd.c
                                      $<TARGET_OBJECTS:fram_vendor_libjpeg_turbo_simd>
  )
  target_compile_definitions(fram_vendor_libjpeg_turbo PRIVATE JPEG_SIMD_ARCHITECTURE=X86_64)
  set_property(
    SOURCE libwebp/src/dsp/alpha_processing_sse41.c
           libwebp/src/dsp/dec_sse41.c
           libwebp/src/dsp/enc_sse41.c
           libwebp/src/dsp/lossless_enc_sse41.c
           libwebp/src/dsp/lossless_sse41.c
           libwebp/src/dsp/upsampling_sse41.c
           libwebp/src/dsp/yuv_sse41.c
    APPEND
    PROPERTY COMPILE_OPTIONS -msse4.1
  )
  set_property(
    SOURCE libwebp/src/dsp/lossless_avx2.c libwebp/src/dsp/lossless_enc_avx2.c
    APPEND
    PROPERTY COMPILE_OPTIONS -mavx2
  )
  target_compile_definitions(fram_vendor_libwebp PRIVATE WEBP_HAVE_SSE41 WEBP_HAVE_AVX2)
elseif(fram_vendor_processor MATCHES "^(aarch64|arm64)$")
  target_sources(
    fram_vendor_libjpeg_turbo
    PRIVATE libjpeg-turbo/simd/jsimd.c
            libjpeg-turbo/simd/arm/aarch64/jsimdcpu.c
            libjpeg-turbo/simd/arm/aarch64/jchuff-neon.c
            libjpeg-turbo/simd/arm/jccolor-neon.c
            libjpeg-turbo/simd/arm/jcgray-neon.c
            libjpeg-turbo/simd/arm/jcphuff-neon.c
            libjpeg-turbo/simd/arm/jcsample-neon.c
            libjpeg-turbo/simd/arm/jdcolor-neon.c
            libjpeg-turbo/simd/arm/jdmerge-neon.c
            libjpeg-turbo/simd/arm/jdsample-neon.c
            libjpeg-turbo/simd/arm/jfdctfst-neon.c
            libjpeg-turbo/simd/arm/jfdctint-neon.c
            libjpeg-turbo/simd/arm/jidctfst-neon.c
            libjpeg-turbo/simd/arm/jidctint-neon.c
            libjpeg-turbo/simd/arm/jidctred-neon.c
            libjpeg-turbo/simd/arm/jquanti-neon.c
  )
  target_compile_definitions(fram_vendor_libjpeg_turbo PRIVATE JPEG_SIMD_ARCHITECTURE=ARM64)
endif()

add_library(fram_vendor_mustache STATIC mustache4c/mustache.c)
target_include_directories(
  fram_vendor_mustache SYSTEM PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/mustache4c>"
)

add_library(fram_vendor_stb STATIC stb/stb.c)
target_include_directories(
  fram_vendor_stb SYSTEM PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/stb>"
)
# `stb.c` routes stb's allocator hooks to `image_alloc` in `src/formats/image.c`, so it includes that
# module's header, which includes sharedstuff's `str.h`.
target_include_directories(fram_vendor_stb PRIVATE "${PROJECT_SOURCE_DIR}/src")

add_library(
  fram_vendor_sharedstuff STATIC sharedstuff/src/arena.c sharedstuff/src/buf.c
                                  sharedstuff/src/log.c sharedstuff/src/str.c
)
target_include_directories(
  fram_vendor_sharedstuff SYSTEM
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/sharedstuff/include>"
)
target_link_libraries(fram_vendor_sharedstuff PRIVATE fram_build_project)
target_link_libraries(fram_vendor_stb PRIVATE fram_vendor_sharedstuff)

add_library(fram_vendor_tomlc17 STATIC tomlc17/tomlc17.c)
target_include_directories(
  fram_vendor_tomlc17 SYSTEM PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/tomlc17>"
)

foreach(target IN ITEMS fram_vendor_libjpeg_turbo fram_vendor_libwebp fram_vendor_mustache
                        fram_vendor_stb fram_vendor_tomlc17
)
  target_compile_options(${target} PRIVATE $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-w>)
  target_link_libraries(${target} PRIVATE fram_build_vendor)
endforeach()

# stb_image_write's JPEG bit buffer deliberately shifts through the sign bit. UBSan diagnoses the
# shift even though the result is valid. Keep the exception on this pinned vendor translation unit
# only. All first-party callers remain fully instrumented.
if(FRAM_SANITIZER STREQUAL "address")
  set_property(
    SOURCE stb/stb.c
    APPEND
    PROPERTY COMPILE_OPTIONS $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-fno-sanitize=shift>
  )
endif()
