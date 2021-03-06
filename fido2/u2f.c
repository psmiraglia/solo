/*
 * Copyright (C) 2018 SoloKeys, Inc. <https://solokeys.com/>
 *
 * This file is part of Solo.
 *
 * Solo is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Solo is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Solo.  If not, see <https://www.gnu.org/licenses/>
 *
 * This code is available under licenses for commercial use.
 * Please contact SoloKeys for more information.
 */
#include <stdlib.h>
#include "u2f.h"
#include "ctap.h"
#include "crypto.h"
#include "log.h"
#include "device.h"
#include "wallet.h"
#include APP_CONFIG

// void u2f_response_writeback(uint8_t * buf, uint8_t len);
static int16_t u2f_register(struct u2f_register_request * req);
static int16_t u2f_authenticate(struct u2f_authenticate_request * req, uint8_t control);
int8_t u2f_response_writeback(const uint8_t * buf, uint16_t len);
void u2f_reset_response();


static CTAP_RESPONSE * _u2f_resp = NULL;

void u2f_request(struct u2f_request_apdu* req, CTAP_RESPONSE * resp)
{
    uint16_t rcode = 0;
    uint64_t t1,t2;
    uint32_t len = ((req->LC3) | ((uint32_t)req->LC2 << 8) | ((uint32_t)req->LC1 << 16));
    uint8_t byte;

    u2f_set_writeback_buffer(resp);

    if (req->cla != 0)
    {
        printf1(TAG_U2F, "CLA not zero\n");
        rcode = U2F_SW_CLASS_NOT_SUPPORTED;
        goto end;
    }
#ifdef ENABLE_U2F_EXTENSIONS
    rcode = extend_u2f(req, len);
#endif
    if (rcode != U2F_SW_NO_ERROR)       // If the extension didn't do anything...
    {
#ifdef ENABLE_U2F
        switch(req->ins)
        {
            case U2F_REGISTER:
                printf1(TAG_U2F, "U2F_REGISTER\n");
                if (len != 64)
                {
                    rcode = U2F_SW_WRONG_LENGTH;
                }
                else
                {
                    t1 = millis();
                    rcode = u2f_register((struct u2f_register_request*)req->payload);
                    t2 = millis();
                    printf1(TAG_TIME,"u2f_register time: %d ms\n", t2-t1);
                }
                break;
            case U2F_AUTHENTICATE:
                printf1(TAG_U2F, "U2F_AUTHENTICATE\n");
                t1 = millis();
                rcode = u2f_authenticate((struct u2f_authenticate_request*)req->payload, req->p1);
                t2 = millis();
                printf1(TAG_TIME,"u2f_authenticate time: %d ms\n", t2-t1);
                break;
            case U2F_VERSION:
                printf1(TAG_U2F, "U2F_VERSION\n");
                if (len)
                {
                    rcode = U2F_SW_WRONG_LENGTH;
                }
                else
                {
                    rcode = u2f_version();
                }
                break;
            case U2F_VENDOR_FIRST:
            case U2F_VENDOR_LAST:
                printf1(TAG_U2F, "U2F_VENDOR\n");
                rcode = U2F_SW_NO_ERROR;
                break;
            default:
                printf1(TAG_ERR, "Error, unknown U2F command\n");
                rcode = U2F_SW_INS_NOT_SUPPORTED;
                break;
        }
#endif
    }

end:
    if (rcode != U2F_SW_NO_ERROR)
    {
        printf1(TAG_U2F,"U2F Error code %04x\n", rcode);
        ctap_response_init(_u2f_resp);
    }

    byte = (rcode & 0xff00)>>8;
    u2f_response_writeback(&byte,1);
    byte = rcode & 0xff;
    u2f_response_writeback(&byte,1);

    printf1(TAG_U2F,"u2f resp: "); dump_hex1(TAG_U2F, _u2f_resp->data, _u2f_resp->length);
}


int8_t u2f_response_writeback(const uint8_t * buf, uint16_t len)
{
    if ((_u2f_resp->length + len) > _u2f_resp->data_size)
    {
        printf2(TAG_ERR, "Not enough space for U2F response, writeback\n");
        exit(1);
    }
    memmove(_u2f_resp->data + _u2f_resp->length, buf, len);
    _u2f_resp->length += len;
    return 0;
}

void u2f_reset_response()
{
    ctap_response_init(_u2f_resp);
}

void u2f_set_writeback_buffer(CTAP_RESPONSE * resp)
{
    _u2f_resp = resp;
}

static void dump_signature_der(uint8_t * sig)
{
    uint8_t sigder[72];
    int len;
    len = ctap_encode_der_sig(sig, sigder);
    u2f_response_writeback(sigder, len);
}
static int8_t u2f_load_key(struct u2f_key_handle * kh, uint8_t * appid)
{
    crypto_ecc256_load_key((uint8_t*)kh, U2F_KEY_HANDLE_SIZE, NULL, 0);
    return 0;
}

static void u2f_make_auth_tag(struct u2f_key_handle * kh, uint8_t * appid, uint8_t * tag)
{
    uint8_t hashbuf[32];
    crypto_sha256_hmac_init(CRYPTO_MASTER_KEY, 0, hashbuf);
    crypto_sha256_update(kh->key, U2F_KEY_HANDLE_KEY_SIZE);
    crypto_sha256_update(appid, U2F_APPLICATION_SIZE);
    crypto_sha256_hmac_final(CRYPTO_MASTER_KEY, 0,hashbuf);
    memmove(tag, hashbuf, CREDENTIAL_TAG_SIZE);
}

