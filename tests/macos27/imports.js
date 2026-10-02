async (page) => {
 const origin=page.url().split('/').slice(0,3).join('/');const phase=origin.endsWith('18783')?'after':'baseline';const results=[];
 for(const width of [390,1440])for(const [route,handler,id]of [
 ['/automation','showImportSourceModal()','source'],['/automation','showImportRuleModal()','rule'],['/automation','showImportActionModal()','action'],['/commands','showImportSshCommandModal()','ssh-cmd'],['/security','showImportSshHostModal()','ssh-host']]){
  await page.setViewportSize({width,height:900});await page.goto(origin+'/?state=populated&lang=en-US#'+route);await page.reload();await page.waitForTimeout(400);
  await page.locator('[onclick='+JSON.stringify(handler)+']').filter({visible:true}).first().click();
  for(const valid of [true,false]){
   await page.locator('#import-'+id+'-file').setInputFiles('/private/tmp/macos27-'+(valid?'valid':'invalid')+'.tscfg');await page.waitForTimeout(250);
   results.push({width,id,valid,...await page.evaluate(id=>({innerWidth,overflow:document.documentElement.scrollWidth>innerWidth,disabled:document.getElementById('import-'+id+'-btn').disabled,result:document.getElementById('import-'+id+'-result').textContent}),id)});
   await page.screenshot({path:'output/macos27-20260928/'+phase+'/import-'+id+'-'+valid+'-'+width+'.png'});
  }
 }
 return results;
}
