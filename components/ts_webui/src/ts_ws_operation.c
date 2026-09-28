#include "ts_ws_operation.h"
#include "ts_ws_subscriptions.h"
#include "freertos/FreeRTOS.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>

#define OP_OUTPUT_TARGETS (2 * TS_WS_CONNECTIONS)
typedef struct { uint64_t id; bool active; } op_target_t;
struct ts_ws_operation {
    ts_ws_op_stats_t ledger;
    ts_ws_op_kind_t kind;
    bool initializing, builder;
    bool business_accepted;
    ts_ws_business_phase_t business;
    ts_ws_stop_reason_t first_stop;
    unsigned stop_reasons;
    ts_ws_peer_t peer, terminal_peers[TS_WS_CONNECTIONS];
    unsigned terminal_count;
    ts_ws_reservation_t terminal;
    op_target_t output[OP_OUTPUT_TARGETS], final[TS_WS_CONNECTIONS];
    void *data;
    void (*destroy)(void *);
    bool (*maintain)(void *);
};
static portMUX_TYPE s_op_lock = portMUX_INITIALIZER_UNLOCKED;
static ts_ws_operation_t s_ops[TS_WS_OP_KINDS];
static uint64_t s_op_sequence, s_op_delivery;
static bool s_op_accepting, s_op_tick;
#define OP_LOCK() portENTER_CRITICAL(&s_op_lock)
#define OP_UNLOCK() portEXIT_CRITICAL(&s_op_lock)

