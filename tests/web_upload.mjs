import {readFileSync} from 'node:fs';
import {createHash} from 'node:crypto';
import vm from 'node:vm';
import assert from 'node:assert/strict';
const source=readFileSync(new URL('../src/web/app.js',import.meta.url),'utf8');
const run=source.slice(source.indexOf('function stopOutgoingFolder('),source.indexOf('async function poll()'));
const shaSource=readFileSync(new URL('../src/web/sha256.js',import.meta.url));
const {Sha256}=await import(`data:text/javascript;base64,${shaSource.toString('base64')}`);
for(const direct of [true,false]){
 const content=Buffer.from('browser upload fixture'),file=new Blob([content]);let reads=0;
 const slice=file.slice.bind(file);file.slice=(...args)=>{reads++;if(direct)throw new Error('direct upload must not pre-read file');return slice(...args);};
 const op={file,path:'video.bin',target:'host',key:'upload-key',item:0,plan:{id:'plan',promise:Promise.resolve('1')},cancelled:false};
 let sent,headers={},reports=[];
 class Xhr {
   constructor(){this.upload={};this.status=201;this.responseText='{"id":"7"}';}
   open(){}setRequestHeader(k,v){headers[k]=v;}
   send(body){sent=body;queueMicrotask(()=>{this.upload.onload();this.onload();this.onloadend();});}
 }
 const context={queueRunning:false,queue:[op],webBatchFiles:false,webUploadChecksumOptional:direct,webUploadStatus:false,Sha256:direct?class{constructor(){throw new Error('unexpected hashing');}}:Sha256,
   serverSession:{headers:()=>({})},online:true,peers:[{id:'host'}],hostId:'host',deviceName:'Phone',token:'token',client:'client',tab:'tab',
   encode:s=>Buffer.from(s).toString('hex'),refreshUploads(){},poll(){},command:async(...args)=>reports.push(args),
   XMLHttpRequest:Xhr,performance,Blob,Uint8Array,AbortController,setTimeout,clearTimeout,Date};
 await vm.runInNewContext(run+'\nrunQueue()',context);await Promise.resolve();
 for(let i=0;i<100&&!op.finished;i++)await new Promise(resolve=>setTimeout(resolve,1));
 assert.equal(op.status,'已完成');assert.equal(op.error,undefined);assert.equal(headers['X-Chuan-Checksum'],direct?'none':'sha256');
 if(direct){assert.equal(reads,0);assert.equal(sent,file);assert.equal(op.hashMs,0);}
 else {assert(reads>0);const payload=Buffer.from(await sent.arrayBuffer());assert.deepEqual(payload.subarray(0,content.length),content);assert.deepEqual(payload.subarray(content.length),createHash('sha256').update(content).digest());}
 assert.equal(reports[0][0],'/web/api/diagnostic');
}
console.log('PASS actual browser queue: direct File upload without pre-read/hash; legacy SHA-256 trailer fallback; diagnostic reporting');
