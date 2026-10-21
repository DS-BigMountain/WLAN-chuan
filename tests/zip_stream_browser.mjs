// Production browser stream: first destination bytes precede source completion.
import {spawn,spawnSync} from 'node:child_process';import {mkdir,writeFile,readdir,stat,readFile} from 'node:fs/promises';import path from 'node:path';import net from 'node:net';import dgram from 'node:dgram';import {randomUUID} from 'node:crypto';import assert from 'node:assert/strict';import {pathToFileURL} from 'node:url';
const root=path.resolve('build/zip-stream-qa',randomUUID()),children=[];const delay=ms=>new Promise(r=>setTimeout(r,ms));
const until=async(fn,label)=>{const end=Date.now()+20000;while(Date.now()<end){const value=await fn();if(value)return value;await delay(30);}throw Error('timeout: '+label);};
async function port(udp=false){const s=udp?dgram.createSocket('udp4'):net.createServer();await new Promise(r=>udp?s.bind(0,'127.0.0.1',r):s.listen(0,'127.0.0.1',r));const p=s.address().port;await new Promise(r=>s.close(r));return p;}
async function host(platform,name){const work=path.join(root,name),tcp=await port(),udp=await port(true);await mkdir(path.join(work,'config'),{recursive:true});await writeFile(path.join(work,'config','settings.ini'),'[Chuan]\r\nZipFolders=1\r\n');
 const child=platform==='windows'?spawn(path.resolve('build/v2.0/Release/Chuan.exe'),['--headless','--port',String(tcp),'--discovery-port',String(udp),'--data-dir',path.join(work,'config'),'--receive-dir',path.join(work,'received'),'--run-seconds','180'],{windowsHide:true,stdio:'ignore'}):spawn('java',['-Dfile.encoding=UTF-8','-Dstdout.encoding=UTF-8','-cp',path.resolve('build/mobile-core'),'local.chuan.test.HostMain','--root',work,'--assets',path.resolve('src/web'),'--port',String(tcp),'--discovery-port',String(udp)],{windowsHide:true,stdio:['pipe','pipe','inherit']});children.push(child);
 let buffer='',sequence=0;const answers=new Map();if(platform==='android'){child.stdout.setEncoding('utf8');child.stdout.on('data',chunk=>{buffer+=chunk;let at;while((at=buffer.indexOf('\n'))>=0){const line=buffer.slice(0,at).trim();buffer=buffer.slice(at+1);if(line.startsWith('ANSWER ')){const a=JSON.parse(line.slice(7));answers.get(a.request)?.(a.result);answers.delete(a.request);}}});}
 const command=body=>new Promise(resolve=>{const request=String(++sequence);answers.set(request,resolve);child.stdin.write(JSON.stringify({...body,request})+'\n');});
 const info=await until(async()=>{try{return await(await fetch('http://127.0.0.1:'+tcp+'/api/info')).json();}catch{}},'host ready');if(platform==='android')await command({command:'settings',zipFolders:true,autoReceive:true});return {work,tcp,info,command};
}
const {chromium}=await import(pathToFileURL(path.resolve('build/web-qa/node_modules/playwright-core/index.mjs')));const browser=await chromium.launch({channel:'chrome',headless:true});
try{for(const [platform,remote] of [['windows',false],['android',false],['windows',true],['android',true]]){
 if(process.argv[2]==='remote'&&!remote)continue;
 const server=await host(platform,platform+'-'+remote),receiver=remote?await host(platform==='windows'?'android':'windows',platform+'-destination'):server;
 if(remote){const java=platform==='android'?server:receiver,windows=platform==='windows'?server:receiver;await java.command({command:'connect',address:'127.0.0.1:'+windows.tcp});}
 const page=await browser.newPage(),errors=[];page.on('pageerror',e=>errors.push(e.message));await page.goto('http://127.0.0.1:'+server.tcp);
 const device=page.locator('.device[data-peer-id="'+receiver.info.id+'"]');await device.waitFor();await device.click();
 await page.locator('#limits').click();assert.equal(await page.locator('#zip-folders').isChecked(),false,'native host ZIP switch does not enable Web ZIP');await page.locator('#zip-folders').check();await page.locator('#limits-form button[value="save"]').click();
 const name='流式-'+platform+'-'+remote,source=path.join(root,'source',name);await mkdir(source,{recursive:true});await writeFile(path.join(source,'a.bin'),Buffer.alloc(8*1024*1024,31));await writeFile(path.join(source,'late.txt'),'last source');
 let release,first=false,chunks=0,firstRequest;const gate=new Promise(r=>release=r);
 await page.route('**/web/api/archive/chunk/*',async route=>{chunks++;if(chunks===1){firstRequest=route.request();const response=await route.fetch();assert.equal(response.status(),202);first=true;await gate;await route.fulfill({response});}else await route.continue();});
 try{
  await page.locator('#folder-picker').setInputFiles(source);await until(()=>first,'first chunk delivered');
  await until(async()=>{const files=await readdir(path.join(receiver.work,'received'));return files.some(f=>f.endsWith('.part'));},'destination has partial data before remaining source is generated');assert.equal(chunks,1);assert(!(await readdir(path.join(receiver.work,'received'))).includes(name+'.zip'));
  const replayHeaders={...firstRequest.headers()};delete replayHeaders['content-length'];replayHeaders['x-chuan-offset']='0';
  const replay=await fetch(firstRequest.url(),{method:'POST',headers:replayHeaders,body:new Uint8Array([0])});assert(!replay.ok,'duplicate chunk offset is rejected');
  replayHeaders['x-chuan-offset']='1048576';replayHeaders['x-chuan-tab']=randomUUID().replaceAll('-','');
  const foreign=await fetch(firstRequest.url(),{method:'POST',headers:replayHeaders,body:new Uint8Array([0])});assert(!foreign.ok,'another page cannot append to this stream');
  release();
  const zip=path.join(receiver.work,'received',name+'.zip');await until(async()=>{try{return(await stat(zip)).size>8*1024*1024;}catch{}},'complete streamed ZIP');await until(async()=>/已完成|已送达目标设备/.test(await page.locator('#jobs').innerText()),'sender confirmation');
  const python='C:/Users/17630/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe';const checked=spawnSync(python,['-X','utf8','-c','import zipfile,sys;z=zipfile.ZipFile(sys.argv[1]);assert z.testzip() is None;assert z.read(sys.argv[2]+"/late.txt")==b"last source";assert z.read(sys.argv[2]+"/a.bin")==bytes([31])*(8*1024*1024)',zip,name],{encoding:'utf8',windowsHide:true});assert.equal(checked.status,0,checked.stderr);assert.deepEqual(errors,[]);
  if(remote)assert(!(await readdir(path.join(server.work,'received'))).includes(name+'.zip'),'forwarder does not store the archive');console.log('PASS Web -> '+platform+(remote?' -> other native client':'')+': first destination bytes precede remaining ZIP generation, valid CRC/SHA, independent ZIP setting');
  await page.unroute('**/web/api/archive/chunk/*');
  for(const mode of (remote&&platform==='windows'?['receiver-cancel']:!remote?['cancel','source-change','checksum']:[])){
   const modeName=name+'-'+mode,modeSource=path.join(root,'source',modeName);await mkdir(modeSource,{recursive:true});await writeFile(path.join(modeSource,'a.bin'),Buffer.alloc(8*1024*1024,19));await writeFile(path.join(modeSource,'late.txt'),'late source');
   let releaseMode,firstMode=false,count=0,rejected=false;const modeGate=new Promise(r=>releaseMode=r);
   await page.route('**/web/api/archive/chunk/*',async route=>{
    const body=route.request().postDataBuffer();count++;
    if(mode==='checksum'&&body.length<1048576){body[body.length-1]^=1;const response=await route.fetch({postData:body});rejected=response.status()!==201;await route.fulfill({response}).catch(()=>{});return;}
    if(count===1){const response=await route.fetch();assert.equal(response.status(),202);firstMode=true;await modeGate;await route.fulfill({response}).catch(()=>{});}else await route.continue().catch(()=>{});
   });
   try{
    await page.locator('#folder-picker').setInputFiles(modeSource);await until(()=>firstMode,'first failure-mode chunk');
    if(mode==='cancel')await page.locator('#jobs .lifecycle-active button').filter({hasText:'取消'}).first().click();
    if(mode==='source-change')await writeFile(path.join(modeSource,'late.txt'),'changed after source selection');
    if(mode==='receiver-cancel'){const incoming=await until(async()=>{const state=await receiver.command({command:'state'});return state.jobs.find(j=>j.name===modeName+'.zip'&&j.state<3);},'native receiver cancellation target '+modeName);await receiver.command({command:'cancel',id:incoming.id});}
    releaseMode();
    if(mode==='checksum')await until(()=>rejected,'checksum rejection');
    else if(mode==='source-change')await until(async()=>(await page.locator('#jobs').innerText()).includes('late.txt'),'source path failure');
    else await until(async()=>(await page.locator('#jobs').innerText()).includes('已取消'),'cancelled UI');
    assert(!(await readdir(path.join(receiver.work,'received'))).includes(modeName+'.zip'));
    console.log('PASS '+platform+(remote?' relay ':' local ')+mode+': no incomplete committed ZIP');
   }finally{releaseMode();await page.unroute('**/web/api/archive/chunk/*');}
   if(mode==='checksum')break;
  }

 }finally{release();await page.close();}
}console.log('RESULTS '+root);}finally{await browser.close();for(const child of children)if(child.exitCode===null)child.kill();}
