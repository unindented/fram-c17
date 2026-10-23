/* The build's equivalent of the jconfigint.h that libjpeg-turbo 3.2.0 generates
 * from src/jconfigint.h.in. Values that depend on the target come from the
 * compiler or from the build, so one file serves every supported target. */

/* libjpeg-turbo build number */
#define BUILD  "20260630"

/* How to hide global symbols. */
#define HIDDEN  __attribute__((visibility("hidden")))

/* Compiler's inline keyword */
#undef inline

/* How to obtain function inlining. */
#define INLINE  __inline__ __attribute__((always_inline))

/* How to obtain thread-local storage */
#define THREAD_LOCAL  _Thread_local

/* Define to the full name of this package. */
#define PACKAGE_NAME  "libjpeg-turbo"

/* Version number of package */
#define VERSION  "3.2.0"

/* The size of `size_t', as computed by sizeof. */
#define SIZEOF_SIZE_T  __SIZEOF_SIZE_T__

/* Define if your compiler has __builtin_ctzl() and sizeof(unsigned long) == sizeof(size_t). */
#if __SIZEOF_LONG__ == __SIZEOF_SIZE_T__
#define HAVE_BUILTIN_CTZL
#endif

/* Define to 1 if you have the <intrin.h> header file. */
/* #undef HAVE_INTRIN_H */

#if defined(_MSC_VER) && defined(HAVE_INTRIN_H)
#if (SIZEOF_SIZE_T == 8)
#define HAVE_BITSCANFORWARD64
#elif (SIZEOF_SIZE_T == 4)
#define HAVE_BITSCANFORWARD
#endif
#endif

#if defined(__has_attribute)
#if __has_attribute(fallthrough)
#define FALLTHROUGH  __attribute__((fallthrough));
#else
#define FALLTHROUGH
#endif
#else
#define FALLTHROUGH
#endif

/*
 * Define BITS_IN_JSAMPLE as either
 *   8   for 8-bit sample values (the usual setting)
 *   12  for 12-bit sample values
 * Only 8 and 12 are legal data precisions for lossy JPEG according to the
 * JPEG standard, and the IJG code does not support anything else!
 */

#ifndef BITS_IN_JSAMPLE
#define BITS_IN_JSAMPLE  8      /* use 8 or 12 */
#endif

#undef C_ARITH_CODING_SUPPORTED
#undef D_ARITH_CODING_SUPPORTED
#undef WITH_SIMD

#if BITS_IN_JSAMPLE == 8

/* Support arithmetic encoding */
#define C_ARITH_CODING_SUPPORTED 1

/* Support arithmetic decoding */
#define D_ARITH_CODING_SUPPORTED 1

/* Use accelerated SIMD routines. The build defines JPEG_SIMD_ARCHITECTURE as
 * X86_64 or ARM64 when it compiles that architecture's SIMD sources, and leaves
 * it undefined on any other target, which uses the C routines. */
#ifdef JPEG_SIMD_ARCHITECTURE
#define WITH_SIMD 1
#define SIMD_ARCHITECTURE  JPEG_SIMD_ARCHITECTURE
#endif

#endif

/* #undef WITH_PROFILE */

/* Read no environment variables, so neither JPEGMEM nor a JSIMD_FORCE* setting
 * changes how the library decodes or how much memory it uses. Only the
 * libjpeg-turbo programs set one, so the library needs no setenv() either. */
#define NO_GETENV
#define NO_PUTENV
