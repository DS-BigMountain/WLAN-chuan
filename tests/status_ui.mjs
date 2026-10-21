// Production Windows web UI and Android WebView use the same current-operation model.
import http from 'node:http';import {readFile,mkdir} from 'node:fs/promises';import path from 'node:path';import {pathToFileURL} from 'node:url';import assert from 'node:assert/strict';
const {chromium}=await import(pathToFileURL(path.resolve('build/web-qa/node_modules/playwright-core/index.mjs')));
const output=path.resolve('build/status-speed/screens');await mkdir(output,{recursive:true});
const state={hostId:'host',serverInstance:'test',token:'token',clientName:'浏览器',clientIp:'127.0.0.1',name:'测试电脑',version:'2.0',autoReceive:true,peers:[],jobs:[],requests:[],inbox:[]};
const server=http.createServer(async(req,res)=>{try{
  req.resume();if(req.url==='/api/info'){res.setHeader('Content-Type','application/json');res.end(JSON.stringify({protocol:1,id:'host',instance:'test'}));return;}
  if(req.url.startsWith('/web/api/')){res.setHeader('Content-Type','application/json');res.end(req.url==='/web/api/state'?JSON.stringify(state):'');return;}
  const file=req.url==='/android'?'mobile/src/main/assets/native/app.html':req.url==='/'?'src/web/index.html':req.url==='/web/chuan.svg'?'src/assets/chuan.svg':req.url.startsWith('/native/')?'mobile/src/main/assets'+req.url:'src'+req.url;
  let body=await readFile(file,'utf8');
  if(req.url==='/android')body=body.replace('<!-- ORIGINAL_ICONS -->',(await readFile('src/web/index.html','utf8')).match(/<svg class="symbols"[\s\S]*?<\/svg>/)[0]);
  if(req.url==='/web/app.js')body=body.replace('poll();setInterval(poll,2000);',`await poll();globalThis.statusUI={set(jobs){latestJobs=jobs;for(const job of jobs)if(job.receiving){folderDestinations.set(job.batchId,{});if(job.state===3)savedFiles.add(String(job.id));}renderJobs();}};`);
  if(req.url==='/native/app.js')body=body.replace('render();setInterval(render,700);',`render();globalThis.statusUI={set(jobs){globalThis.fixture.jobs=jobs;render();}};`);
  res.setHeader('Content-Type',file.endsWith('.js')?'text/javascript':file.endsWith('.css')?'text/css':'text/html');res.end(body);
}catch(error){res.writeHead(404).end(String(error));}});await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));let browser;
try{
  browser=await chromium.launch({channel:'chrome',headless:true});
  for(const platform of ['web','android']){
    const page=await browser.newPage({viewport:{width:platform==='web'?1440:390,height:920}}),errors=[];page.on('pageerror',error=>errors.push(error.message));
    if(platform==='android')await page.addInitScript(()=>{globalThis.fixture={running:true,instance:'test',name:'Android 手机',addresses:[],port:45871,peers:[],requests:[],jobs:[]};window.ChuanNative={state:()=>JSON.stringify(fixture)};});
    await page.goto('http://127.0.0.1:'+server.address().port+(platform==='android'?'/android':'/'));await page.waitForFunction(()=>!!globalThis.statusUI);
    const base={folderId:'folder',folderName:'项目资料',folderCount:2,folderTotal:200,peer:'A1',receiving:true,total:100,done:0,state:0,speed:0,activity:''};
    const jobs=[{...base,id:'1',name:'项目资料/排队文件.bin'},{...base,id:'2',name:'项目资料/图纸/当前文件.bin'}];
    await page.evaluate(jobs=>statusUI.set(jobs),jobs);
    assert.equal(await page.locator('.current-operation').textContent(),'等待发送端开始下一项');
    jobs[1].activity='等待发送端核对同名文件';await page.evaluate(jobs=>statusUI.set(jobs),jobs);
    const line=page.locator('.current-operation');assert((await line.textContent()).includes('发送端正在核对同名文件：图纸/当前文件.bin'));assert(!(await line.textContent()).includes('排队文件'));
    assert(!(await page.locator('#jobs .progress-metrics').textContent()).includes('/s'),'disk checking is not represented as zero network speed');
    await page.screenshot({path:path.join(output,platform+'-checking.png'),fullPage:true});
    jobs[1]={...jobs[1],state:2,activity:'',done:50,speed:100};await page.evaluate(jobs=>statusUI.set(jobs),jobs);
    assert((await line.textContent()).includes('接收文件：图纸/当前文件.bin'));assert((await line.textContent()).includes('50 B / 100 B'));
    await page.screenshot({path:path.join(output,platform+'-receiving.png'),fullPage:true});
    for(const width of [360,820,1440]){await page.setViewportSize({width,height:920});assert(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth),'no overflow: '+platform+' '+width);}
    jobs.forEach(job=>{job.state=3;job.done=100;});await page.evaluate(jobs=>statusUI.set(jobs),jobs);assert(await page.locator('.current-operation').isHidden(),'ended operation clears active information');assert.deepEqual(errors,[]);await page.close();
  }
  console.log('PASS actual Web and Android UI: queued files excluded, remote checking phase, current-file progress, ended cleanup, responsive layout');
}finally{await browser?.close();await new Promise(resolve=>server.close(resolve));}
