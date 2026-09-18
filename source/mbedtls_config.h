#ifndef WIIMC_MBEDTLS_CONFIG_H
#define WIIMC_MBEDTLS_CONFIG_H
/* Mbed TLS 3.6: TLS 1.2 client, ECDHE, verified PEM certificate chains.
 * Entropy is supplied by gc_tls.c from a provisioned, rotating SD seed.
 *
 * Deliberately do not define MBEDTLS_HAVE_TIME_DATE.  A GameCube whose RTC
 * battery is flat cannot decide whether a certificate is current.  This is
 * also the effective GCRadio configuration: signatures, CA chains and host
 * names are verified, but certificate validity dates are not. */
#define MBEDTLS_HAVE_TIME
#define MBEDTLS_PLATFORM_MS_TIME_ALT
#define MBEDTLS_NO_UDBL_DIVISION
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_SSL_SERVER_NAME_INDICATION
#define MBEDTLS_SSL_EXTENDED_MASTER_SECRET
#define MBEDTLS_SSL_ENCRYPT_THEN_MAC
#define MBEDTLS_SSL_IN_CONTENT_LEN 16384
#define MBEDTLS_SSL_OUT_CONTENT_LEN 4096
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED
#define MBEDTLS_ECP_C
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED
#define MBEDTLS_ECP_DP_CURVE25519_ENABLED
#define MBEDTLS_ECP_NIST_OPTIM
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_PKCS1_V21
#define MBEDTLS_CIPHER_C
#define MBEDTLS_AES_C
#define MBEDTLS_GCM_C
#define MBEDTLS_CHACHA20_C
#define MBEDTLS_POLY1305_C
#define MBEDTLS_CHACHAPOLY_C
#define MBEDTLS_MD_C
#define MBEDTLS_SHA256_C
/* Unlike GCRadio's mbedTLS 2.x, 3.6 requires SHA-384 to be enabled separately
 * from SHA-512. Public radio certificate chains use SHA-384 signatures. */
#define MBEDTLS_SHA384_C
#define MBEDTLS_SHA512_C
#define MBEDTLS_SHA1_C
#define MBEDTLS_CTR_DRBG_C
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_OID_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_BASE64_C
#define MBEDTLS_PEM_PARSE_C
#define MBEDTLS_ERROR_C
#endif
