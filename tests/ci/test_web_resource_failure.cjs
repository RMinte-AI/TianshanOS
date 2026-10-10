// One negative against the emitted directory and the existing real browser test.
const {test}=require('node:test');
const assert=require('node:assert/strict'),fs=require('node:fs'),os=require('node:os'),path=require('node:path'),{spawnSync}=require('node:child_process');
test('missing built stylesheet blocks the actual offline browser scenario',()=>{
 const source=process.env.PROJECT_WEB_ROOT;assert(source,'Set PROJECT_WEB_ROOT to this build output');
 const temp=fs.mkdtempSync(path.join(os.tmpdir(),'ts-built-resource-'));
 try{
  const root=path.join(temp,'web');fs.cpSync(source,root,{recursive:true});fs.unlinkSync(path.join(root,'css/style.css'));
  // A nested test runner must not inherit the parent's worker context.
  const env={...process.env,PROJECT_WEB_ROOT:root,OFFLINE_RESULTS:path.join(temp,'results')};
  delete env.NODE_TEST_CONTEXT;
  const result=spawnSync(process.execPath,['--test','--test-name-pattern','en-US:','tests/prompts/xterm-offline.test.cjs'],{
   env,encoding:'utf8',timeout:60000});
  assert.equal(result.error,undefined);assert.notEqual(result.status,0);
  assert((result.stdout+result.stderr).includes('Unexpected resource failures'),result.stdout+result.stderr);
 }finally{fs.rmSync(temp,{recursive:true,force:true});}
});
