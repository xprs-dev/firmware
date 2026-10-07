# mbedtls_ecp

mbedtls 3.5.2, the six source files that make `common/xprs_sig` work --
`bignum.c`, `bignum_core.c`, `ecp.c`, `ecp_curves.c`, `constant_time.c`,
`platform_util.c` -- with the headers they need, copied verbatim from the
ESP-IDF's `components/mbedtls/mbedtls` (Apache-2.0, `LICENSE`). The ESP32
boards use the IDF's own copy; this one exists because the nRF52840 build has
no IDF and the Adafruit core ships no mbedtls.

`include/xprs_mbedtls_config.h` is the whole configuration: bignum and ECP
over secp256k1, nothing else. It is what `xprssig.c`'s device branch links
against here, so the signature maths is one file on every board.
