// Real-browser directed checks for one ZIP per folder, consent, cancellation and failure propagation.
import http from 'node:http';
import net from 'node:net';
import dgram from 'node:dgram';
import {spawn,spawnSync} from 'node:child_process';
import {mkdir,writeFile,readFile,readdir} from 'node:fs/promises';
import {randomUUID} from 'node:crypto';
import {pathToFileURL} from 'node:url';
import path from 'node:path';
import assert from 'node:assert/strict';
const work=path.resolve(process.argv[2]||'build/zip-web',randomUUID()),android=process.argv[3]==='android';await mkdir(work,{recursive:true});
const delay=ms=>new Promise(resolve=>setTimeout(resolve,ms));
async function until(fn,label,ms=20000){const end=Date.now()+ms;while(Date.now()<end){const value=await fn();if(value)return value;await delay(50);}throw Error('timeout: '+label);}
async function port(udp=false){const s=udp?dgram.createSocket('udp4'):net.createServer();await new Promise(resolve=>udp?s.bind(0,'127.0.0.1',resolve):s.listen(0,'127.0.0.1',resolve));const p=s.address().port;await new Promise(resolve=>s.close(resolve));return p;}
function request(port,url,headers={},body=null){return new Promise((resolve,reject)=>{const req=http.request({host:'127.0.0.1',port,path:url,method:body===null?'GET':'POST',agent:false,headers:{...headers,...(body===null?{}:{'Content-Length':Buffer.byteLength(body)})}},res=>{let text='';res.on('data',data=>text+=data);res.on('end',()=>resolve({status:res.statusCode,text}));res.on('error',reject);});req.on('error',reject);req.end(body);});}
const python='C:/Users/17630/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe';
let browser,child;
try{
 const tcp=await port(),udp=await port(true),host=path.join(work,'host');
 child=android?spawn('java',['-Dfile.encoding=UTF-8','-cp',path.resolve('build/mobile-core'),'local.chuan.test.HostMain','--root',host,'--assets',path.resolve('src/web'),'--port',String(tcp),'--discovery-port',String(udp)],{windowsHide:true,stdio:['pipe','ignore','inherit']}):spawn(path.resolve('build/v2.0/Release/Chuan.exe'),['--headless','--port',String(tcp),'--discovery-port',String(udp),'--data-dir',path.join(host,'config'),'--receive-dir',path.join(host,'received'),'--run-seconds','240'],{windowsHide:true,stdio:'ignore'});
 await until(async()=>{try{return (await request(tcp,'/api/info')).status===200;}catch{return false;}},'host startup');
 const {chromium}=await import(pathToFileURL(path.resolve('build/web-qa/node_modules/playwright-core/index.mjs')));browser=await chromium.launch({channel:'chrome',headless:true});
 const reports=[];
 for(const mode of ['HT-1490','cancel-packing','source-changed','cancel-upload','checksum-failure','network-failure','saved-response-lost']){
  const context=await browser.newContext({viewport:{width:1440,height:1000}}),page=await context.newPage(),errors=[];page.on('pageerror',error=>errors.push(error.message));
  await page.addInitScript(()=>{const Original=Worker;globalThis.packWorkers=0;globalThis.Worker=class extends Original{constructor(...args){super(...args);packWorkers++;}};});
  let client='',uploads=0,offers=0,activeBytes=0,releaseOffer,releaseWorker;
  const offerGate=new Promise(resolve=>releaseOffer=resolve),workerGate=new Promise(resolve=>releaseWorker=resolve);
  page.on('request',req=>{if(req.url().endsWith('/web/api/state'))client=req.headers()['x-chuan-client'];});
  const root=mode==='HT-1490'?'HT':mode,source=path.join(work,'sources',root),expected=[];await mkdir(source,{recursive:true});
  if(mode==='HT-1490'){
   const manifest=JSON.parse(await readFile('build/ht-diagnostics/manifest.json','utf8'));
   for(const [i,item] of manifest.entries()){const file=path.join(work,'sources',item.path);await mkdir(path.dirname(file),{recursive:true});const content=Buffer.from(String(i).padStart(8,'0'));await writeFile(file,content);expected.push({name:item.path,size:content.length});}
  }else{await mkdir(path.join(source,'子目录'),{recursive:true});await writeFile(path.join(source,'子目录','数据.bin'),Buffer.alloc(2*1024*1024,120));await writeFile(path.join(source,'空.txt'),'');expected.push({name:root+'/子目录/数据.bin',size:2*1024*1024},{name:root+'/空.txt',size:0});}
  const state=async()=>JSON.parse((await request(tcp,'/web/api/state',{'X-Chuan-Client':client})).text);
  await page.route('**/web/api/offer',async route=>{offers++;if(mode==='HT-1490'&&offers===1){const reply=await route.fetch();await offerGate;await route.fulfill({response:reply});}else await route.continue();});
  if(mode==='cancel-packing'||mode==='source-changed')await page.route('**/web/zip_worker.js',async route=>{await workerGate;await route.continue().catch(()=>{});});
  await page.route('**/web/api/upload',async route=>{
   uploads++;const headers=route.request().headers(),body=route.request().postDataBuffer();assert.equal(Buffer.from(headers['x-chuan-path'],'hex').toString(),root+'.zip');assert.equal(headers['x-chuan-checksum'],'sha256');assert.equal(body.length,Number(headers['x-chuan-size'])+32);assert(!headers['x-chuan-folder']);
   if(uploads===1&&mode==='network-failure'){await route.abort('connectionreset');return;}
   if(uploads===1&&mode==='checksum-failure'){body[body.length-1]^=1;const reply=await route.fetch({postData:body});assert.equal(reply.status(),422);await route.fulfill({response:reply});return;}
   if(uploads===1&&mode==='saved-response-lost'){const reply=await route.fetch();assert.equal(reply.status(),201);await route.abort('connectionreset');return;}
   if(uploads===1&&mode==='cancel-upload'){
    const raw=http.request({host:'127.0.0.1',port:tcp,path:'/web/api/upload',method:'POST',agent:false,headers:{...headers,'Content-Length':body.length}},res=>res.resume());raw.on('error',()=>{});raw.write(body.subarray(0,65536));
    const receiving=await until(async()=>{const job=(await state()).jobs.find(job=>job.uploadKey===headers['x-chuan-upload']);return job?.state===2&&job.done>0?job:null;},'partial ZIP received');activeBytes=receiving.done;
    const snapshot=await state();assert.equal((await request(tcp,'/web/api/cancel/'+receiving.id,{'X-Chuan-Client':client,'X-Chuan-Token':snapshot.token},'')).status,200);raw.destroy();await route.abort('connectionreset');return;
   }
   await route.continue();
  });
  await page.goto('http://127.0.0.1:'+tcp);await page.locator('#devices .device').first().click();await page.locator('#folder-picker').setInputFiles(source);
  if(mode==='HT-1490'){await page.locator('#jobs .job-status').filter({hasText:'等待对方确认'}).waitFor();assert.equal(await page.evaluate(()=>packWorkers),0);assert.equal(uploads,0);releaseOffer();}
  if(mode==='source-changed'){
   await until(async()=>(await state()).jobs.some(job=>job.name===root+'.zip'&&job.state===0),'source check placeholder');
   await writeFile(path.join(source,'子目录','数据.bin'),'changed after selection');releaseWorker();
   await page.locator('#jobs .job-status').filter({hasText:'无法打包源文件'}).waitFor({timeout:15000});assert.equal(uploads,0);
   const snapshot=await until(async()=>{const s=await state();return s.jobs[0]?.state===5?s:null;},'packing error delivered to receiver');assert.match(snapshot.jobs[0].error,/数据.bin/);assert(!(await readdir(path.join(host,'received'))).some(name=>name.startsWith(root)));
  }else if(mode==='cancel-packing'){
   const announced=await until(async()=>{const job=(await state()).jobs.find(job=>job.name===root+'.zip');return job?.state===0?job:null;},'packing placeholder');assert.match(announced.stateText,/打包 ZIP/);const snapshot=await state();await request(tcp,'/web/api/cancel/'+announced.id,{'X-Chuan-Client':client,'X-Chuan-Token':snapshot.token},'');
   await page.locator('#jobs .job-status').filter({hasText:'已取消'}).waitFor();releaseWorker();assert.equal(uploads,0);
  }else{
   const expectedStatus=mode==='cancel-upload'?'已取消':mode==='checksum-failure'||mode==='network-failure'?'发送失败':'已完成';await page.locator('#jobs .job-status').filter({hasText:expectedStatus}).waitFor({timeout:45000});
   if(mode==='checksum-failure'||mode==='network-failure'||mode==='cancel-upload'){
    const snapshot=await state();assert.equal(snapshot.jobs.length,1);assert.equal(snapshot.jobs[0].state,mode==='cancel-upload'?4:5);assert(!(await readdir(path.join(host,'received'))).some(name=>name.startsWith(root)), 'no incomplete final archive');
    await page.locator('#jobs .retry').click();await page.locator('#jobs .job-status').filter({hasText:'已完成'}).waitFor({timeout:45000});assert.equal(uploads,2);assert.equal(offers,2);assert.equal(await page.evaluate(()=>packWorkers),2,'retry repacks original selection');
   }else{assert.equal(uploads,1);assert.equal(offers,1);}
   const completed=(await state()).jobs.filter(job=>job.state===3);assert.equal(completed.length,1);assert.equal(completed[0].name,root+'.zip');
   const expectedFile=path.join(work,root+'-expected.json');await writeFile(expectedFile,JSON.stringify(expected));
   const checked=spawnSync(python,['-c','import zipfile,json,sys; z=zipfile.ZipFile(sys.argv[1]); expected=json.load(open(sys.argv[2],encoding="utf-8")); assert z.testzip() is None; actual={i.filename:i for i in z.infolist() if not i.is_dir()}; assert len(actual)==len(expected); assert all(i.compress_type==0 for i in z.infolist()); assert all(actual[e["name"]].file_size==e["size"] for e in expected); print("ZIP verified")',path.join(host,'received',root+'.zip'),expectedFile],{encoding:'utf8',windowsHide:true});assert.equal(checked.status,0,checked.stderr);
  }
  await until(async()=>!(await readdir(path.join(host,'received'))).some(name=>name.endsWith('.part')),'partial cleanup');assert.deepEqual(errors,[]);await page.screenshot({path:path.join(work,mode+'.png'),fullPage:true});reports.push({mode,uploads,offers,activeBytes,packWorkers:await page.evaluate(()=>packWorkers)});console.log('PASS '+mode);await context.close();
 }
 await writeFile(path.join(work,'results.json'),JSON.stringify(reports,null,2));console.log('RESULTS '+work);
}catch(error){if(browser)for(const page of browser.contexts().flatMap(c=>c.pages()))try{console.log((await page.locator('body').innerText()).slice(-2200));await page.screenshot({path:path.join(work,'failure.png'),fullPage:true});}catch{}throw error;
}finally{await browser?.close();if(child?.exitCode===null)child.kill();}
