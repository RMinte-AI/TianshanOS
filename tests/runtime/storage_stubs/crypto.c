/* Native SHA-256 boundary for the configuration/legacy fixtures. */
#include "ts_crypto.h"
#include <assert.h>
#include <stdio.h>
#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#else
#include <openssl/sha.h>
#endif
esp_err_t ts_crypto_hash(ts_hash_algo_t algo,const void *data,size_t size,void *out,size_t capacity)
{
    assert(algo==TS_HASH_SHA256 && capacity==32);
#ifdef __APPLE__
    CC_SHA256(data,(CC_LONG)size,out);
#else
    SHA256(data,size,out);
#endif
    return ESP_OK;
}
esp_err_t ts_crypto_hex_encode(const void *bytes,size_t length,char *out,size_t capacity)
{
    assert(capacity>=length*2+1);
    for(size_t i=0;i<length;++i)sprintf(out+i*2,"%02x",((const unsigned char*)bytes)[i]);
    return ESP_OK;
}
