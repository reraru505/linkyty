#ifndef KYTY_COMMON_CONFIG_H_
#define KYTY_COMMON_CONFIG_H_

#define KYTY_COMPILER_GCC     1
#define KYTY_COMPILER_CLANG   2
#if defined(__clang__)
#define KYTY_COMPILER         KYTY_COMPILER_CLANG
#else
#define KYTY_COMPILER         KYTY_COMPILER_GCC
#endif

#define KYTY_LINKER_LD        1
#define KYTY_LINKER_LLD       2
#define KYTY_LINKER_LLD_LINK  3
#define KYTY_LINKER           KYTY_LINKER_LLD

#define KYTY_BUILD_DEBUG      1
#define KYTY_BUILD_RELEASE    2
#if defined(NDEBUG)
#define KYTY_BUILD            KYTY_BUILD_RELEASE
#else
#define KYTY_BUILD            KYTY_BUILD_DEBUG
#endif

#define KYTY_ENDIAN_BIG       1
#define KYTY_ENDIAN_LITTLE    2
#ifndef KYTY_ENDIAN
#define KYTY_ENDIAN           KYTY_ENDIAN_LITTLE
#endif

#define KYTY_VERSION          "0.3.0"
#define KYTY_BUILD_REPOSITORY ""
#define KYTY_RELEASE_TAG      ""

#endif /* KYTY_COMMON_CONFIG_H_ */
