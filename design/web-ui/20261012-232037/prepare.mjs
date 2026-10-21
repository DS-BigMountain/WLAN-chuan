import {readFile,writeFile} from 'node:fs/promises';
import {fileURLToPath} from 'node:url';
import path from 'node:path';
const root=path.dirname(fileURLToPath(import.meta.url));
const project=path.resolve(root,'../../..');
const currentWeb=await readFile(path.join(project,'src/web/index.html'),'utf8');
const sprite=currentWeb.match(/<svg class="symbols"[\s\S]*?<\/svg>/)?.[0];
if(!sprite)throw new Error('无法读取原有 SVG 图标定义。');
const template=await readFile(path.join(root,'template.html'),'utf8');
const css=await readFile(path.join(root,'ui.css'),'utf8');
const mobileStart=css.indexOf('@media(max-width:760px){')+'@media(max-width:760px){'.length;
const mobileEnd=css.indexOf('\n@media(prefers-reduced-motion');
const mobileRules=css.slice(mobileStart,mobileEnd).trim().slice(0,-1);
await writeFile(path.join(root,'ui-mobile.css'),css+'\n'+mobileRules+'\n:root{background:#e9edf3}body{max-width:420px;min-height:100vh;margin:0 auto}dialog{max-width:388px}#device-dialog{width:min(420px,100vw);left:50%;right:auto;transform:translateX(-50%)}\n');
for(const layout of ['desktop','mobile']){
  let html=template.replace('__LAYOUT__',layout).replace('<!-- CHUAN_ORIGINAL_ICONS -->',sprite);
  if(layout==='mobile')html=html.replace('href="ui.css"','href="ui-mobile.css"');
  await writeFile(path.join(root,'web-'+layout+'.html'),html);
}
console.log('Prepared two Web UI prototypes with the original SVG definitions.');
