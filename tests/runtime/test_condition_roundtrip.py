#!/usr/bin/env python3
"""Compare typed browser payloads through the real codec and condition evaluator."""
from pathlib import Path
import json, os, subprocess, sys, tempfile
root=Path(__file__).resolve().parents[2]
os.chdir(root)
idf=Path(os.environ.get('IDF_PATH','/Users/massif/esp/v5.5.2/esp-idf'))
source=Path('components/ts_automation/src/ts_rule_engine.c').read_text()
def function(signature):
    start=source.index(signature)
    return source[start:source.index('\n}',start)+2]
if len(sys.argv)>1:
    pairs=json.loads(Path(sys.argv[1]).read_text())
else:
    pairs=[]
    for value,typ in [('ready',4),('0',4),('true',4),('',4),('a"b\'\\c<&>\n',4),('&quot;ready&quot;',4),(' ready ',4),(0,2),(-42,2),(1.25,3),(1,3),(True,1),(False,1),(None,0),('a'*63,4),('中'*20,4)]:
        cond=dict(variable='model.status',operator='ne',value=value,value_type=typ)
        rule=dict(id='fixture',name='Fixture',enabled=False,manual_trigger=False,logic='or',conditions=[cond],actions=[dict(type='log',message='fixture',condition=cond)])
        pairs.append(dict(original=rule,saved=rule))
assert pairs, 'No roundtrip evidence supplied'
code=r'''
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ts_rule_codec.h"
#include "ts_action_manager.h"
#define ESP_LOGW(...) ((void)0)
#define TAG "test"
void *heap_caps_malloc(size_t n,unsigned caps){return malloc(n);}
void *heap_caps_calloc(size_t n,size_t s,unsigned caps){return calloc(n,s);}
esp_err_t ts_action_template_get(const char *id,ts_action_template_t *tpl){return ESP_ERR_NOT_FOUND;}
static ts_auto_value_t observed;
esp_err_t ts_variable_get(const char *name,ts_auto_value_t *out){*out=observed;return ESP_OK;}
'''
code+=function('static int compare_values(const ts_auto_value_t *a, const ts_auto_value_t *b)\n{')+'\n'
code+=function('bool ts_rule_eval_condition(const ts_auto_condition_t *condition)\n{')+'\n'
code+=r'''
static unsigned checks;
static void compare(const ts_auto_condition_t *a,const ts_auto_condition_t *b){
 assert(!strcmp(a->variable,b->variable));assert(a->op==b->op);assert(!memcmp(&a->value,&b->value,sizeof(a->value)));
 ts_auto_value_t candidates[8]={a->value,{.type=TS_AUTO_VAL_NULL},{.type=TS_AUTO_VAL_BOOL,.bool_val=false},{.type=TS_AUTO_VAL_INT,.int_val=0},{.type=TS_AUTO_VAL_FLOAT,.float_val=1.0},{.type=TS_AUTO_VAL_STRING,.str_val="ready"},{.type=TS_AUTO_VAL_STRING,.str_val="prefix-ready-suffix"},{.type=TS_AUTO_VAL_STRING,.str_val="0"}};
 for(int op=0;op<=TS_AUTO_OP_CONTAINS;op++)for(unsigned i=0;i<8;i++){
  ts_auto_condition_t lhs=*a,rhs=*b;lhs.op=rhs.op=op;observed=candidates[i];
  assert(ts_rule_eval_condition(&lhs)==ts_rule_eval_condition(&rhs));checks++;
 }
}
int main(int argc,char **argv){
 assert(argc==2);FILE*f=fopen(argv[1],"rb");assert(f);fseek(f,0,SEEK_END);long len=ftell(f);rewind(f);char*text=calloc(len+1,1);assert(fread(text,1,len,f)==(size_t)len);fclose(f);
 cJSON*root=cJSON_Parse(text);free(text);assert(cJSON_IsArray(root));cJSON*pair;
 cJSON_ArrayForEach(pair,root){
  ts_auto_rule_t a,b,c;assert(ts_rule_decode(cJSON_GetObjectItem(pair,"original"),&a)==ESP_OK);assert(ts_rule_decode(cJSON_GetObjectItem(pair,"saved"),&b)==ESP_OK);
  assert(a.conditions.count==b.conditions.count&&a.conditions.logic==b.conditions.logic&&a.manual_trigger==b.manual_trigger);
  for(unsigned i=0;i<a.conditions.count;i++)compare(&a.conditions.conditions[i],&b.conditions.conditions[i]);
  assert(a.action_count==b.action_count);for(unsigned i=0;i<a.action_count;i++){
   assert(a.actions[i].condition.has_condition==b.actions[i].condition.has_condition);
   if(a.actions[i].condition.has_condition){ts_auto_condition_t ca={0},cb={0};strcpy(ca.variable,a.actions[i].condition.variable);strcpy(cb.variable,b.actions[i].condition.variable);ca.op=a.actions[i].condition.op;cb.op=b.actions[i].condition.op;ca.value=a.actions[i].condition.value;cb.value=b.actions[i].condition.value;compare(&ca,&cb);}
  }
  cJSON*encoded=ts_rule_encode(&b);assert(encoded&&ts_rule_decode(encoded,&c)==ESP_OK);
  assert(!memcmp(b.conditions.conditions,c.conditions.conditions,b.conditions.count*sizeof(*b.conditions.conditions)));
  assert(!memcmp(b.actions,c.actions,b.action_count*sizeof(*b.actions)));
  ts_rule_dispose(&a);ts_rule_dispose(&b);ts_rule_dispose(&c);cJSON_Delete(encoded);
 }
 // String-like numbers and booleans retain the evaluator's existing mismatch semantics.
 ts_auto_condition_t condition={.op=TS_AUTO_OP_EQ,.value={.type=TS_AUTO_VAL_STRING,.str_val="0"}};
 observed=(ts_auto_value_t){.type=TS_AUTO_VAL_INT,.int_val=0};assert(!ts_rule_eval_condition(&condition));
 observed=(ts_auto_value_t){.type=TS_AUTO_VAL_STRING,.str_val="0"};assert(ts_rule_eval_condition(&condition));
 printf("PASS typed conditions: %d rule pairs, %u unchanged comparison results, real codec encode/decode\n",cJSON_GetArraySize(root),checks);cJSON_Delete(root);
}
'''
with tempfile.TemporaryDirectory(prefix='tianshan-condition-') as tmp:
    cfile=Path(tmp)/'condition.c';binary=Path(tmp)/'condition';fixtures=Path(tmp)/'pairs.json'
    cfile.write_text(code);fixtures.write_text(json.dumps(pairs,ensure_ascii=False))
    env=dict(os.environ,DEVELOPER_DIR=os.environ.get('DEVELOPER_DIR','/Library/Developer/CommandLineTools'))
    args=['cc','-std=c11','-g','-fsanitize=address,undefined','-Wno-deprecated-declarations','-Itests/runtime/stubs','-Itests/certificate/stubs','-Icomponents/ts_automation/include','-Icomponents/ts_security/include','-I'+str(idf/'components/json/cJSON'),str(cfile),'components/ts_automation/src/ts_rule_codec.c','components/ts_automation/src/ts_action_filter.c',str(idf/'components/json/cJSON/cJSON.c'),'-lm','-o',str(binary)]
    subprocess.run(args,check=True,env=env);subprocess.run([str(binary),str(fixtures)],check=True,env=env)
