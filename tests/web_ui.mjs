import http from 'node:http';
import {readFile,mkdir,writeFile} from 'node:fs/promises';
import path from 'node:path';
import {pathToFileURL} from 'node:url';
import assert from 'node:assert/strict';
const {chromium}=await import(pathToFileURL(path.resolve('build/web-qa/node_modules/playwright-core/index.mjs')));
const work=path.resolve(process.argv[2]||'build/ui-1.1/web-ui');await mkdir(work,{recursive:true});
const request={id:'90',batch:'batch-90',sender:'BIGMOUNTAIN',items:[{name:'方案图纸',folder:true,count:'8',total:'86822093'},{name:'会议纪要.pdf',folder:false,count:'1',total:'3355443'}]};
const state={token:'fixture-token',hostId:'host',serverInstance:'fixture-instance',platform:'windows',androidAvailable:true,name:'BIGMOUNTAIN',clientName:'浏览器-01',clientIp:'192.168.1.40',version:'1.1',autoReceive:false,hostNote:'',webUploadChecksumOptional:true,webTransferWait:true,maxTransferFiles:'0',maxTransferBytes:'0',peers:[{id:'other',name:'DESKTOP-02',ip:'192.168.1.20',browser:false},{id:'phone',name:'手机浏览器',ip:'192.168.1.35',browser:true}],jobs:[],inbox:[],requests:[request]};
const commands=[],errors=[],downloads=[];
const server=http.createServer(async(req,res)=>{
  try{
    if(req.url==='/api/info'){res.setHeader('Content-Type','application/json');res.end(JSON.stringify({protocol:1,id:state.hostId,instance:'fixture-instance'}));return;}
    if(req.url.startsWith('/web/api/')){
      const chunks=[];for await(const chunk of req)chunks.push(chunk);const body=Buffer.concat(chunks).toString();if(req.method==='POST')commands.push({path:req.url,body});
      if(req.url==='/web/api/decision/90'){state.requests=[];state.jobs=[{id:'91',name:'会议纪要.pdf',peer:'BIGMOUNTAIN',total:3355443,done:0,speed:0,state:1,stateText:'等待接收',receiving:true,batchId:'batch-90',uploadKey:''}];state.inbox=[{id:'91',received:false}];}
      res.setHeader('Content-Type','application/json');res.end(req.url==='/web/api/state'?JSON.stringify(state):'');return;
    }
    const file=req.url==='/'?'index.html':req.url.replace('/web/','');if(!['index.html','app.js','style.css','files.js','transfers.js','sha256.js','session.js','zip_store.js','zip_worker.js','hash_file.js','hash_worker.js','zip_stream_worker.js'].includes(file)){res.writeHead(404);res.end();return;}
    let body=await readFile(path.join('src/web',file),'utf8');
    if(file==='app.js')body=body.replace('poll();setInterval(poll,2000);',`await poll();globalThis.uiTest={
      set(data){latestJobs=data.jobs;inbox=data.files||[];operations.splice(0,operations.length,...(data.ops||[]));renderJobs();},
      offline(value){online=!value;renderRequests(currentRequests);updateTarget();renderJobs();},
      opTarget(){return operations[0]?.target;},
      requests(data){renderRequests(data);}
    };`);
    res.setHeader('Content-Type',file.endsWith('.js')?'text/javascript':file.endsWith('.css')?'text/css':'text/html');res.end(body);
  }catch(error){res.writeHead(500);res.end(String(error));}
});
await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));let browser;
try{
  browser=await chromium.launch({channel:'chrome',headless:true,timeout:15000});
  const page=await browser.newPage({viewport:{width:1440,height:1080},deviceScaleFactor:2});page.on('pageerror',error=>errors.push(error.message));
  page.on('request',request=>{if(new URL(request.url()).pathname.startsWith('/web/files/'))downloads.push(request.url());});
  await page.addInitScript(()=>{window.showDirectoryPicker=undefined;});
  await page.goto('http://127.0.0.1:'+server.address().port);await page.waitForFunction(()=>!!window.uiTest);
  assert(await page.locator('#choose-files').isDisabled());
  await page.locator('#devices .device').nth(1).click();assert(await page.locator('#choose-files').isEnabled());
  assert(await page.getByRole('button',{name:'全部接受',exact:true}).isDisabled());
  assert(await page.locator('.approval-choice input').first().isDisabled());
  await page.locator('.approval-choice input').nth(1).check();
  await page.evaluate(()=>uiTest.offline(true));assert(await page.getByRole('button',{name:'接受所选',exact:true}).isDisabled());
  assert(await page.getByRole('button',{name:'拒绝',exact:true}).isDisabled());
  await page.evaluate(()=>uiTest.offline(false));assert(await page.locator('.approval-choice input').nth(1).isChecked());
  const upload={id:'101',name:'项目资料/a.bin',peer:'DESKTOP-02',total:1048576,done:1048576,speed:0,state:3,stateText:'已完成',receiving:false,uploadKey:'upload-1',stage:'staging',canDelete:true};
  const forward={...upload,id:'102',done:262144,speed:1048576,state:2,stateText:'发送中',stage:'forward'};
  await page.evaluate(data=>uiTest.set(data),{jobs:[forward,upload],ops:[{key:'upload-1',path:'项目资料/a.bin',file:{size:1048576},fileId:'101',forwardId:'102',target:'other',finished:true,status:'已提交目标设备'}]});
  assert.equal(await page.locator('#jobs .row').count(),1);assert.equal(await page.locator('#jobs progress').evaluate(node=>node.value),25);
  assert(await page.locator('#jobs .stages').isHidden(),'transfer status has no duplicate route explanation');
  await page.locator('#devices .device').first().click();assert.equal(await page.evaluate(()=>uiTest.opTarget()),'other');
  await page.evaluate(jobs=>uiTest.set({jobs}),[forward,upload]);assert.equal(await page.locator('#jobs .row').count(),1);assert.equal(await page.locator('#jobs progress').evaluate(node=>node.value),25);
  await page.evaluate(jobs=>uiTest.set({jobs}),[upload]);assert((await page.locator('#jobs .job-status').textContent()).includes('等待目标设备接收'));
  await page.setViewportSize({width:390,height:940});
  await page.locator('#switch-device').click();assert(await page.locator('#peer-sidebar').evaluate(node=>node.classList.contains('sheet-open')));
  await page.locator('#devices .device').nth(1).click();assert((await page.locator('#target-name').textContent()).startsWith('DESKTOP-02 · '));assert(await page.locator('#device-backdrop').isHidden());
  assert(await page.locator('.send-section').isVisible());assert(await page.locator('#requests').isVisible());assert(await page.locator('.jobs-section').isVisible());
  await page.locator('.approval-choice input').nth(1).check();await page.getByRole('button',{name:'接受所选',exact:true}).click();
  await page.waitForFunction(()=>document.querySelector('#requests').hidden&&document.querySelector('#jobs .row'));assert(await page.locator('.send-section').isVisible());
  assert(commands.some(command=>command.path==='/web/api/decision/90'&&command.body==='01'));
  assert.equal(await page.getByRole('link',{name:'下载',exact:true}).count(),1);
  assert.equal(downloads.length,0,'single file approval does not force a download');
  assert(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth));
  await page.screenshot({path:path.join(work,'mobile-status.png'),fullPage:true});
  await page.evaluate(data=>{uiTest.requests(data.requests);uiTest.set({jobs:data.jobs,files:data.files});}, {requests:[request],jobs:[forward,upload],files:[]});
  await page.locator('.approval-choice input').nth(1).check();
  await page.screenshot({path:path.join(work,'mobile-receive.png'),fullPage:true});
  const folder={...forward,name:'项目资料/a.bin',folderId:'design',folderName:'项目资料',folderCount:24,folderTotal:1374389534,total:1374389534,done:Math.ceil(1374389534*.65),speed:112*1048576};
  const received={id:'190',name:'操作说明.pdf',peer:'BIGMOUNTAIN',total:3355443,done:3355443,speed:0,state:3,stateText:'已完成',receiving:true,uploadKey:''};
  await page.evaluate(data=>uiTest.set(data),{jobs:[folder,received],files:[{id:'190',received:false}]});
  await page.locator('[data-job-filter=send]').click();assert.equal(await page.locator('#jobs .row').count(),1);assert.equal(await page.locator('#jobs .direction').textContent(),'发送');assert(await page.locator('.send-section').isVisible());assert(await page.locator('#requests').isVisible());
  await page.locator('[data-job-filter=receive]').click();assert.equal(await page.locator('#jobs .row').count(),1);assert.equal(await page.locator('#jobs .direction').textContent(),'接收');assert(await page.locator('.send-section').isVisible());
  await page.locator('[data-job-filter=all]').click();assert.equal(await page.locator('#jobs .row').count(),2);assert.deepEqual(await page.locator('[data-job-filter]').allTextContents(),['全部 (2)','发送 (1)','接收 (1)']);
  await page.setViewportSize({width:1560,height:1110});await page.evaluate(()=>document.fonts.ready);await page.screenshot({path:path.join(work,'desktop.png'),fullPage:true});
  const cdp=await page.context().newCDPSession(page);await cdp.send('DOM.enable');await cdp.send('CSS.enable');
  const {root}=await cdp.send('DOM.getDocument');const {nodeId}=await cdp.send('DOM.querySelector',{nodeId:root.nodeId,selector:'.content-heading h2'});
  const fonts=await cdp.send('CSS.getPlatformFontsForNode',{nodeId});
  const visualMetrics=await page.evaluate(()=>Object.fromEntries(['.content-heading h2','.drop h3','#choose-files','.sidebar','.approval-card'].map(selector=>{const element=document.querySelector(selector),style=getComputedStyle(element),bounds=element.getBoundingClientRect();return [selector,{font:style.fontFamily,fontSize:style.fontSize,fontWeight:style.fontWeight,color:style.color,background:style.backgroundColor,x:bounds.x,y:bounds.y,width:bounds.width,height:bounds.height}];})));
  await page.setViewportSize({width:420,height:940});
  await page.evaluate(()=>new Promise(resolve=>requestAnimationFrame(()=>{scrollTo(0,0);requestAnimationFrame(resolve);})));await page.screenshot({path:path.join(work,'mobile-send.png'),fullPage:true});

  await page.evaluate(()=>new Promise(resolve=>requestAnimationFrame(()=>{scrollTo(0,0);requestAnimationFrame(resolve);})));await page.screenshot({path:path.join(work,'mobile-receive.png'),fullPage:true});
  const breakpoints=[];
  for(const width of [320,360,390,420,768,820,1280,1560]){
    await page.setViewportSize({width,height:940});
    assert(await page.locator('.send-section').isVisible());assert(await page.locator('#requests').isVisible());assert(await page.locator('.jobs-section').isVisible());
    assert(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth),'no horizontal overflow at '+width);
    breakpoints.push(width);
  }
  await writeFile(path.join(work,'visual-metrics.json'),JSON.stringify({fonts,visualMetrics,breakpoints},null,2));
  await page.locator('#choose-folder').click({trial:true});
  const chooser=await Promise.all([page.waitForEvent('filechooser'),page.locator('#choose-folder').click()]);
  assert.equal(await chooser[0].element().evaluate(input=>input.id),'folder-picker');
  assert.deepEqual(errors,[]);console.log('PASS original production UI: target freeze, relay progress and reload, capability-based mixed approval, offline selection, mobile single-screen flow, filters and manual single-file download');
}finally{await browser?.close();await new Promise(resolve=>server.close(resolve));}
