/* Copyright (C) 2014 Stony Brook University

   This file is part of Graphene Library OS.

   Graphene Library OS is free software: you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public License
   as published by the Free Software Foundation, either version 3 of the
   License, or (at your option) any later version.

   Graphene Library OS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

/*
 * Cryptographic primitive abstractions. This layer provides a way to
 * change the crypto library without changing the rest of Graphene code
 * by providing a small crypto library adaptor implementing these methods.
 */

#ifndef PAL_CRYPTO_H
#define PAL_CRYPTO_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>

#define SHA256_DIGEST_LEN 32

#ifdef CRYPTO_USE_MBEDTLS
#define CRYPTO_PROVIDER_SPECIFIED

#include "mbedtls/cmac.h"
typedef struct AES LIB_AES_CONTEXT;

#include "mbedtls/dhm.h"
#include "mbedtls/rsa.h"
#include "mbedtls/sha256.h"

typedef mbedtls_sha256_context LIB_SHA256_CONTEXT;

/* DH_SIZE is tied to the choice of parameters in mbedtls_dh.c. */
#define DH_SIZE 256
#include "mbedtls/dhm.h"
typedef mbedtls_dhm_context LIB_DH_CONTEXT;
typedef mbedtls_rsa_context LIB_RSA_KEY;
typedef struct {
    mbedtls_cipher_type_t cipher;
    mbedtls_cipher_context_t ctx;
} LIB_AESCMAC_CONTEXT;

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ssl.h"
typedef struct {
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_ssl_config conf;
    mbedtls_ssl_context ssl;
    int ciphersuites[2];  /* [0] is actual ciphersuite, [1] must be 0 to indicate end of array */
    ssize_t (*pal_recv_cb)(int fd, void* buf, size_t len);
    ssize_t (*pal_send_cb)(int fd, const void* buf, size_t len);
    int stream_fd;
} LIB_SSL_CONTEXT;

#endif /* CRYPTO_USE_MBEDTLS */

#ifndef CRYPTO_PROVIDER_SPECIFIED
# error "Unknown crypto provider. Set CRYPTO_PROVIDER in Makefile"
#endif

/* SHA256 */
int lib_SHA256Init(LIB_SHA256_CONTEXT *context);
int lib_SHA256Update(LIB_SHA256_CONTEXT *context, const uint8_t *data,
		     uint64_t len);
int lib_SHA256Final(LIB_SHA256_CONTEXT *context, uint8_t *output);

/* Diffie-Hellman Key Exchange */
int lib_DhInit(LIB_DH_CONTEXT *context);
int lib_DhCreatePublic(LIB_DH_CONTEXT *context, uint8_t *public,
                       uint64_t *public_size);
int lib_DhCalcSecret(LIB_DH_CONTEXT *context, uint8_t *peer, uint64_t peer_size,
                     uint8_t *secret, uint64_t *secret_size);
void lib_DhFinal(LIB_DH_CONTEXT *context);

/* AES-CMAC */
int lib_AESCMAC(const uint8_t *key, uint64_t key_len, const uint8_t *input,
                uint64_t input_len, uint8_t *mac, uint64_t mac_len);

/* note: 'lib_AESCMAC' is the combination of 'lib_AESCMACInit',
 * 'lib_AESCMACUpdate', and 'lib_AESCMACFinish'. */
int lib_AESCMACInit(LIB_AESCMAC_CONTEXT * context,
                    const uint8_t *key, uint64_t key_len);
int lib_AESCMACUpdate(LIB_AESCMAC_CONTEXT * context, const uint8_t * input,
                      uint64_t input_len);
int lib_AESCMACFinish(LIB_AESCMAC_CONTEXT * context, uint8_t * mac,
                      uint64_t mac_len);

/* RSA. Limited functionality. */
// Initializes the key structure
int lib_RSAInitKey(LIB_RSA_KEY *key);
// Must call lib_RSAInitKey first
int lib_RSAGenerateKey(LIB_RSA_KEY *key, uint64_t length_in_bits,
                       uint64_t exponent);

int lib_RSAExportPublicKey(LIB_RSA_KEY *key, uint8_t *e, uint64_t *e_size,
                           uint8_t *n, uint64_t *n_size);

int lib_RSAImportPublicKey(LIB_RSA_KEY *key, const uint8_t *e, uint64_t e_size,
                           const uint8_t *n, uint64_t n_size);

// Sign and verify signatures.

// This function must implement RSA signature verification using PKCS#1 v1.5
// padding, with SHA256 as the hash mechanism. These signatures are generated
// by the Graphene filesystem build (so outside of a running Graphene
// application), but are verified within the Graphene application.
int lib_RSAVerifySHA256(LIB_RSA_KEY* key, const uint8_t* hash, uint64_t hash_len,
                        const uint8_t* signature, uint64_t signature_len);

// Frees memory allocated in lib_RSAInitKey.
int lib_RSAFreeKey(LIB_RSA_KEY *key);

// Encode and decode Base64 messages.
// These two functions can be used to query encode and decode sizes if dst is given NULL
int lib_Base64Encode(const uint8_t* src, size_t slen, char* dst, size_t* dlen);
int lib_Base64Decode(const char *src, size_t slen, uint8_t* dst, size_t* dlen);

/* SSL/TLS */
int lib_SSLInit(LIB_SSL_CONTEXT* ssl_ctx, int stream_fd, bool is_server,
                const uint8_t* psk, size_t psk_size,
                ssize_t (*pal_recv_cb)(int fd, void* buf, size_t len),
                ssize_t (*pal_send_cb)(int fd, const void* buf, size_t len),
                const uint8_t* buf_load_ssl_ctx, size_t buf_size);
int lib_SSLFree(LIB_SSL_CONTEXT* ssl_ctx);
int lib_SSLRead(LIB_SSL_CONTEXT* ssl_ctx, uint8_t* buf, size_t len);
int lib_SSLWrite(LIB_SSL_CONTEXT* ssl_ctx, const uint8_t* buf, size_t len);
int lib_SSLSave(LIB_SSL_CONTEXT* ssl_ctx, uint8_t* buf, size_t len, size_t* olen);
#endif
