// 真实 Chrome、Web 服务主机与原生接收端的文件夹中转基准。
// node tests/web_relay.mjs <Chuan.exe> <结果目录> [替换的 app.js] [--concurrent]
import http from 'node:http';
import net from 'node:net';
import dgram from 'node:dgram';
import {spawn} from 'node:child_process';
import {readFile,writeFile,mkdir} from 'node:fs/promises';
import {randomUUID} from 'node:crypto';
import {pathToFileURL} from 'node:url';
import path from 'node:path';
import assert from 'node:assert/strict';
const {chromium}=await import(pathToFileURL(path.resolve('build/web-qa/node_modules/playwright-core/index.mjs')));
const exe=path.resolve(process.argv[2]),work=path.resolve(process.argv[3],randomUUID());await mkdir(work,{recursive:true});
const browserSource=process.argv[4]&&!process.argv[4].startsWith('--')?process.argv[4]:null;
const children=[],errors=[];let browser;
const delay=ms=>new Promise(r=>setTimeout(r,ms));
async function port(){const server=net.createServer();await new Promise(r=>server.listen(0,'127.0.0.1',r));const result=server.address().port;await new Promise(r=>server.close(r));return result;}
async function udpPort(){const socket=dgram.createSocket('udp4');await new Promise(r=>socket.bind(0,'127.0.0.1',r));const result=socket.address().port;socket.close();return result;}
function get(port,url){return new Promise((resolve,reject)=>{const req=http.get({host:'127.0.0.1',port,path:url,agent:false},res=>{const chunks=[];res.on('data',b=>chunks.push(b));res.on('end',()=>resolve({status:res.statusCode,body:Buffer.concat(chunks)}));res.on('error',reject);});req.on('error',reject);req.setTimeout(5000,()=>req.destroy(new Error('request timeout')));});}
async function start(name){
  const tcp=await port(),udp=await udpPort(),root=path.join(work,name);
  const child=spawn(exe,['--headless','--port',String(tcp),'--discovery-port',String(udp),'--data-dir',path.join(root,'config'),'--receive-dir',path.join(root,'received'),'--name',name,'--run-seconds','180'],{windowsHide:true,stdio:'inherit'});
  children.push({child,exited:new Promise(r=>child.on('exit',r))});
  for(let i=0;i<100;i++){try{const result=await get(tcp,'/api/info');if(result.status===200)return {tcp,udp,root,info:JSON.parse(result.body)};}catch{}await delay(50);}
  throw new Error('server startup: '+name);
}
try{
  const relay=await start('RelayHost'),receiver=await start('RelayReceiver');
  const socket=dgram.createSocket('udp4');await new Promise((resolve,reject)=>socket.send(Buffer.from(`CHUAN1|${receiver.info.id}|${receiver.tcp}|${receiver.info.nameHex}`),relay.udp,'127.0.0.1',e=>e?reject(e):resolve()));socket.close();
  browser=await chromium.launch({channel:'chrome',headless:true,timeout:15000});
  const page=await browser.newPage();page.on('pageerror',e=>errors.push(e.message));
  if(browserSource)await page.route('**/web/app.js',async route=>route.fulfill({contentType:'text/javascript',body:await readFile(browserSource,'utf8')}));
  const requests={state:0,upload:0,forward:0,wait:0};let measure=false;
  page.on('request',request=>{if(measure)for(const key of Object.keys(requests))if(new URL(request.url()).pathname.startsWith('/web/api/'+key))requests[key]++;});
  await page.goto('http://127.0.0.1:'+relay.tcp);await page.getByRole('button',{name:/RelayReceiver/}).click({timeout:15000});
  const runs=[];
  for(let run=0;run<3;run++){
    const name='资料-'+run,folder=path.join(work,name),fixtures=[];await mkdir(path.join(folder,'子目录'),{recursive:true});
    for(let i=0;i<24;i++){
      const relative=(i%2?'子目录/':'')+'文件-'+i+'.bin',data=Buffer.alloc(i===0?0:4096,i);
      await writeFile(path.join(folder,relative),data);fixtures.push({relative,data});
    }
    for(const key of Object.keys(requests))requests[key]=0;measure=true;const started=performance.now();
    await page.locator('#folder-picker').setInputFiles(folder);
    await page.waitForFunction(name=>[...document.querySelectorAll('.folder-row')].some(row=>row.textContent.includes(name+' · 24/24')&&row.textContent.includes('已完成')),name,{timeout:90000});
    const milliseconds=performance.now()-started;measure=false;
    for(const {relative,data} of fixtures)assert.deepEqual(await readFile(path.join(receiver.root,'received',name,relative)),data);
    const result={run:run+1,files:fixtures.length,bytes:fixtures.reduce((n,f)=>n+f.data.length,0),milliseconds,requests:{...requests},byteIdentity:true};runs.push(result);console.log(JSON.stringify(result));
  }
  const concurrent=[];
  if(process.argv.includes('--concurrent'))await Promise.all([1,2].map(async index=>{
    const sender=await browser.newPage();sender.on('pageerror',error=>errors.push(error.message));
    await sender.goto('http://127.0.0.1:'+relay.tcp);await sender.getByRole('button',{name:/RelayReceiver/}).click({timeout:15000});
    const name='并发-'+index,folder=path.join(work,name),data=Buffer.alloc(8192,index);await mkdir(folder,{recursive:true});
    for(let i=0;i<32;i++)await writeFile(path.join(folder,i+'.bin'),data);
    const start=performance.now();await sender.locator('#folder-picker').setInputFiles(folder);
    await sender.waitForFunction(name=>[...document.querySelectorAll('.folder-row')].some(row=>row.textContent.includes(name+' · 32/32')&&row.textContent.includes('已完成')),name,{timeout:90000});
    for(let i=0;i<32;i++)assert.deepEqual(await readFile(path.join(receiver.root,'received',name,i+'.bin')),data);
    concurrent.push({client:index,files:32,milliseconds:performance.now()-start,byteIdentity:true});await sender.close();
  }));
  assert.deepEqual(errors,[]);await page.screenshot({path:path.join(work,'completed.png'),fullPage:true});
  const result={scope:'Windows loopback; actual Chrome directory input, relay and native receiver; includes browser completion display',exe,browserSource:browserSource||'embedded',runs,concurrent};
  await writeFile(path.join(work,'results.json'),JSON.stringify(result,null,2));console.log('PASS nested paths, empty file, repeated batches and byte identity');console.log('RESULTS',path.join(work,'results.json'));
  if(concurrent.length)console.log('PASS simultaneous browser folders',JSON.stringify(concurrent));
}finally{await browser?.close();for(const {child} of children)child.kill();await Promise.all(children.map(x=>x.exited));}
