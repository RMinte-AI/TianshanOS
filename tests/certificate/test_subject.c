#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "ts_cert_subject.h"
#include "mbedtls/x509.h"
#include "mbedtls/oid.h"
#include "mbedtls/asn1write.h"
#include "mbedtls/x509_csr.h"
#include "mbedtls/pk.h"

static mbedtls_pk_context test_key;
/* Synthetic test key only; never used by the device. */
static int test_rng(void *ctx, unsigned char *out, size_t len) {
    (void)ctx;
    for (size_t i = 0; i < len; i++) out[i] = (unsigned char)rand();
    return 0;
}

static void check_names(const mbedtls_x509_name *names, const char *cn, const char *org, const char *ou) {
    unsigned found = 0;
    for (const mbedtls_x509_name *n = names; n; n = n->next) {
        const char *name; assert(mbedtls_oid_get_attr_short_name(&n->oid, &name) == 0);
        const char *expected = !strcmp(name, "CN") ? cn : !strcmp(name, "O") ? org : ou;
        assert(expected && strlen(expected) == n->val.len);
        assert(!memcmp(expected, n->val.p, n->val.len)); found++;
    }
    assert(found == 1 + !!(org && *org) + !!(ou && *ou));
}

static void check(const char *cn, const char *org, const char *ou) {
    char subject[TS_CERT_SUBJECT_CAPACITY];
    assert(ts_cert_build_subject(cn, org, ou, subject, sizeof(subject)) == ESP_OK);
    mbedtls_asn1_named_data *names = NULL;
    assert(mbedtls_x509_string_to_names(&names, subject) == 0);
    check_names(names, cn, org, ou);
    mbedtls_asn1_free_named_data_list(&names);
    mbedtls_x509write_csr writer; mbedtls_x509write_csr_init(&writer);
    mbedtls_x509write_csr_set_key(&writer, &test_key);
    mbedtls_x509write_csr_set_md_alg(&writer, MBEDTLS_MD_SHA256);
    assert(mbedtls_x509write_csr_set_subject_name(&writer, subject) == 0);
    unsigned char pem[2048];
    assert(mbedtls_x509write_csr_pem(&writer, pem, sizeof(pem), test_rng, NULL) == 0);
    mbedtls_x509_csr csr; mbedtls_x509_csr_init(&csr);
    assert(mbedtls_x509_csr_parse(&csr, pem, strlen((char *)pem)+1) == 0);
    check_names(&csr.subject, cn, org, ou);
    mbedtls_x509_csr_free(&csr); mbedtls_x509write_csr_free(&writer);
}
int main(void) {
    mbedtls_pk_init(&test_key);
    assert(mbedtls_pk_setup(&test_key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) == 0);
    assert(mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(test_key), test_rng, NULL) == 0);
    check("TIANSHAN-DEVICE-001", "TianShanOS", "Device");
    check("device", "ACME, Ltd", "Device");
    check("reviewer's \"device\"", " leading #,&+\\tail\\", "部门");
    check("#dev", " spaced ", "CN=x,O=wrong");
    check("device", NULL, NULL);
    check("中文设备", "comma, and;equals=plus+less<greater>", "line\nbreak");
    char id[271]; memset(id, 'a', 270); id[270] = 0;
    char out[256]; assert(ts_cert_build_subject(id, "Org", "Device", out, sizeof(out)) == ESP_ERR_INVALID_SIZE);
    assert(out[0] == 0); id[63] = 0; check(id, NULL, NULL);
    id[64] = 0; id[63] = 'a'; assert(ts_cert_build_subject(id, NULL, NULL, out, sizeof(out)) == ESP_ERR_INVALID_SIZE);
    char org[400]; memset(org, 'x', sizeof(org)-1); org[399] = 0;
    assert(ts_cert_build_subject("device", org, "unit", out, sizeof(out)) == ESP_ERR_INVALID_SIZE);
    assert(ts_cert_build_subject("device", NULL, NULL, out, 4) == ESP_ERR_INVALID_SIZE);
    assert(ts_cert_build_subject("", NULL, NULL, out, sizeof(out)) == ESP_ERR_INVALID_ARG);
    mbedtls_pk_free(&test_key);
    puts("PASS CSR subject: actual signed CSR parsed with MbedTLS, DN special characters, UTF-8, default identity, byte/capacity limits");
}
