import {Sha256} from '/web/sha256.js';
import {selectedFiles,droppedFiles,batchPlan,limitValue} from '/web/files.js';
import {transferGroups,pingEffect,timedNotice} from '/web/transfers.js';
const $ = id => document.getElementById(id);
const hex = bytes => Array.from(bytes,b=>b.toString(16).padStart(2,'0')).join('');
const encode = text => hex(new TextEncoder().encode(text));
const random = () => hex(crypto.getRandomValues(new Uint8Array(16)));
let client=random(), deviceName='';
const tab=random();
let chosen='', peers=[], latestJobs=[], inbox=[], pingAck=0, followPoll=false;
try {client=localStorage.getItem('chuan-client')||client;localStorage.setItem('chuan-client',client);} catch {}
let token='', online=false, polling=false, queueRunning=false, actionMessage='', hostId='';
const queue=[], operations=[], signatures=new Map();
const expandedFolders=new Set(), flashEffect=pingEffect($('ping-flash'));
const bytes = n => n<1024 ? `${n} B` : n<1048576 ? `${(n/1024).toFixed(1)} KB` : n<1073741824 ? `${(n/1048576).toFixed(1)} MB` : `${(n/1073741824).toFixed(1)} GB`;
function text(tag,content,cls='') { const e=document.createElement(tag); e.textContent=content; e.className=cls; return e; }
function icon(name) {
  const svg=document.createElementNS('http://www.w3.org/2000/svg','svg');svg.classList.add('icon');svg.setAttribute('aria-hidden','true');
  const use=document.createElementNS(svg.namespaceURI,'use');use.setAttribute('href','#i-'+name);svg.append(use);return svg;
}
function button(label,action) { const b=text('button',label);b.type='button';b.onclick=action;const glyph={'删除':'delete','取消':'stop'}[label];if(glyph)b.prepend(icon(glyph));return b; }
function ask(title,description,value=null) {
  const dialog=$('dialog');if(dialog.open)return Promise.resolve(null);
  $('dialog-title').textContent=title;$('dialog-description').textContent=description;
  $('dialog-input').hidden=value===null;$('dialog-label').hidden=value===null;$('dialog-input').value=value||'';
  $('dialog-confirm').textContent=value===null?'确认删除':'保存';dialog.returnValue='cancel';
  return new Promise(resolve=>{dialog.addEventListener('close',()=>resolve(dialog.returnValue==='confirm'?(value===null?true:$('dialog-input').value):null),{once:true});dialog.showModal();if(value!==null)$('dialog-input').focus();else $('dialog-form').querySelector('button[value="cancel"]').focus();});
}
function showNotice(message) {$('connection').textContent=message; $('connection').hidden=!message;}
const notice=timedNotice(message=>{actionMessage=message;showNotice(message);});
function row(name,detail) {
  const r=text('div','','row'), d=text('div','','details');d.append(text('strong',name,'name'),text('div',detail,'detail'));
  const a=text('div','','actions');r.append(icon('file'),d,a);return {r,d,a};
}
async function command(path,extra={},body=null,signal=undefined) {
  const response=await fetch(path,{method:'POST',headers:{'X-Chuan-Token':token,'X-Chuan-Client':client,'X-Chuan-Tab':tab,...extra},body,signal});
  const message=await response.text();if(!response.ok)throw new Error(message||`请求失败 (${response.status})`);
  return message;
}
function refreshUploads() { renderJobs(); }
function renderDevices() {
  const host=$('devices');host.replaceChildren();
  for(const peer of peers) {
    const b=button('',()=>{chosen=peer.id;renderDevices();updateTarget();});b.className='device'+(chosen===peer.id?' selected':'');
    b.setAttribute('aria-pressed',String(chosen===peer.id));
    const glyph=text('span','','device-glyph');glyph.append(icon(peer.browser?'browser':'computer'));
    const copy=text('span','','device-copy');copy.append(text('strong',peer.name),text('span',peer.ip+(peer.note?' · '+peer.note:'')),text('span',peer.browser?'在线 · 浏览器':'在线 · Windows','presence'));
    b.append(glyph,copy);
    host.append(b);
  }
}
function renderJobs() {
  const keys=new Set(latestJobs.map(j=>j.uploadKey).filter(Boolean));
  const local=operations.filter(op=>!op.hidden&&!keys.has(op.key)).map(op=>({id:op.key,name:op.path,total:op.file.size,
    done:op.uploadDone||0,speed:op.speed||0,state:op.rejected?6:op.cancelled?4:op.error?5:op.finished?3:op.xhr?2:0,stateText:op.status,
    peer:peers.find(peer=>peer.id===op.target)?.name||'目标设备',receiving:false,op,
    folderId:op.batch?.id,folderName:op.batch?.name,folderCount:op.batch?.count,folderTotal:op.batch?.total}));
  const opsByKey=new Map(operations.map(op=>[op.key,op]));
  const jobs=latestJobs.map(job=>{
    const op=opsByKey.get(job.uploadKey);
    const staging=op&&!op.finished&&job.state===3&&op.target!==hostId;
    return {...job,op,state:op?.error?5:staging?1:job.state,done:staging?0:job.done,speed:staging?0:job.speed,
      stateText:op?.error?op.status:staging?'正在提交目标设备':job.receiving&&job.state===3?'已发送至浏览器，请核对下载结果':job.stateText};
  });
  const signature=JSON.stringify([jobs,local,inbox,[...expandedFolders]] ,(key,value)=>key==='op'?undefined:value);
  if(signatures.get('jobs')===signature)return;signatures.set('jobs',signature);
  const host=$('jobs');host.replaceChildren();
  async function stop(item){if(item.op&&!item.op.finished)await cancel(item.op);if(item.uploadKey!==undefined)await command('/web/api/cancel/'+item.id);}
  async function remove(item){if(item.uploadKey!==undefined)await command('/web/api/delete/'+item.id);if(item.op)item.op.hidden=true;}
  function renderItem(job,parent,child=false) {
    const active=job.state<3, file=inbox.find(f=>String(f.id)===String(job.id));
    const displayName=child?job.name.replaceAll('\\','/').split('/').slice(1).join('/'):job.name;
    const {r,d,a}=row(displayName,(job.receiving?'接收':'发送')+' · '+job.peer+' · '+bytes(job.total)+' · '+job.stateText+(job.error?' · '+job.error:'')+' · '+bytes(Math.round(active?job.speed:0))+'/s');
    if(child)r.classList.add('child-row');
    if(active){const p=document.createElement('progress');p.max=100;p.value=job.total?job.done*100/job.total:0;d.querySelector('.detail').append(' · '+Math.floor(p.value)+'%');d.append(p);}
    if(file){const link=text('a',file.received?'重新下载':'接收','button');link.prepend(icon('download'));link.href='/web/files/'+file.id+'?client='+client;a.append(link);}
    if(active)a.append(button('取消',async()=>{try{await stop(job);await poll();renderJobs();}catch(e){notice(e.message);}}));
    else a.append(button('删除',async()=>{
      if(job.canDelete&&!await ask('删除传输记录','将删除此记录及对应服务端副本。浏览器电脑上的原文件不受影响。\n\n'+job.name))return;
      try{await remove(job);await poll();renderJobs();}catch(e){notice(e.message);}
    }));
    parent.append(r);
  }
  for(const group of transferGroups([...jobs,...local])) {
    if(!group.group){renderItem(group,host);continue;}
    const section=text('section','','folder-group');
    const {r,d,a}=row(group.name+' · '+group.completed+'/'+group.count+' 个文件',(group.receiving?'接收':'发送')+' · '+group.peer+' · '+group.stateText+' · '+bytes(group.done)+' / '+bytes(group.total)+' · '+group.percent+'% · '+bytes(Math.round(group.speed))+'/s');
    r.classList.add('folder-row');r.firstChild.replaceWith(icon('folder'));
    const available=group.items.some(item=>inbox.some(file=>String(file.id)===String(item.id)));
    if(group.receiving&&available&&!folderDestinations.has(group.items[0].batchId))a.append(button('保存文件夹',async()=>{
      try{await chooseDestination(group.items[0].batchId);await receiveApproved();}catch(error){notice(error.message);}
    }));
    const progress=document.createElement('progress');progress.max=100;progress.value=group.percent;d.append(progress);
    if(group.state<3)a.append(button('取消',async()=>{try{for(const item of group.items)if(item.state<3)await stop(item);await poll();renderJobs();}catch(e){notice(e.message);}}));
    else a.append(button('删除',async()=>{
      if(!await ask('删除文件夹批次','删除此批次的记录及可删除的服务端副本。浏览器电脑上的原文件不受影响。\n\n'+group.name))return;
      try{for(const item of group.items)await remove(item);await poll();renderJobs();}catch(e){notice(e.message);}
    }));
    section.append(r);
    host.append(section);
  }
  if(!host.childElementCount){const empty=text('p','暂无传输任务','empty');empty.prepend(icon('file'));host.append(empty);}
}
async function cancel(op) {
  op.cancelled=true;
  if(op.batch)for(const queued of queue)if(queued.batch===op.batch){queued.cancelled=true;queued.finished=true;queued.status='文件夹批次已取消';}
  if(op.xhr) {
    // Cancel the receive task as well when a manual confirmation holds its socket.
    try {await command(`/web/api/cancel/${op.key}`);} catch {}
    op.xhr.abort();
  }
  if(op.forwardId)try{await command('/web/api/cancel/'+op.forwardId);}catch{}
  if(op.plan?.controller&&operations.filter(item=>item.plan===op.plan).every(item=>item.cancelled))op.plan.controller.abort();
  op.status='已取消';op.finished=true;refreshUploads();
}
function enqueue(files,target=chosen) {
  if(!online){notice('Web服务未连接，无法发送文件。');return;}
  if(!target||!peers.some(peer=>peer.id===target)){notice('请选择在线的目标设备。');return;}
  if(!files.length){notice('未找到可发送文件；空文件夹不会传输。');return;}
  notice('');
  let plan;
  try{plan=batchPlan(files,random,minimumLimit(limits.files,hostLimits.files),minimumLimit(limits.bytes,hostLimits.bytes));}catch(error){notice(error.message);return;}
  plan.target=target;
  for(const {file,path,batch,item} of plan.entries)if(file instanceof File) {
    const op={file,path,batch,plan,item,target,key:random(),status:'等待整批审批',percent:0,cancelled:false,finished:false};operations.push(op);queue.push(op);
  }
  refreshUploads();runQueue();
}
async function runQueue() {
  if(queueRunning)return;queueRunning=true;
  try {
    while(queue.length) {
      const op=queue.shift();if(op.cancelled)continue;
      try {
        if(!op.plan.promise)op.plan.promise=negotiate(op.plan);
        const selection=await op.plan.promise;
        if(selection[op.item]!=='1'){op.status='接收方未选择此项';op.rejected=true;continue;}
        if(op.cancelled)continue;
        op.status='计算完整性校验';refreshUploads();
        const hash=new Sha256(), size=op.file.size;
        for(let offset=0;offset<size;offset+=1048576) {
          if(op.cancelled)break;
          hash.update(new Uint8Array(await op.file.slice(offset,offset+1048576).arrayBuffer()));
          op.percent=Math.min(100,Math.round((offset+1048576)*100/size));refreshUploads();
          await new Promise(resolve=>setTimeout(resolve,0));
        }
        if(op.cancelled)continue;
        if(!online||!peers.some(p=>p.id===op.target))throw new Error('目标设备已离线');
        op.status='发送中';op.percent=0;refreshUploads();
        await new Promise((resolve,reject)=>{
          const xhr=new XMLHttpRequest();op.xhr=xhr;xhr.open('POST','/web/api/upload');
          for(const [key,value] of Object.entries({'Content-Type':'application/octet-stream','X-Chuan-Token':token,'X-Chuan-Client':client,'X-Chuan-Tab':tab,'X-Chuan-Target':encode(op.target),'X-Chuan-Upload':op.key,'X-Chuan-Path':encode(op.path),'X-Chuan-Sender':encode(deviceName||'Web 浏览器'),'X-Chuan-Batch':op.plan.id,'X-Chuan-Item':String(op.item),'X-Chuan-Size':String(size)}))xhr.setRequestHeader(key,value);
          if(op.batch)for(const [key,value] of Object.entries({'X-Chuan-Folder':op.batch.id,'X-Chuan-Folder-Name':encode(op.batch.name),'X-Chuan-Folder-Count':String(op.batch.count),'X-Chuan-Folder-Size':op.batch.totalExact}))xhr.setRequestHeader(key,value);
          xhr.timeout=0; // Transfer lifetime is bounded by transport activity, not file size.
          xhr.upload.onprogress=e=>{if(op.cancelled)return;const now=performance.now(),done=Math.min(size,e.loaded);op.speed=op.progressAt?(done-(op.uploadDone||0))*1000/Math.max(1,now-op.progressAt):0;op.progressAt=now;op.uploadDone=done;op.percent=e.lengthComputable?Math.floor(e.loaded*100/e.total):0;op.status=op.percent===100?'等待接收确认或校验':'发送中';refreshUploads();};
          xhr.onload=()=>{if(xhr.status===201){op.fileId=JSON.parse(xhr.responseText).id;resolve();}else reject(new Error(xhr.responseText||`发送失败 (${xhr.status})`));};
          xhr.onerror=()=>reject(new Error('连接中断'));xhr.ontimeout=()=>reject(new Error('请求超时'));xhr.onabort=()=>reject(new Error('已取消'));
          xhr.send(new Blob([op.file,hash.digest()],{type:'application/octet-stream'}));
        });
        if(op.cancelled)continue;
        if(op.target!==hostId){
          const response=await command(`/web/api/forward/${op.fileId}`,{'X-Chuan-Peer':encode(op.target),'X-Chuan-Upload':op.key});
          op.status='已提交目标设备，见传输状态';
          if(response.startsWith('{'))op.forwardId=JSON.parse(response).id;
          if(op.batch&&op.forwardId&&!peers.find(peer=>peer.id===op.target)?.browser){
            let observed=Date.now();
            while(!op.cancelled){
              await poll();const job=latestJobs.find(job=>String(job.id)===String(op.forwardId));
              if(online&&job)observed=Date.now();
              if(job?.state===3)break;
              if(job?.state>=4)throw new Error(job.error||job.stateText);
              if(Date.now()-observed>15000)throw new Error('无法确认文件夹接收状态，批次已停止');
              await new Promise(resolve=>setTimeout(resolve,500));
            }
          }
        }
        else op.status='已完成';
        op.percent=100;
      }catch(error){if(!op.cancelled){op.status='发送失败：'+error.message;op.error=true;}if(op.batch)for(const queued of queue)if(queued.batch===op.batch){queued.cancelled=true;queued.finished=true;queued.status='文件夹批次已停止';}}
      finally{op.finished=true;op.xhr=null;refreshUploads();poll();}
    }
  }finally{queueRunning=false;}
}
async function poll() {
  if(polling){followPoll=true;return;}polling=true;
  const controller=new AbortController();const timeout=setTimeout(()=>controller.abort(),5000);
  try {
    const response=await fetch('/web/api/state',{headers:{'X-Chuan-Client':client,'X-Chuan-Tab':tab,'X-Chuan-Name':'','X-Chuan-Ping-Ack':String(pingAck)},cache:'no-store',signal:controller.signal});
    if(!response.ok)throw new Error(response.status===403?'Web 服务已关闭': '连接失败 ('+response.status+')');
    const state=await response.json();if(token&&token!==state.token)pingAck=0;token=state.token;online=true;hostId=state.hostId;
    deviceName=state.clientName;
    hostLimits={files:state.maxTransferFiles||'0',bytes:state.maxTransferBytes||'0'};
    if(preferencesToken!==token){await savePreferences();preferencesToken=token;}
    renderRequests(state.requests||[]);
    $('identity').textContent='本客户端：'+state.clientName+' · 局域网 IP：'+state.clientIp;
    $('host').textContent='服务地址：'+location.origin+' · '+state.name;
    $('version').textContent='版本 '+state.version;
    $('receive-mode').textContent=chosen===hostId?(state.autoReceive?'自动接收：开':'接收方确认后传输'):'';
    const list=[{id:hostId,name:state.name,ip:location.hostname,browser:false,note:state.hostNote||''},...state.peers];
    if(chosen&&!list.some(p=>p.id===chosen))chosen='';
    if(JSON.stringify(peers)!==JSON.stringify(list)){peers=list;renderDevices();}
    latestJobs=state.jobs;inbox=state.inbox||[];renderJobs();receiveApproved();updateTarget();showNotice(actionMessage);
    if(state.ping>pingAck){
      pingAck=state.ping;flashEffect.play();followPoll=true;
    }
  }catch(error){online=false;updateTarget();showNotice(error.name==='AbortError'?'连接超时，正在重连':error.message||'无法连接服务主机');}
  finally{clearTimeout(timeout);polling=false;if(followPoll){followPoll=false;setTimeout(poll,0);}}
}
let limits={files:'0',bytes:'0'}, hostLimits={files:'0',bytes:'0'}, preferencesToken='';
try{const saved=JSON.parse(localStorage.getItem('chuan-limits')||'null');if(saved)limits={files:limitValue(saved.files),bytes:limitValue(saved.bytes)};}catch{}
function minimumLimit(a,b){return BigInt(a)&&BigInt(b)?String(BigInt(a)<BigInt(b)?BigInt(a):BigInt(b)):BigInt(a)?a:b;}
async function savePreferences(){await command('/web/api/preferences',{'X-Chuan-Max-Files':limits.files,'X-Chuan-Max-Bytes':limits.bytes});}
async function negotiate(plan){
  const controller=new AbortController(), timer=setTimeout(()=>controller.abort(),150000);plan.controller=controller;
  try{
    const manifest=plan.items.map(item=>encode(item.name)+'\t'+(item.folder?'D':'F')+'\t'+item.count+'\t'+item.total+'\n').join('');
    const result=await command('/web/api/offer',{'X-Chuan-Batch':plan.id,'X-Chuan-Sender':encode(deviceName),'X-Chuan-Target':encode(plan.target),'Content-Type':'text/plain'},manifest,controller.signal);
    if(result.length!==plan.items.length||/[^01]/.test(result))throw new Error('批次审批响应无效');
    return result;
  }finally{clearTimeout(timer);plan.controller=null;}
}
const folderDestinations=new Map(), acceptedBatches=new Set(), downloaded=new Set();let downloading=false;
async function chooseDestination(batch){
  if(!window.showDirectoryPicker)throw new Error('当前浏览器无法直接写入目录。保留文件夹结构接收需要使用 Windows 客户端，或支持目录写入的浏览器安全页面。');
  const handle=await window.showDirectoryPicker({mode:'readwrite'});folderDestinations.set(batch,handle);acceptedBatches.add(batch);
}
async function receiveApproved(){
  if(downloading)return;downloading=true;
  try{
    for(const job of latestJobs){
      if(!job.receiving||!acceptedBatches.has(job.batchId)||downloaded.has(job.id)||!inbox.some(file=>String(file.id)===String(job.id)))continue;
      downloaded.add(job.id);
      try{
        if(job.folderId){
          let directory=folderDestinations.get(job.batchId);if(!directory){downloaded.delete(job.id);continue;}
          const parts=job.name.replaceAll('\\','/').split('/');
          for(const name of parts.slice(0,-1))directory=await directory.getDirectoryHandle(name,{create:true});
          let filename=parts.at(-1), suffix=0; const dot=filename.lastIndexOf('.'), stem=dot>0?filename.slice(0,dot):filename, extension=dot>0?filename.slice(dot):'';
          for(;;){try{await directory.getFileHandle(filename);filename=stem+' ('+(++suffix)+')'+extension;}catch(error){if(error.name==='NotFoundError')break;throw error;}}
          const file=await directory.getFileHandle(filename,{create:true});const writable=await file.createWritable();
          try{const response=await fetch('/web/files/'+job.id+'?client='+client);if(!response.ok)throw new Error('文件夹接收失败');await response.body.pipeTo(writable);}catch(error){await writable.abort().catch(()=>{});throw error;}
        }else{
          const link=document.createElement('a');link.href='/web/files/'+job.id+'?client='+client;link.download=job.name;document.body.append(link);link.click();link.remove();
        }
      }catch(error){notice(error.message);downloaded.delete(job.id);acceptedBatches.delete(job.batchId);}
    }
  }finally{downloading=false;}
}
function renderRequests(requests){
  const signature=JSON.stringify(requests);if(signatures.get('requests')===signature)return;signatures.set('requests',signature);
  const host=$('requests');host.replaceChildren();host.hidden=!requests.length;
  for(const request of requests){
    const card=text('section','','approval-card');const folder=request.items.length===1&&request.items[0].folder;
    const count=request.items.reduce((sum,item)=>sum+BigInt(item.count),0n),total=request.items.reduce((sum,item)=>sum+BigInt(item.total),0n);
    card.append(text('h3',folder?'接收文件夹 · '+request.items[0].name:'接收一批文件'),text('p',request.sender+' · '+count+' 个文件 · '+bytes(Number(total))));
    const actions=text('div','','approval-actions'), choices=text('div','','approval-choices');choices.hidden=true;
    const selected=new Set();let built=false;
    const decide=async bits=>{
      const controls=card.querySelectorAll('button');controls.forEach(button=>button.disabled=true);
      try{
        if(request.items.some((item,index)=>item.folder&&bits[index]==='1'))await chooseDestination(request.batch);
        await command('/web/api/decision/'+request.id,{},bits);if(bits.includes('1'))acceptedBatches.add(request.batch);await poll();
      }catch(error){notice(error.name==='AbortError'?'目录选择已取消':error.message);controls.forEach(button=>button.disabled=false);}
    };
    actions.append(button('拒绝',()=>decide('0'.repeat(request.items.length))));
    if(request.items.length>1){
      const partial=button('接受部分',()=>{
        choices.hidden=!choices.hidden;partial.setAttribute('aria-expanded',String(!choices.hidden));
        if(!built){built=true;
          for(const [index,item] of request.items.entries()){
            const label=text('label','','approval-choice'), check=document.createElement('input');check.type='checkbox';check.onchange=()=>check.checked?selected.add(index):selected.delete(index);
            label.append(check,icon(item.folder?'folder':'file'),text('span',item.name),text('small',bytes(Number(item.total))));choices.append(label);
          }
          choices.append(button('接受所选',()=>{if(!selected.size){notice('请至少选择一个项目。');return;}decide(request.items.map((_,index)=>selected.has(index)?'1':'0').join(''));}));
        }
      });partial.setAttribute('aria-expanded','false');actions.append(partial);
    }
    const accept=button(folder?'接受文件夹':'全部接受',()=>decide('1'.repeat(request.items.length)));accept.className='primary';actions.append(accept);card.append(actions,choices);host.append(card);
  }
}
$('limits').onclick=()=>{$('limit-files').value=limits.files;$('limit-mib').value=String(BigInt(limits.bytes)/1048576n);$('limits-error').textContent='';$('limits-dialog').showModal();};
$('limits-form').onsubmit=async event=>{
  if(event.submitter.value!=='save')return;event.preventDefault();
  try{const next={files:limitValue($('limit-files').value),bytes:limitValue($('limit-mib').value,1048576n)};await command('/web/api/preferences',{'X-Chuan-Max-Files':next.files,'X-Chuan-Max-Bytes':next.bytes});limits=next;try{localStorage.setItem('chuan-limits',JSON.stringify(limits));}catch{}$('limits-dialog').close();notice('传输上限已保存',3000);}catch(error){$('limits-error').textContent=error.message;}
};
for(const id of ['picker','folder-picker'])$(id).onchange=e=>{try{enqueue(selectedFiles(e.target.files,id==='folder-picker'));}catch(error){notice(error.message);}finally{e.target.value='';}};
$('choose-files').onclick=()=>$('picker').click();
$('choose-folder').onclick=()=>{
  if(!('webkitdirectory' in $('folder-picker'))){notice('当前浏览器不支持文件夹选择。请将文件夹压缩后通过“选择文件”发送。');return;}
  $('folder-picker').click();
};
function updateTarget(){
  const enabled=online&&peers.some(p=>p.id===chosen);$('ping').disabled=!enabled;$('note').disabled=!enabled;$('picker').disabled=!enabled;$('drop').classList.toggle('disabled',!enabled);$('drop').setAttribute('aria-disabled',String(!enabled));$('drop').tabIndex=enabled?0:-1;
  for(const id of ['folder-picker','choose-files','choose-folder'])$(id).disabled=!enabled;
  $('drop-title').textContent=enabled?'拖入文件或文件夹':'请先选择目标设备';$('drop-detail').textContent=enabled?`目标：${peers.find(p=>p.id===chosen)?.name||''}`:'选择在线设备后启用发送';
}
$('ping').onclick=async()=>{const target=chosen;$('ping').disabled=true;try{await command('/web/api/ping',{'X-Chuan-Peer':encode(target)});notice('Ping 已送达',3000);}catch(e){notice(e.message,5000);}finally{updateTarget();}};
$('note').onclick=async()=>{const peer=peers.find(p=>p.id===chosen);if(!peer)return;const note=await ask('IP 临时备注','IP '+peer.ip+'\n备注仅在当前客户端可见。',peer.note||'');if(note===null)return;try{await command('/web/api/note',{'X-Chuan-Address':peer.ip,'X-Chuan-Note':encode(note.trim())});await poll();}catch(e){notice(e.message);}};
$('clear').onclick=async()=>{try{await command('/web/api/clear');for(const op of operations)if(op.finished)op.hidden=true;await poll();}catch(e){notice(e.message);}};
$('refresh').onclick=async()=>{try{await command('/web/api/refresh');await poll();}catch(error){notice(error.message);}};
$('drop').onclick=()=>{if(!$('picker').disabled)$('picker').click();};
$('drop').onkeydown=e=>{if((e.key==='Enter'||e.key===' ')&&!$('picker').disabled){e.preventDefault();$('picker').click();}};
$('drop').ondragover=e=>{e.preventDefault();e.dataTransfer.dropEffect=$('picker').disabled?'none':'copy';if(!$('picker').disabled)$('drop').classList.add('dragover');};
$('drop').ondragleave=()=>$('drop').classList.remove('dragover');
$('drop').ondrop=async e=>{e.preventDefault();$('drop').classList.remove('dragover');if($('picker').disabled)return;const target=chosen;try{notice('正在读取文件夹…');enqueue(await droppedFiles(e.dataTransfer),target);}catch(error){notice('读取失败：'+error.message);}};
window.addEventListener('pagehide',()=>{if(token)fetch('/web/api/leave',{method:'POST',keepalive:true,headers:{'X-Chuan-Token':token,'X-Chuan-Client':client,'X-Chuan-Tab':tab}}).catch(()=>{});});
window.addEventListener('pageshow',()=>poll());
document.addEventListener('visibilitychange',()=>{if(!document.hidden){flashEffect.resume();poll();}});
window.addEventListener('online',()=>poll());
poll();setInterval(poll,2000);
