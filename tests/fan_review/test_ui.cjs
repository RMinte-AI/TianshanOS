const assert = require('node:assert/strict');
const fs = require('node:fs');
const {harness} = require('../prompts/harness.cjs');
const input = process.argv[2];
const probe = process.argv.includes('--probe');
const records = fs.readFileSync(input, 'utf8').trim().split('\n').map(JSON.parse);
(async () => {
 for (const lang of ['zh-CN', 'en-US']) {
  const h=harness(lang);await h.ready();h.load();h.el('fans-grid');h.el('fan-global-duty');h.el('fan-global-temp');
  const checks=[];
  function test(name, fn) {try {fn();checks.push({name,passed:true});}catch(e){checks.push({name,passed:false,error:e.message});if(!probe)throw e;}}
  for(const r of records.filter(r=>r.case.startsWith('R2_'))) {
   h.ctx.fixture={fans:[r.fan]};h.run('updateFanInfo(fixture)');
   const known=r.case!=='R2_configure_fail';const expected=known?(r.case==='R2_inverted'?100-r.hal_output:r.hal_output):'--';
   test(r.case,()=>{assert(h.el('fans-grid').innerHTML.includes(`fan-speed-num">${expected}</span>`));assert.equal(h.el('fan-global-duty').textContent,`${expected}%`);});
  }
  for(const mode of ['auto','curve','manual','off']){
   h.ctx.fixture={fans:[{id:0,mode,target_duty:70,enabled:true}]};h.run('updateFanInfo(fixture)');
   test(`unknown_${mode}`,()=>{assert.equal(h.el('fan-global-duty').textContent,'--%');assert(h.el('fans-grid').innerHTML.includes('fan-slider-value">--%</span>'));});
  }
  const num=h.el('test-big');num.textContent='37';const value=h.el('test-slider-value');value.textContent='37%';
  const slider=h.el('fan-slider-0');slider.parentElement={style:{setProperty(){}}};slider.closest=()=>({querySelector:s=>s==='.fan-speed-num'?num:s==='.fan-slider-value'?value:null});
  h.run('updateFanSliderUI(0,70)');test('draft_does_not_replace_output',()=>assert.equal(num.textContent,'37'));
  h.ctx.fixture={fans:[{id:0,mode:'manual',duty:37,duty_valid:true,target_duty:37,enabled:true}]};h.run('updateFanInfo(fixture)');
  if(!probe){test('draft_survives_poll',()=>{assert(h.el('fans-grid').innerHTML.includes(lang==='zh-CN'?'准备设置为 70%':'Ready to set 70%'));assert.equal(h.el('fan-global-duty').textContent,'37%');});}
  console.log(JSON.stringify({language:lang,checks}));
 }
})().catch(e=>{console.error(e);process.exitCode=1;});