void ts_ws_op_enable(void) { OP_LOCK(); s_op_accepting=true; OP_UNLOCK(); }
void ts_ws_op_stop_admission(void) { OP_LOCK(); s_op_accepting=false; OP_UNLOCK(); }
bool ts_ws_op_busy(void) {
    OP_LOCK(); bool busy=false;
    for(unsigned i=0;i<TS_WS_OP_KINDS;i++) busy |= s_ops[i].ledger.phase!=OP_FREE;
    OP_UNLOCK(); return busy;
}
ts_ws_operation_t *ts_ws_op_create(ts_ws_op_kind_t kind, ts_ws_peer_t peer, esp_err_t *error) {
    if(error)*error=ESP_ERR_INVALID_ARG;
    if(kind>=TS_WS_OP_KINDS)return NULL;
    if(error)*error=ESP_ERR_INVALID_STATE;
    OP_LOCK(); ts_ws_operation_t *op=&s_ops[kind];
    if(!s_op_accepting || op->ledger.phase!=OP_FREE || s_op_sequence==UINT32_MAX) { OP_UNLOCK(); return NULL; }
    *op=(ts_ws_operation_t){.kind=kind,.peer=peer,.initializing=true,
        .ledger={.phase=OP_STARTING,.identity=++s_op_sequence,.refs=1,.executors=1}};
    OP_UNLOCK();
    esp_err_t ret=kind==TS_WS_OP_SHELL ? ts_ws_reserve(&peer,1,true,4096,&op->terminal) :
        ts_ws_result_reserve(TS_WS_LEGACY_BYTES,&op->terminal);
    OP_LOCK(); op->initializing=false; OP_UNLOCK();
    if(error)*error=ret;
    if(ret!=ESP_OK) { ts_ws_op_discard(op); ts_ws_op_executor_done(op); return NULL; }
    return op;
}
void ts_ws_op_bind(ts_ws_operation_t *op,void *data,void (*destroy)(void *)) {
    OP_LOCK(); assert(op->ledger.phase==OP_STARTING && !op->data);op->data=data;op->destroy=destroy;OP_UNLOCK();
}
void ts_ws_op_set_maintenance(ts_ws_operation_t *op,bool (*maintain)(void *)) {OP_LOCK();op->maintain=maintain;OP_UNLOCK();}
bool ts_ws_op_needs_tick(void) {OP_LOCK();bool yes=s_op_tick;OP_UNLOCK();return yes;}
void *ts_ws_op_data(ts_ws_operation_t *op) { OP_LOCK();void *p=op->data;OP_UNLOCK();return p; }
uint32_t ts_ws_op_id(ts_ws_operation_t *op) { return (uint32_t)op->ledger.identity; }
ts_ws_peer_t ts_ws_op_peer(ts_ws_operation_t *op) { return op->peer; }
ts_ws_operation_t *ts_ws_op_acquire(ts_ws_op_kind_t kind,uint32_t id) {
    OP_LOCK();ts_ws_operation_t *op=&s_ops[kind];
    if(op->initializing || op->ledger.phase==OP_FREE || op->ledger.phase==OP_RECLAIMING || (id && op->ledger.identity!=id))op=NULL;
    else op->ledger.refs++;
    OP_UNLOCK();return op;
}
void ts_ws_op_retain(ts_ws_operation_t *op) { OP_LOCK();assert(op->ledger.refs);op->ledger.refs++;OP_UNLOCK(); }
void ts_ws_op_release(ts_ws_operation_t *op) {
    OP_LOCK();assert(op->ledger.refs);op->ledger.refs--;OP_UNLOCK();ts_ws_subscriptions_wake();
}
void ts_ws_op_executor_take(ts_ws_operation_t *op) {
    OP_LOCK();assert(op->ledger.refs);op->ledger.refs++;op->ledger.executors++;OP_UNLOCK();
}
void ts_ws_op_executor_done(ts_ws_operation_t *op) {
    OP_LOCK();assert(op->ledger.executors && op->ledger.refs);op->ledger.executors--;op->ledger.refs--;OP_UNLOCK();ts_ws_subscriptions_wake();
}
bool ts_ws_op_executing(ts_ws_operation_t *op) { OP_LOCK();bool yes=op->ledger.executors!=0;OP_UNLOCK();return yes; }
bool ts_ws_op_open(ts_ws_operation_t *op) {
    OP_LOCK();bool yes=op->ledger.phase==OP_STARTING;if(yes){op->ledger.phase=OP_OPEN;op->business_accepted=true;}OP_UNLOCK();return yes;
}
bool ts_ws_op_is_open(ts_ws_operation_t *op) { OP_LOCK();bool yes=op->ledger.phase==OP_OPEN;OP_UNLOCK();return yes; }
bool ts_ws_op_starting(ts_ws_operation_t *op) { OP_LOCK();bool yes=op->ledger.phase==OP_STARTING;OP_UNLOCK();return yes; }
void ts_ws_op_discard(ts_ws_operation_t *op) {
    OP_LOCK();assert((op->ledger.phase==OP_STARTING || op->ledger.phase==OP_OPEN) && !op->ledger.preparing && !op->ledger.unsettled);op->ledger.phase=OP_FINALIZING;OP_UNLOCK();
    ts_ws_reservation_release(&op->terminal);
    OP_LOCK();op->ledger.phase=OP_TERMINAL_SETTLED;OP_UNLOCK();
}
/* Caller holds a ref. The winner takes a second ref for snapshot/encoding. */
static bool close_locked(ts_ws_operation_t *op) {
    if(op->ledger.phase!=OP_OPEN && op->ledger.phase!=OP_STARTING)return false;
    op->ledger.phase=OP_CLOSING;op->builder=true;op->ledger.refs++;return true;
}
static void close_snapshot(ts_ws_operation_t *op) {
    if(op->kind==TS_WS_OP_SHELL){op->terminal_peers[0]=op->peer;op->terminal_count=1;}
    else op->terminal_count=ts_ws_peer_snapshot(op->terminal_peers,TS_WS_CONNECTIONS);
}
bool ts_ws_op_close_begin(ts_ws_operation_t *op) {
    OP_LOCK();bool winner=close_locked(op);OP_UNLOCK();
    if(winner){close_snapshot(op);TS_WS_TEST_POINT("B07");}return winner;
}
static void fallback(ts_ws_operation_t *op,char *text,size_t size,bool failed) {
    const char *reason=failed ? "SSH output delivery incomplete; remote command result is not confirmed" : "SSH result delivery failed; result is unknown";
    if(op->kind==TS_WS_OP_SHELL && !failed)reason="SSH status delivery failed";
    if(op->kind==TS_WS_OP_SHELL)snprintf(text,size,"{\"type\":\"ssh_status\",\"status\":\"error\",\"message\":\"%s\"}",reason);
    else snprintf(text,size,"{\"type\":\"ssh_exec_error\",\"session_id\":%lu,\"error\":\"%s\"}",(unsigned long)ts_ws_op_id(op),reason);
}
void ts_ws_op_close_finish(ts_ws_operation_t *op,const char *json) {
    char text[256];fallback(op,text,sizeof(text),false);
    if(!json || ts_ws_reserved_text(&op->terminal,json)!=ESP_OK)ts_ws_reserved_text(&op->terminal,text);
    OP_LOCK();assert(op->builder);op->builder=false;assert(op->ledger.refs);op->ledger.refs--;OP_UNLOCK();ts_ws_subscriptions_wake();
}
static void fail_locked(ts_ws_operation_t *op,bool *winner) {
    op->ledger.failed=true;*winner=close_locked(op);
}
static void settle_target(uint64_t identity,uint64_t token,esp_err_t result) {
    ts_ws_operation_t *op=NULL;bool winner=false,terminal=false;
    OP_LOCK();
    for(unsigned i=0;i<TS_WS_OP_KINDS;i++)if(s_ops[i].ledger.identity==identity && s_ops[i].ledger.phase!=OP_FREE){op=&s_ops[i];break;}
    op_target_t *record=NULL;
    if(op){
        for(unsigned i=0;i<OP_OUTPUT_TARGETS;i++)if(op->output[i].active && op->output[i].id==token){record=&op->output[i];break;}
        if(!record)for(unsigned i=0;i<TS_WS_CONNECTIONS;i++)if(op->final[i].active && op->final[i].id==token){record=&op->final[i];terminal=true;break;}
    }
    if(!record){if(op)op->ledger.duplicates++;OP_UNLOCK();return;}
    record->active=false;
    if(terminal){assert(op->ledger.terminals);op->ledger.terminals--;}
    else {
        if(result!=ESP_OK)fail_locked(op,&winner);
        assert(op->ledger.unsettled);op->ledger.unsettled--;op->ledger.settled++;
    }
    /* Target ref becomes the callback ref; not dropped at counter decrement. */
    OP_UNLOCK();
    if(winner){close_snapshot(op);ts_ws_op_close_finish(op,NULL);}
    TS_WS_TEST_POINT("B06");
    ts_ws_op_release(op); /* final access: old callback cannot enter replacement */
}
bool ts_ws_op_output_begin(ts_ws_operation_t *op,ts_ws_output_ticket_t *t) {
    *t=(ts_ws_output_ticket_t){0};TS_WS_TEST_POINT("B01");
    OP_LOCK();bool ok=op->ledger.phase==OP_OPEN;
    if(ok){op->ledger.preparing++;t->sequence=++op->ledger.tickets;op->ledger.refs++;t->op=op;}OP_UNLOCK();
    if(!ok)return false;
    if(op->kind==TS_WS_OP_SHELL){t->peers[0]=op->peer;t->count=1;}
    else t->count=ts_ws_peer_snapshot(t->peers,TS_WS_CONNECTIONS);
    TS_WS_TEST_POINT("B02");return true;
}
void ts_ws_op_output_finish(ts_ws_output_ticket_t *t,const char *json) {
    if(!t->op)return;
    ts_ws_operation_t *op=t->op;ts_ws_message_t *m=NULL;
    if(t->count && json)m=ts_ws_message_text(json,strlen(json));
    uint64_t tokens[TS_WS_CONNECTIONS]={0};bool winner=false,registered=false;
    TS_WS_TEST_POINT("B03");
    OP_LOCK();
    unsigned available=0;for(unsigned i=0;i<OP_OUTPUT_TARGETS;i++)available+=!op->output[i].active;
    if(t->count && (!m || available<t->count || s_op_delivery>UINT64_MAX-t->count))fail_locked(op,&winner);
    else {
        for(unsigned i=0,j=0;i<OP_OUTPUT_TARGETS && j<t->count;i++)if(!op->output[i].active){
            tokens[j++]=++s_op_delivery;op->output[i]=(op_target_t){s_op_delivery,true};
        }
        op->ledger.unsettled+=t->count;op->ledger.registered+=t->count;op->ledger.refs+=t->count;registered=true;
    }
    OP_UNLOCK();
    if(winner){close_snapshot(op);ts_ws_op_close_finish(op,NULL);}
    TS_WS_TEST_POINT("B04");
    if(registered)for(unsigned i=0;i<t->count;i++) {
        esp_err_t ret=ts_ws_transport_submit(t->peers[i],m,op->ledger.identity,tokens[i],NULL,settle_target);
        if(ret!=ESP_OK)settle_target(op->ledger.identity,tokens[i],ret);
    }
    ts_ws_message_release(m);
    OP_LOCK();assert(op->ledger.preparing);op->ledger.preparing--;op->ledger.finished++;OP_UNLOCK();
    t->op=NULL;ts_ws_op_release(op); /* entire publication, including early done */
}
void ts_ws_op_poll(void) {
    bool tick=false;
    for(unsigned k=0;k<TS_WS_OP_KINDS;k++) {
        ts_ws_operation_t *op=&s_ops[k];ts_ws_reservation_t r={0};bool finalize=false,retire=false;
        uint64_t tokens[TS_WS_CONNECTIONS]={0};
        OP_LOCK();
        bool maintain=op->maintain && op->ledger.phase>=OP_OPEN && op->ledger.phase<OP_RECLAIMING;
        if(maintain)op->ledger.refs++;
        OP_UNLOCK();
        if(maintain){tick |= op->maintain(op->data);OP_LOCK();assert(op->ledger.refs);op->ledger.refs--;OP_UNLOCK();}
        OP_LOCK();
        if(op->ledger.phase==OP_CLOSING && !op->builder && !op->ledger.preparing && !op->ledger.unsettled) {
            op->ledger.phase=OP_FINALIZING;op->ledger.refs++;op->ledger.finalizers++;
            r=op->terminal;op->terminal=(ts_ws_reservation_t){0};finalize=true;
        }
        OP_UNLOCK();
        if(finalize) {
            char text[256];
            if(op->ledger.failed){fallback(op,text,sizeof(text),true);ts_ws_reserved_text(&r,text);}
            OP_LOCK();
            unsigned n=op->terminal_count;
            if(s_op_delivery>UINT64_MAX-n)n=0; /* lifetime exhaustion: no token reuse */
            for(unsigned i=0;i<n;i++){tokens[i]=++s_op_delivery;op->final[i]=(op_target_t){tokens[i],true};}
            op->ledger.terminals=n;op->ledger.refs+=n;op->ledger.phase=OP_TERMINAL_PENDING;
            OP_UNLOCK();
            for(unsigned i=0;i<n;i++) {
                esp_err_t ret=ts_ws_reserved_target(&r,i,op->terminal_peers[i],op->ledger.identity,tokens[i],settle_target);
                if(ret!=ESP_OK)settle_target(op->ledger.identity,tokens[i],ret);
                TS_WS_TEST_POINT("B08");
            }
            ts_ws_reservation_release(&r);ts_ws_op_release(op);
        }
        OP_LOCK();
        if(op->ledger.phase==OP_TERMINAL_PENDING && !op->ledger.terminals)op->ledger.phase=OP_TERMINAL_SETTLED;
        if(op->ledger.phase==OP_TERMINAL_SETTLED && !op->ledger.refs) {
            assert(!op->ledger.executors && !op->ledger.preparing && !op->ledger.unsettled && !op->terminal.message);
            op->ledger.phase=OP_RECLAIMING;retire=true;
        }
        OP_UNLOCK();
        if(retire) {
            TS_WS_TEST_POINT("B10");if(op->destroy)op->destroy(op->data);
            OP_LOCK();op->data=NULL;op->destroy=NULL;op->maintain=NULL;op->ledger.phase=OP_FREE;OP_UNLOCK();
        }
    }
    OP_LOCK();s_op_tick=tick;OP_UNLOCK();
}
void ts_ws_op_stats(ts_ws_op_kind_t kind,ts_ws_op_stats_t *stats) {OP_LOCK();*stats=s_ops[kind].ledger;OP_UNLOCK();}

