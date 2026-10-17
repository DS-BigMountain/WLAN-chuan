// Targeted Android WebView check: a single transfer screen with shared task filters.
import http from 'node:http';
import {readFile,mkdir} from 'node:fs/promises';
import {pathToFileURL} from 'node:url';
import path from 'node:path';
import assert from 'node:assert/strict';
const root=path.resolve(process.argv[2]||'build/mobile-retry-ui');await mkdir(root,{recursive:true});
const server=http.createServer(async(req,res)=>{try{
  const file=req.url==='/'?'mobile/src/main/assets/native/app.html':req.url==='/web/chuan.svg'?'src/assets/chuan.svg':req.url.startsWith('/native/')?'mobile/src/main/assets'+req.url:'src'+req.url;
  let body=await readFile(file,'utf8');if(req.url==='/')body=body.replace('<!-- ORIGINAL_ICONS -->',(await readFile('src/web/index.html','utf8')).match(/<svg class="symbols"[\s\S]*?<\/svg>/)[0]);
  res.setHeader('Content-Type',file.endsWith('.js')?'text/javascript':file.endsWith('.css')?'text/css':file.endsWith('.svg')?'image/svg+xml':'text/html');res.end(body);
}catch(error){res.writeHead(404);res.end(String(error));}});
await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));let browser;
try{
  const {chromium}=await import(pathToFileURL(path.resolve('build/web-qa/node_modules/playwright-core/index.mjs')));browser=await chromium.launch({channel:'chrome',headless:true});
  const page=await browser.newPage({viewport:{width:390,height:844}}),errors=[];page.on('pageerror',error=>errors.push(error.message));
  await page.addInitScript(()=>{
    const base={id:'1',name:'资料/a.txt',peer:'电脑',total:12,done:0,speed:0,state:6,stateText:'已拒绝',receiving:false,canRetry:true,folderId:'folder',folderName:'资料',folderCount:1,folderTotal:12};
    globalThis.fixture={running:true,instance:'test',name:'Android 手机',addresses:['192.168.1.10'],port:45871,peers:[],requests:[],jobs:[base,{...base,id:'2',folderId:'',name:'接收.txt',receiving:true,canRetry:false}],notice:''};
    globalThis.retried=[];globalThis.opened=[];globalThis.actions=[];window.ChuanNative={state:()=>JSON.stringify(fixture),retry:id=>retried.push(id),openFile:id=>opened.push(id),shareFile:id=>opened.push('share:'+id),
      pickFiles:id=>actions.push(['files',id]),pickFolder:id=>actions.push(['folder',id]),chooseReceive:()=>actions.push(['directory']),configure:value=>{fixture.autoReceive=JSON.parse(value).autoReceive;actions.push(['auto',fixture.autoReceive]);return '{}';},
      listReceived:()=>JSON.stringify({parent:null,entries:[],next:-1}),decide:(id,bits)=>{actions.push(['decision',id,bits]);fixture.requests=[];fixture.jobs.push({id:'4',name:'确认接收.txt',peer:'电脑',total:16,done:4,speed:4,state:2,stateText:'接收中',receiving:true});},cancel:id=>{actions.push(['cancel',id]);const job=fixture.jobs.find(job=>job.id===id);job.state=4;job.stateText='已取消';}};
  });
  await page.goto('http://127.0.0.1:'+server.address().port);await page.locator('#jobs .row').first().waitFor();
  assert(await page.locator('.send-section').isVisible());assert(await page.locator('.jobs-section').isVisible());
  await page.locator('[data-filter=receive]').click();assert.equal(await page.locator('#jobs .row').count(),1);assert.equal(await page.getByRole('button',{name:'重试',exact:true}).count(),0);assert(await page.locator('.send-section').isVisible());
  await page.locator('[data-filter=send]').click();assert.equal(await page.locator('#jobs .row').count(),1);assert.equal(await page.locator('#jobs .folder-toggle').count(),1);
  await page.locator('[data-filter=all]').click();assert.equal(await page.locator('#jobs .row').count(),2);
  assert.equal(await page.getByRole('button',{name:'重试',exact:true}).count(),1,'only failed outbound task offers retry');
  await page.getByRole('button',{name:'重试',exact:true}).click();assert.deepEqual(await page.evaluate(()=>retried),['1']);
  await page.locator('#jobs .folder-toggle').first().click();assert.equal(await page.locator('#jobs [data-path="资料/a.txt"]').count(),1);assert((await page.locator('#jobs .folder-tree').first().textContent()).includes('12 B'));
  await page.evaluate(()=>fixture.jobs.push({id:'3',name:'接收目录/b.txt',peer:'电脑',total:8,done:8,speed:0,state:3,stateText:'已完成',receiving:true,canRetry:false,folderId:'received',folderName:'接收目录',folderCount:1,folderTotal:8,publicUri:'content://fixture/b'}));
  await page.locator('#jobs .folder-toggle').nth(1).click();assert.equal(await page.locator('#folder-dialog').evaluate(node=>node.open),false);await page.locator('#jobs').getByRole('button',{name:'打开',exact:true}).click();await page.locator('#jobs').getByRole('button',{name:'分享',exact:true}).click();assert.deepEqual(await page.evaluate(()=>opened),['3','share:3']);await page.screenshot({path:path.join(root,'android-folder-tree.png'),fullPage:true});
  assert(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth));await page.screenshot({path:path.join(root,'android-retry.png'),fullPage:true});
  await page.evaluate(()=>{fixture.peers=['windows','android','desktop-web','mobile-web'].map((platform,i)=>({id:'device-'+i,name:'A'+(i+1),ip:'192.168.1.'+(20+i),platform,browser:platform.endsWith('web')}));});
  await page.locator('#switch-device').click();await page.locator('#devices .device-kind').first().waitFor();assert.deepEqual(await page.locator('#devices .device-kind').allTextContents(),['Win 客户端','Android 客户端','电脑 Web','手机 Web']);assert(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth));await page.screenshot({path:path.join(root,'android-device-labels.png'),fullPage:true});
  await page.locator('#devices .device').first().click();await page.locator('#choose-folder').click();assert.deepEqual(await page.evaluate(()=>actions.at(-1)),['folder','device-0']);
  await page.evaluate(()=>{fixture.requests=[{id:'90',sender:'电脑',items:[{name:'目录资料.zip',count:1,total:100,folder:false},{name:'确认接收.txt',count:1,total:16,folder:false}]}];});
  await page.locator('#requests .approval-panel').waitFor();assert(await page.locator('.send-section').isVisible());assert(await page.locator('.jobs-section').isVisible());
  await page.locator('.approval-file').first().locator('input').uncheck();await page.getByRole('button',{name:'接收',exact:true}).click();
  await page.waitForFunction(()=>document.getElementById('requests').hidden&&document.querySelector('#jobs').textContent.includes('确认接收.txt'));
  assert.deepEqual(await page.evaluate(()=>actions.find(item=>item[0]==='decision')),['decision','90','-1']);assert(await page.locator('#choose-files').isVisible());
  await page.getByRole('button',{name:'取消',exact:true}).click();await page.waitForFunction(()=>document.getElementById('jobs').textContent.includes('已取消'));assert.deepEqual(await page.evaluate(()=>actions.filter(item=>item[0]==='cancel')),[['cancel','4']]);
  await page.locator('#auto-receive').check();assert.deepEqual(await page.evaluate(()=>actions.at(-1)),['auto',true]);
  await page.locator('#change-directory').click();assert.deepEqual(await page.evaluate(()=>actions.at(-1)),['directory']);await page.locator('#choose-directory').click();assert(await page.locator('#folder-dialog').evaluate(node=>node.open));await page.locator('#folder-dialog [data-close]').click();
  for(const width of [320,360,390,600,820]){await page.setViewportSize({width,height:844});assert(await page.locator('.send-section').isVisible());assert(await page.locator('.jobs-section').isVisible());assert(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth),'Android width '+width);}
  await page.setViewportSize({width:390,height:844});await page.screenshot({path:path.join(root,'android-main.png'),fullPage:true});
  assert.deepEqual(errors,[]);console.log('PASS Android single-screen approval, task filters, retry, inline tree, cancellation, receive controls and responsive layout');
}finally{await browser?.close();await new Promise(resolve=>server.close(resolve));}
