#define main existing_main
#include "/Users/massif/TianshanOS/tests/ws_subscriptions/test_manager.c"
#undef main
int main(void){
 cJSON_Hooks hooks={tracked_malloc,tracked_free};cJSON_InitHooks(&hooks);
 start();ts_ws_peer_t a=open_peer(1);
 for(unsigned i=0;i<8;i++)subscribe(a,topics[i].name,10000);
 for(unsigned n=0;n<100;n++){test_now+=10000000;run_batch();pump();}
 for(unsigned i=0;i<8;i++)printf("%s: api=%llu serialized=%llu\n",topics[i].name,(unsigned long long)(i?s_stats.api[topics[i].metric]:s_stats.api[0]),(unsigned long long)s_stats.serialized[i]);
 assert(s_stats.serialized[5]==100 && s_stats.serialized[6]==0 && s_stats.serialized[7]==0);
 puts("CONFIRMED R5: fan/status and service/list starve for 100 completed cycles, although their APIs are collected each time");
 stop();assert(!live_allocations);
}