/* All identity checks, reference acquisition and control decisions linearize
 * under the same operation lock. No resource or network work under this lock. */
static ts_ws_operation_t *shell_for_peer(ts_ws_peer_t peer,bool control) {
    OP_LOCK();ts_ws_operation_t *op=&s_ops[TS_WS_OP_SHELL];
    if(op->initializing || op->ledger.phase==OP_FREE || op->ledger.phase==OP_RECLAIMING ||
       (control && op->ledger.phase!=OP_OPEN) || !ts_ws_peer_equal(op->peer,peer))op=NULL;
    else op->ledger.refs++;
    OP_UNLOCK();return op;
}
ts_ws_operation_t *ts_ws_op_shell_control(ts_ws_peer_t peer) {
    ts_ws_operation_t *op=shell_for_peer(peer,true);TS_WS_TEST_POINT("C01");return op;
}
ts_ws_operation_t *ts_ws_op_shell_connection(ts_ws_peer_t peer) {
    return shell_for_peer(peer,false);
}
ts_ws_operation_t *ts_ws_op_shell_owner(void) {
    return ts_ws_op_acquire(TS_WS_OP_SHELL,0); /* service/connection owner only */
}
static bool request_stop_locked(ts_ws_operation_t *op,ts_ws_stop_reason_t reason) {
    if(op->kind!=TS_WS_OP_EXEC || !op->business_accepted || op->business==OP_BUSINESS_ENDED)return false;
    op->stop_reasons|=reason;
    if(!op->first_stop)op->first_stop=reason;
    return true;
}
ts_ws_operation_t *ts_ws_op_cancel_exec(uint32_t id) {
    OP_LOCK();ts_ws_operation_t *op=&s_ops[TS_WS_OP_EXEC];
    if(!id || op->ledger.phase==OP_FREE || op->ledger.phase==OP_RECLAIMING ||
       op->ledger.identity!=id || !request_stop_locked(op,OP_STOP_USER))op=NULL;
    else op->ledger.refs++;
    OP_UNLOCK();return op;
}
bool ts_ws_op_request_stop(ts_ws_operation_t *op,ts_ws_stop_reason_t reason) {
    OP_LOCK();bool accepted=request_stop_locked(op,reason);OP_UNLOCK();return accepted;
}
bool ts_ws_op_cancelled(void *context) {
    ts_ws_operation_t *op=context;OP_LOCK();bool yes=op->stop_reasons!=0;OP_UNLOCK();return yes;
}
bool ts_ws_op_claim_submit(ts_ws_operation_t *op) {
    TS_WS_TEST_POINT("C02");OP_LOCK();
    bool yes=op->business_accepted && op->business==OP_BUSINESS_PREPARING && !op->stop_reasons;
    if(yes)op->business=OP_BUSINESS_SUBMITTED;
    OP_UNLOCK();TS_WS_TEST_POINT("C03");return yes;
}
ts_ws_stop_reason_t ts_ws_op_business_end(ts_ws_operation_t *op,bool deadline) {
    OP_LOCK();
    if(op->business!=OP_BUSINESS_ENDED){
        if(deadline && !op->first_stop)request_stop_locked(op,OP_STOP_TIMEOUT);
        op->business=OP_BUSINESS_ENDED;
    }
    ts_ws_stop_reason_t reason=op->first_stop;OP_UNLOCK();return reason;
}
bool ts_ws_op_business_running(ts_ws_operation_t *op) {
    OP_LOCK();bool yes=op->business_accepted && op->business!=OP_BUSINESS_ENDED;OP_UNLOCK();return yes;
}
