#include "ts_cert_time.h"
#include "../../main/ts_https_retry.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    int64_t t;
    assert(ts_cert_time_utc(1970,1,1,0,0,0,&t)&&t==0);
    assert(ts_cert_time_utc(1969,12,31,23,59,59,&t)&&t==-1);
    assert(ts_cert_time_utc(9999,12,31,23,59,59,&t)&&t==253402300799LL);
    assert(ts_cert_time_utc(2050,1,1,0,0,0,&t)&&t==2524608000LL);
    assert(ts_cert_time_utc(2000,2,29,0,0,0,&t)&&t==951782400);
    assert(ts_cert_time_utc(2024,2,29,0,0,0,&t)&&t==1709164800);
    assert(!ts_cert_time_utc(2100,2,29,0,0,0,&t));
    assert(!ts_cert_time_utc(0,1,1,0,0,0,&t));
    assert(!ts_cert_time_utc(10000,1,1,0,0,0,&t));
    assert(!ts_cert_time_utc(2026,13,1,0,0,0,&t));
    assert(!ts_cert_time_utc(2026,4,31,0,0,0,&t));
    assert(!ts_cert_time_utc(2026,1,1,24,0,0,&t));
    assert(!ts_cert_time_utc(2026,1,1,0,60,0,&t));
    assert(!ts_cert_time_utc(2026,1,1,0,0,60,&t));
    int64_t seconds[]={86399,0,-1,-86400,-86401};int days[]={0,0,-1,-1,-2};
    for(int i=0;i<5;++i)assert(ts_cert_time_days(seconds[i])==days[i]);
    assert(!ts_cert_time_ready(1735689599,2025)&&ts_cert_time_ready(1735689600,2025));
    unsigned char bytes[33];memset(bytes,0xff,sizeof(bytes));
    size_t sizes[]={0,1,20,31,32,33};
    for(int i=0;i<6;++i){struct {char hex[65];char guard;} v;v.guard='!';
        assert(ts_cert_hex(bytes,sizes[i],v.hex,sizeof(v.hex))==(sizes[i]>32));
        assert(strlen(v.hex)==2*(sizes[i]<32?sizes[i]:32));assert(v.guard=='!');}
    ts_https_retry_t r={0};
    assert(!ts_https_retry_due(&r,1,1,true,false,0));
    assert(ts_https_retry_due(&r,1,1,true,true,0));
    ts_https_retry_failed(&r,0);
    for(int i=0;i<100;++i)assert(!ts_https_retry_due(&r,1,1,true,true,999));
    assert(ts_https_retry_due(&r,1,1,true,true,1000));ts_https_retry_failed(&r,1000);
    assert(!ts_https_retry_due(&r,1,1,true,true,5999));
    assert(ts_https_retry_due(&r,1,1,true,true,6000));ts_https_retry_failed(&r,6000);
    assert(!ts_https_retry_due(&r,1,1,true,true,20999));
    assert(ts_https_retry_due(&r,1,1,true,true,21000));ts_https_retry_failed(&r,21000);
    for(int i=0;i<100;++i)assert(!ts_https_retry_due(&r,1,1,true,true,999999));
    assert(ts_https_retry_due(&r,2,1,true,true,999999));
    ts_https_retry_failed(&r,999999);
    assert(!ts_https_retry_due(&r,2,2,false,true,9999999));
    for(int i=0;i<100;++i)assert(!ts_https_retry_due(&r,2,2,false,true,99999999));
    assert(ts_https_retry_due(&r,2,3,true,true,99999999));
    puts("PASS UTC/serial bounds and bounded retry/control policy");
}
