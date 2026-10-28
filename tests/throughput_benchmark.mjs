// Isolated application benchmark. Loopback results do not measure cross-device throughput.
// Usage: node tests/throughput_benchmark.mjs executable output-directory [MiB]
import http from 'node:http';
import net from 'node:net';
import dgram from 'node:dgram';
import path from 'node:path';
import {spawn} from 'node:child_process';
import {mkdir,readFile,readdir,writeFile} from 'node:fs/promises';
import {createReadStream} from 'node:fs';
import {createHash,randomBytes,randomUUID} from 'node:crypto';
import {once} from 'node:events';
import assert from 'node:assert/strict';

const exe=path.resolve(process.argv[2]),root=path.resolve(process.argv[3],randomUUID());
const size=Number(process.argv[4]||128)*1024*1024;
assert(Number.isSafeInteger(size)&&size>0&&size<=1024**3);
await mkdir(root,{recursive:true});
async function port(udp=false){const s=udp?dgram.createSocket('udp4'):net.createServer();await new Promise(r=>udp?s.bind(0,'127.0.0.1',r):s.listen(0,'127.0.0.1',r));const p=s.address().port;await new Promise(r=>s.close(r));return p;}
const tcp=await port(),udp=await port(true),client=randomUUID().replaceAll('-','');
const child=spawn(exe,['--headless','--port',String(tcp),'--discovery-port',String(udp),'--data-dir',path.join(root,'config'),'--receive-dir',path.join(root,'received'),'--run-seconds','180'],{windowsHide:true,stdio:'ignore'});
const exited=once(child,'exit');let token='';
function request(url,headers={},body=null,hashBody=false){
  let req;
  const result=new Promise((resolve,reject)=>{
    req=http.request({hostname:'127.0.0.1',port:tcp,path:url,method:body===null?'GET':'POST',headers,agent:false},res=>{
      const chunks=[],hash=hashBody?createHash('sha256'):null;let bytes=0;
      res.on('data',chunk=>{bytes+=chunk.length;if(hash)hash.update(chunk);else chunks.push(chunk);});
      res.on('error',reject);res.on('end',()=>resolve({status:res.statusCode,bytes,body:hash?hash.digest('hex'):Buffer.concat(chunks).toString()}));
    });
    req.on('error',reject);req.setTimeout(30000,()=>req.destroy(Error('timeout')));
  });
  if(body!==false)req.end(body);
  return {req,result};
}
try{
  let initial;
  for(let n=0;n<100;n++){
    try{const answer=await request('/web/api/state',{'X-Chuan-Client':client}).result;if(answer.status===200){initial=JSON.parse(answer.body);break;}}catch{}
    await new Promise(r=>setTimeout(r,50));
  }
  assert(initial,'host startup');token=initial.token;
  const block=randomBytes(1024*1024),digest=createHash('sha256');
  for(let offset=0;offset<size;offset+=block.length)digest.update(block.subarray(0,Math.min(block.length,size-offset)));
  const expected=digest.digest('hex'),samples=[];
  for(const chunkBytes of [4096,256*1024]){
    for(let repeat=0;repeat<2;repeat++){
      const name=`payload-${chunkBytes}-${repeat}.bin`;
      const headers={'X-Chuan-Client':client,'X-Chuan-Token':token,'X-Chuan-Path':Buffer.from(name).toString('hex'),'X-Chuan-Sender':Buffer.from('Benchmark').toString('hex'),'X-Chuan-Size':String(size),'X-Chuan-Checksum':'none','X-Chuan-Upload':randomUUID().replaceAll('-',''),'Content-Length':String(size)};
      const started=performance.now(),{req,result}=request('/web/api/upload',headers,false);
      for(let offset=0;offset<size;offset+=chunkBytes){const length=Math.min(chunkBytes,size-offset);if(!req.write(block.subarray(offset%block.length,offset%block.length+length)))await once(req,'drain');}
      req.end();const uploaded=await result,uploadMs=performance.now()-started;
      assert.equal(uploaded.status,201,uploaded.body);const id=JSON.parse(uploaded.body).id;
      const saved=createHash('sha256');for await(const chunk of createReadStream(path.join(root,'received',name)))saved.update(chunk);
      assert.equal(saved.digest('hex'),expected,'saved payload');
      const downloadAt=performance.now(),download=await request('/web/files/'+id+'?client='+client,{},null,true).result;
      const downloadMs=performance.now()-downloadAt;assert.equal(download.status,200);assert.equal(download.bytes,size);assert.equal(download.body,expected,'downloaded payload');
      const sample={chunkBytes,repeat,id,bytes:size,uploadMs,downloadMs,uploadMiBs:size/1048576/(uploadMs/1000),downloadMiBs:size/1048576/(downloadMs/1000)};
      samples.push(sample);console.log(JSON.stringify(sample));
    }
  }
  await new Promise(r=>setTimeout(r,500));
  const directory=path.join(root,'config','logs'),events=[];
  for(const name of await readdir(directory))if(name.endsWith('.jsonl'))for(const line of (await readFile(path.join(directory,name),'utf8')).trim().split('\n')){const event=JSON.parse(line);if(event.event==='transfer_end')events.push(event);}
  const report={scope:'Windows loopback application overhead only; cross-device throughput remains unmeasured',exe,samples,events};
  await writeFile(path.join(root,'results.json'),JSON.stringify(report,null,2));console.log('RESULTS '+path.join(root,'results.json'));
}finally{child.kill();await exited;}
