/* mbedtls, cut down to what an XPRS signature needs on a chip without an
 * IDF: bignum + ECP over secp256k1. Nothing else. See ../README.md. */
#ifndef XPRS_MBEDTLS_CONFIG_H
#define XPRS_MBEDTLS_CONFIG_H
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_ECP_C
#define MBEDTLS_ECP_DP_SECP256K1_ENABLED
/* Smaller precomputation tables: RAM is 256 KB and a signature a second is
 * plenty. Window 2 keeps the muladd's table at two points. */
#define MBEDTLS_ECP_WINDOW_SIZE 2
#define MBEDTLS_ECP_FIXED_POINT_OPTIM 0
#define MBEDTLS_ECP_MAX_BITS 256
#define MBEDTLS_MPI_MAX_SIZE 64
#define MBEDTLS_MPI_WINDOW_SIZE 2
#define MBEDTLS_NO_PLATFORM_ENTROPY
#include "mbedtls/check_config.h"
#endif
