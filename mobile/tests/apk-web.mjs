// Targeted check against a signed APK running on the dedicated Android emulator.
// The emulator's TCP 45871 must be forwarded to the same localhost port.
import {readFile,writeFile,mkdir} from 'node:fs/promises';
import path from 'node:path';
import {pathToFileURL} from 'node:url';
import assert from 'node:assert/strict';
const root=path.resolve('build/mobile-device');await mkdir(root,{recursive:true});
const origin='http://127.0.0.1:45871';
const info=await (await fetch(origin+'/api/info')).json();assert.equal(info.platform,'android');
for(const file of ['index.html','style.css','app.js','files.js','transfers.js','sha256.js','session.js']){
  const response=await fetch(origin+(file==='index.html'?'/':'/web/'+file));assert.equal(response.status,200);
  assert.deepEqual(Buffer.from(await response.arrayBuffer()),await readFile('src/web/'+file),file);
}
for(const [url,local] of [['/download/Chuan.exe','build/v2.0/Release/Chuan.exe'],['/download/Chuan.apk','mobile/build/outputs/Chuan-2.0-android.apk']]){
  const response=await fetch(origin+url);assert.equal(response.status,200);
  assert.match(response.headers.get("content-disposition"),/attachment; filename=/);assert.deepEqual(Buffer.from(await response.arrayBuffer()),await readFile(local),url);
}
const {chromium}=await import(pathToFileURL(path.resolve('build/web-qa/node_modules/playwright-core/index.mjs')));
const browser=await chromium.launch({channel:'chrome',headless:true});
try{
  const context=await browser.newContext({acceptDownloads:true,viewport:{width:1360,height:900}});const page=await context.newPage(),errors=[];page.on('pageerror',error=>errors.push(error.message));
  await page.goto(origin);await page.locator('.device[data-peer-id="'+info.id+'"]').click();
  const file=path.join(root,'web-to-android.txt');await writeFile(file,'Chromium → 正式 Android APK → 持久授权目录');
  await page.locator('#picker').setInputFiles(file);
  await page.waitForFunction(()=>[...document.querySelectorAll('#jobs .row')].some(row=>row.textContent.includes('web-to-android.txt')&&row.textContent.includes('已完成')));
  assert(await page.locator('#android-client').isVisible());
  assert.equal(await page.locator('#android-client').getAttribute('href'),'/download/Chuan.apk');
  assert.deepEqual(errors,[]);await page.screenshot({path:path.join(root,'apk-web.png'),fullPage:true});
  console.log('PASS installed signed APK: exact embedded Web/Windows/APK downloads, actual Chromium upload and visible APK download entry');
}catch(error){for(const page of browser.contexts().flatMap(context=>context.pages())){console.log('APK_WEB_PAGE',page.url(),(await page.locator('body').innerText()).slice(0,1800));await page.screenshot({path:path.join(root,'apk-web-failure.png'),fullPage:true});}throw error;}finally{await browser.close();}
