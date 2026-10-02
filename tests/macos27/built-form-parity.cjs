// Compare the actual built stylesheet and form controls with their source rendering.
const fs=require('node:fs'),assert=require('node:assert/strict');
const {chromium}=require('/Users/massif/.cache/codex-runtimes/codex-primary-runtime/dependencies/node/node_modules/playwright');
const out='output/macos27-20260929-card-revision';
(async()=>{const browser=await chromium.launch({channel:'chrome',headless:true}),rows=[];try{
const page=await browser.newPage();
const css=await page.evaluate(({source,built})=>{
 const parse=text=>{const sheet=new CSSStyleSheet();sheet.replaceSync(text);const walk=rules=>[...rules].flatMap(r=>r.selectorText?[r.selectorText]:r.cssRules?walk(r.cssRules):[]);return walk(sheet.cssRules)};
 return{source:parse(source),built:parse(built)};
},{source:fs.readFileSync('components/ts_webui/web/css/style.css','utf8'),built:fs.readFileSync(out+'/after/web_optimized/css/style.css','utf8')});
assert(css.source.length>100);assert.deepEqual(css.built,css.source);rows.push({name:'stylesheet-selector-parity',count:css.source.length,status:'PASS'});
for(const width of [320,390,1440]){
 await page.setViewportSize({width,height:1000});const values=[];
 for(const port of [18783,18789]){
 await page.goto('http://127.0.0.1:'+port+'/?state=populated#/security');await page.locator('#root-new-password').waitFor();await page.waitForTimeout(150);
 const data=await page.locator('#root-new-password').evaluate(e=>{const s=getComputedStyle(e);return{font:s.fontSize,fontFamily:s.fontFamily,radius:s.borderRadius,width:e.getBoundingClientRect().width,parentWidth:e.parentElement.getBoundingClientRect().width,padding:s.padding,overflow:document.documentElement.scrollWidth>innerWidth}});values.push(data);
 if(port===18789)await page.screenshot({path:out+'/after/fixed-built-security-'+width+'.png'});
 }
 assert.deepEqual(values[1],values[0]);assert.equal(values[1].font,'14px');assert.equal(values[1].width,values[1].parentWidth);assert.equal(values[1].overflow,false);rows.push({name:'password-form',width,source:values[0],built:values[1],status:'PASS'});
}
}finally{fs.writeFileSync(out+'/built-form-parity.json',JSON.stringify(rows,null,2));await browser.close()}})().catch(e=>{console.error(e);process.exitCode=1});
