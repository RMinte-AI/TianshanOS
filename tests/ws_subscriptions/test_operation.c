#define main manager_regressions
#include "test_manager.c"
#undef main
static ts_ws_operation_t *op;
static ts_ws_op_kind_t kind;
static unsigned destroys;
static void destroy(void *p){(void)p;destroys++;}
static void drive(void){for(unsigned i=0;i<8;i++){ts_ws_op_poll();pump();}}
static void close_op(void){if(ts_ws_op_close_begin(op))ts_ws_op_close_finish(op,"{\"type\":\"done\"}");}
static void inspect(void){
 ts_ws_op_stats_t s;ts_ws_op_stats(kind,&s);
 assert(s.tickets==s.finished+s.preparing);
 assert(s.registered==s.settled+s.unsettled);
 assert(s.finalizers<=1);
 if(s.phase==OP_FREE)assert(!s.refs && !s.executors && !s.preparing && !s.unsettled && !s.terminals);
}
static void close_during_prepare(void){close_op();ts_ws_op_poll();ts_ws_op_stats_t s;ts_ws_op_stats(kind,&s);assert(s.phase==OP_CLOSING && s.preparing==1);}
static void try_replace(void){assert(!ts_ws_op_create(kind,ts_ws_op_peer(op),NULL));inspect();}
static void old_failure_exit(void){ts_ws_op_executor_done(op);try_replace();ts_ws_op_poll();try_replace();}
static void deny_ticket(void){close_op();ts_ws_op_stop_admission();}
int main(void){
 cJSON_Hooks hooks={tracked_malloc,tracked_free};cJSON_InitHooks(&hooks);
 const char *points[]={"B01","B02","B03","B04","B05","B06","B07","B08","B10"};
 for(kind=0;kind<TS_WS_OP_KINDS;kind++)for(unsigned scenario=0;scenario<9;scenario++)for(unsigned early=0;early<2;early++) {
  start();ts_ws_op_enable();ts_ws_peer_t peer=open_peer(1);if(kind==TS_WS_OP_EXEC)open_peer(2);
  op=ts_ws_op_create(kind,peer,NULL);assert(op);ts_ws_op_bind(op,NULL,destroy);assert(ts_ws_op_open(op));
  immediate=early;barrier_name=points[scenario];
  barrier_action=scenario==0?deny_ticket:scenario>=1&&scenario<=3?close_during_prepare:scenario==5?old_failure_exit:try_replace;
  ts_ws_output_ticket_t ticket;
  bool accepted=ts_ws_op_output_begin(op,&ticket);
  if(scenario==5)queue_fail=true;
  if(accepted)ts_ws_op_output_finish(&ticket,"{\"type\":\"output\"}");
  queue_fail=false;
  if(scenario!=5)close_op();
  drive();inspect();
  if(scenario!=5){ /* B09: settled observation cannot release business context */
   assert(ts_ws_op_executing(op));try_replace();ts_ws_output_ticket_t denied;assert(!ts_ws_op_output_begin(op,&denied));
   ts_ws_op_executor_done(op);
  }
  drive();inspect();assert(!ts_ws_op_busy());assert(!barrier_action);
  immediate=false;stop();assert(!live_allocations);
 }
 for(kind=0;kind<TS_WS_OP_KINDS;kind++)for(unsigned mode=0;mode<4;mode++)for(unsigned early=0;early<2;early++) {
  start();ts_ws_op_enable();ts_ws_peer_t peer=open_peer(1);if(kind==TS_WS_OP_EXEC)open_peer(2);
  op=ts_ws_op_create(kind,peer,NULL);assert(op && ts_ws_op_open(op));ts_ws_output_ticket_t t;assert(ts_ws_op_output_begin(op,&t));
  immediate=early;queue_fail=mode==1;send_fail=mode==2;
  if(mode==3){close_peer(1);open_peer(1);} /* fixed old peer never targets the replacement */
  ts_ws_op_output_finish(&t,mode==0?NULL:"data");queue_fail=send_fail=false;immediate=false;
  uint64_t identity=op->ledger.identity,token=op->output[0].id;
  close_op();drive();inspect();uint64_t settled=op->ledger.settled;
  settle_target(identity,token,ESP_FAIL);settle_target(identity,token,ESP_OK);
  assert(op->ledger.settled==settled && op->ledger.duplicates>=2);inspect();
  ts_ws_op_executor_done(op);drive();assert(!ts_ws_op_busy());stop();assert(!live_allocations);
 }
 puts("PASS 16 failure schedules: encode/SDK/socket/stale peer, early/deferred done, duplicate tokens do not double-settle");
 printf("PASS production operation/TX: 36 barrier schedules, ticket/target ledgers and B09 executor retention; destroyed=%u\n",destroys);
 return 0;
}
