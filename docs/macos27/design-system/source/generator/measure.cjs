const {chromium}=require('playwright');const fs=require('fs');
(async()=>{const b=await chromium.launch({executablePath:'/opt/pw-browsers/chromium'});const out={};
for(const n of ['Sheets1','Sheets2','SheetsAuto','SheetsLed','SheetsSec','SheetsStates','SheetsTypes','SheetsMore']){const p=await b.newPage({viewport:{width:1440,height:1200}});
await p.goto('file://'+__dirname+'/dc/t-'+n+'.html');await p.waitForTimeout(300);
out[n]=await p.evaluate(()=>Math.ceil(document.querySelector('.flow-wrap').getBoundingClientRect().height)+0);
}
fs.writeFileSync(__dirname+'/h2.json',JSON.stringify(out));console.log(out);await b.close()})();
