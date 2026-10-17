// Directed cross-host approvals: Windows and Android brokers retain independent pending batches.
import http from 'node:http';
import net from 'node:net';
import dgram from 'node:dgram';
import {spawn} from 'node:child_process';
import {mkdir,readFile,writeFile} from 'node:fs/promises';
import {randomUUID} from 'node:crypto';
import {pathToFileURL} from 'node:url';
import path from 'node:path';
import assert from 'node:assert/strict';
const work=path.resolve(process.argv[2]||'build/pending-cross',randomUUID());await mkdir(work,{recursive:true});
const delay=ms=>new Promise(resolve=>setTimeout(resolve,ms));
async function until(fn,label,timeout=16000){const deadline=Date.now()+timeout;while(Date.now()<deadline){const value=await fn();if(value)return value;await delay(60);}throw Error('timeout: '+label);}
async function port(udp=false){const socket=udp?dgram.createSocket('udp4'):net.createServer();await new Promise(resolve=>udp?socket.bind(0,'127.0.0.1',resolve):socket.listen(0,'127.0.0.1',resolve));const value=socket.address().port;await new Promise(resolve=>socket.close(resolve));return value;}
function request(tcp,url,headers={},body=null){return new Promise((resolve,reject)=>{const req=http.request({host:'127.0.0.1',port:tcp,path:url,method:body===null?'GET':'POST',agent:false,headers:{...headers,...(body===null?{}:{'Content-Length':Buffer.byteLength(body)})}},res=>{const chunks=[];res.on('data',b=>chunks.push(b));res.on('end',()=>resolve({status:res.statusCode,text:Buffer.concat(chunks).toString()}));res.on('error',reject);});req.on('error',reject);req.end(body);});}
let browser,win,java,sequence=0,buffer='';const answers=new Map(),errors=[],reports=[];
async function command(data){const request=++sequence;const result=new Promise(resolve=>answers.set(request,resolve));java.stdin.write(JSON.stringify({...data,request})+'\n');return result;}
try{
 const wp=await port(),wu=await port(true),ap=await port(),au=await port(true);
 win=spawn(path.resolve('build/v2.0/Release/Chuan.exe'),['--headless','--port',String(wp),'--discovery-port',String(wu),'--data-dir',path.join(work,'windows/config'),'--receive-dir',path.join(work,'windows/received'),'--run-seconds','180'],{windowsHide:true,stdio:'ignore'});
 java=spawn('java',['-Dfile.encoding=UTF-8','-Dstdout.encoding=UTF-8','-cp',path.resolve('build/mobile-core'),'local.chuan.test.HostMain','--root',path.join(work,'android'),'--assets',path.resolve('src/web'),'--port',String(ap),'--discovery-port',String(au),'--auto-receive','false'],{windowsHide:true,stdio:['pipe','pipe','inherit']});
 java.stdout.on('data',chunk=>{buffer+=chunk;let end;while((end=buffer.indexOf('\n'))>=0){const line=buffer.slice(0,end);buffer=buffer.slice(end+1);if(line.startsWith('ANSWER ')){const reply=JSON.parse(line.slice(7));answers.get(reply.request)?.(reply.result);answers.delete(reply.request);}}});
 for(const p of [wp,ap])await until(async()=>{try{return(await request(p,'/api/info')).status===200;}catch{return false;}},'host startup');
 const {chromium}=await import(pathToFileURL(path.resolve('build/web-qa/node_modules/playwright-core/index.mjs')));browser=await chromium.launch({channel:'chrome',headless:true});
 for(const androidBroker of [true,false]){
   const receiver=await(await browser.newContext({viewport:{width:390,height:844}})).newPage(),sender=await(await browser.newContext({viewport:{width:390,height:844}})).newPage();for(const page of [sender,receiver])page.on('pageerror',error=>errors.push(error.message));
   await receiver.goto('http://127.0.0.1:'+(androidBroker?wp:ap));await receiver.locator('#service-status').filter({hasText:'已连接服务主机'}).waitFor();const name=await receiver.locator('#browser-name').textContent();
   await sender.goto('http://127.0.0.1:'+(androidBroker?ap:wp));await command({command:'connect',address:'127.0.0.1:'+wp});
   const peer=await until(async()=>{const snapshot=await command({command:'state'});return snapshot.peers.find(peer=>peer.browser&&peer.name===name);},'remote browser route');
   await sender.locator('#switch-device').click();await sender.locator('#devices .device[data-peer-id="'+peer.id+'"]').click();
   const tag=androidBroker?'Android':'Windows';await sender.locator('#picker').setInputFiles([1,2].map(i=>({name:tag+'-'+i+'.txt',mimeType:'text/plain',buffer:Buffer.from(tag+' payload '+i)})));
   await until(async()=>await receiver.locator('.approval-file').count()===2,'both cross-host pending requests visible');
   await receiver.getByLabel('全选',{exact:true}).uncheck();await receiver.locator('.approval-file').filter({hasText:tag+'-2.txt'}).locator('input').check();await receiver.getByRole('button',{name:'接收',exact:true}).click();
   const row=receiver.locator('#jobs .row').filter({hasText:tag+'-2.txt'});await row.getByRole('link',{name:'下载',exact:true}).waitFor();const [download]=await Promise.all([receiver.waitForEvent('download'),row.getByRole('link',{name:'下载',exact:true}).click()]);const saved=path.join(work,tag+'.txt');await download.saveAs(saved);assert.equal(await readFile(saved,'utf8'),tag+' payload 2');
   assert(await receiver.locator('.approval-file').filter({hasText:tag+'-1.txt'}).isVisible());assert((await sender.locator('#jobs .row').filter({hasText:tag+'-1.txt'}).textContent()).includes('等待对方确认'));
   await sender.close();await until(async()=>await receiver.locator('.approval-file').count()===0,'cross-host source closure cancels the remaining request',6000);await receiver.close();reports.push(tag+' broker: later acceptance passes intact payload while first waits; closure cancels remote consent');
 }
 assert.deepEqual(errors,[]);await writeFile(path.join(work,'results.json'),JSON.stringify(reports,null,2));console.log('PASS '+reports.join('; '));console.log('RESULTS '+work);
}catch(error){if(browser)for(const[i,page]of browser.contexts().flatMap(context=>context.pages()).entries())try{console.log('PAGE '+i+' '+(await page.locator('body').innerText()).slice(-2600));await page.screenshot({path:path.join(work,'failure-'+i+'.png'),fullPage:true});}catch{}throw error;}
finally{await browser?.close();for(const child of [win,java])if(child?.exitCode===null)child.kill();}
