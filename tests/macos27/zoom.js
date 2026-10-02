async(page)=>{
 await page.setViewportSize({width:1440,height:1000});await page.goto('http://127.0.0.1:18783/?state=populated#/');await page.waitForTimeout(400);
 const read=()=>page.evaluate(()=>({innerWidth,innerHeight,outerWidth,outerHeight,dpr:devicePixelRatio,scale:visualViewport.scale,overflow:document.documentElement.scrollWidth>innerWidth}));
 const before=await read();await page.keyboard.press('Meta+0');for(let i=0;i<4;i++)await page.keyboard.press('Meta+Equal');await page.waitForTimeout(300);const after=await read();await page.screenshot({path:'output/macos27-20260928/after/zoom-shortcut-attempt.png'});await page.keyboard.press('Meta+0');return {before,after,status:after.dpr/before.dpr===2?'PASS':'NOT_RUN',note:'Only actual DPR and layout viewport change counts as browser zoom; unchanged shortcut is NOT_RUN'};
}
