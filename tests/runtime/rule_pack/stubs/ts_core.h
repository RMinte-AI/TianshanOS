#pragma once
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#define TS_MALLOC_PSRAM malloc
#define TS_CALLOC_PSRAM calloc
#define TS_STRDUP_PSRAM strdup
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define heap_caps_malloc(n,c) malloc(n)
#define heap_caps_calloc(n,s,c) calloc(n,s)
