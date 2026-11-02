#pragma once
#include <stddef.h>
static inline void esp_fill_random(void *p,size_t n){static unsigned v=1;unsigned char *b=p;while(n--)*b++=(unsigned char)(v++);}
