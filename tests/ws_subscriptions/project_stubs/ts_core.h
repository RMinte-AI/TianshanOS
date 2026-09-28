#pragma once
#define TS_MALLOC_PSRAM(n) tracked_malloc(n)
#define TS_CALLOC_PSRAM(n,z) host_calloc(n,z)
#define TS_REALLOC_PSRAM(p,n) realloc(p,n)
