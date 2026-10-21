// Production browser stream: first destination bytes precede source completion.
import {spawn,spawnSync} from 'node:child_process';import {mkdir,writeFile,readdir,stat,readFile} from 'node:fs/promises';import path from 'node:path';import net from 'node:net';import dgram from 'node:dgram';import {randomUUID,createHash} from 'node:crypto';import assert from 'node:assert/strict';import {pathToFileURL} from 'node:url';
const root=path.resolve('build/zip-stream-qa',randomUUID()),children=[];const delay=ms=>new Promise(r=>setTimeout(r,ms));
const until=async(fn,label)=>{const end=Date.now()+20000;while(Date.now()<end){const value=await fn();if(value)return value;await delay(30);}throw Error('timeout: '+label);};
async function port(udp=false){const s=udp?dgram.createSocket('udp4'):net.createServer();await new Promise(r=>udp?s.bind(0,'127.0.0.1',r):s.listen(0,'127.0.0.1',r));const p=s.address().port;await new Promise(r=>s.close(r));return p;}
async function host(platform,name,source){const work=path.join(root,name),tcp=await port(),udp=await port(true);await mkdir(path.join(work,'config'),{recursive:true});await writeFile(path.join(work,'config','settings.ini'),'[Chuan]\r\nZipFolders=1\r\n');
 const child=platform==='windows'?spawn(path.resolve('build/v2.0/Release/zip_stream_host.exe'),[work,String(tcp),String(udp),source],{windowsHide:true,stdio:'ignore'}):spawn('java',['-Dfile.encoding=UTF-8','-Dstdout.encoding=UTF-8','-cp',path.resolve('build/mobile-core'),'local.chuan.test.HostMain','--root',work,'--assets',path.resolve('src/web'),'--port',String(tcp),'--discovery-port',String(udp)],{windowsHide:true,stdio:['pipe','pipe','inherit']});children.push(child);
 let buffer='',sequence=0;const answers=new Map();if(platform==='android'){child.stdout.setEncoding('utf8');child.stdout.on('data',chunk=>{buffer+=chunk;let at;while((at=buffer.indexOf('\n'))>=0){const line=buffer.slice(0,at).trim();buffer=buffer.slice(at+1);if(line.startsWith('ANSWER ')){const a=JSON.parse(line.slice(7));answers.get(a.request)?.(a.result);answers.delete(a.request);}}});}
 const command=body=>new Promise(resolve=>{const request=String(++sequence);answers.set(request,resolve);child.stdin.write(JSON.stringify({...body,request})+'\n');});
 const info=await until(async()=>{try{return await(await fetch('http://127.0.0.1:'+tcp+'/api/info')).json();}catch{}},'host ready');if(platform==='android')await command({command:'settings',zipFolders:true,autoReceive:true});return {work,tcp,info,command};
}

const {chromium}=await import(pathToFileURL(path.resolve('build/web-qa/node_modules/playwright-core/index.mjs')));const browser=await chromium.launch({channel:'chrome',headless:true});
try{for(const platform of ['windows','android']){
 if(process.argv[2]&&platform!==process.argv[2])continue;
 const source=path.join(root,platform+'-source','原生流式');await mkdir(source,{recursive:true});await writeFile(path.join(source,'a.txt'),'native streaming archive');await writeFile(path.join(source,'b.bin'),Buffer.alloc(4*1024*1024,33));
 const server=await host(platform,platform,source),page=await browser.newPage();await page.goto('http://127.0.0.1:'+server.tcp);await page.locator('.device').first().waitFor();
 if(platform==='android'){const peer=await until(async()=>(await server.command({command:'state'})).peers.find(p=>p.browser),'browser registration');await server.command({command:'send',target:peer.id,files:[source]});}
 await page.locator('#requests .primary').click();const link=page.locator('#jobs a.button').filter({hasText:'下载'}).first();await link.waitFor();
 // Read the authenticated response through HTTP directly. Never navigate to an
 // attachment URL: browser download integrations may launch external programs.
 const href=new URL(await link.getAttribute('href'),page.url()).href,cookies=await page.context().cookies(href);
 const response=await fetch(href,{headers:{cookie:cookies.map(c=>c.name+'='+c.value).join('; ')},signal:AbortSignal.timeout(20000)});assert.equal(response.status,200);assert.match(response.headers.get('content-disposition'),new RegExp("filename\\*=UTF-8''"+encodeURIComponent('原生流式.zip'),'i'));
 const full=Buffer.from(await response.arrayBuffer()),offset=2*1024*1024,headers={cookie:cookies.map(c=>c.name+'='+c.value).join('; ')},zip=path.join(root,platform+'-download.zip');await writeFile(zip,full);
 const suffix=await fetch(href,{headers:{...headers,Range:'bytes='+offset+'-'}});assert.equal(suffix.status,206);assert.equal(suffix.headers.get('content-range'),'bytes '+offset+'-'+(full.length-1)+'/'+full.length);assert.deepEqual(Buffer.from(await suffix.arrayBuffer()),full.subarray(offset));
 const prefix=createHash('sha256').update(full.subarray(0,offset)).digest('hex'),resumed=await fetch(href+'&checksum=trailer',{headers:{...headers,'X-Chuan-Offset':String(offset),'X-Chuan-Prefix':prefix}});assert.equal(resumed.status,200);assert.deepEqual(Buffer.from(await resumed.arrayBuffer()),Buffer.concat([full.subarray(offset),createHash('sha256').update(full).digest()]));
 const mismatch=await fetch(href+'&checksum=trailer',{headers:{...headers,'X-Chuan-Offset':String(offset),'X-Chuan-Prefix':'0'.repeat(64)}});assert(!mismatch.ok);assert.match(await mismatch.text(),/续传内容不一致/);
 const python='C:/Users/17630/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe';const checked=spawnSync(python,['-X','utf8','-c','import zipfile,sys;z=zipfile.ZipFile(sys.argv[1]);assert z.testzip() is None;assert z.read("原生流式/a.txt")==b"native streaming archive"',zip],{encoding:'utf8',windowsHide:true});assert.equal(checked.status,0,checked.stderr);
 try{assert.equal((await readdir(path.join(server.work,'config','archive-send'))).length,0);}catch(error){if(error.code!=='ENOENT')throw error;}
 console.log('PASS '+platform+' -> browser: generated ZIP downloads without a completed sender cache, correct filename and CRC');await page.close();
}console.log('RESULTS '+root);}finally{await browser.close();for(const child of children)if(child.exitCode===null)child.kill();}
