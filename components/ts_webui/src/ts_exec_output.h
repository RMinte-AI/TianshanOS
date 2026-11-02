#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#define EXEC_OUTPUT_BYTES 16384
#define EXEC_OUTPUT_SPANS 64
#define EXEC_OUTPUT_CHUNK 4096
/* Producer and consumer hold the caller's short output mutex. Offsets count
 * every remote byte, including dropped bytes; gaps belong to each observer. */
typedef struct { size_t len; uint64_t offset; bool is_stderr; } exec_span_t;
typedef struct {
    unsigned char bytes[EXEC_OUTPUT_BYTES];
    exec_span_t spans[EXEC_OUTPUT_SPANS];
    size_t head, used; unsigned first, count;
    uint64_t total; bool incomplete;
} exec_output_t;
static void exec_output_put(exec_output_t *r,const char *data,size_t len,bool is_stderr) {
    uint64_t offset=r->total;r->total+=len;
    exec_span_t *last=r->count?&r->spans[(r->first+r->count-1)%EXEC_OUTPUT_SPANS]:NULL;
    bool merge=last && last->is_stderr==is_stderr && last->offset+last->len==offset;
    size_t n=len;
    if(n>EXEC_OUTPUT_BYTES-r->used)n=EXEC_OUTPUT_BYTES-r->used;
    if(!merge && r->count==EXEC_OUTPUT_SPANS)n=0;
    if(n){
        size_t tail=(r->head+r->used)%EXEC_OUTPUT_BYTES,part=EXEC_OUTPUT_BYTES-tail;
        if(part>n)part=n;
        memcpy(r->bytes+tail,data,part);memcpy(r->bytes,data+part,n-part);
        if(merge)last->len+=n;
        else {r->spans[(r->first+r->count)%EXEC_OUTPUT_SPANS]=(exec_span_t){n,offset,is_stderr};r->count++;}
        r->used+=n;
    }
    r->incomplete |= n!=len;
}
static exec_span_t exec_output_peek(exec_output_t *r,char *out) {
    if(!r->count)return (exec_span_t){0};
    exec_span_t span=r->spans[r->first];if(span.len>EXEC_OUTPUT_CHUNK)span.len=EXEC_OUTPUT_CHUNK;
    size_t part=EXEC_OUTPUT_BYTES-r->head;if(part>span.len)part=span.len;
    memcpy(out,r->bytes+r->head,part);memcpy(out+part,r->bytes,span.len-part);return span;
}
static void exec_output_pop(exec_output_t *r,size_t n) {
    exec_span_t *s=&r->spans[r->first];s->offset+=n;s->len-=n;
    r->head=(r->head+n)%EXEC_OUTPUT_BYTES;r->used-=n;
    if(!s->len){r->first=(r->first+1)%EXEC_OUTPUT_SPANS;r->count--;}
}
/* Binary transport preserves NUL and UTF-8 split across SSH callbacks. The
 * browser decodes each stream incrementally, rather than JSON-encoding partial UTF-8. */
static void exec_output_base64(const unsigned char *src,size_t n,char *dst) {
    static const char abc[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t j=0;
    for(size_t i=0;i<n;i+=3){
        unsigned v=(unsigned)src[i]<<16;
        if(i+1<n)v|=(unsigned)src[i+1]<<8;
        if(i+2<n)v|=src[i+2];
        dst[j++]=abc[v>>18];dst[j++]=abc[(v>>12)&63];
        dst[j++]=i+1<n?abc[(v>>6)&63]:'=';dst[j++]=i+2<n?abc[v&63]:'=';
    }
    dst[j]=0;
}
