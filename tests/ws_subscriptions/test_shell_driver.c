#define OP_ADAPTER_NO_MAIN
#define PROJECT_REAL_DRIVER
#include "test_operation_adapter.c"
static unsigned notified;
static void cli_output(const char *data,size_t len,void *arg){(void)data;(void)len;(void)arg;}
#define CONFIG_ESP_CONSOLE_UART_NUM 0
static unsigned cli_interrupts;
static int uart_read_bytes(int port,uint8_t *b,size_t n,unsigned wait){(void)port;(void)n;(void)wait;*b=0x1c;return 1;}
static void cli_request_interrupt(void){cli_interrupts++;}
static void cli_printf(const char *fmt,...){(void)fmt;}
#define ts_console_request_interrupt cli_request_interrupt
#define ts_console_printf cli_printf
#include "cli_shell_fixture.inc"
#undef ts_console_request_interrupt
#undef ts_console_printf
static void driver_closed_cb(int code,void *arg){(void)code;(void)arg;notified++;}
static void make_shell(ts_ssh_session_t *session,ts_ssh_shell_t *shell){
 ts_ssh_config_t c=TS_SSH_DEFAULT_CONFIG();c.host="192.0.2.1";c.username="u";c.auth.password="p";
 assert(ts_ssh_session_create(&c,session)==0 && ts_ssh_connect(*session)==0);
 assert(ts_ssh_shell_open(*session,NULL,shell)==0);
}
static void empty(void){assert(!live_allocations && !driver_channels && !driver_sessions && !driver_sockets);}
int main(int argc,char **argv){
 (void)argv;
 cJSON_Hooks hooks={tracked_malloc,tracked_free};cJSON_InitHooks(&hooks);
 ts_ssh_session_t session;ts_ssh_shell_t shell;char buf[8];size_t n;int64_t before;
 if(argc>1 && !strcmp(argv[1],"--cli-exit")){make_shell(&session,&shell);size_t ignored=0;shell_input_callback(&ignored,shell);fprintf(stderr,"CLI exit: active=%d interrupt=%u\n",ts_ssh_shell_is_active(shell),cli_interrupts);assert(!ts_ssh_shell_is_active(shell) && cli_interrupts==1);ts_ssh_shell_close(shell);ts_ssh_session_destroy(session);empty();return 0;}
 if(argc>1 && !strcmp(argv[1],"--metrics")){unsigned begin=allocations;make_shell(&session,&shell);unsigned opened=allocations;assert(ts_ssh_shell_write(shell,"abc",3,&n)==ESP_OK);unsigned wrote=allocations;assert(ts_ssh_shell_send_signal(shell,"TERM")==ESP_ERR_NOT_SUPPORTED);unsigned rejected=allocations;ts_ssh_shell_close(shell);ts_ssh_session_destroy(session);empty();printf("{\"open_allocations\":%u,\"write_allocations\":%u,\"unsupported_allocations\":%u,\"close_allocations\":%u,\"retained\":%u}\n",opened-begin,wrote-opened,rejected-wrote,allocations-rejected,live_allocations);return 0;}
 if(argc>1){make_shell(&session,&shell);driver_eof=true;assert(ts_ssh_shell_read(shell,buf,sizeof(buf),&n)==ESP_ERR_INVALID_STATE);assert(ts_ssh_shell_close(shell)==0);ts_ssh_session_destroy(session);fprintf(stderr,"EOF ledger: allocations=%u channels=%d sessions=%d sockets=%d\n",live_allocations,driver_channels,driver_sessions,driver_sockets);empty();return 0;}
 make_shell(&session,&shell);assert(ts_ssh_shell_read(shell,buf,sizeof(buf),&n)==ESP_ERR_TIMEOUT && ts_ssh_shell_is_active(shell));
 ts_ssh_shell_set_close_cb(shell,driver_closed_cb,NULL);driver_eof=true;
 assert(ts_ssh_shell_poll(shell)==ESP_ERR_INVALID_STATE && notified==1);assert(ts_ssh_shell_close(shell)==0);shell=NULL;assert(notified==1);assert(ts_ssh_session_destroy(session)==0);empty();driver_eof=false;
 puts("PASS real Shell+client: zero data is not EOF; EOF then close releases wrapper/channel/session/socket; close callback exactly once");
 make_shell(&session,&shell);driver_write_fail=true;assert(ts_ssh_shell_write(shell,"x",1,&n)==ESP_FAIL && n==0 && !ts_ssh_shell_is_active(shell));driver_write_fail=false;
 assert(ts_ssh_shell_close(shell)==0);ts_ssh_session_destroy(session);empty();
 make_shell(&session,&shell);driver_partial=true;assert(ts_ssh_shell_write(shell,"abc",3,&n)==ESP_FAIL && n==1 && !ts_ssh_shell_is_active(shell));driver_write_fail=false;ts_ssh_shell_close(shell);ts_ssh_session_destroy(session);empty();
 make_shell(&session,&shell);driver_resize_reject=true;assert(ts_ssh_shell_resize(shell,80,24)==ESP_FAIL && ts_ssh_shell_is_active(shell));driver_resize_reject=false;driver_read_fail=true;assert(ts_ssh_shell_read(shell,buf,sizeof(buf),&n)==ESP_FAIL && !ts_ssh_shell_is_active(shell));driver_read_fail=false;ts_ssh_shell_close(shell);ts_ssh_session_destroy(session);empty();
 make_shell(&session,&shell);driver_write_again=true;before=test_now;assert(ts_ssh_shell_write(shell,"x",1,&n)==ESP_ERR_TIMEOUT && test_now-before<=1000000);driver_write_again=false;
 driver_close_again=true;before=test_now;assert(ts_ssh_shell_close(shell)==0 && test_now==before);ts_ssh_session_destroy(session);driver_close_again=false;empty();
 puts("PASS real Shell+client: fatal write/timeout retires channel; persistent close EAGAIN falls back to local session teardown without waiting");
 for(int phase=1;phase<=4;phase++){
  ts_ssh_config_t c=TS_SSH_DEFAULT_CONFIG();c.host="192.0.2.1";c.username="u";c.auth.password="p";
  assert(ts_ssh_session_create(&c,&session)==0 && ts_ssh_connect(session)==0);driver_phase=phase;driver_close_again=true;before=test_now;shell=(void*)1;
  assert(ts_ssh_shell_open(session,NULL,&shell)!=0 && !shell && test_now-before<=10000000);ts_ssh_session_destroy(session);driver_phase=0;driver_close_again=false;empty();
 }
 puts("PASS real Shell+client: partial initialization/open/PTY/shell-start failure and bounded EAGAIN all release owned resources");
  make_shell(&session,&shell);before=driver_reads;assert(ts_ssh_shell_run(shell,cli_output,shell_input_callback,shell)==ESP_OK && driver_reads==before && cli_interrupts==1);ts_ssh_shell_close(shell);ts_ssh_session_destroy(session);empty();
 puts("PASS Shell run: local exit requested by input callback stops before another network read; close still owns cleanup");
  setup();cJSON *j=cJSON_Parse("{\"host\":\"192.0.2.1\",\"user\":\"u\",\"password\":\"p\"}");handle_ssh_connect(&reqs[1],j);cJSON_Delete(j);assert(shell_task.fn);ssh_shell_context_t *ctx=shell_task.arg;ssh_cleanup();shell_resources_close(ctx);shell_resources_close(ctx);shell_task.fn(shell_task.arg);finish_all();empty();
 puts("PASS actual WebUI cleanup twice uses cleared handles; original operation/ticket/target and actual lower resources drain");
 setup();shell_task.fn=NULL;j=cJSON_Parse("{\"host\":\"192.0.2.1\",\"user\":\"u\",\"password\":\"p\"}");handle_ssh_connect(&reqs[1],j);cJSON_Delete(j);assert(shell_task.fn);
 ssh_send_output(shell_task.arg,"accepted",8);
 assert(ts_webui_ws_stop((void*)11)==ESP_ERR_TIMEOUT);assert(driver_channels==1 && ts_ws_op_busy());
 delay_hook=stop_progress_shell;assert(ts_webui_ws_stop((void*)11)==ESP_OK);delay_hook=NULL;for(int i=0;i<32;i++)if(reqs[i].sess_ctx)close_peer(i);ts_webui_ws_stopped((void*)11);empty();
 puts("PASS real lower resources survive stop timeout; accepted output and terminal drain on retry with no new business message");
}
