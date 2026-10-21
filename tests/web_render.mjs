// 真实 Chromium 页面验证增量渲染、操作对象一致性及响应式布局。
// 依赖：npm install --prefix build/web-qa --no-save --package-lock=false playwright-core
// 运行：node tests/web_render.mjs <结果目录> [基线 app.js]
import http from 'node:http';
import {readFile,mkdir,writeFile} from 'node:fs/promises';
import {pathToFileURL} from 'node:url';
import path from 'node:path';
import assert from 'node:assert/strict';
const {chromium}=await import(pathToFileURL(path.resolve('build/web-qa/node_modules/playwright-core/index.mjs')));
const work=path.resolve(process.argv[2]),baseline=!!process.argv[3];await mkdir(work,{recursive:true});
const commands=[],errors=[];
const state={token:'fixture-token',clientName:'测试浏览器',clientIp:'127.0.0.1',hostId:'host',serverInstance:'fixture-instance',platform:'windows',androidAvailable:true,name:'接收电脑',version:'1.0.0',autoReceive:true,peers:[{id:'other',name:'另一台电脑',ip:'192.168.1.20',browser:false}],jobs:[],inbox:[],requests:[]};
const server=http.createServer(async(req,res)=>{
  try{
    if(req.url==='/api/info'){res.setHeader('Content-Type','application/json');res.end(JSON.stringify({protocol:1,id:state.hostId,instance:'fixture-instance'}));return;}
    if(req.url.startsWith('/web/api/')){
      if(req.method==='POST')commands.push(req.url);
      req.resume();res.setHeader('Content-Type','application/json');res.end(req.url==='/web/api/state'?JSON.stringify(state):'');return;
    }
    const file=req.url==='/'?'index.html':req.url.replace('/web/','');
    if(!['index.html','app.js','style.css','files.js','transfers.js','sha256.js','session.js','zip_store.js','zip_worker.js','hash_file.js','hash_worker.js','zip_stream_worker.js','approvals.js'].includes(file)){res.writeHead(404);res.end();return;}
    let body=await readFile(file==='app.js'&&baseline?process.argv[3]:path.join('src/web',file),'utf8');
    if(file==='app.js')body=body.replace('poll();setInterval(poll,2000);',`await poll();
      globalThis.renderTest={
        set(jobs,files=[]){latestJobs=jobs;inbox=files;renderJobs();},
        destination(batch){folderDestinations.set(batch,{});renderJobs();}
      };`);
    res.setHeader('Content-Type',file.endsWith('.js')?'text/javascript':file.endsWith('.css')?'text/css':'text/html');res.end(body);
  }catch(error){res.writeHead(500);res.end(String(error));}
});
await new Promise(r=>server.listen(0,'127.0.0.1',r));
let browser;
try{
  browser=await chromium.launch({channel:'chrome',headless:true,timeout:15000});
  const page=await browser.newPage({viewport:{width:1280,height:900}});page.on('pageerror',error=>errors.push(error.message));
  await page.goto('http://127.0.0.1:'+server.address().port);await page.waitForFunction(()=>!!globalThis.renderTest);
  const job=(id,extra={})=>({id,name:'资料-'+id+'.bin',peer:'测试电脑',total:1048576,done:262144,speed:1048576,state:2,stateText:'传输中',receiving:false,uploadKey:'',...extra});
  if(!baseline){
    await page.locator('.device').first().click();assert.equal(await page.locator('#receive-mode').textContent(),'自动接收：开');
    await page.locator('.device').nth(1).click();assert.equal(await page.locator('#receive-mode').textContent(),'');
    const first=job(1),second=job(2);await page.evaluate(jobs=>renderTest.set(jobs),[first,second]);
    await page.locator('#jobs button').first().focus();
    await page.evaluate(()=>{globalThis.savedCancel=document.activeElement;globalThis.savedRow=document.querySelector('#jobs .row');});
    await page.evaluate(jobs=>renderTest.set(jobs),[{...first,done:524288,speed:2097152},{...second,state:3,stateText:'已完成'}]);
    assert(await page.evaluate(()=>document.activeElement===savedCancel&&document.querySelector('#jobs .row')===savedRow),'progress and unrelated completion preserve focus and row');
    assert.equal(await page.locator('#jobs progress').evaluate(element=>element.value),50);
    assert((await page.locator('#jobs .progress-metrics').first().textContent()).includes('50%'));
    await page.locator('#jobs button').first().click();await page.waitForFunction(()=>document.querySelector('#jobs .empty'));assert(commands.includes('/web/api/cancel/1'));
    const folder={folderId:'folder',folderName:'设计资料',folderCount:2,folderTotal:2097152,batchId:'batch'};
    await page.evaluate(jobs=>renderTest.set(jobs),[job(10,folder)]);
    await page.evaluate(jobs=>renderTest.set(jobs),[job(10,folder),job(11,folder)]);
    await page.locator('#jobs').getByRole('button',{name:'取消',exact:true}).click();await page.waitForFunction(()=>document.querySelector('#jobs .empty'));assert(commands.includes('/web/api/cancel/10')&&commands.includes('/web/api/cancel/11'),'folder action uses latest children');
    const receive=job(20,{...folder,receiving:true});await page.evaluate(data=>renderTest.set(data.jobs,data.files),{jobs:[receive],files:[{id:20}]});
    assert.equal(await page.getByRole('button',{name:'保存文件夹',exact:true}).count(),1);
    await page.evaluate(()=>renderTest.destination('batch'));assert.equal(await page.getByRole('button',{name:'保存文件夹',exact:true}).count(),0);
    await page.evaluate(data=>renderTest.set(data.jobs,data.files),{jobs:[job(30)],files:[{id:30,received:false}]});
    assert.equal(await page.getByRole('link',{name:'下载',exact:true}).count(),1);
    await page.evaluate(data=>renderTest.set(data.jobs,data.files),{jobs:[job(30)],files:[{id:30,received:true}]});
    assert.equal(await page.getByRole('link',{name:'重新下载',exact:true}).count(),1);
    await page.evaluate(()=>renderTest.set([]));assert.equal(await page.locator('#jobs .row').count(),0);assert.equal(await page.locator('#jobs .empty').count(),1);
    await page.evaluate(()=>{globalThis.emptyNode=document.querySelector('#jobs .empty');renderTest.set([]);});
    assert(await page.evaluate(()=>emptyNode===document.querySelector('#jobs .empty')),'idle empty state retained');
  }
  const measurements=await page.evaluate(()=>{
    const jobs=Array.from({length:500},(_,id)=>({id,name:'文件-'+id+'.bin',peer:'设备',total:10485760,done:100,speed:1024,state:2,stateText:'传输中',receiving:false,uploadKey:''}));
    renderTest.set(jobs);const host=document.getElementById('jobs'),original=[...host.children];let added=0,removed=0;
    const observer=new MutationObserver(()=>{});observer.observe(host,{childList:true,subtree:true});
    const durations=[];
    for(let i=0;i<30;i++){
      jobs[0]={...jobs[0],done:200+i,speed:2000+i};const start=performance.now();renderTest.set(jobs);durations.push(performance.now()-start);
      for(const record of observer.takeRecords())if(record.target===host){added+=record.addedNodes.length;removed+=record.removedNodes.length;}
    }
    observer.disconnect();durations.sort((a,b)=>a-b);
    return {rows:jobs.length,updates:30,medianMs:durations[15],p95Ms:durations[28],addedTopLevelNodes:added,removedTopLevelNodes:removed,retainedRows:original.filter((row,i)=>host.children[i]===row).length};
  });
  if(!baseline){assert.equal(measurements.addedTopLevelNodes,0);assert.equal(measurements.removedTopLevelNodes,0);assert.equal(measurements.retainedRows,500);}
  await page.evaluate(jobs=>renderTest.set(jobs),[job(1,{name:'设计资料与测试数据.bin'}),job(2,{name:'操作说明.pdf',state:3,stateText:'已完成',done:1048576}),job(3,{folderId:'design',folderName:'设计资料',folderCount:5,folderTotal:5242880})]);
  await page.screenshot({path:path.join(work,'desktop.png'),fullPage:true});
  await page.setViewportSize({width:390,height:844});await page.screenshot({path:path.join(work,'mobile.png'),fullPage:true});
  assert(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth),'mobile layout has no horizontal overflow');
  assert.deepEqual(errors,[],'no browser runtime errors');
  await writeFile(path.join(work,'results.json'),JSON.stringify({baseline,...measurements},null,2));
  console.log('PASS actual Chromium rendering, responsive layout'+(baseline?' baseline':' focus, current action targets, download labels and empty-state lifecycle'));
  console.log(JSON.stringify({baseline,...measurements}));
}finally{await browser?.close();await new Promise(r=>server.close(r));}
