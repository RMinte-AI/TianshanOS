// Pass the actual ESP-IDF web_optimized directory; do not substitute source assets.
const fs=require('node:fs'),path=require('node:path'),assert=require('node:assert/strict'),{test}=require('node:test');
const harness=require('./harness.cjs');
assert(process.env.TS_PACKAGED_WEB&&path.isAbsolute(process.env.TS_PACKAGED_WEB),'Set TS_PACKAGED_WEB to the built web_optimized directory');
harness.root=process.env.TS_PACKAGED_WEB;
assert(fs.existsSync(path.join(harness.root,'index.html')),'Build the firmware web assets first');
const kit=require('./pagekit.cjs'),{serviceFixture,OLD}=require('./service-runs-fixture.cjs');
kit.install();
test('packaged resources share a content version and retain configuration/run controls',async()=>{
 const d=serviceFixture();
 await kit.withPage('en-US',d.respond,async({page})=>{
  const version=await page.evaluate(()=>window.TS_ASSET_VERSION);
  assert.match(version,/^[0-9a-f]{16}$/);
  const urls=await page.evaluate(()=>performance.getEntriesByType('resource').map(entry=>entry.name));
  for(const asset of ['/js/api.js','/js/app.js','/js/lang/en-US.js','/css/style.css']){
   assert(urls.some(url=>new URL(url).pathname===asset&&new URL(url).searchParams.get('v')===version),asset);
  }
  await page.evaluate(()=>loadAutomationPage());await page.evaluate(()=>showAutomationConfig());
  assert.match(await page.locator('#automation-config-modal').innerText(),/Configuration & backup/);
  await page.locator('#automation-config-modal .sf button').click();
  await page.evaluate(()=>showServiceRuns());
  await page.locator(`[data-run="${OLD}"] [data-run-action="verify"]`).click();
  await page.waitForFunction(id=>serviceRuns.get(id)?.state==='running',OLD);
  assert.equal(d.state.controls.length,1);assert.equal(d.state.controls[0].run_id,OLD);
 });
});
