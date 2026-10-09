/**
 * @file ts_api_pack.h
 * @brief Shared reply for a failed config pack (.tscfg) operation, used by every API that exports or imports packs
 */
#ifndef TS_API_PACK_H
#define TS_API_PACK_H

#include "ts_api.h"
#include "ts_config_pack.h"

/** A config pack operation failed: the reason token for the page, the library's own English text for the folded details. */
static inline void pack_failure(ts_api_result_t *result, ts_config_pack_result_t pack_result)
{
    ts_api_result_code_t code = pack_result == TS_CONFIG_PACK_ERR_NO_MEM ? TS_API_ERR_NO_MEM
        : pack_result == TS_CONFIG_PACK_ERR_PERMISSION ? TS_API_ERR_NO_PERMISSION
        : (pack_result == TS_CONFIG_PACK_ERR_IO || pack_result == TS_CONFIG_PACK_ERR_NOT_INIT) ? TS_API_ERR_INTERNAL
        : TS_API_ERR_INVALID_ARG;
    ts_api_result_reason(result, code, ts_config_pack_reason(pack_result), ts_config_pack_strerror(pack_result));
}

#endif /* TS_API_PACK_H */
