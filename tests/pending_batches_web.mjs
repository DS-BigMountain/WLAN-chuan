// Directed real-host regression: later approvals must remain independent of the first pending batch.
import http from 'node:http';
import net from 'node:net';
import dgram from 'node:dgram';
import {spawn} from 'node:child_process';
import {mkdir,readFile,writeFile} from 'node:fs/promises';
import {randomUUID} from 'node:crypto';
import {pathToFileURL} from 'node:url';
import path from 'node:path';
import assert from 'node:assert/strict';
const android=process.argv[3]==='android',work=path.resolve(process.argv[2]||'build/pending-batches',randomUUID());await mkdir(work,{recursive:true});
const delay=ms=>new Promise(resolve=>setTimeout(resolve,ms));
async function until(fn,label,timeout=16000){const deadline=Date.now()+timeout;while(Date.now()<deadline){const value=await fn();if(value)return value;await delay(60);}throw Error('timeout: '+label);}
async function port(udp=false){const socket=udp?dgram.createSocket('udp4'):net.createServer();await new Promise(resolve=>udp?socket.bind(0,'127.0.0.1',resolve):socket.listen(0,'127.0.0.1',resolve));const value=socket.address().port;await new Promise(resolve=>socket.close(resolve));return value;}
function request(tcp,url,headers={},body=null){return new Promise((resolve,reject)=>{const req=http.request({host:'127.0.0.1',port:tcp,path:url,method:body===null?'GET':'POST',agent:false,headers:{...headers,...(body===null?{}:{'Content-Length':Buffer.byteLength(body)})}},res=>{const chunks=[];res.on('data',b=>chunks.push(b));res.on('end',()=>resolve({status:res.statusCode,text:Buffer.concat(chunks).toString()}));res.on('error',reject);});req.on('error',reject);req.end(body);});}
const reports=[],errors=[];let browser,child,sequence=0;const answers=new Map();
async function native(command){const request=++sequence;const result=new Promise(resolve=>answers.set(request,resolve));child.stdin.write(JSON.stringify({...command,request})+'\n');return result;}
try{
  const tcp=await port(),udp=await port(true),host=path.join(work,'host'),origin='http://127.0.0.1:'+tcp;
  child=android?spawn('java',['-Dfile.encoding=UTF-8','-Dstdout.encoding=UTF-8','-cp',path.resolve('build/mobile-core'),'local.chuan.test.HostMain','--root',host,'--assets',path.resolve('src/web'),'--port',String(tcp),'--discovery-port',String(udp),'--auto-receive','false'],{windowsHide:true,stdio:['pipe','pipe','inherit']}):spawn(path.resolve('build/v2.0/Release/Chuan.exe'),['--headless','--port',String(tcp),'--discovery-port',String(udp),'--data-dir',path.join(host,'config'),'--receive-dir',path.join(host,'received'),'--run-seconds','180'],{windowsHide:true,stdio:'ignore'});
  if(android){let buffer='';child.stdout.on('data',chunk=>{buffer+=chunk;let end;while((end=buffer.indexOf('\n'))>=0){const line=buffer.slice(0,end);buffer=buffer.slice(end+1);if(line.startsWith('ANSWER ')){const reply=JSON.parse(line.slice(7));answers.get(reply.request)?.(reply.result);answers.delete(reply.request);}}});}
  await until(async()=>{try{return (await request(tcp,'/api/info')).status===200;}catch{return false;}},'host startup');
  const {chromium}=await import(pathToFileURL(path.resolve('build/web-qa/node_modules/playwright-core/index.mjs')));browser=await chromium.launch({channel:'chrome',headless:true});
  const receiver=await(await browser.newContext({viewport:{width:390,height:844}})).newPage(),sender=await(await browser.newContext({viewport:{width:390,height:844}})).newPage();
  let senderClient='',receiverClient='';for(const [page,set] of [[sender,value=>senderClient=value],[receiver,value=>receiverClient=value]]){page.on('pageerror',error=>errors.push(error.message));page.on('request',req=>{if(req.url().endsWith('/web/api/state'))set(req.headers()['x-chuan-client']);});}
  await sender.addInitScript(()=>{const Original=Worker;globalThis.packWorkers=0;globalThis.Worker=class extends Original{constructor(...args){super(...args);packWorkers++;}};});
  await receiver.goto(origin);await receiver.locator('#service-status').filter({hasText:'已连接服务主机'}).waitFor();await sender.goto(origin);await sender.locator('#switch-device').click();await sender.locator('#devices .device[data-peer-id^="web:"]').first().click();
  await sender.locator('#folder-picker').evaluate(input=>{const files=new DataTransfer();files.items.add(new File(['flat'],'flat.txt'));input.files=files.files;input.dispatchEvent(new Event('change',{bubbles:true}));});
  await sender.locator('#connection').filter({hasText:'浏览器未提供文件夹目录'}).waitFor();assert.equal(await sender.locator('#jobs .row').count(),0,'a file-only picker result cannot silently become a folder transfer');
  const uploads=[];sender.on('request',req=>{if(req.url().endsWith('/web/api/upload'))uploads.push(Buffer.from(req.headers()['x-chuan-path'],'hex').toString());});
  const folder=path.join(work,'队列文件夹');await mkdir(path.join(folder,'子目录'),{recursive:true});await writeFile(path.join(folder,'a.txt'),'folder-a');await writeFile(path.join(folder,'子目录','b.txt'),'folder-b');
  for(let i=1;i<=7;i++)await sender.locator('#picker').setInputFiles({name:'pending-'+i+'.txt',mimeType:'text/plain',buffer:Buffer.from('payload-'+i)});
  await sender.locator('#folder-picker').setInputFiles(folder);
  await until(async()=>await receiver.locator('.approval-card').count()===8,'all eight pending cards visible');
  assert.equal(uploads.length,0);assert.equal(await sender.evaluate(()=>packWorkers),0,'no folder packing before approval');
  assert.equal(await sender.locator('#jobs .job-status').filter({hasText:'等待对方确认'}).count(),8);
  const state=async()=>JSON.parse((await request(tcp,'/web/api/state',{'X-Chuan-Client':senderClient})).text);
  const pending=await state();assert.equal(pending.webOfferAsync,true);assert.equal(pending.offers.filter(offer=>offer.state===1).length,8);
  const outsider=randomUUID().replaceAll('-',''),foreign=JSON.parse((await request(tcp,'/web/api/state',{'X-Chuan-Client':outsider})).text);assert.deepEqual(foreign.offers,[]);
  assert((await request(tcp,'/web/api/cancel/'+pending.offers[0].id,{'X-Chuan-Client':outsider,'X-Chuan-Token':foreign.token},'')).status>=400,'another browser cannot cancel this offer');
  const card=name=>receiver.locator('.approval-card').filter({hasText:name}),outgoing=name=>sender.locator('#jobs .row').filter({hasText:name});
  async function acceptAndSave(name,destination){
    await card(name).getByRole('button',{name:'全部接受',exact:true}).click();
    const row=receiver.locator('#jobs .row').filter({hasText:name});await row.getByRole('link',{name:'下载',exact:true}).waitFor();
    const [download]=await Promise.all([receiver.waitForEvent('download'),row.getByRole('link',{name:'下载',exact:true}).click()]);await download.saveAs(destination);
  }
  const second=path.join(work,'second.txt');await acceptAndSave('pending-2.txt',second);assert.equal(await readFile(second,'utf8'),'payload-2');
  assert((await outgoing('pending-1.txt').textContent()).includes('等待对方确认'));assert.deepEqual(uploads,['pending-2.txt']);
  await card('pending-3.txt').getByRole('button',{name:'拒绝',exact:true}).click();await outgoing('pending-3.txt').getByRole('button',{name:'重试',exact:true}).waitFor();
  await outgoing('pending-3.txt').getByRole('button',{name:'重试',exact:true}).click();await card('pending-3.txt').waitFor();await acceptAndSave('pending-3.txt',path.join(work,'retried.txt'));assert.equal(await readFile(path.join(work,'retried.txt'),'utf8'),'payload-3');
  await outgoing('pending-1.txt').getByRole('button',{name:'取消',exact:true}).click();await until(async()=>await card('pending-1.txt').count()===0,'sender cancellation removes only its pending request');assert(await card('pending-4.txt').isVisible());
  await acceptAndSave('队列文件夹.zip',path.join(work,'folder.zip'));assert.equal(await sender.evaluate(()=>packWorkers),1);assert.deepEqual(uploads,['pending-2.txt','pending-3.txt','队列文件夹.zip']);
  assert(await card('pending-4.txt').isVisible());assert(await sender.evaluate(()=>document.documentElement.scrollWidth<=innerWidth));assert(await receiver.evaluate(()=>document.documentElement.scrollWidth<=innerWidth));
  await sender.screenshot({path:path.join(work,'sender.png'),fullPage:true});await receiver.screenshot({path:path.join(work,'receiver.png'),fullPage:true});
  reports.push('eight independent approvals; accept a later item before the first; rejection and retry; cancel one pending item; one ZIP sent only after consent; session isolation');
  await sender.close();await until(async()=>await receiver.locator('.approval-card').count()===0,'closing sender clears remaining async prompts');reports.push('explicit sender closure cancels pending requests');
  if(android){
    const page=await(await browser.newContext({viewport:{width:390,height:844}})).newPage();page.on('pageerror',error=>errors.push(error.message));await page.goto(origin);await page.locator('#switch-device').click();await page.locator('#devices .device').first().click();
    for(let i=1;i<=8;i++)await page.locator('#picker').setInputFiles({name:'native-'+i+'.txt',mimeType:'text/plain',buffer:Buffer.from('native-'+i)});
    const snapshot=await until(async()=>{const s=await native({command:'state'});return s.requests.length===8?s:null;},'Android native receiver sees all eight pending requests');
    const later=snapshot.requests.find(request=>request.items[0].name==='native-2.txt');await native({command:'approve',id:later.id,selection:'1'});
    await page.locator('#jobs .row').filter({hasText:'native-2.txt'}).locator('.job-status').filter({hasText:'已完成'}).waitFor();assert.equal(await readFile(path.join(host,'received','native-2.txt'),'utf8'),'native-2');
    assert.equal((await native({command:'state'})).requests.length,7);await page.close();await until(async()=>(await native({command:'state'})).requests.length===0,'Android native pending prompts cleared on source closure');reports.push('Android native receive list: eight prompts and independent out-of-order acceptance');
  }
  assert.deepEqual(errors,[]);await writeFile(path.join(work,'results.json'),JSON.stringify({android,reports},null,2));console.log('PASS '+(android?'Android':'Windows')+' '+reports.join('; '));console.log('RESULTS '+work);
}catch(error){if(browser)for(const [i,page] of browser.contexts().flatMap(context=>context.pages()).entries())try{console.log('PAGE '+i+' '+(await page.locator('body').innerText()).slice(-3000));await page.screenshot({path:path.join(work,'failure-'+i+'.png'),fullPage:true});}catch{}throw error;}
finally{await browser?.close();if(child?.exitCode===null)child.kill();}
