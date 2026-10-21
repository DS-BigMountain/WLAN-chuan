// Targeted browser directory persistence using Chromium's real origin-private filesystem.
import {spawn} from 'node:child_process';import {mkdir,writeFile} from 'node:fs/promises';import path from 'node:path';import {randomUUID} from 'node:crypto';import net from 'node:net';import dgram from 'node:dgram';import assert from 'node:assert/strict';import {pathToFileURL} from 'node:url';
const root=path.resolve('build/resume-receive',randomUUID());await mkdir(root,{recursive:true});const children=[];
const delay=ms=>new Promise(r=>setTimeout(r,ms));async function until(fn,label){for(let i=0;i<400;i++){const v=await fn();if(v)return v;await delay(100);}throw Error('timeout: '+label);}
async function port(udp=false){const s=udp?dgram.createSocket('udp4'):net.createServer();await new Promise(r=>udp?s.bind(0,'127.0.0.1',r):s.listen(0,'127.0.0.1',r));const p=s.address().port;await new Promise(r=>s.close(r));return p;}
async function host(platform,name){const work=path.join(root,name),tcp=await port(),udp=await port(true);await mkdir(work,{recursive:true});const child=platform==='windows'?spawn(path.resolve('build/v2.0/Release/Chuan.exe'),['--headless','--port',String(tcp),'--discovery-port',String(udp),'--data-dir',path.join(work,'config'),'--receive-dir',path.join(work,'received'),'--run-seconds','180'],{windowsHide:true,stdio:'ignore'}):spawn('java',['-Dfile.encoding=UTF-8','-Dstdout.encoding=UTF-8','-cp',path.resolve('build/mobile-core'),'local.chuan.test.HostMain','--root',work,'--assets',path.resolve('src/web'),'--port',String(tcp),'--discovery-port',String(udp)],{windowsHide:true,stdio:['pipe','pipe','inherit']});children.push(child);
 let buffer='',sequence=0;const answers=new Map();if(platform!=='windows'){child.stdout.setEncoding('utf8');child.stdout.on('data',chunk=>{buffer+=chunk;let at;while((at=buffer.indexOf('\n'))>=0){const line=buffer.slice(0,at).trim();buffer=buffer.slice(at+1);if(line.startsWith('ANSWER ')){const a=JSON.parse(line.slice(7));answers.get(a.request)?.(a.result);answers.delete(a.request);}}});}
 await until(async()=>{try{return (await fetch('http://127.0.0.1:'+tcp+'/api/info')).ok;}catch{return false;}},'host ready');return {tcp,child,command:body=>new Promise(resolve=>{const request=String(++sequence);answers.set(request,resolve);child.stdin.write(JSON.stringify({...body,request})+'\n');})};
}
const {chromium}=await import(pathToFileURL(path.resolve('build/web-qa/node_modules/playwright-core/index.mjs')));const browser=await chromium.launch({channel:'chrome',headless:true});

try{
 for(const platform of ['windows','android'])for(const mode of ['pause','disconnect']){
  const source=path.join(root,platform+'-'+mode+'-source','目录'),data=Buffer.alloc(8*1024*1024,31);await mkdir(source,{recursive:true});await writeFile(path.join(source,'large.bin'),data);
  const receiver=await host(platform,platform+'-'+mode),sender=await host('android',platform+'-'+mode+'-sender'),page=await browser.newPage(),offsets=[],errors=[];
  page.on('pageerror',e=>errors.push(e.message));page.on('request',r=>{if(r.url().includes('/web/files/'))offsets.push(Number(r.headers()['x-chuan-offset']||0));});
  await page.addInitScript(({mode})=>{
   window.showDirectoryPicker=async()=>navigator.storage.getDirectory();const actual=window.fetch;let intercepted=false;window.__received=0;
   window.fetch=async(input,options={})=>{const response=await actual(input,options);if(intercepted||!String(input).includes('/web/files/'))return response;intercepted=true;const reader=response.body.getReader();let control;
    const stream=new ReadableStream({start(controller){control=controller;options.signal?.addEventListener('abort',()=>{controller.error(new DOMException('test pause','AbortError'));reader.cancel().catch(()=>{});},{once:true});},async pull(controller){if(window.__received>=2*1024*1024)return;const next=await reader.read();if(next.done){controller.close();return;}const chunk=next.value.subarray(0,Math.min(next.value.length,2*1024*1024-window.__received));window.__received+=chunk.length;controller.enqueue(chunk);if(window.__received===2*1024*1024){window.__blocked=true;if(mode==='disconnect')setTimeout(()=>{controller.error(new TypeError('test disconnect'));reader.cancel().catch(()=>{});},100);}},cancel(){reader.cancel().catch(()=>{});}});return new Response(stream,{status:response.status,headers:response.headers});
   };
  },{mode});
  await page.goto('http://127.0.0.1:'+receiver.tcp);await page.locator('.device').first().waitFor();await sender.command({command:'connect',address:'127.0.0.1:'+receiver.tcp});const peer=await until(async()=>(await sender.command({command:'state'})).peers.find(p=>p.browser),'browser peer');await sender.command({command:'send',target:peer.id,files:[source]});await page.locator('#requests .primary').click();await until(()=>page.evaluate(()=>window.__blocked),'two MiB written');
  if(mode==='pause'){
   const active=page.locator('#jobs .folder-group.lifecycle-active').first();await active.locator('button.pause').click();
   await until(()=>page.evaluate(async()=>{try{const dir=await(await navigator.storage.getDirectory()).getDirectoryHandle('目录');return(await(await dir.getFileHandle('large.bin')).getFile()).size===2*1024*1024;}catch{return false;}}),'committed partial on pause');await delay(600);assert.equal(offsets.length,1,'paused receives do not resume by themselves');await active.locator('button.pause').click();
  }
  await until(()=>page.evaluate(async()=>{try{const dir=await(await navigator.storage.getDirectory()).getDirectoryHandle('目录');return(await(await dir.getFileHandle('large.bin')).getFile()).size===8*1024*1024;}catch{return false;}}),'resumed destination');
  const bytes=await page.evaluate(async()=>{const dir=await(await navigator.storage.getDirectory()).getDirectoryHandle('目录'),file=await(await dir.getFileHandle('large.bin')).getFile();return Array.from(new Uint8Array(await file.arrayBuffer()));});assert.deepEqual(Buffer.from(bytes),data);assert(offsets.slice(1).includes(2*1024*1024),'second request starts at retained prefix');assert.deepEqual(errors,[]);
  await until(async()=>{const jobs=(await sender.command({command:'state'})).jobs;return jobs.length&&jobs.every(j=>j.state===3);},'sender sees durable completion');
  const markers=await page.evaluate(async()=>{const dir=await(await navigator.storage.getDirectory()).getDirectoryHandle('目录'),names=[];for await(const name of dir.keys())names.push(name);return names.filter(name=>name.startsWith('.chuan-browser-'));});assert.deepEqual(markers,[]);
  console.log('PASS '+platform+' browser directory '+mode+': offsets='+offsets.join(',')+'; exact saved bytes; sender confirmed');await page.close();receiver.child.kill();sender.child.kill();
 }
 console.log('RESULTS '+root);
}finally{await browser.close();for(const child of children)if(child.exitCode===null)child.kill();}
