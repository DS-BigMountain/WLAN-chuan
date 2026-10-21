// Production browser UI against both server implementations, with independent phase gates.
import {spawn} from 'node:child_process';import {mkdir,writeFile,readFile} from 'node:fs/promises';
import net from 'node:net';import dgram from 'node:dgram';import path from 'node:path';
import {randomUUID,createHash} from 'node:crypto';import {pathToFileURL} from 'node:url';import assert from 'node:assert/strict';
const root=path.resolve('build/pipeline-qa/browser',randomUUID()),children=[];
const {chromium}=await import(pathToFileURL(path.resolve('build/web-qa/node_modules/playwright-core/index.mjs')));
const browser=await chromium.launch({channel:'chrome',headless:true});
const gate=()=>{let release;return {promise:new Promise(resolve=>release=resolve),release:()=>release()};};
const delay=ms=>new Promise(resolve=>setTimeout(resolve,ms));
async function until(fn,label){const end=Date.now()+15000;while(Date.now()<end){if(await fn())return;await delay(30);}throw Error('timeout: '+label);}
async function port(udp=false){const socket=udp?dgram.createSocket('udp4'):net.createServer();await new Promise(resolve=>udp?socket.bind(0,'127.0.0.1',resolve):socket.listen(0,'127.0.0.1',resolve));const value=socket.address().port;await new Promise(resolve=>socket.close(resolve));return value;}
try{for(const platform of ['windows','android']){
  const work=path.join(root,platform),tcp=await port(),udp=await port(true);await mkdir(work,{recursive:true});
  const child=platform==='windows'?spawn(path.resolve('build/v2.0/Release/Chuan.exe'),['--headless','--port',String(tcp),'--discovery-port',String(udp),'--data-dir',path.join(work,'config'),'--receive-dir',path.join(work,'received'),'--run-seconds','120'],{windowsHide:true,stdio:'ignore'}):spawn('java',['-Dfile.encoding=UTF-8','-cp',path.resolve('build/mobile-core'),'local.chuan.test.HostMain','--root',work,'--assets',path.resolve('src/web'),'--port',String(tcp),'--discovery-port',String(udp)],{windowsHide:true,stdio:['pipe','ignore','inherit']});children.push(child);
  await until(async()=>{try{return (await fetch('http://127.0.0.1:'+tcp+'/api/info')).ok;}catch{return false;}},'server');
  const source=path.join(work,'source','流水线'),bytes=Buffer.alloc(65536,71);await mkdir(source,{recursive:true});await mkdir(path.join(work,'received','流水线'),{recursive:true});for(let i=0;i<17;i++){await writeFile(path.join(source,'a-slow-'+i+'.bin'),bytes);await writeFile(path.join(work,'received','流水线','a-slow-'+i+'.bin'),bytes);}for(let i=0;i<4;i++)await writeFile(path.join(source,'new-'+i+'.bin'),bytes);
  const slow=gate(),payload=gate(),page=await browser.newPage(),errors=[];let slowChecks=0,uploads=0,slowReleased=false;
  page.on('pageerror',error=>errors.push(error.message));
  await page.route('**/web/api/copy-probe',async route=>{
    const name=Buffer.from(route.request().headers()['x-chuan-path']||'','hex').toString('utf8');
    if(/\/a-slow-\d+\.bin$/.test(name)&&!slowReleased){slowChecks++;await slow.promise;}await route.continue();
  });
  await page.route('**/web/api/upload',async route=>{
    const name=Buffer.from(route.request().headers()['x-chuan-path']||'','hex').toString('utf8');
    if(name.includes('/new-')){uploads++;await payload.promise;}await route.continue();
  });
  await page.addInitScript(()=>{
    const RealWorker=window.Worker;window.hashWorkers={started:0,active:0};
    window.Worker=class extends RealWorker{
      constructor(url,options){super(url,options);this.hash=String(url).endsWith('/hash_worker.js');if(this.hash){hashWorkers.started++;hashWorkers.active++;}}
      terminate(){if(this.hash){hashWorkers.active--;this.hash=false;}return super.terminate();}
    };
  });
  await page.goto('http://127.0.0.1:'+tcp);const hashResult=await page.evaluate(async()=>{
    const {hashFile}=await import('/web/hash_file.js');const data=Uint8Array.from({length:2097165},(_,i)=>i*37&255);
    const empty=Array.from(await hashFile(new Blob([]))).map(b=>b.toString(16).padStart(2,'0')).join('');
    const digest=Array.from(await hashFile(new Blob([data]))).map(b=>b.toString(16).padStart(2,'0')).join('');
    const controller=new AbortController(),pending=hashFile(new Blob([new Uint8Array(16*1024*1024)]),controller.signal);controller.abort();
    let cancelled=false;try{await pending;}catch(error){cancelled=error.name==='AbortError';}
    return {empty,digest,cancelled,...hashWorkers};
  });
  assert.equal(hashResult.empty,createHash('sha256').digest('hex'));assert.equal(hashResult.digest,createHash('sha256').update(Buffer.from(Uint8Array.from({length:2097165},(_,i)=>i*37&255))).digest('hex'));assert(hashResult.cancelled);assert.equal(hashResult.active,0);
  await page.locator('.device').first().click();await page.locator('#folder-picker').setInputFiles(source);
  try{await until(()=>slowChecks===15&&uploads===4,'four browser uploads while all 15 verification slots are blocked');
    await page.locator('#jobs .current-operation').waitFor();
    payload.release();for(let i=0;i<4;i++)await until(async()=>{try{return (await readFile(path.join(work,'received','流水线','new-'+i+'.bin'))).equals(bytes);}catch{return false;}},'ready payload committed');
    slowReleased=true;slow.release();await until(async()=>(await page.locator('#jobs .job-status').innerText()).includes('完成'),'folder complete');assert.deepEqual(errors,[]);assert.equal(await page.evaluate(()=>hashWorkers.active),0);assert.equal(await page.evaluate(()=>hashWorkers.started),20,'17 existing files hash in workers; four new files need no hash pre-pass');
    await page.locator('#limits').click();const download=page.waitForEvent('download');await page.locator('#export-diagnostics').click();const artifact=await download,log=path.join(work,'exported.jsonl');await artifact.saveAs(log);
    const raw=await readFile(log,'utf8'),events=raw.trim().split('\n').map(JSON.parse);
    assert(events.some(event=>event.event==='file_phase'&&event.path?.endsWith('new-0.bin')));
    assert(events.some(event=>event.event==='pipeline'&&event.transferring===4));
    assert(events.some(event=>event.event==='pipeline'&&event.verifying===15));
    assert(events.filter(event=>event.event==='pipeline').every(event=>event.preparing<=15&&event.verifying<=15&&event.transferring<=4));
    assert(events.filter(event=>event.event==='pipeline').every(event=>event.preparation_limit===15&&event.verification_limit===15&&event.ready_limit===32));
    assert(events.some(event=>['transfer_end','payload_end'].includes(event.event)),'export includes host payload timings');
    assert(events.filter(event=>event.event==='export_manifest').every(event=>event.dropped_events===0));
    await page.reload();await page.locator('.device').first().waitFor();
    await page.locator('#limits').click();const secondDownload=page.waitForEvent('download');await page.locator('#export-diagnostics').click();const persisted=path.join(work,'after-reload.jsonl');await(await secondDownload).saveAs(persisted);
    assert((await readFile(persisted,'utf8')).includes('"direction":"send"'),'browser pipeline records survive reload');
    console.log('PASS authenticated combined JSONL export, payload timings, four-slot snapshots, durable browser diagnostics');
    console.log('PASS browser -> '+platform+': four payloads commit ahead of 15 blocked verifications; synchronized concurrency UI');
  }finally{slow.release();payload.release();await page.close();}
}}finally{await browser.close();for(const child of children)if(child.exitCode===null)child.kill();}
