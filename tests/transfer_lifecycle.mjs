import {spawn} from 'node:child_process';
import {mkdir,writeFile,readFile,stat} from 'node:fs/promises';
import path from 'node:path';import {randomUUID} from 'node:crypto';import net from 'node:net';import dgram from 'node:dgram';import assert from 'node:assert/strict';import {pathToFileURL} from 'node:url';
const root=path.resolve('build/transfer-lifecycle',randomUUID()),children=[];await mkdir(root,{recursive:true});
const delay=ms=>new Promise(r=>setTimeout(r,ms));
async function until(fn,label,timeout=45000){const end=Date.now()+timeout;while(Date.now()<end){if(await fn())return;await delay(80);}throw Error('timeout '+label);}
async function port(udp=false){const s=udp?dgram.createSocket('udp4'):net.createServer();await new Promise(r=>udp?s.bind(0,'127.0.0.1',r):s.listen(0,'127.0.0.1',r));const p=s.address().port;await new Promise(r=>s.close(r));return p;}
const {chromium}=await import(pathToFileURL(path.resolve('build/web-qa/node_modules/playwright-core/index.mjs'))),browser=await chromium.launch({channel:'chrome',headless:true});
try{for(const platform of (process.argv.includes('--windows-only')?['windows']:['windows','android'])){
 const work=path.join(root,platform),tcp=await port(),udp=await port(true);await mkdir(work,{recursive:true});
 const child=platform==='windows'?spawn(path.resolve('build/v2.0/Release/Chuan.exe'),['--headless','--port',String(tcp),'--discovery-port',String(udp),'--data-dir',path.join(work,'config'),'--receive-dir',path.join(work,'received')],{windowsHide:true,stdio:'ignore'}):spawn('java',['-Dfile.encoding=UTF-8','-cp',path.resolve('build/mobile-core'),'local.chuan.test.HostMain','--root',work,'--assets',path.resolve('src/web'),'--port',String(tcp),'--discovery-port',String(udp)],{windowsHide:true,stdio:['pipe','ignore','inherit']});children.push(child);
 await until(async()=>{try{return(await fetch('http://127.0.0.1:'+tcp+'/api/info')).ok;}catch{return false;}},'host');
 const page=await browser.newPage(),errors=[];page.on('pageerror',e=>errors.push(e.message));let state={jobs:[]};page.on('response',async response=>{if(response.url().endsWith('/web/api/state'))try{state=await response.json();}catch{}});
 await page.goto('http://127.0.0.1:'+tcp);await page.locator('.device').first().click();
 const folder=path.join(work,'sources','并行恢复');await mkdir(folder,{recursive:true});for(const [name,body] of [['a.txt','saved'],['b.bin',Buffer.alloc(1024*1024,39)],['c.txt','continues']])await writeFile(path.join(folder,name),body);
 let attempts=0,release,held=false;
 await page.route('**/web/api/upload',async route=>{const name=Buffer.from(route.request().headers()['x-chuan-path'],'hex').toString();if(name.endsWith('/b.bin')){attempts++;if(attempts===1){await route.abort('connectionreset');return;}held=true;await new Promise(r=>release=r);}await route.continue();});
 await page.locator('#folder-picker').setInputFiles(folder);await until(()=>held,'retry started');
 const destination=path.join(work,'received','并行恢复');await until(async()=>{try{return(await readFile(path.join(destination,'c.txt'),'utf8'))==='continues';}catch{return false;}},'healthy lane continues');
 await until(()=>state.jobs.filter(j=>j.name.replaceAll('\\','/').startsWith('并行恢复')).length===4,'full manifest');
 const before=(await stat(path.join(destination,'a.txt'))).mtimeMs;await page.locator('#clear').click();await delay(400);
 assert.equal(state.jobs.filter(j=>j.name.replaceAll('\\','/').startsWith('并行恢复')).length,4,'clear must retain completed members of active batch');
 assert(await page.locator('#jobs .lifecycle-active').count());release();
 await until(async()=>(await page.locator('#jobs').innerText()).includes('已结束 · 已完成'),'automatic recovery');assert.equal(attempts,2);assert.equal((await stat(path.join(destination,'a.txt'))).mtimeMs,before);await page.unroute('**/web/api/upload');
 console.log('PASS '+platform+': full manifest, clear preserves live batch, independent retry continues later file');
 const timeoutFolder=path.join(work,'sources','超时恢复');await mkdir(timeoutFolder,{recursive:true});await writeFile(path.join(timeoutFolder,'a.txt'),'keep');await writeFile(path.join(timeoutFolder,'b.bin'),Buffer.alloc(128*1024,8));let failedAt=0,requests=0;
 await page.route('**/web/api/upload',async route=>{const name=Buffer.from(route.request().headers()['x-chuan-path'],'hex').toString();if(name==='超时恢复/b.bin'){failedAt ||= Date.now();requests++;await route.abort('connectionreset');}else await route.continue();});
 await page.locator('#folder-picker').setInputFiles(timeoutFolder);await until(async()=>(await page.locator('#jobs').innerText()).includes('传输失败超时'),'30 second timeout');
 const elapsed=Date.now()-failedAt;assert(elapsed>=29500&&elapsed<37000,'timeout elapsed '+elapsed);assert(requests>1);await until(()=>state.jobs.some(job=>job.name.replaceAll('\\','/')==='超时恢复/b.bin'&&job.state===5&&job.error.includes('传输失败超时')),'timeout must reach server history');
 const saved=path.join(work,'received','超时恢复','a.txt'),mtime=(await stat(saved)).mtimeMs;await page.unroute('**/web/api/upload');
 const failedGroup=page.locator('.folder-group').filter({hasText:'超时恢复'});await failedGroup.getByRole('button',{name:'重试',exact:true}).click();
 await until(async()=>{try{return(await stat(path.join(work,'received','超时恢复','b.bin'))).size===128*1024;}catch{return false;}},'manual retry');assert.equal((await stat(saved)).mtimeMs,mtime);await until(async()=>await page.locator('#jobs .lifecycle-active').count()===0,'old failed group must remain ended after manual retry');console.log('PASS '+platform+': 30 second timeout and manual retry only missing file');
 const receiverContext=await browser.newContext(),receiver=await receiverContext.newPage();await receiver.addInitScript(()=>{Object.defineProperty(window,'showDirectoryPicker',{value:undefined,configurable:true});});await receiver.goto('http://127.0.0.1:'+tcp);await until(()=>state.peers?.some(peer=>peer.browser&&!peer.directoryReceive),'browser capability');const peer=state.peers.find(peer=>peer.browser&&!peer.directoryReceive);await page.locator('[data-peer-id="'+peer.id+'"]').click();await page.locator('#folder-picker').setInputFiles(folder);
 await page.getByRole('button',{name:'打包传输',exact:true}).waitFor();assert((await page.locator('dialog[open]').innerText()).includes('不支持直接保存目录'));await page.screenshot({path:path.join(work,'zip-fallback.png'),fullPage:true});await page.getByRole('button',{name:'打包传输',exact:true}).click();
 await until(async()=>(await receiver.locator('#requests').innerText()).includes('并行恢复.zip'),'ZIP offer');assert(!(await receiver.locator('#requests').innerText()).includes('当前 Web 页面无法直接接收文件夹'));await receiver.getByRole('button',{name:'接收',exact:true}).click();
 await until(async()=>await receiver.getByRole('link',{name:'下载',exact:true}).count()>0,'ZIP download available');
 const download=receiver.waitForEvent('download');await receiver.getByRole('link',{name:'下载',exact:true}).click();const artifact=await download;assert.equal(await artifact.failure(),null);await artifact.saveAs(path.join(work,'received-from-browser.zip'));assert((await stat(path.join(work,'received-from-browser.zip'))).size>0);
 assert.deepEqual(errors,[]);console.log('PASS '+platform+': unsupported Web receiver notice and one-click ZIP transfer');await receiverContext.close();await page.screenshot({path:path.join(work,'lifecycle.png'),fullPage:true});await page.close();child.kill();
}console.log('RESULTS '+root);}finally{await browser.close();for(const child of children)if(child.exitCode===null)child.kill();}