static int8_t u2f_new_keypair(struct u2f_key_handle * kh, uint8_t * appid, uint8_t * pubkey)
{
    ctap_generate_rng(kh->key, U2F_KEY_HANDLE_KEY_SIZE);
    u2f_make_auth_tag(kh, appid, kh->tag);

    crypto_ecc256_derive_public_key((uint8_t*)kh, U2F_KEY_HANDLE_SIZE, pubkey, pubkey+32);
    return 0;
}



static int8_t u2f_appid_eq(struct u2f_key_handle * kh, uint8_t * appid)
{
    uint8_t tag[U2F_KEY_HANDLE_TAG_SIZE];
    u2f_make_auth_tag(kh, appid, tag);
    if (memcmp(kh->tag, tag, U2F_KEY_HANDLE_TAG_SIZE) == 0)
    {
        return 0;
    }
    else
    {
        printf1(TAG_U2F, "key handle + appid not authentic\n");
        printf1(TAG_U2F, "calc tag: \n"); dump_hex1(TAG_U2F,tag, U2F_KEY_HANDLE_TAG_SIZE);
        printf1(TAG_U2F, "inp  tag: \n"); dump_hex1(TAG_U2F,kh->tag, U2F_KEY_HANDLE_TAG_SIZE);
        return -1;
    }
}



static int16_t u2f_authenticate(struct u2f_authenticate_request * req, uint8_t control)
{

    uint8_t up = 1;
    uint32_t count;
    uint8_t hash[32];
    uint8_t * sig = (uint8_t*)req;

    if (control == U2F_AUTHENTICATE_CHECK)
    {
        if (u2f_appid_eq(&req->kh, req->app) == 0)
        {
            return U2F_SW_CONDITIONS_NOT_SATISFIED;
        }
        else
        {
            return U2F_SW_WRONG_DATA;
        }
    }
    if (
            control != U2F_AUTHENTICATE_SIGN ||
            req->khl != U2F_KEY_HANDLE_SIZE  ||
            u2f_appid_eq(&req->kh, req->app) != 0 ||     // Order of checks is important
            u2f_load_key(&req->kh, req->app) != 0

        )
    {
        return U2F_SW_WRONG_PAYLOAD;
    }



    if (ctap_user_presence_test() == 0)
    {
        return U2F_SW_CONDITIONS_NOT_SATISFIED;
    }

    count = ctap_atomic_count(0);
    hash[0] = (count >> 24) & 0xff;
    hash[1] = (count >> 16) & 0xff;
    hash[2] = (count >> 8) & 0xff;
    hash[3] = (count >> 0) & 0xff;
    crypto_sha256_init();

    crypto_sha256_update(req->app,32);
    crypto_sha256_update(&up,1);
    crypto_sha256_update(hash,4);
    crypto_sha256_update(req->chal,32);

    crypto_sha256_final(hash);

    printf1(TAG_U2F, "sha256: "); dump_hex1(TAG_U2F,hash,32);
    crypto_ecc256_sign(hash, 32, sig);

    u2f_response_writeback(&up,1);
    hash[0] = (count >> 24) & 0xff;
    hash[1] = (count >> 16) & 0xff;
    hash[2] = (count >> 8) & 0xff;
    hash[3] = (count >> 0) & 0xff;
    u2f_response_writeback(hash,4);
    dump_signature_der(sig);

    return U2F_SW_NO_ERROR;
}

static int16_t u2f_register(struct u2f_register_request * req)
{
    uint8_t i[] = {0x0,U2F_EC_FMT_UNCOMPRESSED};

    struct u2f_key_handle key_handle;
    uint8_t pubkey[64];
    uint8_t hash[32];
    uint8_t * sig = (uint8_t*)req;


    const uint16_t attest_size = attestation_cert_der_size;

    if ( ! ctap_user_presence_test())
    {
        return U2F_SW_CONDITIONS_NOT_SATISFIED;
    }

    if ( u2f_new_keypair(&key_handle, req->app, pubkey) == -1)
    {
        return U2F_SW_INSUFFICIENT_MEMORY;
    }

    crypto_sha256_init();
    crypto_sha256_update(i,1);
    crypto_sha256_update(req->app,32);

    crypto_sha256_update(req->chal,32);

    crypto_sha256_update((uint8_t*)&key_handle,U2F_KEY_HANDLE_SIZE);
    crypto_sha256_update(i+1,1);
    crypto_sha256_update(pubkey,64);
    crypto_sha256_final(hash);

    crypto_ecc256_load_attestation_key();

    printf1(TAG_U2F, "sha256: "); dump_hex1(TAG_U2F,hash,32);
    crypto_ecc256_sign(hash, 32, sig);

    i[0] = 0x5;
    u2f_response_writeback(i,2);
    u2f_response_writeback(pubkey,64);
    i[0] = U2F_KEY_HANDLE_SIZE;
    u2f_response_writeback(i,1);
    u2f_response_writeback((uint8_t*)&key_handle,U2F_KEY_HANDLE_SIZE);

    u2f_response_writeback(attestation_cert_der,attest_size);

    dump_signature_der(sig);

    /*printf1(TAG_U2F, "dersig: "); dump_hex1(TAG_U2F,sig,74);*/


    return U2F_SW_NO_ERROR;
}

int16_t u2f_version()
{
    const char version[] = "U2F_V2";
    u2f_response_writeback((uint8_t*)version, sizeof(version)-1);
    return U2F_SW_NO_ERROR;
}
