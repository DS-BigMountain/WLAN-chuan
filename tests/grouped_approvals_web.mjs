// Directed approval regression against the actual Windows/JVM servers and browser modules.
import http from 'node:http';
import net from 'node:net';
import dgram from 'node:dgram';
import {spawn} from 'node:child_process';
import {mkdir,readFile,writeFile} from 'node:fs/promises';
import {randomUUID} from 'node:crypto';
import {pathToFileURL} from 'node:url';
import path from 'node:path';
import assert from 'node:assert/strict';
const android=process.argv[3]==='android',work=path.resolve(process.argv[2]||'build/grouped-approvals',randomUUID());await mkdir(work,{recursive:true});
const delay=ms=>new Promise(resolve=>setTimeout(resolve,ms));
async function until(fn,label,timeout=18000){const end=Date.now()+timeout;while(Date.now()<end){const value=await fn();if(value)return value;await delay(70);}throw Error('timeout: '+label);}
async function port(udp=false){const socket=udp?dgram.createSocket('udp4'):net.createServer();await new Promise(resolve=>udp?socket.bind(0,'127.0.0.1',resolve):socket.listen(0,'127.0.0.1',resolve));const value=socket.address().port;await new Promise(resolve=>socket.close(resolve));return value;}
function request(tcp,url,headers={},body=null){return new Promise((resolve,reject)=>{const req=http.request({host:'127.0.0.1',port:tcp,path:url,method:body===null?'GET':'POST',agent:false,headers:{...headers,...(body===null?{}:{'Content-Length':Buffer.byteLength(body)})}},res=>{const chunks=[];res.on('data',b=>chunks.push(b));res.on('end',()=>resolve({status:res.statusCode,text:Buffer.concat(chunks).toString()}));res.on('error',reject);});req.on('error',reject);req.end(body);});}
let browser,child;const errors=[],reports=[];
try{
 const tcp=await port(),udp=await port(true),host=path.join(work,'host'),origin='http://127.0.0.1:'+tcp;
 child=android?spawn('java',['-Dfile.encoding=UTF-8','-Dstdout.encoding=UTF-8','-cp',path.resolve('build/mobile-core'),'local.chuan.test.HostMain','--root',host,'--assets',path.resolve('src/web'),'--port',String(tcp),'--discovery-port',String(udp),'--auto-receive','false'],{windowsHide:true,stdio:['pipe','ignore','inherit']}):spawn(path.resolve('build/v2.0/Release/Chuan.exe'),['--headless','--port',String(tcp),'--discovery-port',String(udp),'--data-dir',path.join(host,'config'),'--receive-dir',path.join(host,'received'),'--run-seconds','180'],{windowsHide:true,stdio:'ignore'});
 await until(async()=>{try{return(await request(tcp,'/api/info')).status===200;}catch{return false;}},'host startup');
 const {chromium}=await import(pathToFileURL(path.resolve('build/web-qa/node_modules/playwright-core/index.mjs')));browser=await chromium.launch({channel:'chrome',headless:true});
 const receiver=await(await browser.newContext({viewport:{width:1280,height:900}})).newPage();await receiver.goto(origin);await receiver.locator('#service-status').filter({hasText:'已连接服务主机'}).waitFor();
 const receiverName=await receiver.locator('#browser-name').textContent(),senders=[];
 for(let i=0;i<2;i++){const sender=await(await browser.newContext({viewport:{width:390,height:844}})).newPage();sender.on('pageerror',error=>errors.push(error.message));await sender.goto(origin);await sender.locator('#switch-device').click();await sender.locator('#devices .device').filter({hasText:receiverName}).click();senders.push(sender);}
 receiver.on('pageerror',error=>errors.push(error.message));const [sender,other]=senders;
 const fixture=name=>({name,mimeType:'text/plain',buffer:Buffer.from('content:'+name)});
 const leaves=receiver.locator('.approval-file'),all=receiver.getByLabel('全选',{exact:true}),file=name=>receiver.locator('.approval-file').filter({hasText:name}),out=name=>sender.locator('#jobs .row').filter({hasText:name});
 await sender.locator('#picker').setInputFiles(['alpha.txt','beta.txt','gamma.txt'].map(fixture));await other.locator('#picker').setInputFiles(fixture('delta.txt'));
 await until(async()=>await leaves.count()===4,'four files in two sender groups');assert.equal(await receiver.locator('.approval-sender').count(),2);assert.equal(await leaves.locator('input:checked').count(),4);assert(await all.isChecked());
 await file('beta.txt').locator('input').uncheck();const firstGroup=receiver.locator('.approval-sender').filter({hasText:'alpha.txt'});await firstGroup.locator('summary').click();
 await sender.locator('#picker').setInputFiles(fixture('epsilon.txt'));await until(async()=>await leaves.count()===5,'new offer appended to original sender');assert.equal(await receiver.locator('.approval-sender').count(),2);assert.equal(await firstGroup.evaluate(node=>node.open),false);assert.equal(await file('beta.txt').locator('input').isChecked(),false);assert.equal(await file('epsilon.txt').locator('input').isChecked(),true);await firstGroup.locator('summary').click();
 await all.check();assert.equal(await leaves.locator('input:checked').count(),5);await all.uncheck();assert.equal(await leaves.locator('input:checked').count(),0);
 await file('beta.txt').locator('input').check();await file('delta.txt').locator('input').check();await receiver.getByRole('button',{name:'接收',exact:true}).click();
 async function save(name){const row=receiver.locator('#jobs .row').filter({hasText:name});await row.getByRole('link',{name:'下载',exact:true}).waitFor();const [download]=await Promise.all([receiver.waitForEvent('download'),row.getByRole('link',{name:'下载',exact:true}).click()]);const dest=path.join(work,name);await download.saveAs(dest);assert.equal(await readFile(dest,'utf8'),'content:'+name);}
 await save('beta.txt');await save('delta.txt');await until(async()=>await leaves.count()===3,'unselected same-batch items remain pending');assert(await file('alpha.txt').isVisible());assert(await file('gamma.txt').isVisible());assert((await out('alpha.txt').textContent()).includes('等待对方确认'));assert((await out('gamma.txt').textContent()).includes('等待对方确认'));
 await file('alpha.txt').locator('input').check();await receiver.getByRole('button',{name:'接收',exact:true}).click();await save('alpha.txt');await until(async()=>await leaves.count()===2,'second partial approval of same batch');assert(await file('gamma.txt').isVisible());
 await file('gamma.txt').locator('input').check();await receiver.getByRole('button',{name:'拒绝',exact:true}).click();await out('gamma.txt').getByRole('button',{name:'重试',exact:true}).waitFor();assert(await file('epsilon.txt').isVisible());
 await out('gamma.txt').getByRole('button',{name:'重试',exact:true}).click();await file('gamma.txt').waitFor();assert(await file('gamma.txt').locator('input').isChecked());assert.equal(await file('epsilon.txt').locator('input').isChecked(),false,'manual unchecked selection survives another arrival');
 await out('epsilon.txt').getByRole('button',{name:'取消',exact:true}).click();await until(async()=>await file('epsilon.txt').count()===0,'only canceled offer removed');assert(await file('gamma.txt').isVisible());
 for(const width of [320,390,760,1280,1920]){await receiver.setViewportSize({width,height:900});assert(await receiver.evaluate(()=>document.documentElement.scrollWidth<=innerWidth),'no overflow at '+width);assert(await receiver.locator('#picker').count());assert(await receiver.locator('.file-actions').isVisible());const bounds=await receiver.evaluate(()=>{const a=document.querySelector('#requests').getBoundingClientRect(),b=document.querySelector('.jobs-section').getBoundingClientRect();return[a.bottom,b.top];});assert(bounds[0]<=bounds[1],'approval and history do not overlap');}
 await receiver.screenshot({path:path.join(work,'grouped-desktop.png'),fullPage:true});await receiver.setViewportSize({width:390,height:844});await receiver.screenshot({path:path.join(work,'grouped-mobile.png'),fullPage:true});
 reports.push('sender grouping, new checked arrivals, retained unchecked selection and expansion, global toggle, repeated partial acceptance within one batch, rejection/retry, isolated cancellation, 320–1920px layout with existing history');
 await sender.close();await until(async()=>await leaves.count()===0,'source closure removes final pending offer');assert.deepEqual(errors,[]);await writeFile(path.join(work,'results.json'),JSON.stringify({android,reports},null,2));console.log('PASS '+(android?'Android':'Windows')+' '+reports.join('; '));console.log('RESULTS '+work);
}catch(error){if(browser)for(const[i,page]of browser.contexts().flatMap(context=>context.pages()).entries())try{console.log('PAGE '+i+' '+(await page.locator('body').innerText()).slice(-2500));await page.screenshot({path:path.join(work,'failure-'+i+'.png'),fullPage:true});}catch{}throw error;}
finally{await browser?.close();if(child?.exitCode===null)child.kill();}
