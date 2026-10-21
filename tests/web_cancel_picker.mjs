// 定向验证：远端取消及状态轮询不得触发源文件选择器。
import http from 'node:http';
import {readFile} from 'node:fs/promises';
import {pathToFileURL} from 'node:url';
import path from 'node:path';
import assert from 'node:assert/strict';
const {chromium}=await import(pathToFileURL(path.resolve('build/web-qa/node_modules/playwright-core/index.mjs')));
const state={token:'fixture-token',hostId:'host',serverInstance:'fixture-instance',platform:'windows',name:'服务主机',version:'2.0',
  clientName:'测试浏览器',clientIp:'127.0.0.1',peers:[{id:'other',name:'接收客户端',ip:'127.0.0.2',browser:false}],jobs:[],inbox:[],requests:[]};
const requests=[],errors=[];
const server=http.createServer(async(req,res)=>{
  try{
    req.resume();res.setHeader('Content-Type','application/json');
    if(req.url==='/api/info'){res.end(JSON.stringify({protocol:1,id:state.hostId,instance:state.serverInstance}));return;}
    if(req.url==='/web/api/state'){res.end(JSON.stringify(state));return;}
    if(req.url.startsWith('/web/api/')){requests.push(req.url);res.end(req.url.startsWith('/web/api/wait/')?JSON.stringify(state.jobs[0]):'');return;}
    const file=req.url==='/'?'index.html':req.url.replace('/web/','');
    if(!['index.html','style.css','app.js','session.js','sha256.js','files.js','transfers.js','approvals.js','zip_store.js','zip_worker.js','hash_file.js','hash_worker.js','zip_stream_worker.js'].includes(file)){res.writeHead(404).end();return;}
    let body=await readFile(path.join('src/web',file),'utf8');
    if(file==='app.js')body=body.replace('poll();setInterval(poll,2000);',`await poll();globalThis.cancelTest={
      poll,
      set(wait){operations.length=0;webTransferWait=wait;const batch={id:'folder',name:'资料',count:1,total:100};
        const op={key:'upload',file:{size:100},path:'资料/a.bin',batch,target:'other',fileId:'10',forwardId:'11',finished:false,status:'等待接收'};
        op.retryPending=true;op.retryTimer=setTimeout(()=>{op.error=true;op.status='错误的重试超时';},150);
        operations.push(op);renderJobs();},
      wait(){return waitForForward(operations[0]);},
      operation(){const op=operations[0];return {cancelled:op.cancelled,rejected:op.rejected,retryPending:op.retryPending,error:op.error,status:op.status};}
    };`);
    res.setHeader('Content-Type',file.endsWith('.js')?'text/javascript':file.endsWith('.css')?'text/css':'text/html');res.end(body);
  }catch(error){res.writeHead(500).end(String(error));}
});
await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));let browser;
try{
  browser=await chromium.launch({channel:'chrome',headless:true,timeout:15000});
  const page=await browser.newPage();let choosers=0;
  page.on('filechooser',()=>choosers++);page.on('pageerror',error=>errors.push(error.message));
  await page.addInitScript(()=>{globalThis.directoryPickers=0;window.showDirectoryPicker=async()=>{directoryPickers++;throw new DOMException('cancel','AbortError');};});
  await page.goto('http://127.0.0.1:'+server.address().port);await page.waitForFunction(()=>!!globalThis.cancelTest);
  for(const wait of [false,true])for(const endState of [4,6]){
    const job={id:'11',uploadKey:'upload',name:'资料/a.bin',peer:'接收客户端',total:100,done:10,speed:0,state:2,stateText:'传输中',
      receiving:false,stage:'forward',folderId:'folder',folderName:'资料',folderCount:1,folderTotal:100};
    state.jobs=[job];await page.evaluate(()=>cancelTest.poll());await page.evaluate(wait=>cancelTest.set(wait),wait);
    state.jobs=[{...job,state:endState,stateText:endState===4?'已取消':'已拒绝'}];
    await page.evaluate(()=>cancelTest.wait());
    for(let i=0;i<3;i++)await page.evaluate(()=>cancelTest.poll());
    await page.waitForTimeout(200);
    const op=await page.evaluate(()=>cancelTest.operation());
    assert.equal(endState===4?op.cancelled:op.rejected,true);assert.equal(op.retryPending,false);assert.equal(op.error,false);
    assert.equal(choosers,0,'remote termination never opens an input file chooser');
    assert.equal(await page.evaluate(()=>directoryPickers),0,'remote termination never opens a directory picker');
    assert.equal(await page.locator('#jobs .retry').count(),1);
  }
  // 原文件已失效时，重试只提供显式重新选择操作。
  await page.locator('#jobs .retry').click();
  const reselect=page.getByRole('button',{name:'重新选择文件夹',exact:true});await reselect.waitFor();
  assert.equal(choosers,0,'retry fallback does not automatically open a chooser');
  await page.evaluate(()=>cancelTest.poll());await reselect.waitFor();
  await Promise.all([page.waitForEvent('filechooser'),reselect.click()]);assert.equal(choosers,1);
  assert.equal(await page.locator('#folder-picker').getAttribute('data-retry-target'),'other','reselection retains original target');
  assert(!requests.some(url=>url.includes('/upload')||url.includes('/retry-upload')),'cancel does not restart upload');
  assert.deepEqual(errors,[]);console.log('PASS remote cancel/reject via polling/wait, stopped retry timer, no automatic picker, explicit folder reselection');
}finally{await browser?.close();await new Promise(resolve=>server.close(resolve));}
