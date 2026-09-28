#include "platform.h"
#include "ts_ws_subscriptions.c"
int64_t test_now=10000000;
static int reads,broadcasts;
int ts_api_call(const char*m,const cJSON*p,ts_api_result_t*r) {(void)m;(void)p;reads++;r->code=0;r->data=cJSON_CreateObject();return 0;}
int ts_webui_broadcast(const char*s) {(void)s;broadcasts++;return 0;}
int main(int argc,char**argv) {
 assert(argc==2);assert(ts_ws_subscriptions_init()==0);
 cJSON*p=cJSON_Parse("{\"interval\":10000}");
 ts_ws_subscribe(1,"system.dashboard",p);cJSON_Delete(p);
 if (!strcmp(argv[1],"T02")) {
  ts_ws_unsubscribe(1,"system.dashboard");
  if(s_dashboard_timer->active)s_dashboard_timer->callback(NULL);
  printf("T02 no subscribers: API reads=%d (expected 0)\n",reads);return reads?1:0;
 }
 if (!strcmp(argv[1],"T05")) {
  dashboard_timer_callback(NULL);reads=0;test_now+=1000000;
  dashboard_timer_callback(NULL);
  printf("T05 before next deadline: API reads=%d (expected 0)\n",reads);return reads?1:0;
 }
 ts_ws_subscribe(2,"system.dashboard",NULL);
 cJSON*d=cJSON_CreateObject();ts_ws_broadcast_to_topic("system.dashboard",d);cJSON_Delete(d);
 printf("T06/T07 2 targets, 3 connected: global broadcast calls=%d => %d frames (expected 2 targeted frames)\n",broadcasts,broadcasts*3);
 return broadcasts?1:0;
}
