// Focused real-host checks: responsive sidebar, persistent consent and retries.
import http from 'node:http';
import net from 'node:net';
import dgram from 'node:dgram';
import {spawn} from 'node:child_process';
import {mkdir,readFile,writeFile} from 'node:fs/promises';
import {randomUUID} from 'node:crypto';
import {pathToFileURL} from 'node:url';
import path from 'node:path';
import assert from 'node:assert/strict';
const root=path.resolve(process.argv[2]||'build/approval-web',randomUUID());await mkdir(root,{recursive:true});
const waitMs=Number(process.argv[3]||155000);
const delay=ms=>new Promise(resolve=>setTimeout(resolve,ms)),key=()=>randomUUID().replaceAll('-',''),hex=s=>Buffer.from(s).toString('hex');
async function port(udp=false){const s=udp?dgram.createSocket('udp4'):net.createServer();await new Promise(r=>udp?s.bind(0,'127.0.0.1',r):s.listen(0,'127.0.0.1',r));const p=s.address().port;await new Promise(r=>s.close(r));return p;}
async function until(fn,label,timeout=15000){const end=Date.now()+timeout;while(Date.now()<end){try{const value=await fn();if(value)return value;}catch{}await delay(100);}throw Error('timeout: '+label);}
function request(tcp,url,{headers={},body=null}={}){return new Promise((resolve,reject)=>{const bytes=body===null?null:Buffer.from(body);const req=http.request({host:'127.0.0.1',port:tcp,path:url,method:bytes?'POST':'GET',agent:false,headers:{Connection:'close',...headers,...(bytes?{'Content-Length':bytes.length}:{})}},res=>{const chunks=[];res.on('data',b=>chunks.push(b));res.on('end',()=>resolve({status:res.statusCode,text:Buffer.concat(chunks).toString()}));res.on('error',reject);});req.on('error',reject);req.end(bytes);});}
const children=[],errors=[],reports=[];let browser;
const report=(name,detail={})=>{console.log('PASS '+name);reports.push({name,...detail});};
try{
  const tcp=await port(),udp=await port(true),winRoot=path.join(root,'Windows');
  const win=spawn(path.resolve('build/v2.0/Release/Chuan.exe'),['--headless','--port',String(tcp),'--discovery-port',String(udp),'--data-dir',path.join(winRoot,'config'),'--receive-dir',path.join(winRoot,'received'),'--name','WindowsHost','--run-seconds','300'],{windowsHide:true,stdio:'ignore'});children.push(win);
  await until(async()=>{const r=await request(tcp,'/api/info');return r.status===200;},'Windows startup');
  const mobilePort=await port(),mobileUdp=await port(true),java=spawn('java',['-Dfile.encoding=UTF-8','-Dstdout.encoding=UTF-8','-cp',path.resolve('build/mobile-core'),'local.chuan.test.HostMain','--root',path.join(root,'Android'),'--assets',path.resolve('src/web'),'--port',String(mobilePort),'--discovery-port',String(mobileUdp),'--name','AndroidSender','--auto-receive','false'],{windowsHide:true,stdio:['pipe','pipe','pipe']});children.push(java);
  let buffer='',sequence=0;const answers=new Map();let javaReady=false;
  java.stdout.setEncoding('utf8');java.stdout.on('data',chunk=>{buffer+=chunk;let at;while((at=buffer.indexOf('\n'))>=0){const line=buffer.slice(0,at).trim();buffer=buffer.slice(at+1);if(line.startsWith('READY '))javaReady=true;if(line.startsWith('ANSWER ')){const answer=JSON.parse(line.slice(7));answers.get(answer.request)?.(answer.result);answers.delete(answer.request);}}});
  const command=body=>new Promise(resolve=>{const id=String(++sequence);answers.set(id,resolve);java.stdin.write(JSON.stringify({...body,request:id})+'\n');});
  await until(()=>javaReady,'Android core startup');
  const {chromium}=await import(pathToFileURL(path.resolve('build/web-qa/node_modules/playwright-core/index.mjs')));
  browser=await chromium.launch({channel:'chrome',headless:true});
  const sender=await (await browser.newContext({viewport:{width:1920,height:1080}})).newPage();
  const receiver=await (await browser.newContext({viewport:{width:1280,height:900}})).newPage();
  for(const page of [sender,receiver])page.on('pageerror',error=>errors.push(error.message));
  await receiver.addInitScript(()=>{
    globalThis.savedPayloads={};
    const directory=prefix=>({getDirectoryHandle:async name=>directory(prefix+name+'/'),getFileHandle:async(name,options)=>{
      if(!options?.create)throw new DOMException('missing','NotFoundError');
      return {createWritable:async()=>{const chunks=[];return new WritableStream({write(data){chunks.push(...data);},close(){savedPayloads[prefix+name]=chunks;}});}};
    }});window.showDirectoryPicker=async()=>directory('');
  });
  const origin='http://127.0.0.1:'+tcp;await receiver.goto(origin);await sender.goto(origin);
  await sender.locator('#devices .device[data-peer-id^="web:"]').first().click();
  const receiverId=await sender.locator('#devices .device.selected').getAttribute('data-peer-id');
  const webFolder=path.join(root,'持久等待资料');await mkdir(path.join(webFolder,'子目录'),{recursive:true});
  await writeFile(path.join(webFolder,'a.txt'),'Web persistent consent');await writeFile(path.join(webFolder,'子目录','b.txt'),'nested payload');
  const batches=[];sender.on('request',req=>{if(req.url().endsWith('/web/api/offer'))batches.push(req.headers()['x-chuan-batch']);});
  await sender.locator('#folder-picker').setInputFiles(webFolder);
  const webCard=receiver.locator('.approval-card').filter({hasText:'持久等待资料'});await webCard.waitFor();
  await sender.locator('#jobs .job-status').filter({hasText:'等待对方确认'}).waitFor();
  assert(await sender.locator('#jobs .job-progress').isHidden(),'pending consent does not masquerade as stalled transfer');
  const started=Date.now();
  await command({command:'connect',address:'127.0.0.1:'+tcp});
  const remoteBrowser=await until(async()=>{const s=await command({command:'state'});return s.peers.find(peer=>peer.browser&&peer.id===receiverId);},'Android mesh browser route');
  const nativeFolder=path.join(root,'Android待确认资料');await mkdir(nativeFolder);await writeFile(path.join(nativeFolder,'native.txt'),'Native retry payload');
  await command({command:'send',target:remoteBrowser.id,files:[nativeFolder]});
  const nativeCard=receiver.locator('.approval-card').filter({hasText:'Android待确认资料'});await nativeCard.waitFor();
  // A direct native request also exercises the Android receiver's persistent wait.
  let mobileHeartbeats=0;const mobileBatch=key(),manifest=hex('Android接收确认.txt')+'\tF\t1\t3\n';
  const mobileOffer=new Promise((resolve,reject)=>{const req=http.request({host:'127.0.0.1',port:mobilePort,path:'/api/offer',method:'POST',agent:false,headers:{'Content-Length':Buffer.byteLength(manifest),'X-Chuan-Id':hex('test-native'),'X-Chuan-Sender':hex('TestSender'),'X-Chuan-Batch':mobileBatch,'X-Chuan-Offer-Wait':'1'}},res=>{let body='';res.on('data',b=>body+=b);res.on('end',()=>resolve({status:res.statusCode,body}));});req.on('information',info=>{if(info.statusCode===102)mobileHeartbeats++;});req.on('error',reject);req.end(manifest);});
  const mobilePrompt=await until(async()=>{const s=await command({command:'state'});return s.requests[0];},'Android approval visible');
  const metrics=[];
  for(const [width,height] of [[2560,1390],[1920,1080],[1440,900],[1280,800],[1024,768],[820,650],[760,700],[390,844],[360,640]]){
    await sender.setViewportSize({width,height});
    if(width<=760)await sender.locator('[data-view-tab=send]').click();
    const m=await sender.evaluate(()=>{const box=selector=>{const r=document.querySelector(selector).getBoundingClientRect();return {x:r.x,y:r.y,width:r.width,height:r.height,bottom:r.bottom};};return {width:innerWidth,overflow:document.documentElement.scrollWidth>innerWidth,sidebar:box('.sidebar'),header:box('header'),headerActions:box('.header-actions'),brand:box('.brand')};});
    assert(!m.overflow,'no horizontal overflow at '+width);
    assert(m.headerActions.bottom<=m.header.bottom+1,'header actions fit at '+width);
    if(width>760){assert(m.sidebar.x<=24,'no fixed outer gutter at '+width);assert(m.sidebar.width<=272,'sidebar width bounded at '+width);assert(m.sidebar.height<700,'short device list remains compact');}
    metrics.push(m);
    if(width===1920)await sender.screenshot({path:path.join(root,'web-desktop-waiting.png'),fullPage:true});
    if(width===390)await sender.screenshot({path:path.join(root,'web-mobile-waiting.png'),fullPage:true});
  }
  await sender.setViewportSize({width:1440,height:900});
  for(let elapsed=0;elapsed<waitMs;){elapsed=Date.now()-started;if(elapsed>=waitMs)break;await delay(Math.min(30000,waitMs-elapsed));console.log('WAIT active consent '+Math.floor((Date.now()-started)/1000)+'s');}
  assert(await webCard.isVisible());assert(await nativeCard.isVisible());assert((await sender.locator('#jobs .job-status').textContent()).includes('等待对方确认'));
  assert((await command({command:'state'})).requests.some(item=>item.id===mobilePrompt.id));if(waitMs>=150000)assert(mobileHeartbeats>=10);
  report(waitMs>=150000?'approval remains visible beyond the former 120/130/150-second limits':'approval remains visible during focused retry verification',{elapsedMs:Date.now()-started,mobileHeartbeats});
  sender.on('response',async response=>{if(response.url().endsWith('/web/api/state')){try{await writeFile(path.join(root,'sender-state.json'),JSON.stringify(await response.json(),null,2));}catch{}}});
  receiver.on('response',async response=>{if(response.url().endsWith('/web/api/state')){try{await writeFile(path.join(root,'receiver-state.json'),JSON.stringify(await response.json(),null,2));}catch{}}});
  await webCard.getByRole('button',{name:'拒绝',exact:true}).click();await sender.getByRole('button',{name:'重试',exact:true}).waitFor();
  await sender.locator('#devices .device').first().click(); // Retry must retain the original recipient.
  await sender.getByRole('button',{name:'重试',exact:true}).evaluate(button=>{button.click();button.click();});await webCard.waitFor();
  assert.equal(new Set(batches).size,2,'retry uses a fresh batch and fresh consent');
  await webCard.getByRole('button',{name:'接受文件夹',exact:true}).click();
  await sender.locator('#jobs .job-status').filter({hasText:'已完成'}).waitFor();
  await until(async()=>await receiver.evaluate(()=>Object.keys(savedPayloads).some(name=>name.endsWith('子目录/b.txt'))),'browser folder saved');
  assert.equal(await receiver.evaluate(()=>new TextDecoder().decode(new Uint8Array(savedPayloads['持久等待资料/a.txt']))),'Web persistent consent');
  report('Web folder rejection → retry → fresh approval → correct recipient and payload');
  await nativeCard.getByRole('button',{name:'拒绝',exact:true}).click();
  const rejected=await until(async()=>{const s=await command({command:'state'});return s.jobs.find(job=>job.state===6&&job.name.includes('native.txt'));},'Android rejection');
  await command({command:'retry',id:rejected.id});await nativeCard.waitFor();await nativeCard.getByRole('button',{name:'接受文件夹',exact:true}).click();
  await until(async()=>{const s=await command({command:'state'});return s.jobs.some(job=>job.id!==rejected.id&&job.state===3&&job.name.includes('native.txt'));},'Android retry complete');
  report('Android sender consumes keepalives and retries rejection with new consent');
  await command({command:'approve',id:mobilePrompt.id,selection:'1'});assert.deepEqual(await mobileOffer,{status:200,body:'1'});
  report('Android receiver accepts retained native approval');
  // A real transport failure must expose a usable retry on a single-file sender row.
  let failUpload=true;
  await sender.route('**/web/api/upload',route=>failUpload?route.abort('connectionreset'):route.continue());
  await sender.locator('#devices .device').first().click();
  await sender.locator('#picker').setInputFiles({name:'network-retry.txt',mimeType:'text/plain',buffer:Buffer.from('network retry bytes')});
  await sender.getByRole('button',{name:'重试',exact:true}).waitFor();
  failUpload=false;await sender.getByRole('button',{name:'重试',exact:true}).click();
  await until(async()=>{const r=await request(tcp,'/api/info');return r.status===200&&await sender.locator('#jobs .job-status').filter({hasText:'已完成'}).count()>=2;},'Web network retry');
  report('single-file network failure → retry → completed upload');
  assert.deepEqual(errors,[]);await writeFile(path.join(root,'results.json'),JSON.stringify({scope:'Targeted responsive, consent and retry checks; no full suite',reports,metrics},null,2));
  console.log('RESULTS '+root);
}catch(error){if(browser)for(const [i,page] of browser.contexts().flatMap(context=>context.pages()).entries()){try{console.log('PAGE',i,(await page.locator('body').innerText()).slice(0,2400));await page.screenshot({path:path.join(root,'failure-'+i+'.png'),fullPage:true});}catch{}}throw error;}
finally{await browser?.close();for(const child of children)if(child.exitCode===null)child.kill();}
