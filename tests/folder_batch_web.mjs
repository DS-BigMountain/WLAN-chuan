// Targeted HT reproduction: 1,490 paths, isolated synthetic contents, one upload failure.
import http from 'node:http';
import net from 'node:net';
import dgram from 'node:dgram';
import {spawn} from 'node:child_process';
import {mkdir,writeFile,readFile,open,stat} from 'node:fs/promises';
import {randomUUID} from 'node:crypto';
import {pathToFileURL} from 'node:url';
import path from 'node:path';
import {networkInterfaces} from 'node:os';
import assert from 'node:assert/strict';
const work=path.resolve(process.argv[2]||'build/ht-batch-qa',randomUUID());await mkdir(work,{recursive:true});
const android=process.argv[3]==='android',quick=process.argv.includes('--quick');
const qaHost=process.argv.includes('--lan')?Object.entries(networkInterfaces()).sort(([a],[b])=>Number(/WLAN|Wi-Fi/i.test(b))-Number(/WLAN|Wi-Fi/i.test(a))).flatMap(([,entries])=>entries).find(entry=>entry.family==='IPv4'&&!entry.internal&&/^192\.168\.|^10\.|^172\.(1[6-9]|2\d|3[01])\./.test(entry.address))?.address:'127.0.0.1';assert(qaHost,'LAN interface available');
const metadata=JSON.parse(await readFile('build/ht-diagnostics/manifest.json','utf8'));
assert.equal(metadata.length,1490);const failurePath=metadata[93].path,source=path.join(work,'source');
for(const [index,item] of metadata.entries()){const file=path.join(source,item.path);await mkdir(path.dirname(file),{recursive:true});if(item.path===failurePath){const handle=await open(file,'w');await handle.truncate(32*1024*1024);await handle.close();}else await writeFile(file,String(index).padStart(8,'0'));}
const delay=ms=>new Promise(r=>setTimeout(r,ms));
async function port(udp=false){const s=udp?dgram.createSocket('udp4'):net.createServer();await new Promise(r=>udp?s.bind(0,'127.0.0.1',r):s.listen(0,'127.0.0.1',r));const p=s.address().port;await new Promise(r=>s.close(r));return p;}
async function until(fn,label,ms=90000){const end=Date.now()+ms;while(Date.now()<end){try{const value=await fn();if(value)return value;}catch{}await delay(100);}throw Error('timeout: '+label);}
async function jsonRequest(port,url,headers={},body=null){return new Promise((resolve,reject)=>{const req=http.request({host:qaHost,port,path:url,method:body===null?'GET':'POST',agent:false,headers:{...headers,...(body===null?{}:{'Content-Length':Buffer.byteLength(body)})}},res=>{let text='';res.on('data',data=>text+=data);res.on('end',()=>resolve({status:res.statusCode,text}));});req.on('error',reject);req.end(body);});}
let browser,child;const errors=[];let client='',token='',statePolls=0,uploads=0,failed=false,pollInterrupted=false;
try{
 const tcp=await port(),udp=await port(true),root=path.join(work,'host');
 child=android?spawn('java',['-Dfile.encoding=UTF-8','-cp',path.resolve('build/mobile-core'),'local.chuan.test.HostMain','--root',root,'--assets',path.resolve('src/web'),'--port',String(tcp),'--discovery-port',String(udp),'--name','HT Receiver'],{windowsHide:true,stdio:['pipe','ignore','ignore']}):spawn(path.resolve('build/v2.0/Release/Chuan.exe'),['--headless','--port',String(tcp),'--discovery-port',String(udp),'--data-dir',path.join(root,'config'),'--receive-dir',path.join(root,'received'),'--name','HT Receiver','--run-seconds','240'],{windowsHide:true,stdio:'ignore'});
 await until(async()=>{const r=await jsonRequest(tcp,'/api/info');return r.status===200;},'host startup',10000);
 const {chromium}=await import(pathToFileURL(path.resolve('build/web-qa/node_modules/playwright-core/index.mjs')));browser=await chromium.launch({channel:'chrome',headless:true});
 const page=await browser.newPage({viewport:{width:1440,height:1050}});page.on('pageerror',error=>errors.push(error.message));
 page.on('request',req=>{if(req.url().endsWith('/web/api/state')){client=req.headers()['x-chuan-client'];statePolls++;}});
 page.on('response',async res=>{if(res.url().endsWith('/web/api/state'))try{token=(await res.json()).token;}catch{}});
 await page.route('**/web/api/upload',async route=>{uploads++;const relative=Buffer.from(route.request().headers()['x-chuan-path'],'hex').toString();if(!failed&&relative===failurePath){failed=true;await route.abort('connectionreset');}else await route.continue();});
 await page.route('**/web/api/state',async route=>{if(uploads>=50&&!pollInterrupted){pollInterrupted=true;await route.abort('connectionreset');}else await route.continue();});
 await page.goto('http://'+qaHost+':'+tcp);await page.locator('#devices .device').first().click();
 await page.locator('#folder-picker').setInputFiles(path.join(source,'HT'));
 await page.locator('#jobs .folder-toggle').click();await page.locator('#jobs .folder-tree>ul>li>details>summary').first().click();
 await page.locator('#jobs .job-status').filter({hasText:'部分失败'}).waitFor({timeout:120000});
 assert(await page.locator('#jobs .folder-tree').evaluate(node=>node.open),'completion preserves open directory tree');assert(await page.locator('#jobs .folder-tree>ul>li>details').first().evaluate(node=>node.open),'nested expansion preserved');
 const snapshot=await until(async()=>{const r=await jsonRequest(tcp,'/web/api/state',{'X-Chuan-Client':client});const s=JSON.parse(r.text);return s.jobs.length===1490&&s.jobs.every(j=>j.state>=3)?s:null;},'receiver terminal state');
 const complete=snapshot.jobs.filter(j=>j.state===3),failures=snapshot.jobs.filter(j=>j.state===5);
 assert.equal(complete.length,1489,'remaining files continue after an individual failure');assert.equal(failures.length,1);assert.equal(failures[0].name.replaceAll('\\','/'),failurePath);assert(failures[0].error.includes('连接中断'));
 assert.equal(uploads,1490);assert(pollInterrupted);assert(statePolls<45,'large batches do not poll once per file');
 const completedBytes=complete.reduce((sum,j)=>sum+j.done,0);assert.equal(completedBytes,1489*8);
 assert((await page.locator('#jobs .detail').textContent()).includes('1489/1490'));assert((await page.locator('#jobs .progress-metrics').textContent()).includes('<0.1%'));
 await page.waitForFunction(()=>document.querySelectorAll('#jobs [data-path]').length>=1490);
 await page.evaluate(relative=>{const row=[...document.querySelectorAll('#jobs [data-path]')].find(node=>node.dataset.path===relative);if(!row)throw Error('failed path absent from tree');for(let p=row.parentElement;p;p=p.parentElement)if(p.tagName==='DETAILS')p.open=true;row.scrollIntoView({block:'center'});scrollTo(0,0);},failurePath);
 await page.screenshot({path:path.join(work,'web-folder-partial.png'),fullPage:true});
 for(const job of complete.slice(0,8)){const file=path.join(root,'received',job.name);assert.equal((await stat(file)).size,8);assert.equal((await readFile(file)).length,8);}
 // Terminal reports cannot forge a saved file into failure or mutate another session.
 const completed=complete[0],headers={'X-Chuan-Client':client,'X-Chuan-Token':token,'X-Chuan-Batch':completed.batchId},body=completed.uploadKey+'\t5\t'+Buffer.from('should not replace saved state').toString('hex')+'\n';
 assert.equal((await jsonRequest(tcp,'/web/api/batch-status',headers,body)).status,200);
 const other=randomUUID().replaceAll('-',''),otherState=JSON.parse((await jsonRequest(tcp,'/web/api/state',{'X-Chuan-Client':other})).text);
 assert.notEqual((await jsonRequest(tcp,'/web/api/batch-status',{...headers,'X-Chuan-Client':other,'X-Chuan-Token':otherState.token},body)).status,200);
 await page.reload();await page.locator('#jobs .job-status').filter({hasText:'部分失败'}).waitFor();assert((await page.locator('#jobs .detail').textContent()).includes('1489/1490'));
 const after=JSON.parse((await jsonRequest(tcp,'/web/api/state',{'X-Chuan-Client':client})).text);assert.equal(after.jobs.find(j=>j.id===completed.id).state,3);
 await page.setViewportSize({width:390,height:844});await page.locator('[data-view-tab=status]').click();assert(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth));await page.screenshot({path:path.join(work,'web-folder-mobile.png'),fullPage:true});
 const transferPolls=statePolls;
 if(!quick){
 // A stopped sender cannot report its own failure; the receiver must end its pending rows.
 const ghost=randomUUID().replaceAll('-',''),ghostState=JSON.parse((await jsonRequest(tcp,'/web/api/state',{'X-Chuan-Client':ghost})).text),ghostBatch=randomUUID().replaceAll('-',''),ghostFolder=randomUUID().replaceAll('-',''),ghostUploads=[randomUUID().replaceAll('-',''),randomUUID().replaceAll('-','')],toHex=value=>Buffer.from(value).toString('hex');
 const ghostHeaders={'X-Chuan-Client':ghost,'X-Chuan-Token':ghostState.token,'X-Chuan-Batch':ghostBatch,'X-Chuan-Target':toHex(ghostState.hostId),'X-Chuan-Sender':toHex('Disconnected fixture')};
 assert.equal((await jsonRequest(tcp,'/web/api/offer',ghostHeaders,toHex('中断样本')+'\tD\t2\t16\n')).status,200);
 assert.equal((await jsonRequest(tcp,'/web/api/batch-files',ghostHeaders,ghostUploads.map((key,i)=>'0\t'+key+'\t'+ghostFolder+'\t'+toHex('中断样本/'+i+'.txt')+'\t8\n').join(''))).status,200);
 const detectionDeadline=Date.now()+48000;while(Date.now()<detectionDeadline){await jsonRequest(tcp,'/web/api/state',{'X-Chuan-Client':client});await delay(1000);}
 const abandoned=JSON.parse((await jsonRequest(tcp,'/web/api/state',{'X-Chuan-Client':ghost})).text).jobs;assert.equal(abandoned.length,2);assert(abandoned.every(job=>job.state===5&&job.error.includes('离线')),'receiver ends abandoned manifest even without a terminal report');
 }
 assert.deepEqual(errors,[]);await writeFile(path.join(work,'results.json'),JSON.stringify({source:'HT metadata only; all payloads synthetic',files:1490,complete:1489,failed:1,completedBytes,transferPolls,uploads,pollInterrupted,receiverNotified:true,qaHost,reloadPreservesResult:true,abandonedSenderDetected:!quick},null,2));
 console.log('PASS 1490-file HT structure: individual failure, transient poll failure, continuation, receiver status, actual bytes, directory tree, reload and ownership');console.log('RESULTS '+work);
}catch(error){if(browser)for(const page of browser.contexts().flatMap(c=>c.pages()))try{console.log((await page.locator('body').innerText()).slice(-4500));await page.screenshot({path:path.join(work,'failure.png'),fullPage:true});}catch{}throw error;}
finally{await browser?.close();if(child?.exitCode===null)child.kill();}
