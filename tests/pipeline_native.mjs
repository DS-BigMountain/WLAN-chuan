// Real Windows sender/receiver; the TCP proxy holds 15 checks and four payloads independently.
import net from 'node:net';import dgram from 'node:dgram';import path from 'node:path';
import {spawn} from 'node:child_process';import {mkdir,writeFile,readFile} from 'node:fs/promises';
import {randomUUID} from 'node:crypto';import assert from 'node:assert/strict';
const root=path.resolve('build/pipeline-qa/native',randomUUID()),children=[],connections=new Set();
const sleep=ms=>new Promise(resolve=>setTimeout(resolve,ms));
async function until(fn,label){const end=Date.now()+15000;while(Date.now()<end){if(await fn())return;await sleep(20);}throw Error('timeout: '+label);}
async function port(udp=false){const server=udp?dgram.createSocket('udp4'):net.createServer();await new Promise(resolve=>udp?server.bind(0,'127.0.0.1',resolve):server.listen(0,'127.0.0.1',resolve));const value=server.address().port;await new Promise(resolve=>server.close(resolve));return value;}
async function host(name,extra=[]){const tcp=await port(),udp=await port(true),work=path.join(root,name);await mkdir(work,{recursive:true});const child=spawn(path.resolve('build/v2.0/Release/Chuan.exe'),['--headless','--port',String(tcp),'--discovery-port',String(udp),'--data-dir',path.join(work,'config'),'--receive-dir',path.join(work,'received'),'--run-seconds','120',...extra],{windowsHide:true,stdio:'ignore'});children.push(child);await until(async()=>{try{return (await fetch('http://127.0.0.1:'+tcp+'/api/info')).ok;}catch{return false;}},'host ready');return {tcp,work,child};}
let proxy;
try{
  const bytes=Buffer.alloc(65536,53),source=path.join(root,'source','流水线'),receiver=await host('receiver');
  await mkdir(source,{recursive:true});await mkdir(path.join(receiver.work,'received','流水线'),{recursive:true});
  for(let i=0;i<17;i++){await writeFile(path.join(source,'slow-'+i+'.bin'),bytes);await writeFile(path.join(receiver.work,'received','流水线','slow-'+i+'.bin'),bytes);}
  for(let i=0;i<4;i++)await writeFile(path.join(source,'new-'+i+'.bin'),bytes);
  let checks=0,uploads=0,releaseBodies=false,slowReleased=false;const waiting=[],slowReleases=[],seen=[];
  proxy=net.createServer(client=>{
    const upstream=net.connect(receiver.tcp,'127.0.0.1');connections.add(client);connections.add(upstream);
    client.on('error',()=>{});upstream.on('error',()=>client.destroy());upstream.pipe(client);
    client.on('close',()=>{connections.delete(client);upstream.destroy();});upstream.on('close',()=>connections.delete(upstream));
    let header=Buffer.alloc(0),parsed=false,bodyGate=false,slowGate=false;const chunks=[];
    client.on('data',data=>{
      if(!parsed){header=Buffer.concat([header,data]);const end=header.indexOf('\r\n\r\n');if(end<0)return;parsed=true;
        const text=header.toString('ascii',0,end),name=Buffer.from(text.match(/X-Chuan-Path: ([^\r\n]+)/i)?.[1]||'','hex').toString('utf8').replaceAll('\\','/');
        seen.push([text.split('\r\n')[0],name]);
        if(text.startsWith('POST /api/copy-probe ')&&/\/slow-\d+\.bin$/.test(name)&&!slowReleased){checks++;slowGate=true;chunks.push(header);slowReleases.push(()=>{slowGate=false;for(const chunk of chunks.splice(0))upstream.write(chunk);});return;}
        if(text.startsWith('POST /api/upload ')&&name.includes('/new-')){uploads++;bodyGate=true;upstream.write(header.subarray(0,end+4));data=header.subarray(end+4);waiting.push(()=>{bodyGate=false;for(const chunk of chunks.splice(0))upstream.write(chunk);});}
        else{upstream.write(header);return;}
      }
      if(slowGate||bodyGate&&!releaseBodies)chunks.push(data);else upstream.write(data);
    });
  });await new Promise(resolve=>proxy.listen(0,'127.0.0.1',resolve));
  const sender=await host('sender',['--send-to','127.0.0.1:'+proxy.address().port,'--send',source]);
  try{await until(()=>checks===15&&uploads===4,'four uploads while slow preparation remains blocked');}catch(error){console.log({checks,uploads,seen});throw error;}
  releaseBodies=true;for(const release of waiting)release();
  for(let i=0;i<4;i++)await until(async()=>{try{return (await readFile(path.join(receiver.work,'received','流水线','new-'+i+'.bin'))).equals(bytes);}catch{return false;}},'commit ready payload');
  assert.equal(sender.child.exitCode,null,'slow checks are still outstanding');assert.equal(checks,15);slowReleased=true;for(const release of slowReleases)release();
  assert.equal(sender.child.exitCode??await new Promise(resolve=>sender.child.on('exit',resolve)),0);
  console.log('PASS real Windows pipeline: four payloads commit ahead of 15 blocked verifications; exact byte identity');
}finally{for(const child of children)if(child.exitCode===null)child.kill();for(const socket of connections)socket.destroy();if(proxy)await new Promise(resolve=>proxy.close(resolve));}
