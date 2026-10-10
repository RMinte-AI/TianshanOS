#pragma once
#include "ts_config_pack.h"
#include "ts_cert.h"
ts_config_pack_result_t ts_config_pack_signer_trust(const char *signer,
    const ts_cert_snapshot_t *snapshot, const char *package_hash, const char *recipient,
    const ts_config_pack_acceptance_t *accepted, ts_config_pack_acceptance_t *out);
