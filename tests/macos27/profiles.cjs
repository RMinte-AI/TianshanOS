// Synthetic UI branches only. No real host, key, certificate, endpoint or device.
const host={id:'fixture-host',host:'192.0.2.30',port:22,username:'fixture',key_id:'fixture-key'};
const command={id:'fixture-command',host_id:host.id,name:'Fixture command',command:'echo fixture-only',desc:'Synthetic display data',expectPattern:'fixture',timeout:30};
const source={id:'fixture-source',label:'Fixture source',type:'http',enabled:true,poll_interval_ms:5000};
const rule={id:'fixture-rule',name:'Fixture rule',enabled:true,manual_trigger:true,show_on_dashboard:true,conditions_count:1,actions_count:1,trigger_count:2};
const action={id:'fixture-action',name:'Fixture action',type:'log',async:true,description:'Synthetic UI data',log:{level:3,message:'fixture-only'}};
const widgets = ['ring','gauge','temp','number','bar','text','status','icon','dual','percent','log'].map(type => ({
 id:'fixture-'+type,type,label:'Fixture '+type,expression:'${fixture.value}',expression2:'${fixture.value}',
 min:0,max:100,unit:type==='temp'?'°C':'',color:'#4dabf7',decimals:1,
 thresholds:[0,50,80],colors:['#40c057','#fab005','#fa5252'],
 host_id:'fixture-host',path:'/tmp/fixture.log',maxLines:15
}));
module.exports={
 'ui/widgets/get':{widgets,refresh_interval:5000,source:'sdcard'},
 'config/pack/info':{can_export:true,device_type:'Developer',cert_cn:'fixture.invalid',pack_version:1},
 'network/wifi/mode':{mode:'apsta'},
 'ssh/hosts/list':{hosts:[host]},
 'ssh/commands/list':{commands:[command,{...command,id:'fixture-orphan',host_id:'missing-fixture-host',name:'Fixture orphan',orphan:true}]},
 'automation/status':{state:'running',rules_count:1,sources_count:1,variables_count:1,rule_triggers:2,uptime_ms:3600000},
 'automation/sources/list':{sources:[source],loaded:true},
 'automation/rules/list':{rules:[rule],loaded:true},
 'automation/actions/list':{templates:[action],loaded:true},
 'automation/actions/get':{template:action},
 'automation/rules/get':{rule:{...rule,conditions:[{variable:'fixture.value',operator:'>',value:1}],actions:[{template_id:action.id}],cooldown_ms:1000}},
 'automation/variables/list':{variables:[{name:'fixture.value',value:42,type:'number',age_ms:1000}]},
 'key/list':{keys:[{id:'fixture-key',type:'ed25519',comment:'Synthetic key row only',exportable:true,has_pubkey:true}]},
 'ssh/key/list':{keys:[{id:'fixture-key',type:'ed25519',comment:'Synthetic key row only',exportable:true,has_pubkey:true}]},
 'ssh/known_hosts/list':{hosts:[{host:'192.0.2.30',port:22,type:'ssh-ed25519',fingerprint:'SHA256:fixture-only'}]},
 'system/memory_detail':{
  dram:{total:327680,used:294912,free:32768,used_percent:90,largest_block:4096,fragmentation:65,min_free_ever:28672},
  psram:{total:8388608,used:6291456,free:2097152,used_percent:75,largest_block:1048576,fragmentation:45},
  dma:{total:32768,free:8192,largest_block:4096},
  static:{data_size:1024,bss_size:4096,rodata_size:8192,total_dram_static:5120},
  iram:{text_size:8192,heap_total:16384,heap_free:8192},rtc:{total_available:8192,total_used:1024},
  nvs:{total_entries:126,used_entries:63,free_entries:63,used_percent:50,namespace_count:2},
  tasks:[{name:'fixture-task',stack_hwm:128,stack_alloc:4096,stack_used:3968,stack_usage_pct:97,priority:5,state:'Running',cpu_percent:20}],
  task_count:1,total_stack_allocated:4096,tips:['critical:dram_low','warning:dram_fragmented','info:psram_sufficient']
 }
};
