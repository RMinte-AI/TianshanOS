#include "ts_config_pack_trust.h"
#include "ts_crypto.h"
#include "ts_core.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/oid.h"
#include <string.h>
#include <time.h>

#ifndef CONFIG_TS_CONFIG_PACK_SIGNING_ROOT_SHA256
#define CONFIG_TS_CONFIG_PACK_SIGNING_ROOT_SHA256 ""
#endif
#define ACCEPT_POLICY 1

static bool cert_hash(const mbedtls_x509_crt *crt, char out[65]) {
    unsigned char digest[32];
    return ts_crypto_hash(TS_HASH_SHA256, crt->raw.p, crt->raw.len, digest, sizeof digest) == ESP_OK &&
           ts_crypto_hex_encode(digest, sizeof digest, out, 65) == ESP_OK;
}
static bool developer(const mbedtls_x509_crt *crt) {
    unsigned count = 0;
    for (const mbedtls_x509_name *n = &crt->subject; n; n = n->next) {
        if (MBEDTLS_OID_CMP(MBEDTLS_OID_AT_ORG_UNIT, &n->oid) == 0) {
            if (n->val.len != 9 || memcmp(n->val.p, "Developer", 9)) return false;
            ++count;
        }
    }
    return count == 1;
}
/* A local accepted record permits natural expiry/offline reboot, not a new trust root
 * or different package. All non-time certificate validation failures remain fatal. */
static int accepted_time(void *context, mbedtls_x509_crt *crt, int depth, uint32_t *flags) {
    (void)crt; (void)depth;
    if (*(bool *)context)
        *flags &= ~(MBEDTLS_X509_BADCERT_EXPIRED | MBEDTLS_X509_BADCERT_FUTURE);
    return 0;
}
ts_config_pack_result_t ts_config_pack_signer_trust(const char *signer,
    const ts_cert_snapshot_t *snapshot, const char *package_hash, const char *recipient,
    const ts_config_pack_acceptance_t *accepted, ts_config_pack_acceptance_t *out) {
    if (strlen(CONFIG_TS_CONFIG_PACK_SIGNING_ROOT_SHA256) != 64)
        return TS_CONFIG_PACK_ERR_TRUST_NOT_CONFIGURED;
    struct { mbedtls_x509_crt leaf, ca, root; } *certs = TS_CALLOC_PSRAM(1, sizeof(*certs));
    if (!certs) return TS_CONFIG_PACK_ERR_NO_MEM;
    mbedtls_x509_crt_init(&certs->leaf); mbedtls_x509_crt_init(&certs->ca);
    mbedtls_x509_crt_init(&certs->root);
    ts_config_pack_result_t result = TS_CONFIG_PACK_ERR_CERT_CHAIN;
    if (!snapshot->ca || mbedtls_x509_crt_parse(&certs->leaf, (const unsigned char *)signer,
            strlen(signer) + 1) != 0 ||
        mbedtls_x509_crt_parse(&certs->ca, (const unsigned char *)snapshot->ca,
            strlen(snapshot->ca) + 1) != 0) goto done;
    char signer_hash[65], root_hash[65];
    if (!cert_hash(&certs->leaf, signer_hash)) goto done;
    bool found = false;
    for (mbedtls_x509_crt *c = &certs->ca; c; c = c->next) {
        char hash[65];
        if (!cert_hash(c, hash)) goto done;
        if (!strcmp(hash, CONFIG_TS_CONFIG_PACK_SIGNING_ROOT_SHA256)) {
            if (!c->MBEDTLS_PRIVATE(ca_istrue) || mbedtls_x509_crt_parse_der(&certs->root, c->raw.p, c->raw.len)) goto done;
            strcpy(root_hash, hash); found = true;
        }
    }
    if (!found) goto done;
    /* Installed intermediates may complete the chain, but only the explicitly
     * selected certificate is passed as a trust anchor. */
    for (mbedtls_x509_crt *c = &certs->ca; c; c = c->next) {
        char hash[65];
        if (!cert_hash(c, hash)) goto done;
        if (strcmp(hash, root_hash) && mbedtls_x509_crt_parse_der(&certs->leaf, c->raw.p, c->raw.len)) goto done;
    }
    bool reload = accepted && accepted->policy == ACCEPT_POLICY && accepted->accepted_at > 0 &&
        !strcmp(accepted->package_sha256, package_hash) && !strcmp(accepted->recipient, recipient) &&
        !strcmp(accepted->signer_sha256, signer_hash) && !strcmp(accepted->root_sha256, root_hash);
    if (accepted && !reload) goto done;
    ts_cert_pki_status_t status = {0};
    if (!reload && (ts_cert_get_status(&status) != ESP_OK || !status.time_ready)) {
        result = TS_CONFIG_PACK_ERR_TIME_UNVERIFIED; goto done;
    }
    uint32_t flags = 0;
    if (mbedtls_x509_crt_verify(&certs->leaf, &certs->root, NULL, NULL, &flags,
                              accepted_time, &reload) != 0 || flags) goto done;
    result = TS_CONFIG_PACK_ERR_SIGNER_ROLE;
    if(!mbedtls_pk_can_do(&certs->leaf.pk,MBEDTLS_PK_ECDSA) ||
       mbedtls_pk_ec(certs->leaf.pk)->MBEDTLS_PRIVATE(grp).id!=MBEDTLS_ECP_DP_SECP256R1)goto done;
    if (!developer(&certs->leaf) ||
        mbedtls_x509_crt_check_key_usage(&certs->leaf, MBEDTLS_X509_KU_DIGITAL_SIGNATURE)) goto done;
    /* No invented mandatory EKU. If a signer carries EKU restrictions, it must
     * permit code signing or any usage; a TLS-only certificate is not a signing credential. */
    if (certs->leaf.MBEDTLS_PRIVATE(ext_types) & MBEDTLS_X509_EXT_EXTENDED_KEY_USAGE) {
        bool allowed = false;
        for (const mbedtls_x509_sequence *s = &certs->leaf.ext_key_usage; s; s = s->next)
            allowed |= (s->buf.len == MBEDTLS_OID_SIZE(MBEDTLS_OID_CODE_SIGNING) &&
                        !memcmp(s->buf.p, MBEDTLS_OID_CODE_SIGNING, s->buf.len)) ||
                       (s->buf.len == MBEDTLS_OID_SIZE(MBEDTLS_OID_ANY_EXTENDED_KEY_USAGE) &&
                        !memcmp(s->buf.p, MBEDTLS_OID_ANY_EXTENDED_KEY_USAGE, s->buf.len));
        if (!allowed) goto done;
    }
    if (out) {
        memset(out, 0, sizeof(*out)); out->policy = ACCEPT_POLICY;
        out->accepted_at = reload ? accepted->accepted_at : (int64_t)time(NULL);
        strcpy(out->package_sha256, package_hash); strcpy(out->recipient, recipient);
        strcpy(out->signer_sha256, signer_hash); strcpy(out->root_sha256, root_hash);
    }
    result = TS_CONFIG_PACK_OK;
done:
    mbedtls_x509_crt_free(&certs->leaf); mbedtls_x509_crt_free(&certs->ca);
    mbedtls_x509_crt_free(&certs->root); free(certs);
    return result;
}
