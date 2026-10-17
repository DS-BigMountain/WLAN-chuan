// 真实 TCP 请求验证请求头边界，并测量本机回环请求延迟。
// node tests/http_headers.mjs <Chuan.exe> <工作目录>
import net from 'node:net';
import dgram from 'node:dgram';
import {spawn} from 'node:child_process';
import {randomUUID,createHash} from 'node:crypto';
import {mkdir,readFile,writeFile} from 'node:fs/promises';
import path from 'node:path';
import assert from 'node:assert/strict';
const exe=path.resolve(process.argv[2]),work=path.resolve(process.argv[3],randomUUID());
await mkdir(work,{recursive:true});
const listener=net.createServer();await new Promise(r=>listener.listen(0,'127.0.0.1',r));
const port=listener.address().port;await new Promise(r=>listener.close(r));
const udp=dgram.createSocket('udp4');await new Promise(r=>udp.bind(0,'127.0.0.1',r));
const discovery=udp.address().port;udp.close();
const child=spawn(exe,['--headless','--port',String(port),'--discovery-port',String(discovery),'--data-dir',path.join(work,'config'),'--receive-dir',path.join(work,'received'),'--name','HeaderTest','--run-seconds','90'],{windowsHide:true,stdio:'inherit'});
const exited=new Promise(resolve=>child.on('exit',resolve));
const delay=ms=>new Promise(r=>setTimeout(r,ms));
function exchange(parts){return new Promise((resolve,reject)=>{
  const chunks=[],socket=net.createConnection({host:'127.0.0.1',port},async()=>{
    try{for(const part of parts){if(typeof part==='number')await delay(part);else socket.write(part);}socket.end();}catch(error){socket.destroy(error);}
  });
  socket.on('data',b=>chunks.push(b));socket.on('end',()=>resolve(Buffer.concat(chunks).toString()));
  socket.on('error',reject);socket.setTimeout(5000,()=>socket.destroy(new Error('TCP request timeout')));
});}
const request=padding=>'GET /api/info HTTP/1.1\r\nHost: 127.0.0.1\r\nX-Padding: '+'x'.repeat(padding)+'\r\n\r\n';
try{
  let ready=false;for(let n=0;n<100;n++){try{if((await exchange([request(0)])).startsWith('HTTP/1.1 200')){ready=true;break;}}catch{}await delay(50);}assert(ready,'server startup');
  for(const size of [4095,4096,4097,8191,8192,8193,16383]){
    const data=request(size-request(0).length);assert.equal(data.length,size);
    assert((await exchange([data])).startsWith('HTTP/1.1 200'),`header length ${size}`);
  }
  for(const tail of [1,2,3]){
    const data=request(5000);assert((await exchange([data.slice(0,-tail),15,data.slice(-tail)])).startsWith('HTTP/1.1 200'),`split terminator ${tail}`);
  }
  assert(!(await exchange([request(16384-request(0).length)])).startsWith('HTTP/1.1 200'),'oversize header rejected');
  assert(!(await exchange(['GET /api/info HTTP/1.1\r\nHost: a\r\nhOsT: b\r\n\r\n'])).startsWith('HTTP/1.1 200'),'duplicate header rejected');
  assert(!(await exchange(['GET /api/info HTTP/1.1\r\nHost: a\r\n\r'])).startsWith('HTTP/1.1 200'),'truncated header rejected');
  const payload=Buffer.from('首部与正文同包\r\n\r\nBody boundary fixture'),digest=createHash('sha256').update(payload).digest();
  const upload=(name,checksum)=>Buffer.concat([Buffer.from('POST /api/upload HTTP/1.1\r\nHost: 127.0.0.1\r\nExpect: 100-continue\r\nX-Chuan-Sender: 54657374\r\nX-Chuan-Path: '+Buffer.from(name).toString('hex')+'\r\nX-Chuan-Size: '+payload.length+'\r\nContent-Length: '+(payload.length+32)+'\r\n\r\n'),payload,checksum]);
  const uploaded=await exchange([upload('boundary.bin',digest)]);
  assert(uploaded.includes('HTTP/1.1 201'),'body and digest preserved after header: '+uploaded);
  assert.deepEqual(await readFile(path.join(work,'received/boundary.bin')),payload);
  assert((await exchange([upload('bad.bin',Buffer.alloc(32))])).includes('HTTP/1.1 422'),'checksum mismatch rejected');
  const results={scope:'Windows loopback, serial fresh TCP connections; not a Wi-Fi throughput measurement',exe,samples:[]};
  for(const padding of [600,6000]){
    const data=request(padding),times=[];
    for(let i=0;i<110;i++){
      const start=performance.now();assert((await exchange([data])).startsWith('HTTP/1.1 200'));
      if(i>=10)times.push(performance.now()-start);
    }
    times.sort((a,b)=>a-b);
    results.samples.push({headerBytes:data.length,requests:times.length,medianMs:times[50],p95Ms:times[94],meanMs:times.reduce((a,b)=>a+b,0)/times.length});
  }
  await writeFile(path.join(work,'results.json'),JSON.stringify(results,null,2));
  console.log('PASS fragmented headers, buffer boundaries, limit, duplicate/truncated rejection, coalesced body and SHA-256');
  console.log(JSON.stringify(results));console.log('RESULTS',path.join(work,'results.json'));
}finally{child.kill();await exited;}
