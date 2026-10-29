import http from 'node:http';
import {readFile,mkdir} from 'node:fs/promises';
import {pathToFileURL} from 'node:url';
import path from 'node:path';
import assert from 'node:assert/strict';
const root=path.resolve(process.argv[2]||'build/update-mobile-ui');await mkdir(root,{recursive:true});
const server=http.createServer(async(req,res)=>{try{const file=req.url==='/'?'mobile/src/main/assets/native/app.html':req.url==='/web/chuan.svg'?'src/assets/chuan.svg':req.url.startsWith('/native/')?'mobile/src/main/assets'+req.url:'src'+req.url;let body=await readFile(file,'utf8');if(req.url==='/')body=body.replace('<!-- ORIGINAL_ICONS -->',(await readFile('src/web/index.html','utf8')).match(/<svg class="symbols"[\s\S]*?<\/svg>/)[0]);res.setHeader('Content-Type',file.endsWith('.js')?'text/javascript':file.endsWith('.css')?'text/css':file.endsWith('.svg')?'image/svg+xml':'text/html');res.end(body);}catch(e){res.writeHead(404);res.end(String(e));}});
await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));let browser;
try{
  const {chromium}=await import(pathToFileURL(path.resolve('build/web-qa/node_modules/playwright-core/index.mjs')));browser=await chromium.launch({channel:'chrome',headless:true});
  const page=await browser.newPage({viewport:{width:390,height:844}}),errors=[];page.on('pageerror',error=>errors.push(error.message));
  await page.addInitScript(()=>{globalThis.calls=[];globalThis.fixture={version:'2.2.0',running:true,name:'Android 手机',port:45871,peers:[],jobs:[],requests:[],addresses:[],update:{phase:'idle',message:'当前版本 V2.2.0'}};window.ChuanNative={state:()=>JSON.stringify(fixture),readUpdateNotice:detail=>{fixture.update.settingsUnread=false;if(detail)fixture.update.updateUnread=false;},checkUpdate:()=>calls.push('check'),downloadUpdate:()=>calls.push('download'),cancelUpdate:()=>calls.push('cancel'),installUpdate:()=>calls.push('install'),updatePage:()=>calls.push('page')};});
  await page.goto('http://127.0.0.1:'+server.address().port);await page.locator('#settings-button').click();
  await page.locator('#check-update').click();
  assert(await page.locator('#update-progress').isHidden());
  await page.locator('#update-action').click();assert.equal(await page.evaluate(()=>calls.at(-1)),'check');
  const update=async value=>{await page.evaluate(value=>fixture.update=value,value);await page.waitForTimeout(1100);};
  await update({phase:'checking',message:'正在检查 GitHub 更新…'});assert(await page.locator('#update-action').isDisabled());assert(await page.locator('#update-cancel').isVisible());assert(await page.locator('#update-progress').isHidden());
  await page.locator('#update-dialog [data-close]').click();
  await update({phase:'available',message:'发现新版本 V2.3.0',version:'V2.3.0',settingsUnread:true,updateUnread:true,notes:'修复文件传输问题。\n<script>这段文字不得执行</script>'});
  assert(await page.locator('#settings-button').evaluate(e=>e.classList.contains('update-unread')));
  await page.screenshot({path:path.join(root,'android-settings-badge.png')});
  await page.locator('#settings-button').click();
  assert(!(await page.locator('#settings-button').evaluate(e=>e.classList.contains('update-unread'))));
  assert(await page.locator('#check-update').evaluate(e=>e.classList.contains('update-unread')));
  await page.locator('#check-update').scrollIntoViewIfNeeded();await page.screenshot({path:path.join(root,'android-update-entry-badge.png')});
  await page.locator('#settings-dialog [data-close]').click();await page.locator('#settings-button').click();
  assert(!(await page.locator('#settings-button').evaluate(e=>e.classList.contains('update-unread'))));
  assert(await page.locator('#check-update').evaluate(e=>e.classList.contains('update-unread')));
  await page.locator('#check-update').click();assert(!(await page.locator('#check-update').evaluate(e=>e.classList.contains('update-unread'))));
  assert(await page.locator('#update-progress').isHidden());assert.equal(await page.locator('#update-action').textContent(),'下载更新');await page.locator('#update-action').click();assert.equal(await page.evaluate(()=>calls.at(-1)),'download');
  await update({phase:'downloading',message:'正在下载更新…',done:512,total:1024});assert.equal(await page.locator('#update-progress').evaluate(e=>e.value),50);await page.locator('#update-cancel').click();assert.equal(await page.evaluate(()=>calls.at(-1)),'cancel');
  await update({phase:'ready',message:'下载完成，需要安装更新。',version:'V2.3.0',notes:'修复文件传输问题。\n<script>这段文字不得执行</script>'});assert.equal(await page.locator('#update-action').textContent(),'安装更新');assert(await page.locator('#update-cancel').isHidden());assert(await page.locator('#update-progress').isHidden());await page.locator('#update-action').click();assert.equal(await page.evaluate(()=>calls.at(-1)),'install');
  await page.locator('#update-notes-box summary').click();assert((await page.locator('#update-notes').textContent()).includes('<script>'));assert.equal(await page.locator('#update-notes script').count(),0);await page.locator('.update-panel').scrollIntoViewIfNeeded();await page.screenshot({path:path.join(root,'android-update-ready.png')});
  for(const width of [320,390,600]){await page.setViewportSize({width,height:844});assert(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth),'page overflow '+width);assert(await page.locator('#update-dialog').evaluate(e=>e.scrollWidth<=e.clientWidth+1),'dialog overflow '+width);}
  await update({phase:'error',message:'APK 签名与已安装应用不一致，无法覆盖更新'});assert(await page.locator('#update-action').isEnabled());assert.equal(await page.locator('#update-action').textContent(),'检查更新');await page.locator('#update-page').click();assert.equal(await page.evaluate(()=>calls.at(-1)),'page');assert.deepEqual(errors,[]);
  console.log('PASS Android two-level unread badges, read persistence, update phases, progress visibility, errors and responsive dialogs');
}finally{await browser?.close();await new Promise(resolve=>server.close(resolve));}
