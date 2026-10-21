// Production browser stream: first destination bytes precede source completion.
import http from 'node:http';
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
try{for(const platform of ['windows','android']){
 const server=await host(platform,platform),page=await browser.newPage(),errors=[];page.on('pageerror',e=>errors.push(e.message));await page.goto('http://127.0.0.1:'+server.tcp);await page.locator('.device').first().waitFor();await page.locator('.device[data-peer-id="'+server.info.id+'"]').click();
 for(const mode of ['pause','reselect','changed-prefix']){
  const name='续传-'+mode+'.bin',file=path.join(root,name),data=Buffer.alloc(8*1024*1024);for(let i=0;i<data.length;i++)data[i]=(i*17+(i>>>12))%251;await writeFile(file,data);
  let intercepted=false,partial=false,release;const gate=new Promise(r=>release=r),offsets=[];
  await page.route('**/web/api/upload',async route=>{
   const request=route.request(),offset=Number(request.headers()['x-chuan-offset']||0);offsets.push(offset);
   if(!intercepted){intercepted=true;const headers={...request.headers()};delete headers['connection'];delete headers['transfer-encoding'];headers['content-length']=String(request.postDataBuffer().length);
    const socket=http.request(request.url(),{method:'POST',headers},res=>res.resume());socket.on('error',()=>{});socket.write(request.postDataBuffer().subarray(0,2*1024*1024));socket.flushHeaders();
    await until(async()=>{const entries=await readdir(path.join(server.work,'received'));for(const entry of entries.filter(f=>f.endsWith('.part'))){if((await stat(path.join(server.work,'received',entry))).size>=2*1024*1024)return true;}},'saved partial before interruption');partial=true;await gate;socket.destroy();await route.abort().catch(()=>{});
   }else await route.continue();
  });
  try{
   await page.locator('#picker').setInputFiles(file);await until(()=>partial,'partial upload');
   await delay(300);const active=page.locator('#jobs .lifecycle-active').filter({hasText:name}).first();
   if(mode==='pause'){
    await active.locator('button.pause').click();release();await until(async()=>/已暂停/.test(await active.innerText()),'paused state');await delay(800);assert.equal(offsets.length,1,'pause does not automatically retransmit');await active.locator('button.pause').click();
   }else{
    await active.locator('button').filter({hasText:'取消'}).click();release();await until(async()=>/已取消/.test(await page.locator('#jobs').innerText()),'cancelled state');
    if(mode==='changed-prefix'){data[1024*1024]^=1;await writeFile(file,data);}
    await page.locator('#picker').setInputFiles(file);
   }
   const complete=path.join(server.work,'received',name);await until(async()=>{try{return(await stat(complete)).size===data.length;}catch{}},'resumed file committed');assert.deepEqual(await readFile(complete),data);assert(offsets.slice(1).some(n=>n>=2*1024*1024),'saved bytes reused');if(mode==='changed-prefix')assert(offsets.slice(2).includes(0),'mismatched prefix triggers safe restart');assert.deepEqual(errors,[]);
   console.log('PASS '+platform+' '+mode+': offsets='+offsets.join(',')+'; exact content');
  }finally{release();await page.unroute('**/web/api/upload');}
 }
 await page.close();
}console.log('RESULTS '+root);}finally{await browser.close();for(const child of children)if(child.exitCode===null)child.kill();}
