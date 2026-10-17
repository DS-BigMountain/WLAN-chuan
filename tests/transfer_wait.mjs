// 真实服务验证完成等待接口的归属权限、时限和取消唤醒。
// node tests/transfer_wait.mjs <Chuan.exe> <工作目录>
import http from 'node:http';
import net from 'node:net';
import dgram from 'node:dgram';
import {spawn} from 'node:child_process';
import {mkdir,readdir,writeFile} from 'node:fs/promises';
import {randomUUID} from 'node:crypto';
import path from 'node:path';
import assert from 'node:assert/strict';
const exe=path.resolve(process.argv[2]),work=path.resolve(process.argv[3],randomUUID());await mkdir(work,{recursive:true});
const children=[],delay=ms=>new Promise(r=>setTimeout(r,ms)),key=()=>randomUUID().replaceAll('-',''),hex=s=>Buffer.from(s).toString('hex');
async function port(){const server=net.createServer();await new Promise(r=>server.listen(0,'127.0.0.1',r));const value=server.address().port;await new Promise(r=>server.close(r));return value;}
async function udpPort(){const socket=dgram.createSocket('udp4');await new Promise(r=>socket.bind(0,'127.0.0.1',r));const value=socket.address().port;socket.close();return value;}
function request(port,url,headers={},body=null){return new Promise((resolve,reject)=>{
 const req=http.request({host:'127.0.0.1',port,path:url,method:body===null?'GET':'POST',agent:false,headers:{...headers,...(body!==null?{'Content-Length':body.length}:{})}},res=>{
 const chunks=[];res.on('data',b=>chunks.push(b));res.on('error',reject);res.on('end',()=>resolve({status:res.statusCode,text:Buffer.concat(chunks).toString()}));});
 req.on('error',reject);req.setTimeout(5000,()=>req.destroy(new Error('request timeout')));req.end(body);
});}
async function start(name,manual=false){
 const tcp=await port(),udp=await udpPort(),root=path.join(work,name);
 const child=spawn(exe,['--headless','--port',String(tcp),'--discovery-port',String(udp),'--data-dir',path.join(root,'config'),'--receive-dir',path.join(root,'received'),'--name',name,'--run-seconds','60',...(manual?['--no-auto']:[])],{windowsHide:true,stdio:'inherit'});
 children.push({child,exited:new Promise(r=>child.on('exit',r))});
 for(let i=0;i<100;i++){try{const r=await request(tcp,'/api/info');if(r.status===200)return {tcp,udp,root,info:JSON.parse(r.text)};}catch{}await delay(50);}throw new Error('startup '+name);
}
try{
 const relay=await start('Relay'),receiver=await start('ManualReceiver',true),client=key(),other=key();
 const socket=dgram.createSocket('udp4');await new Promise((resolve,reject)=>socket.send(Buffer.from(`CHUAN1|${receiver.info.id}|${receiver.tcp}|${receiver.info.nameHex}`),relay.udp,'127.0.0.1',e=>e?reject(e):resolve()));socket.close();
 const state=async who=>JSON.parse((await request(relay.tcp,'/web/api/state',{'X-Chuan-Client':who})).text);
 let snapshot;for(let i=0;i<100;i++){snapshot=await state(client);if(snapshot.peers.some(p=>p.id===receiver.info.id))break;await delay(100);}assert(snapshot.peers.some(p=>p.id===receiver.info.id));assert.equal(snapshot.webTransferWait,true);
 const credentials={'X-Chuan-Client':client,'X-Chuan-Token':snapshot.token},post=(url,headers={},body=Buffer.alloc(0))=>request(relay.tcp,url,{...credentials,...headers},body);
 const uploadKey=key(),upload=await post('/web/api/upload',{'X-Chuan-Path':hex('等待.bin'),'X-Chuan-Sender':hex('Test'),'X-Chuan-Size':'4','X-Chuan-Checksum':'none','X-Chuan-Upload':uploadKey,'X-Chuan-Target':hex(receiver.info.id)},Buffer.from('test'));
 assert.equal(upload.status,201);const uploadedId=JSON.parse(upload.text).id;
 const forward=await post('/web/api/forward/'+uploadedId,{'X-Chuan-Peer':hex(receiver.info.id),'X-Chuan-Upload':uploadKey});assert.equal(forward.status,202);const id=JSON.parse(forward.text).id,url='/web/api/wait/'+id;
 assert.equal((await post('/web/api/wait/'+uploadedId)).status,404,'receiving record is not an outgoing wait target');
 assert.equal((await post('/web/api/wait/999999999')).status,404);
 assert.equal((await post('/web/api/wait/bad')).status,400);
 assert.equal((await post(url,{},Buffer.from('x'))).status,400,'wait request has no body');
 assert.equal((await post(url,{'X-Chuan-Token':'invalid'})).status,403);
 const otherState=await state(other);
 assert.equal((await post(url,{'X-Chuan-Client':other,'X-Chuan-Token':otherState.token})).status,404,'another browser cannot inspect the task');
 const begin=performance.now(),active=await post(url),boundedWaitMs=performance.now()-begin;
 assert.equal(active.status,200);assert(JSON.parse(active.text).state<3,'one-second wait does not invent completion');assert(boundedWaitMs>=800&&boundedWaitMs<4500,'wait is bounded');
 const waiting=post(url);await delay(100);const cancelledAt=performance.now();
 assert.equal((await post('/web/api/cancel/'+id)).status,200);const stopped=await waiting,cancelWakeMs=performance.now()-cancelledAt;
 assert.equal(stopped.status,200);assert.equal(JSON.parse(stopped.text).state,4,'cancel wakes wait with a terminal state');
 assert.equal(JSON.parse((await post(url)).text).state,4,'terminal state is repeatable');
 assert.equal((await readdir(path.join(receiver.root,'received'))).length,0,'cancelled manual transfer leaves no received file');
 const result={boundedWaitMs,cancelWakeMs,ownership:true,validation:true,cancelled:true};await writeFile(path.join(work,'results.json'),JSON.stringify(result,null,2));
 console.log('PASS completion wait ownership, token, direction, input validation, bounded wait, cancellation and no destination file');console.log(JSON.stringify(result));
}finally{for(const {child} of children)child.kill();await Promise.all(children.map(x=>x.exited));}
