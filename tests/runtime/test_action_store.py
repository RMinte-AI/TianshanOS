#!/usr/bin/env python3
"""Production action file store under FAT rename and injected I/O interruptions."""
from pathlib import Path
import os
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
os.chdir(root)
idf=Path(os.environ.get('IDF_PATH','/Users/massif/esp/v5.5.2/esp-idf'))
code=r'''
#define _POSIX_C_SOURCE 200809L
#include "platform.h"
#include "cJSON.h"
#include <setjmp.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
static jmp_buf shutdown_point;
static int mode, fault, step, rejected_overwrites;
static FILE *open_files[32];
static void remember(FILE *f) { if(!f)return;for(unsigned i=0;i<32;i++)if(!open_files[i]){open_files[i]=f;return;}assert(0); }
static void forget(FILE *f) { for(unsigned i=0;i<32;i++)if(open_files[i]==f)open_files[i]=NULL; }
static bool before(void) { ++step;if(step!=fault)return false;if(mode==2)longjmp(shutdown_point,1);if(mode==1){errno=EIO;return true;}return false; }
static void after(void) { if(step==fault&&mode==3)longjmp(shutdown_point,1); }
static int test_stat(const char *p,struct stat *s) { if(before())return -1;int r=stat(p,s);after();return r; }
static FILE *test_open(const char *p,const char *m) { if(before())return NULL;FILE*f=fopen(p,m);remember(f);after();return f; }
static size_t test_write(const void *p,size_t s,size_t n,FILE *f) { if(before())return 0;size_t r=fwrite(p,s,n,f);after();return r; }
static size_t test_read(void *p,size_t s,size_t n,FILE *f) { if(before())return 0;size_t r=fread(p,s,n,f);after();return r; }
static int test_flush(FILE *f) { if(before())return -1;int r=fflush(f);after();return r; }
static int test_sync(int f) { if(before())return -1;int r=fsync(f);after();return r; }
static int test_close(FILE *f) { bool failed=before();forget(f);int r=fclose(f);after();return failed?-1:r; }
static int test_unlink(const char *p) { if(before())return -1;int r=unlink(p);after();return r; }
static int test_rename(const char *a,const char *b) {
 if(before())return -1;struct stat st;
 if(stat(b,&st)==0){++rejected_overwrites;errno=EEXIST;return -1;}
 int r=rename(a,b);after();if(step==fault&&mode==4&&r==0){errno=EIO;return -1;}return r;
}
#define stat(path, info) test_stat(path, info)
#define fopen test_open
#define fwrite test_write
#define fread test_read
#define fflush test_flush
#define fsync test_sync
#define fclose test_close
#define unlink test_unlink
#define rename test_rename
'''
code += '#include "'+str(root/'components/ts_automation/src/ts_action_store.c')+'"\n'
code += r'''
#undef stat
#undef fopen
#undef fwrite
#undef fread
#undef fflush
#undef fsync
#undef fclose
#undef unlink
#undef rename
static const char *old="{\"id\":\"fixture\",\"type\":\"led\",\"name\":\"old\",\"led\":{\"filter\":\"wave\",\"filter_params\":{\"angle\":0}}}";
static const char *next="{\"id\":\"fixture\",\"type\":\"led\",\"name\":\"new\",\"led\":{\"filter\":\"wave\",\"filter_params\":{\"angle\":0,\"speed\":25}}}";
static void put(const char *p,const char *s){FILE*f=fopen(p,"wb");assert(f);assert(fwrite(s,1,strlen(s),f)==strlen(s));assert(!fclose(f));}
static void reset(int origin){
 mode=0;fault=step=0;
 for(unsigned i=0;i<32;i++)if(open_files[i]){fclose(open_files[i]);open_files[i]=NULL;}
 DIR*d=opendir(TS_ACTIONS_DIR);assert(d);struct dirent*e;char p[512];while((e=readdir(d))){if(!strcmp(e->d_name,".")||!strcmp(e->d_name,".."))continue;snprintf(p,sizeof(p),"%s/%s",TS_ACTIONS_DIR,e->d_name);assert(!unlink(p));}closedir(d);
 paths_t files;assert(paths("fixture",&files));
 if(origin&1)put(files.json,old);
 if(origin&2)put(files.pack,"synthetic original encrypted payload");
 put(TS_ACTIONS_DIR "/unrelated.json","untouched");
}
static bool equal_file(const char *p,const char *s){FILE*f=fopen(p,"rb");if(!f)return false;char b[1024];size_t n=fread(b,1,sizeof(b)-1,f);b[n]=0;fclose(f);return !strcmp(b,s);}
static void assert_recovered(int origin,int outcome){
 paths_t p;assert(paths("fixture",&p));
 bool new_version=equal_file(p.json,next),old_version=(origin&1)?equal_file(p.json,old):access(p.json,F_OK)!=0;
 if(origin&2)old_version=old_version&&equal_file(p.pack,"synthetic original encrypted payload");
 else old_version=old_version&&access(p.pack,F_OK)!=0;
 new_version=new_version&&access(p.pack,F_OK)!=0;
 if(!(old_version||new_version)){fprintf(stderr,"bad recovery: origin=%d fault=%d mode=%d outcome=%d\n",origin,fault,mode,outcome);assert(0);}
 if(outcome==ESP_OK)assert(new_version);
 if(outcome==ESP_FAIL)assert(old_version);
 assert(equal_file(TS_ACTIONS_DIR "/unrelated.json","untouched"));
 assert(access(p.pending,F_OK)!=0&&access(p.old_json,F_OK)!=0&&access(p.old_pack,F_OK)!=0);
}
int main(void){
 unsigned cases=0,recovery_cases=0;
 for(int origin=0;origin<4;origin++)for(int injection=1;injection<=4;injection++)for(int point=1;point<80;point++){
  reset(origin);mode=injection;fault=point;step=0;int outcome=999;
  if(!setjmp(shutdown_point))outcome=ts_action_store_save("fixture",next,true);
  mode=0;
  for(unsigned i=0;i<32;i++)if(open_files[i]){fclose(open_files[i]);open_files[i]=NULL;}
  assert(ts_action_store_recover()==ESP_OK);assert_recovered(origin,outcome);++cases;
 }
 /* Reboot recovery can itself be interrupted, including after restoring only
  * one original or deleting only one committed backup. Repeat the real entry. */
 for(int state=0;state<4;state++)for(int injection=1;injection<=4;injection++)for(int point=1;point<60;point++){
  reset(3);paths_t p;assert(paths("fixture",&p));
  assert(!rename(p.json,p.old_json));assert(!rename(p.pack,p.old_pack));
  if(state==0)put(p.pending,next);
  if(state==1)put(p.json,next);
  if(state==2){put(p.pending,next);assert(!rename(p.old_json,p.json));}
  /* state 3: backups remain but committed target is missing. */
  mode=injection;fault=point;step=0;
  if(!setjmp(shutdown_point))(void)ts_action_store_recover();
  mode=0;
  for(unsigned i=0;i<32;i++)if(open_files[i]){fclose(open_files[i]);open_files[i]=NULL;}
  assert(ts_action_store_recover()==ESP_OK);assert_recovered(3,state==1?ESP_OK:ESP_FAIL);++recovery_cases;
 }
 /* Recovery scans all affected IDs while deleting their journal files. */
 reset(0);paths_t p1,p2;assert(paths("fixture",&p1)&&paths("second",&p2));
 put(p1.pending,next);put(p1.old_json,old);put(p1.old_pack,"synthetic original encrypted payload");
 put(p2.pending,"{\"id\":\"second\",\"type\":\"log\"}");put(p2.old_pack,"second original pack");
 assert(ts_action_store_recover()==ESP_OK);assert_recovered(3,ESP_FAIL);
 assert(equal_file(p2.pack,"second original pack")&&access(p2.json,F_OK)!=0&&access(p2.pending,F_OK)!=0&&access(p2.old_pack,F_OK)!=0);
 assert(rejected_overwrites==0);
 reset(3);assert(ts_action_store_save("fixture",next,false)==ESP_OK);paths_t p;assert(paths("fixture",&p));assert(equal_file(p.pack,"synthetic original encrypted payload"));
 assert(ts_action_store_save("fixture",next,true)==ESP_OK);assert(access(p.pack,F_OK)!=0);
 for(int i=0;i<3;i++)assert(ts_action_store_save("fixture",next,true)==ESP_OK);
 assert(ts_action_store_save("fixture","{\"id\":\"wrong\",\"type\":\"led\"}",true)==ESP_ERR_INVALID_ARG);assert(equal_file(p.json,next));
 printf("PASS production action store: %u save and %u recovery I/O/crash boundaries, applied-rename errors, FAT refuses overwrites, JSON/pack/both/first-create, exact old-or-new recovery, multiple IDs, repeated saves, mirror preserves encrypted source, unrelated files retained\n",cases,recovery_cases);
}
'''
with tempfile.TemporaryDirectory(prefix='ts-action-store-') as tmp:
    d=Path(tmp);(d/'actions').mkdir();f=d/'test.c';b=d/'test';f.write_text('#define TS_ACTIONS_DIR "'+str(d/'actions')+'"\n'+code)
    env={**os.environ,'DEVELOPER_DIR':'/Library/Developer/CommandLineTools','ASAN_OPTIONS':'detect_leaks=0'}
    subprocess.run(['cc','-std=c11','-g','-fsanitize=address,undefined','-Wno-deprecated-declarations',
                    '-Itests/certificate/stubs','-Icomponents/ts_automation/include','-I'+str(idf/'components/json/cJSON'),
                    str(f),str(idf/'components/json/cJSON/cJSON.c'),'-o',str(b)],check=True,env=env)
    subprocess.run([str(b)],check=True,env=env)
