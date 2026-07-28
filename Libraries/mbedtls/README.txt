mbedtls V2.28 LTS - download from https://github.com/Mbed-TLS/mbedtls/releases/tag/v2.28.9
Copy: include/mbedtls/*.h -> bootloader/Libraries/mbedtls/include/mbedtls/
Copy: library/*.c -> bootloader/Libraries/mbedtls/library/
Required files: aes.c, sha256.c, md.c, platform.c, platform_util.c
Minimal config: define MBEDTLS_AES_C, MBEDTLS_CIPHER_MODE_CTR, MBEDTLS_SHA256_C, MBEDTLS_MD_C
