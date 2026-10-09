"""Exercise the production variable expander, including exact capacities."""
from pathlib import Path
import os, re, subprocess
source = Path('components/ts_automation/src/ts_action_manager.c').read_text()
start = source.index('int ts_action_expand_variables(')
function = source[start:source.index('\n}', start)+2]
code = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "ts_automation_types.h"
static ts_auto_value_t value;
static int ts_variable_get(const char *name,ts_auto_value_t *out) {
 if(strcmp(name,"value"))return ESP_ERR_NOT_FOUND;
 *out=value;return ESP_OK;
}
''' + function + r'''
int main(void){
 char out[1024];value.type=TS_AUTO_VAL_STRING;
 for(int n=62;n<=63;n++){
  memset(value.str_val,'X',n);value.str_val[n]=0;
  assert(ts_action_expand_variables("'${value}'",out,sizeof(out))==n+2);
  assert(strlen(out)==n+2&&out[0]=='\''&&out[n+1]=='\'');
  assert(ts_action_expand_variables("${value}",out,n+1)==n&&strlen(out)==n);
  assert(ts_action_expand_variables("${value}",out,n)==-1&&out[0]==0);
 }
 const char *literal="bash -c 'printf \"%s\" \"${HOME}/x\"; echo ${PATH:-/bin}'";
 assert(ts_action_expand_variables(literal,out,strlen(literal)+1)==strlen(literal)&&!strcmp(out,literal));
 char long_name[100];memset(long_name,'a',sizeof(long_name));long_name[0]='$';long_name[1]='{';long_name[98]='}';long_name[99]=0;
 assert(ts_action_expand_variables(long_name,out,100)==99&&!strcmp(out,long_name));
 char input[1024];memset(input,'a',1000);strcpy(input+1000,"${value}");
 assert(ts_action_expand_variables(input,out,sizeof(out))==-1&&out[0]==0);
 assert(ts_action_expand_variables("",out,1)==0&&out[0]==0);
 value.type=TS_AUTO_VAL_INT;value.int_val=123;
 assert(ts_action_expand_variables("${value}",out,4)==3&&!strcmp(out,"123"));
 puts("PASS production expansion: 62/63 chars, exact fit, overflow rejects empty, unknown and Shell expressions preserved, long names, numeric values");
}
'''
build=Path('/tmp/tianshan-runtime-tests');build.mkdir(exist_ok=True)
(build/'expansion.c').write_text(code)
env={**os.environ,'DEVELOPER_DIR':'/Library/Developer/CommandLineTools'}
subprocess.run(['cc','-D_POSIX_C_SOURCE=200809L','-std=c11','-g','-fsanitize=address,undefined','-ftrivial-auto-var-init=pattern','-Itests/runtime/stubs','-Itests/certificate/stubs','-Icomponents/ts_automation/include','-Icomponents/ts_security/include',str(build/'expansion.c'),'-o',str(build/'expansion')],check=True,env=env)
subprocess.run([str(build/'expansion')],check=True,env=env)
