// Exercises the real HTTP endpoints and inspects the diagnostic log, including ownership checks.
// Usage: node tests/diagnostics.mjs path/to/Chuan.exe path/to/test-work
import http from 'node:http';
import net from 'node:net';
import dgram from 'node:dgram';
import {spawn} from 'node:child_process';
import {randomBytes,randomUUID,createHash} from 'node:crypto';
import {mkdir,readFile,readdir} from 'node:fs/promises';
import path from 'node:path';
import assert from 'node:assert/strict';
const exe=path.resolve(process.argv[2]),work=path.resolve(process.argv[3],randomUUID());
await mkdir(work,{recursive:true});
const listener=net.createServer();await new Promise(r=>listener.listen(0,'127.0.0.1',r));const port=listener.address().port;await new Promise(r=>listener.close(r));
const udp=dgram.createSocket('udp4');await new Promise(r=>udp.bind(0,'127.0.0.1',r));const discovery=udp.address().port;udp.close();
const child=spawn(exe,['--headless','--port',String(port),'--discovery-port',String(discovery),'--data-dir',path.join(work,'config'),'--receive-dir',path.join(work,'received'),'--name','DiagnosticsTest','--run-seconds','60'],{windowsHide:true,stdio:'inherit'});
const exited=new Promise(resolve=>child.on('exit',resolve));
const delay=ms=>new Promise(r=>setTimeout(r,ms));
const client=randomUUID().replaceAll('-',''),other=randomUUID().replaceAll('-','');let token='';
function request(url,headers={},body=null){return new Promise((resolve,reject)=>{
 const req=http.request({host:'127.0.0.1',port,path:url,method:body===null?'GET':'POST',agent:false,headers:{...headers,...(body!==null?{'Content-Length':body.length}:{})}},res=>{
 const chunks=[];res.on('data',b=>chunks.push(b));res.on('error',reject);res.on('end',()=>resolve({status:res.statusCode,body:Buffer.concat(chunks)}));});
 req.on('error',reject);req.setTimeout(10000,()=>req.destroy(new Error('request timeout')));req.end(body);
});}
const state=who=>request('/web/api/state',{'X-Chuan-Client':who,'X-Chuan-Tab':who});
const post=(url,body,extra={})=>request(url,{'X-Chuan-Client':client,'X-Chuan-Tab':client,'X-Chuan-Token':token,...extra},body);
const hex=s=>Buffer.from(s).toString('hex');
try {
 let initial;for(let n=0;n<100;n++){try{const res=await state(client);if(res.status===200){initial=JSON.parse(res.body);break;}}catch{}await delay(50);}assert(initial,'server startup');token=initial.token;
 const data=randomBytes(8*1024*1024),digest=createHash('sha256').update(data).digest(),key=randomUUID().replaceAll('-','');
 const headers={'X-Chuan-Path':hex('sample.bin'),'X-Chuan-Sender':hex('Audit'),'X-Chuan-Size':String(data.length),'X-Chuan-Upload':key,'X-Chuan-Hash-Ms':'1234'};
 const upload=await post('/web/api/upload',Buffer.concat([data,digest]),headers);assert.equal(upload.status,201);const id=JSON.parse(upload.body).id;
 assert.deepEqual(await readFile(path.join(work,'received/sample.bin')),data);
 const download=await request('/web/files/'+id+'?client='+client);assert.equal(download.status,200);assert.deepEqual(download.body,data);
 const report={'X-Chuan-Upload':key,'X-Chuan-Hash-Ms':'1234','X-Chuan-Upload-Ms':'1500','X-Chuan-Body-Ms':'1400','X-Chuan-Status':'201'};
 assert.equal((await post('/web/api/diagnostic',Buffer.alloc(0),{...report,'X-Chuan-Token':'wrong'})).status,403);
 const otherState=JSON.parse((await state(other)).body);
 assert.equal((await post('/web/api/diagnostic',Buffer.alloc(0),{...report,'X-Chuan-Client':other,'X-Chuan-Token':otherState.token})).status,404);
 assert.equal((await post('/web/api/diagnostic',Buffer.alloc(0),{...report,'X-Chuan-Hash-Ms':'NaN'})).status,400);
 assert.equal((await post('/web/api/diagnostic',Buffer.alloc(0),report)).status,200);
 assert.equal((await post('/web/api/diagnostic',Buffer.alloc(0),report)).status,404,'duplicate report bounded');
 assert.equal((await post('/web/api/upload',Buffer.concat([Buffer.from('bad'),Buffer.alloc(32)]),{...headers,'X-Chuan-Path':hex('corrupt.bin'),'X-Chuan-Size':'3','X-Chuan-Upload':randomUUID().replaceAll('-','')})).status,422);
 assert.equal(initial.webUploadChecksumOptional,true);
 const fastHeaders={...headers,'X-Chuan-Path':hex('direct.bin'),'X-Chuan-Checksum':'none','X-Chuan-Hash-Ms':'0','X-Chuan-Upload':randomUUID().replaceAll('-','')};
 const fast=await post('/web/api/upload',data,fastHeaders);assert.equal(fast.status,201);const fastId=JSON.parse(fast.body).id;
 assert.deepEqual(await readFile(path.join(work,'received/direct.bin')),data);
 assert.equal((await post('/web/api/upload',Buffer.alloc(0),{...fastHeaders,'X-Chuan-Path':hex('empty.bin'),'X-Chuan-Size':'0'})).status,201);
 assert.equal((await post('/web/api/upload',data,{...fastHeaders,'X-Chuan-Size':String(data.length+1)})).status,400,'length mismatch rejected');
 assert.equal((await post('/web/api/upload',data,{...fastHeaders,'X-Chuan-Checksum':'unknown'})).status,400,'unknown integrity mode rejected');
 assert.equal((await post('/api/upload',data,{...fastHeaders,'Expect':'100-continue'})).status,400,'native SHA-256 remains mandatory');
 const truncated=await new Promise((resolve,reject)=>{
   const socket=net.createConnection({host:'127.0.0.1',port},()=>{
     const h={...fastHeaders,Host:'127.0.0.1:'+port,'Content-Length':'1000','X-Chuan-Size':'1000','X-Chuan-Path':hex('truncated.bin'),'X-Chuan-Client':client,'X-Chuan-Token':token};
     socket.end('POST /web/api/upload HTTP/1.1\r\n'+Object.entries(h).map(([k,v])=>k+': '+v+'\r\n').join('')+'\r\nabc');
   });let response='';socket.on('data',b=>response+=b);socket.on('end',()=>resolve(response));socket.on('error',reject);socket.setTimeout(5000,()=>socket.destroy(new Error('timeout')));
 });assert(truncated.startsWith('HTTP/1.1 422'),'interrupted raw upload rejected');
 assert(!(await readdir(path.join(work,'received'))).some(n=>n==='truncated.bin'||n.endsWith('.part')),'interrupted raw upload cleaned');
 assert.equal((await post('/web/api/diagnostics-export',Buffer.alloc(0),{'X-Chuan-Token':'wrong'})).status,403);
 const exported=await post('/web/api/diagnostics-export',Buffer.alloc(0));assert.equal(exported.status,200);
 const exportedEvents=exported.body.toString('utf8').trim().split('\n').map(JSON.parse);
 assert.equal(exportedEvents[0].event,'export_manifest');assert.equal(exportedEvents[0].dropped_events,0);
 assert(exportedEvents.some(e=>e.event==='file_phase'&&String(e.id)===String(id)));
 assert(exportedEvents.some(e=>e.event==='transfer_end'&&String(e.id)===String(id)));
 const otherExport=await request('/web/api/diagnostics-export',{'X-Chuan-Client':other,'X-Chuan-Tab':other,'X-Chuan-Token':otherState.token},Buffer.alloc(0));
 assert(!otherExport.body.toString('utf8').includes('sample.bin'),'another session cannot export file names from this transfer');
 await delay(350);
 const directory=path.join(work,'config/logs'),logs=(await readdir(directory)).filter(n=>n.endsWith('.jsonl'));assert.equal(logs.length,1);
 const raw=await readFile(path.join(directory,logs[0]),'utf8'),events=raw.trim().split('\n').map(JSON.parse);
 assert(!raw.includes(token)&&!raw.includes(otherState.token),'session secrets excluded');
 const start=events.find(e=>e.event==='transfer_start'&&String(e.id)===String(id));assert.equal(start.direction,'web_upload');assert.equal(start.browser_hash_ms,1234);assert.equal(start.remote_ip,'127.0.0.1');
 const end=events.find(e=>e.event==='transfer_end'&&String(e.id)===String(id));assert.equal(end.result,'verified_and_saved');assert.equal(end.done_bytes,data.length);assert(end.disk_call_ms>0&&end.hash_ms>0&&end.flush_ms>0);
 assert(events.some(e=>e.event==='transfer_end'&&e.result==='sent_to_socket'&&e.done_bytes===data.length));
 assert(events.some(e=>e.event==='transfer_end'&&e.failure_stage==='checksum'&&e.result==='failed'));
 const fastEnd=events.find(e=>e.event==='transfer_end'&&String(e.id)===String(fastId));assert.equal(fastEnd.result,'length_checked_and_saved');assert.equal(fastEnd.hash_ms,0);assert.equal(fastEnd.done_bytes,data.length);
 assert.equal(events.filter(e=>e.event==='browser_upload_report').length,1);
 assert(events.every(e=>!Number.isNaN(Date.parse(e.utc))));
 console.log('PASS upload/download integrity, phase timings, TCP metadata, browser report ownership, validation, deduplication, checksum failure, JSONL and secret exclusion');
 console.log('PASS direct upload, empty file, length mismatch, invalid mode, native checksum enforcement and interrupted upload cleanup');
 console.log('LOG',path.join(directory,logs[0]));
} finally {child.kill();await exited;}
