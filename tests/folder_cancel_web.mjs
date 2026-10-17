// Directed regressions for receiver cancellation, lost acknowledgements and repeated transport errors.
import http from 'node:http';
import net from 'node:net';
import dgram from 'node:dgram';
import {spawn} from 'node:child_process';
import {mkdir,writeFile,readFile,readdir} from 'node:fs/promises';
import {randomUUID} from 'node:crypto';
import {pathToFileURL} from 'node:url';
import path from 'node:path';
import assert from 'node:assert/strict';
const work=path.resolve(process.argv[2]||'build/folder-cancel',randomUUID()),android=process.argv[3]==='android';await mkdir(work,{recursive:true});
const delay=ms=>new Promise(resolve=>setTimeout(resolve,ms));
async function until(fn,label,ms=15000){const end=Date.now()+ms;while(Date.now()<end){const value=await fn();if(value)return value;await delay(40);}throw Error('timeout: '+label);}
async function port(udp=false){const socket=udp?dgram.createSocket('udp4'):net.createServer();await new Promise(resolve=>udp?socket.bind(0,'127.0.0.1',resolve):socket.listen(0,'127.0.0.1',resolve));const value=socket.address().port;await new Promise(resolve=>socket.close(resolve));return value;}
function request(port,url,headers={},body=null){return new Promise((resolve,reject)=>{const req=http.request({host:'127.0.0.1',port,path:url,method:body===null?'GET':'POST',agent:false,headers:{...headers,...(body===null?{}:{'Content-Length':Buffer.byteLength(body)})}},res=>{let text='';res.on('data',data=>text+=data);res.on('end',()=>resolve({status:res.statusCode,text}));});req.on('error',reject);req.end(body);});}
let browser,child;
try{
 const tcp=await port(),udp=await port(true),root=path.join(work,'host');
 child=android?spawn('java',['-Dfile.encoding=UTF-8','-cp',path.resolve('build/mobile-core'),'local.chuan.test.HostMain','--root',root,'--assets',path.resolve('src/web'),'--port',String(tcp),'--discovery-port',String(udp)],{windowsHide:true,stdio:['pipe','ignore','ignore']}):spawn(path.resolve('build/v2.0/Release/Chuan.exe'),['--headless','--port',String(tcp),'--discovery-port',String(udp),'--data-dir',path.join(root,'config'),'--receive-dir',path.join(root,'received'),'--run-seconds','180'],{windowsHide:true,stdio:'ignore'});
 await until(async()=>{try{return (await request(tcp,'/api/info')).status===200;}catch{return false;}},'host startup');
 const {chromium}=await import(pathToFileURL(path.resolve('build/web-qa/node_modules/playwright-core/index.mjs')));browser=await chromium.launch({channel:'chrome',headless:true});
 const results=[];
 for(const mode of ['saved-response-lost','cancel-running','cancel-queued','network-stopped']){
  const context=await browser.newContext({viewport:{width:1440,height:1000}}),page=await context.newPage(),errors=[];page.on('pageerror',error=>errors.push(error.message));
  let client='',uploads=0,activeBytes=0;page.on('request',req=>{if(req.url().endsWith('/web/api/state'))client=req.headers()['x-chuan-client'];});
  const source=path.join(work,mode,'批次'),count=20;await mkdir(path.join(source,'子目录'),{recursive:true});for(let i=0;i<count;i++)await writeFile(path.join(source,'子目录',String(i).padStart(3,'0')+'.bin'),i===1?Buffer.alloc(4*1024*1024,120):Buffer.from('12345678'));
  const state=async()=>JSON.parse((await request(tcp,'/web/api/state',{'X-Chuan-Client':client})).text);
  await page.route('**/web/api/upload',async route=>{
   uploads++;
   if(mode==='network-stopped'){await route.abort('connectionreset');return;}
   if(mode==='saved-response-lost'&&uploads===1){const relative=Buffer.from(route.request().headers()['x-chuan-path'],'hex').toString(),payload=await readFile(path.join(path.dirname(source),relative));const reply=await route.fetch({postData:payload});assert.equal(reply.status(),201);await route.abort('connectionreset');return;}
   if(mode.startsWith('cancel-')&&uploads===2){
    const headers=route.request().headers(),upload=headers['x-chuan-upload'];let raw;
    if(mode==='cancel-running'){
     raw=http.request({host:'127.0.0.1',port:tcp,path:'/web/api/upload',method:'POST',agent:false,headers:{...headers,'Content-Length':4*1024*1024}},res=>res.resume());raw.on('error',()=>{});raw.write(Buffer.alloc(65536,120));
     await until(async()=>{const job=(await state()).jobs.find(item=>item.uploadKey===upload);if(job?.state===2&&job.done>0){activeBytes=job.done;return true;}return false;},'receiving partial file');
    }
    const snapshot=await state(),job=snapshot.jobs.find(item=>item.uploadKey===upload);assert(job);
    assert.equal((await request(tcp,'/web/api/cancel/'+job.id,{'X-Chuan-Client':client,'X-Chuan-Token':snapshot.token},'')).status,200);
    if(raw){raw.destroy();await route.abort('connectionreset');}else await route.continue().catch(()=>{});
    return;
   }
   await route.continue();
  });
  await page.goto('http://127.0.0.1:'+tcp);await page.locator('#devices .device').first().click();await page.locator('#folder-picker').setInputFiles(source);
  await page.locator('#jobs .folder-toggle').click();assert.equal(await page.locator('dialog[open]').count(),0,'folder expands in its transfer item');
  const expected=mode==='saved-response-lost'?'已完成':mode==='network-stopped'?'传输失败':'已取消';
  await page.locator('#jobs .job-status').filter({hasText:expected}).waitFor({timeout:20000});await delay(500);
  const snapshot=await state(),jobs=snapshot.jobs;
  assert.equal(jobs.length,count);assert(jobs.every(job=>job.state>=3));assert.deepEqual(errors,[]);
  if(mode==='saved-response-lost'){assert.equal(uploads,count);assert(jobs.every(job=>job.state===3));assert.equal(await page.locator('#jobs .retry').count(),0);assert.equal((await readFile(path.join(root,'received',jobs[0].name))).length,jobs[0].total);}
  else if(mode==='network-stopped'){assert.equal(uploads,3,'persistent network failure stops the batch');assert.equal(jobs.filter(job=>job.state===5).length,3);assert.equal(jobs.filter(job=>job.state===4).length,count-3);}
  else{assert.equal(uploads,2,'no later uploads after receiver cancellation');assert.equal(jobs.filter(job=>job.state===3).length,1);assert.equal(jobs.filter(job=>job.state===4).length,count-1);assert(!await page.locator('#jobs .detail').textContent().then(text=>text.includes('发送失败')));if(mode==='cancel-running')await until(async()=>(await page.locator('#jobs .detail').textContent()).includes('64.0 KB'),'sender retains receiver-confirmed partial bytes');await until(async()=>!(await readdir(path.join(root,'received'))).some(name=>name.endsWith('.part')),'partial file cleanup');}
  await page.locator('#jobs .folder-tree>ul>li>details>summary').first().click();await page.screenshot({path:path.join(work,mode+'.png'),fullPage:true});
  results.push({mode,uploads,activeBytes,completed:jobs.filter(job=>job.state===3).length,cancelled:jobs.filter(job=>job.state===4).length,failed:jobs.filter(job=>job.state===5).length});
  if(mode==='cancel-running'){await page.locator('#jobs .retry').click();await page.locator('#jobs .job-status').filter({hasText:'已完成'}).waitFor({timeout:20000});const retried=(await state()).jobs;assert.equal(uploads,22);assert.equal(retried.filter(job=>job.state===3).length,21);assert.equal(retried.filter(job=>job.state===4).length,19);assert.equal(retried.find(job=>job.id===jobs.find(item=>item.state===3).id).state,3);results.at(-1).retryCompleted=true;}
  await context.close();
 }
 await writeFile(path.join(work,'results.json'),JSON.stringify(results,null,2));console.log('PASS receiver cancellation while running/queued, saved response recovery, persistent network stop, inline tree');console.log('RESULTS '+work);
}catch(error){if(browser)for(const page of browser.contexts().flatMap(context=>context.pages())){try{console.log((await page.locator('body').innerText()).slice(-3500));await page.screenshot({path:path.join(work,'failure.png'),fullPage:true});}catch{}}throw error;
}finally{await browser?.close();if(child?.exitCode===null)child.kill();}
