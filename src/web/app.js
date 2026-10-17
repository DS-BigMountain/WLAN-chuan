import {createApprovalPanel} from '/web/approvals.js';
import {Sha256} from '/web/sha256.js';
import {selectedFiles,droppedFiles,batchPlan,limitValue} from '/web/files.js';
import {transferGroups,pingEffect,timedNotice,folderProgress,createFolderDisclosure} from '/web/transfers.js';
import {ServerSession,deviceLabel,browserDeviceType} from '/web/session.js';
import {archiveSelection,packZip} from '/web/zip_store.js';
const serverSession=new ServerSession();
const $ = id => document.getElementById(id);
const hex = bytes => Array.from(bytes,b=>b.toString(16).padStart(2,'0')).join('');
const encode = text => hex(new TextEncoder().encode(text));
const random = () => hex(crypto.getRandomValues(new Uint8Array(16)));
let client=random(), deviceName='';
const tab=random();
let chosen='', peers=[], latestJobs=[], inbox=[], pingAck=0, followPoll=false;
let token='', online=false, polling=false, queueRunning=false, actionMessage='', hostId='';
let webUploadChecksumOptional=false, webTransferWait=false, webBatchFiles=false, webUploadStatus=false, webZipArchives=false, webOfferAsync=false, hostAutoReceive=false;
let latestOffers=[];
const approvalWaiters=new Map();
let hostName='',jobFilter='all',currentRequests=[];
const queue=[], operations=[], signatures=new Map();
const retrying=new Set();
const batchReports=new Set();let reportingBatch=false;
const jobRows=new Map();
let uploadFrame=0;
const compactView=()=>matchMedia('(max-width:760px)').matches;
const directoryReceiveAvailable=()=>window.isSecureContext&&typeof window.showDirectoryPicker==='function';
function updateViews(){
  $('requests').hidden=!currentRequests.length;
}
function closeDeviceSheet(){
  $('peer-sidebar').classList.remove('sheet-open');$('peer-sidebar').removeAttribute('role');$('peer-sidebar').removeAttribute('aria-modal');$('device-backdrop').hidden=true;
}
function openDeviceSheet(){
  $('peer-sidebar').classList.add('sheet-open');$('peer-sidebar').setAttribute('role','dialog');$('peer-sidebar').setAttribute('aria-modal','true');$('device-backdrop').hidden=false;
  ($('devices').querySelector('.selected')||$('devices').querySelector('button')||$('close-device-sheet')).focus();
}
function outgoingView(op,uploadSources,jobsById,peerNames,forwardsByKey){
  const source=uploadSources.get(op.key);
  const forward=(op.forwardId?jobsById.get(String(op.forwardId)):null)||forwardsByKey.get(op.key);
  if(forward&&!op.forwardId)op.forwardId=String(forward.id);
  if(forward)op.forwardState=forward.state;
  const relay=op.target!==hostId,uploaded=!!op.fileId||source?.state===3;
  const stage=relay&&uploaded?'forward': 'upload';
  let state=forward?.state??(stage==='forward'?(op.forwardState??1):(source?.state??(op.finished?3:op.xhr?2:0)));
  if(op.rejected)state=6;else if(op.cancelled)state=4;else if(op.error)state=5;
  else if(stage==='upload'&&op.fileId&&op.target===hostId)state=3;
  else if(stage==='upload'&&op.xhr&&state<2)state=2;
  const target=peers.find(peer=>peer.id===op.target);
  const awaitingApproval=state<3&&op.plan?.awaitingApproval===true&&(!op.plan.selection||op.plan.selection[op.item]==='-');
  if(awaitingApproval)state=1;
  let status=stage==='forward'?(state===3?(target?.browser?'已交给目标浏览器，请核对下载结果':'已送达目标设备'):forward?.stateText||'等待目标设备接收'):op.status;
  if(awaitingApproval)status='等待对方确认';
  else if(stage==='forward'&&state===1&&target?.browser)status='等待对方下载或保存';
  if(op.error||op.cancelled||op.rejected)status=op.status;
  if(op.packing&&!op.finished&&state<3){state=2;status=op.status;}
  const metrics=stage==='forward'?forward:source;
  const done=op.packing?op.packDone||0:state===3?op.file.size:metrics?Math.max(metrics.done||0,op.remoteDone||0):op.remoteDone??(stage==='upload'?op.uploadDone||0:0);
  return {id:source?.id??op.key,name:op.path,total:op.packing?op.archive.total:op.file.size,done,
    speed:metrics?.speed??(stage==='upload'?op.speed||0:0),state,stateText:status,peer:peerNames.get(op.target)||'目标设备',receiving:false,op,
    error:op.error?op.status:forward?.error||source?.error||'',uploadKey:source?.uploadKey,stage,awaitingApproval,canDelete:!!source?.canDelete||!!forward?.canDelete,folderId:op.batch?.id,folderName:op.batch?.name,folderCount:op.batch?.count,folderTotal:op.batch?.total};
}
const outgoingActive=op=>!op.finished||(!!op.forwardId&&(op.forwardState??1)<3&&!op.cancelled&&!op.error);
const flashEffect=pingEffect($('ping-flash'));
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
function showNotice(message) {$('connection').textContent=message; $('connection').hidden=!message;if(serverSession.blocked)$('connection').append(button('重新连接',()=>location.reload()));}
const notice=timedNotice(message=>{actionMessage=message;showNotice(message);});
function row(name,detail) {
  const r=text('div','','row'), d=text('div','','details'),heading=text('div','','job-title');
  heading.append(text('span','','direction'),text('strong',name,'name'),text('span','','job-status'));
  d.append(heading,text('div',detail,'detail'));
  const a=text('div','','actions');r.append(icon('file'),d,a);return {r,d,a};
}
async function command(path,extra={},body=null,signal=undefined) {
  serverSession.assert();
  const response=await fetch(path,{method:'POST',headers:{...serverSession.headers(),'X-Chuan-Token':token,'X-Chuan-Client':client,'X-Chuan-Tab':tab,...extra},body,signal});
  const message=await response.text();if(!response.ok)throw new Error(message||`请求失败 (${response.status})`);
  return message;
}
function refreshUploads() {if(!uploadFrame)uploadFrame=requestAnimationFrame(()=>{uploadFrame=0;renderJobs();});}
function renderDevices() {
  const host=$('devices');host.replaceChildren();
  for(const peer of peers) {
    const b=button('',()=>{chosen=peer.id;renderDevices();updateTarget();if(compactView())closeDeviceSheet();});b.className='device'+(chosen===peer.id?' selected':'');
    b.setAttribute('aria-pressed',String(chosen===peer.id));b.dataset.peerId=peer.id;
    const glyph=text('span','','device-glyph');glyph.append(icon(peer.browser?'browser':'computer'));
    const copy=text('span','','device-copy'),heading=text('span','','device-heading');heading.append(text('strong',peer.name),text('span',deviceLabel(peer),'device-kind'));copy.append(heading,text('span',peer.ip+(peer.note?' · '+peer.note:'')),text('span','在线','presence'));
    b.append(glyph,copy);
    host.append(b);
  }
  $('peer-count').textContent=String(peers.length);
}
async function retrySending(items,folder=false){
  const originals=items.map(item=>item.op).filter(Boolean),target=originals[0]?.target||items[0]?.target;
  folder=folder||originals.some(op=>op.archive);
  if(!online){notice('服务主机未连接，请连接后重试。');return;}
  if(!target||!peers.some(peer=>peer.id===target)){notice('原目标设备已离线，请在设备重新上线后重试。');return;}
  const retryKey=JSON.stringify(items.map(item=>String(item.id)));
  if(retrying.has(retryKey))return;retrying.add(retryKey);
  try{
  if(originals.length===items.length&&originals.some(op=>op.archive)){
    const sources=originals.flatMap(op=>op.archive?op.archive.sources:[{file:op.file,path:op.path}]);
    for(const source of sources)try{await source.file.slice(0,Math.min(1,source.file.size)).arrayBuffer();}catch{notice('原文件已变化或无法读取，请重新选择文件夹后重试。');chosen=target;renderDevices();updateTarget();const picker=$('folder-picker');picker.dataset.retryTarget=target;picker.click();return;}
    if(enqueue(sources,target))for(const op of originals)op.hidden=true;
    renderJobs();return;
  }
  if(originals.length===items.length&&originals.every(op=>op.file instanceof File)){
    try{
      for(const op of originals)await op.file.slice(0,Math.min(1,op.file.size)).arrayBuffer();
      if(enqueue(originals.map(op=>({file:op.file,path:op.path})),target))for(const op of originals)op.hidden=true;
      renderJobs();return;
    }catch{notice('原文件已变化或无法读取，请重新选择后重试。');}
  }else notice('浏览器已释放原文件，请重新选择后重试。');
  chosen=target;renderDevices();updateTarget();
  const picker=$(folder?'folder-picker':'picker');picker.dataset.retryTarget=target;picker.click();
  }finally{retrying.delete(retryKey);}
}
function renderJobs() {
  const peerNames=new Map(peers.map(peer=>[peer.id,peer.name]));
  const filesById=new Map(inbox.map(file=>[String(file.id),file]));
  const sourceKeys=new Set(operations.map(op=>op.key)),forwardIds=new Set(operations.map(op=>String(op.forwardId)).filter(id=>id!=='undefined'));
  const jobsById=new Map(latestJobs.map(job=>[String(job.id),job])),uploadSources=new Map(),forwardsByKey=new Map();
  for(const job of latestJobs)if(job.stage==='forward'&&job.uploadKey)forwardsByKey.set(job.uploadKey,job);
  for(const job of latestJobs)if(job.uploadKey){const previous=uploadSources.get(job.uploadKey);if(!previous||BigInt(job.id)<BigInt(previous.id))uploadSources.set(job.uploadKey,job);}
  const local=operations.filter(op=>!op.hidden).map(op=>outgoingView(op,uploadSources,jobsById,peerNames,forwardsByKey));
  const plain=latestJobs.filter(job=>!sourceKeys.has(job.uploadKey)&&!forwardIds.has(String(job.id)));
  const preferred=new Map();
  for(const job of plain)if(job.uploadKey&&!job.receiving){const previous=preferred.get(job.uploadKey);if(!previous||job.stage==='forward'||(previous.stage!=='forward'&&Number(job.id)>Number(previous.id)))preferred.set(job.uploadKey,job);}
  const jobs=plain.filter(job=>!job.uploadKey||job.receiving||preferred.get(job.uploadKey)===job).map(job=>{
    const staging=job.stage==='staging'&&job.state===3;
    const waitingForBrowser=!job.receiving&&job.state===1&&job.stage==='forward'&&peers.some(peer=>peer.id===job.target&&peer.browser);
    const receivingFolder=job.receiving&&!!job.folderId,saving=receivingFolder&&folderDestinations.has(job.batchId),saved=savedFiles.has(String(job.id));
    const cancelled=receivingFolder&&!saved&&cancelledFolderBatches.has(job.batchId),failed=receivingFolder&&!saved&&folderSaveErrors.has(job.batchId);
    return {...job,serverState:job.state,state:staging?1:receivingFolder&&saved?3:cancelled?4:failed?5:saving&&job.state===3&&!saved?(writingFiles.has(String(job.id))?2:1):job.state,
      done:staging||cancelled||failed?0:receivingFolder&&saved?job.total:saving&&job.state===3&&!saved&&!writingFiles.has(String(job.id))?0:job.done,speed:staging?0:job.speed,
      awaitingApproval:!job.receiving&&job.state===1&&!waitingForBrowser,
      stateText:staging?'上传至主机已完成，等待目标设备接收':waitingForBrowser?'等待对方下载或保存':!job.receiving&&job.state===1?'等待对方确认':job.receiving&&job.state===3?'已交给浏览器，请核对下载结果':job.stateText};
  });
  const host=$('jobs'), visible=new Set(), elements=[];
  const setText=(element,value)=>{if(element.textContent!==value)element.textContent=value;};
  function retain(key,shape,model,create){
    visible.add(key);let entry=jobRows.get(key),expanded=null,focusPath=null;
    if(!entry||entry.shape!==shape){
      const previous=entry;
      if(previous&&key.startsWith('folder:')){expanded=[...previous.r.querySelectorAll('details[open]')].map(node=>node.classList.contains('folder-tree')?'':node.closest('[data-path]')?.dataset.path).filter(path=>path!==undefined);const focused=document.activeElement;if(focused?.tagName==='SUMMARY'&&previous.r.contains(focused))focusPath=focused.closest('[data-path]')?.dataset.path??'';}
      entry={...create(model),shape};jobRows.set(key,entry);
      if(previous)previous.r.replaceWith(entry.r);
      if(expanded?.includes(''))entry.r.querySelector('.folder-tree').open=true;
    }
    entry.update(model);
    if(expanded){for(const node of entry.r.querySelectorAll('[data-path]>details'))if(expanded.includes(node.parentElement.dataset.path))node.open=true;const focused=focusPath===''?entry.r.querySelector('.folder-tree>summary'):[...entry.r.querySelectorAll('[data-path]')].find(node=>node.dataset.path===focusPath)?.querySelector('summary');if(focusPath!==null)focused?.focus({preventScroll:true});}
    elements.push(entry.r);
  }
  async function stop(item){
    if(item.op){if(outgoingActive(item.op))await cancel(item.op);return;}
    if(item.receiving&&item.folderId){cancelledFolderBatches.add(item.batchId);acceptedBatches.delete(item.batchId);folderDestinations.delete(item.batchId);for(const job of latestJobs)if(job.batchId===item.batchId)folderDownloadControllers.get(String(job.id))?.abort();}
    if(item.uploadKey!==undefined&&(item.serverState??item.state)<3)await command('/web/api/cancel/'+item.id);
  }
  async function remove(item){const ids=new Set();if(item.uploadKey!==undefined)ids.add(String(item.id));if(item.op?.forwardId)ids.add(String(item.op.forwardId));for(const id of ids)await command('/web/api/delete/'+id);if(item.op)item.op.hidden=true;}
  function renderItem(job) {
    const active=job.state<3, file=filesById.get(String(job.id));
    const {r,d,a}=row(job.name,'');
    const detail=d.querySelector('.detail'),statusLabel=d.querySelector('.job-status'),progress=active?document.createElement('progress'):null;
    const metrics=text('div','','job-progress'),percentLabel=text('small','','progress-metrics');
    d.querySelector('.direction').textContent=job.receiving?'接收':'发送';
    if(progress){progress.max=100;progress.setAttribute('aria-label','传输进度');metrics.append(progress,percentLabel);d.append(metrics);}
    const stage=text('div','','stages');d.append(stage);
    if(file){const link=text('a',file.received?'重新下载':'下载','button');link.prepend(icon('download'));link.href=serverSession.blocked?'#':serverSession.download(file.id,token);link.onclick=event=>{if(!online){event.preventDefault();notice('服务主机未连接，暂时无法下载。');}};a.append(link);}
    if(active)a.append(button('取消',async()=>{try{await stop(job);await poll();renderJobs();}catch(e){notice(e.message);}}));
    else {if(!job.receiving&&job.state>=4){const retry=button('重试',()=>retrySending([job]));retry.className='retry';a.append(retry);}a.append(button('删除',async()=>{
      if(job.canDelete&&!await ask('删除传输记录','将删除此记录及对应服务端副本。浏览器电脑上的原文件不受影响。\n\n'+job.name))return;
      try{await remove(job);await poll();renderJobs();}catch(e){notice(e.message);}
    }));}
    return {r,update(next){
      job=next;const percent=job.total?Math.min(100,job.done*100/job.total):0;
      setText(d.querySelector('.name'),job.name);
      const ready=job.receiving&&file&&!file.received&&job.state!==2;
      const status=job.receiving&&file?(file.received?'已交给浏览器，请核对下载结果':job.state===2?'正在交给浏览器':'可下载'):job.stateText;
      setText(detail,(job.receiving?'来自 ':'目标：')+job.peer+' · '+bytes(job.total)+(job.error?' · '+job.error:''));
      setText(statusLabel,status);statusLabel.classList.toggle('green',job.state===3);
      statusLabel.classList.toggle('waiting',!!job.awaitingApproval);
      if(progress&&progress.value!==percent)progress.value=percent;
      if(progress){metrics.hidden=!!ready||!!job.awaitingApproval;setText(percentLabel,Math.floor(percent)+'%  '+(job.op?.packing?'已打包 '+job.op.packComplete+'/'+job.op.archive.count+' 个文件':bytes(Math.round(job.speed))+'/s'));}
      setText(stage,job.op?.packing&&job.state<3?'正在打包：'+(job.op.packPath||job.op.archive.root):'');stage.hidden=!stage.textContent;
      a.querySelectorAll('a').forEach(link=>{link.setAttribute('aria-disabled',String(!online));link.tabIndex=online?0:-1;});
      const retry=a.querySelector('.retry');if(retry)retry.disabled=!online;
    }};
  }
  function renderFolder(group) {
    const section=text('section','','folder-group');
    const {r,d,a}=row('',''),tree=createFolderDisclosure(group,bytes),name=tree.button,detail=d.querySelector('.detail'),statusLabel=d.querySelector('.job-status');
    d.querySelector('.name').replaceWith(name);
    d.querySelector('.direction').textContent=group.receiving?'接收':'发送';
    r.classList.add('folder-row');r.firstChild.replaceWith(icon('folder'));
    const available=group.items.some(item=>filesById.has(String(item.id)));
    if(group.receiving&&available&&!folderDestinations.has(group.items[0].batchId))a.append(button('保存文件夹',async()=>{
      try{await chooseDestination(group.items[0].batchId);await receiveApproved();}catch(error){notice(error.message);}
    }));
    const progress=document.createElement('progress'),metrics=text('div','','job-progress'),percentLabel=text('small','','progress-metrics');
    progress.max=100;progress.setAttribute('aria-label','文件夹传输进度');metrics.append(progress,percentLabel);d.append(metrics);
    if(group.state<3)a.append(button('取消',async()=>{try{for(const item of group.items)if(item.state<3)await stop(item);await poll();renderJobs();}catch(e){notice(e.message);}}));
    else {if(!group.receiving&&group.state>=4){const retry=button('重试',()=>retrySending(group.items,true));retry.className='retry';a.append(retry);}a.append(button('删除',async()=>{
      if(!await ask('删除文件夹批次','删除此批次的记录及可删除的服务端副本。浏览器电脑上的原文件不受影响。\n\n'+group.name))return;
      try{for(const item of group.items)await remove(item);await poll();renderJobs();}catch(e){notice(e.message);}
    }));}
    section.append(r);
    section.append(tree.element);
    return {r:section,update(next){
      group=next;
      setText(name,group.name);
      const saved=group.receiving&&group.items.every(item=>savedFiles.has(String(item.id)))&&group.completed>=group.count;
      const status=group.receiving?(saved?'已保存到所选目录':group.state===4?'目录保存已取消':group.state===5?'目录保存失败，可重新选择目录':group.state===3?'已交给浏览器，请核对本地目录':folderDestinations.has(group.items[0].batchId)?'保存至所选目录中':'等待选择保存目录'):group.stateText;
      setText(detail,(group.receiving?'来自 ':'目标：')+group.peer+' · '+folderProgress(group,bytes)+(group.error?'\n'+group.error:''));
      setText(statusLabel,status);statusLabel.classList.toggle('green',saved||(!group.receiving&&group.state===3));
      statusLabel.classList.toggle('waiting',!!group.awaitingApproval);
      metrics.hidden=!!group.awaitingApproval;setText(percentLabel,(group.done>0&&group.percent===0?'<0.1':group.percent)+'%  '+bytes(Math.round(group.speed))+'/s');
      if(progress.value!==group.percent)progress.value=group.percent;
      const save=[...a.querySelectorAll('button')].find(button=>button.textContent==='保存文件夹');if(save){save.disabled=!online||!directoryReceiveAvailable();save.title=directoryReceiveAvailable()?'':'此页面不支持目录写入，请使用 Windows 客户端或先压缩后发送。';}
      const retry=a.querySelector('.retry');if(retry)retry.disabled=!online;
      tree.update(group);
    }};
  }
  const groups=transferGroups([...jobs,...local]).sort((a,b)=>Number(b.state<3)-Number(a.state<3));
  document.querySelectorAll('[data-job-filter]').forEach(button=>{const kind=button.dataset.jobFilter,count=groups.filter(group=>kind==='all'||(kind==='receive')===!!group.receiving).length;setText(button,({all:'全部',send:'发送',receive:'接收'})[kind]+' ('+count+')');});
  for(const group of groups.filter(group=>jobFilter==='all'||(jobFilter==='receive')===!!group.receiving)) {
    if(!group.group){
      const file=filesById.get(String(group.id));
      retain('file:'+group.id,JSON.stringify([group.state<3,!group.receiving&&group.state>=4,file?.id,file?.received]),group,renderItem);
    }else{
      const available=group.items.some(item=>filesById.has(String(item.id)));
      retain('folder:'+group.key,JSON.stringify([group.state<3,!group.receiving&&group.state>=4,!!group.receiving,available,folderDestinations.has(group.items[0].batchId)]),group,renderFolder);
    }
  }
  for(const [key,entry] of jobRows)if(!visible.has(key)){entry.r.remove();jobRows.delete(key);}
  if(!elements.length){
    if(!host.querySelector('.empty')){const empty=text('p','暂无传输任务','empty');empty.prepend(icon('file'));host.replaceChildren(empty);}
    return;
  }
  // 仅插入新行或调整顺序；进度更新保留按钮节点及键盘焦点。
  let cursor=host.firstChild;
  for(const element of elements){if(element!==cursor)host.insertBefore(element,cursor);cursor=element.nextSibling;}
  while(cursor){const next=cursor.nextSibling;cursor.remove();cursor=next;}
}
async function cancel(op) {
  op.cancelled=true;
  op.packController?.abort();
  op.waitController?.abort();
  if(op.batch)for(const queued of queue)if(queued.batch===op.batch){queued.cancelled=true;queued.finished=true;queued.status='文件夹批次已取消';}
  if(op.xhr) {
    // Cancel the receive task as well when a manual confirmation holds its socket.
    try {await command(`/web/api/cancel/${op.key}`);} catch {}
    op.xhr.abort();
  }
  if(op.forwardId)try{await command('/web/api/cancel/'+op.forwardId);}catch{}
  if(op.plan?.requestId)cancelPendingItems(op.plan);
  if(op.plan?.controller&&operations.filter(item=>item.plan===op.plan).every(item=>item.cancelled))op.plan.controller.abort();
  op.status='已取消';op.finished=true;if(op.plan?.registered){batchReports.add(op.plan);flushBatchReports();}refreshUploads();
}
async function flushBatchReports(){
  if(reportingBatch||!online||serverSession.blocked)return;reportingBatch=true;
  try{for(const plan of batchReports){
    const ops=operations.filter(op=>op.plan===plan),pending=ops.filter(op=>op.finished&&!op.reported&&op.registered&&(op.error||op.cancelled)&&plan.selection?.[op.item]==='1');
    if(pending.length){
      const body=pending.map(op=>op.key+'\t'+(op.error?5:4)+'\t'+encode((op.status||'本批次已停止，此文件未完成').replace(/[\r\n]/g,' ').slice(0,1024))+'\n').join('');
      const controller=new AbortController(),timeout=setTimeout(()=>controller.abort(),5000);
      try{await command('/web/api/batch-status',{'X-Chuan-Batch':plan.id,'Content-Type':'text/plain'},body,controller.signal);for(const op of pending)op.reported=true;}catch{continue;}finally{clearTimeout(timeout);}
    }
    if(ops.every(op=>op.finished))batchReports.delete(plan);
  }}finally{reportingBatch=false;}
}
function enqueue(files,target=chosen) {
  if(!online){notice('Web服务未连接，无法发送文件。');return;}
  if(!target||!peers.some(peer=>peer.id===target)){notice('请选择在线的目标设备。');return;}
  if(!files.length){notice('未找到可发送文件；空文件夹不会传输。');return;}
  notice('');
  let plan;
  try{batchPlan(files,random,minimumLimit(limits.files,hostLimits.files),minimumLimit(limits.bytes,hostLimits.bytes));plan=batchPlan(archiveSelection(files),random,'0',minimumLimit(limits.bytes,hostLimits.bytes));}catch(error){notice(error.message);return;}
  plan.target=target;
  for(const {file,path,batch,item,archive} of plan.entries)if(file instanceof File||archive) {
    const op={file,path,batch,plan,item,archive,target,key:random(),status:'等待发送',percent:0,cancelled:false,finished:false};operations.push(op);queue.push(op);
  }
  preparePlan(plan);refreshUploads();runQueue();return true;
}
function stopOutgoingFolder(op,message,rejected=false){
  const related=op.batch?operations.filter(item=>item.batch===op.batch):[op];
  for(const item of related)if((!item.fileId&&!item.finished)||item===op){
    item.error=false;item.cancelled=!rejected;item.rejected=rejected;item.status=message;item.finished=true;
    item.xhr?.abort();item.waitController?.abort();item.packController?.abort();
  }
}
function failOutgoing(op,message){
  if(op.finished||op.cancelled||op.rejected)return;
  op.error=true;op.finished=true;op.status='发送失败：'+message;
  op.packController?.abort();op.xhr?.abort();op.waitController?.abort();
}
async function uploadResult(op){
  if(!webUploadStatus)return null;
  const controller=new AbortController(),timeout=setTimeout(()=>controller.abort(),4000);
  try{return JSON.parse(await command('/web/api/upload-status/'+op.key,{},null,controller.signal));}catch{return null;}finally{clearTimeout(timeout);}
}
async function confirmUpload(op,error){
  if(error.receiverCancelled){stopOutgoingFolder(op,'接收端已取消本次传输');return true;}
  if(error.sourceError)return false;
  let result=await uploadResult(op);
  // The payload may be committed even if its HTTP response was lost.
  for(let attempt=0;result?.state===2&&attempt<3;attempt++){await new Promise(resolve=>setTimeout(resolve,250));result=await uploadResult(op);}
  if(result)op.remoteDone=result.done||0;
  if(result?.state===3){op.fileId=String(result.id);try{await deliverUpload(op);op.error=false;op.percent=100;return true;}catch(failure){error.message=failure.message;return false;}}
  if(result?.state===4||result?.state===6){stopOutgoingFolder(op,result.error||(result.state===4?'接收端已取消本次传输':'接收端已拒绝本次传输'),result.state===6);return true;}
  if(result?.state===5&&result.error)error.message=result.error;
  return false;
}
async function deliverUpload(op){
  if(op.target!==hostId){
    const response=await command(`/web/api/forward/${op.fileId}`,{'X-Chuan-Peer':encode(op.target),'X-Chuan-Upload':op.key});
    op.status='已提交目标设备，见传输状态';
    if(response.startsWith('{'))op.forwardId=JSON.parse(response).id;
    if(op.batch&&op.forwardId&&!peers.find(peer=>peer.id===op.target)?.browser)await waitForForward(op);
  }else op.status='已完成';
}
function preparePlan(plan){
  if(plan.observed)return;plan.observed=true;
  plan.promise??=negotiate(plan);
  plan.promise.then(selection=>{plan.selection=selection;plan.ready=true;runQueue();},error=>{plan.failure=error;plan.ready=true;runQueue();});
}
async function registerOperation(op){
  if(op.registered||!webBatchFiles||op.target!==hostId||!op.plan.items.some(item=>item.folder)&&!op.plan.entries.some(entry=>entry.archive))return;
  const ops=operations.filter(item=>item.plan===op.plan&&item.item===op.item&&!item.registered);
  const body=ops.map(item=>item.item+'\t'+item.key+'\t'+(item.batch?.id||'')+'\t'+encode(item.path)+'\t'+item.file.size+(item.archive&&webZipArchives?'\tA':'')+'\n').join('');
  await command('/web/api/batch-files',{'X-Chuan-Batch':op.plan.id,'Content-Type':'text/plain'},body,op.plan.controller?.signal);
  for(const item of ops)item.registered=true;op.plan.registered=true;
}
async function runQueue() {
  if(queueRunning)return;queueRunning=true;
  try {
    for(const op of queue)preparePlan(op.plan);
    while(queue.length) {
      const index=queue.findIndex(op=>op.cancelled||op.plan.ready||op.plan.selection?.[op.item]==='1'||op.plan.selection?.[op.item]==='0');
      if(index<0)break;
      const op=queue.splice(index,1)[0];if(op.cancelled)continue;
      try {
        if(op.plan.failure)throw op.plan.failure;
        const selection=op.plan.selection||await op.plan.promise;
        if(selection[op.item]!=='1'){op.status='接收方未选择此项';op.rejected=true;continue;}
        if(op.cancelled)continue;
        await registerOperation(op);
        if(op.archive){
          op.packing=true;op.packDone=0;op.packComplete=0;op.status='正在打包 ZIP';op.packController=new AbortController();refreshUploads();
          let watching=false,checkedAt=0;
          const check=async()=>{if(watching||!op.registered||Date.now()-checkedAt<900)return;watching=true;checkedAt=Date.now();try{const remote=await uploadResult(op);if(remote?.state===4||remote?.state===6)stopOutgoingFolder(op,remote.error||'接收端已取消本次传输',remote.state===6);else if(remote?.state===5)failOutgoing(op,remote.error||'接收端未能继续接收');}finally{watching=false;}};
          const timer=setInterval(check,1000);
          try{const packed=await packZip(op.archive,progress=>{op.packDone=progress.done;op.packComplete=progress.complete;op.packPath=progress.path;refreshUploads();check();},op.packController.signal);op.file=packed.file;op.digest=packed.digest;await check();}
          finally{clearInterval(timer);op.packing=false;op.packController=null;}
        }
        if(op.cancelled||op.error||op.rejected)continue;
        const size=op.file.size, hash=(op.digest||webUploadChecksumOptional)?null:new Sha256();
        op.hashMs=0;
        if(hash){
          // Older servers require the SHA-256 trailer; current servers accept the original File.
          op.status='准备发送';refreshUploads();
          const hashStarted=performance.now();
          for(let offset=0;offset<size;offset+=1048576) {
            if(op.cancelled)break;
            hash.update(new Uint8Array(await op.file.slice(offset,offset+1048576).arrayBuffer()));
            op.percent=Math.min(100,Math.round((offset+1048576)*100/size));refreshUploads();
            await new Promise(resolve=>setTimeout(resolve,0));
          }
          op.hashMs=Math.round(performance.now()-hashStarted);
        }
        if(op.cancelled)continue;
        if(!online||!peers.some(p=>p.id===op.target)){
          const deadline=Date.now()+15000;
          while(!op.cancelled&&!serverSession.blocked&&Date.now()<deadline&&(!online||!peers.some(p=>p.id===op.target))){op.status='连接中断，正在重连';refreshUploads();await poll();if(online&&peers.some(p=>p.id===op.target))break;await new Promise(resolve=>setTimeout(resolve,500));}
        }
        if(op.cancelled)continue;
        if(!online||!peers.some(p=>p.id===op.target)){op.connectionLost=true;throw new Error('目标设备已离线，未完成文件可在重连后重试');}
        op.status='发送中';op.percent=0;refreshUploads();
        await new Promise((resolve,reject)=>{
          const xhr=new XMLHttpRequest();op.xhr=xhr;xhr.open('POST','/web/api/upload');
          xhr.setRequestHeader('X-Chuan-Hash-Ms',String(op.hashMs));
          xhr.setRequestHeader('X-Chuan-Checksum',hash||op.digest?'sha256':'none');
          const uploadStarted=performance.now();let bodyMs=0;
          xhr.upload.onload=()=>{bodyMs=Math.round(performance.now()-uploadStarted);};
          xhr.onloadend=()=>{
            const controller=new AbortController(),timer=setTimeout(()=>controller.abort(),5000);
            command('/web/api/diagnostic',{'X-Chuan-Upload':op.key,'X-Chuan-Hash-Ms':String(op.hashMs),
              'X-Chuan-Upload-Ms':String(Math.round(performance.now()-uploadStarted)),
              'X-Chuan-Body-Ms':String(bodyMs),'X-Chuan-Status':String(xhr.status)},null,controller.signal)
              .catch(()=>{}).finally(()=>clearTimeout(timer));
          };
          for(const [key,value] of Object.entries({...serverSession.headers(),'Content-Type':'application/octet-stream','X-Chuan-Token':token,'X-Chuan-Client':client,'X-Chuan-Tab':tab,'X-Chuan-Target':encode(op.target),'X-Chuan-Upload':op.key,'X-Chuan-Path':encode(op.path),'X-Chuan-Sender':encode(deviceName||'Web 浏览器'),'X-Chuan-Batch':op.plan.id,'X-Chuan-Item':String(op.item),'X-Chuan-Size':String(size)}))xhr.setRequestHeader(key,value);
          if(op.batch)for(const [key,value] of Object.entries({'X-Chuan-Folder':op.batch.id,'X-Chuan-Folder-Name':encode(op.batch.name),'X-Chuan-Folder-Count':String(op.batch.count),'X-Chuan-Folder-Size':op.batch.totalExact}))xhr.setRequestHeader(key,value);
          xhr.timeout=0; // Transfer lifetime is bounded by transport activity, not file size.
          xhr.upload.onprogress=e=>{if(op.cancelled)return;const now=performance.now(),done=Math.min(size,e.loaded);op.speed=op.progressAt?(done-(op.uploadDone||0))*1000/Math.max(1,now-op.progressAt):0;op.progressAt=now;op.uploadDone=done;op.percent=e.lengthComputable?Math.floor(e.loaded*100/e.total):0;op.status=op.percent===100?'等待接收确认':'发送中';refreshUploads();};
          xhr.onload=()=>{if(xhr.status===201){op.fileId=JSON.parse(xhr.responseText).id;resolve();}else {const error=new Error(xhr.responseText||`发送失败 (${xhr.status})`);error.receiverCancelled=xhr.status===410;reject(error);}};
          xhr.onerror=async()=>{try{await op.file.slice(0,Math.min(1,size)).arrayBuffer();const error=new Error('网络连接中断，未收到接收端保存确认');error.transportError=true;reject(error);}catch{const error=new Error('无法读取源文件，文件可能已移动、被占用或已变更');error.sourceError=true;reject(error);}};xhr.ontimeout=()=>reject(new Error('请求超时'));xhr.onabort=()=>reject(new Error('已取消'));
          xhr.send(hash||op.digest?new Blob([op.file,op.digest||hash.digest()],{type:'application/octet-stream'}):op.file);
        });
        if(op.cancelled)continue;
        await deliverUpload(op);
        op.percent=100;
        if(op.batch)op.batch.networkFailures=0;
      }catch(error){
        if(!op.cancelled&&!op.error&&!op.rejected&&!await confirmUpload(op,error)){
          op.status='发送失败：'+error.message;op.error=true;
          if(error.transportError&&op.batch)op.batch.networkFailures=(op.batch.networkFailures||0)+1;
          if(op.batch&&(op.connectionLost||serverSession.blocked||op.batch.networkFailures>=3))for(const queued of queue)if(queued.batch===op.batch){queued.cancelled=true;queued.finished=true;queued.status='连接持续中断，批次已停止，请检查连接后重试';}
        }
      }
      finally{op.finished=true;op.xhr=null;if(op.plan?.registered){batchReports.add(op.plan);await flushBatchReports();}refreshUploads();if(!op.batch||!queue.length||op.error)poll();}
    }
  }finally{queueRunning=false;}
}
async function waitForForward(op){
  let useWait=webTransferWait,observed=Date.now();
  while(!op.cancelled){
    let job;
    if(useWait){
      const controller=new AbortController(),timer=setTimeout(()=>controller.abort(),5000);op.waitController=controller;
      try{
        job=JSON.parse(await command('/web/api/wait/'+op.forwardId,{},null,controller.signal));
        if(!Number.isInteger(job.state)||job.state<0||job.state>6)throw new Error('传输状态响应无效');
      }catch{
        if(op.cancelled)return;
        // 请求失败或服务端不支持时，恢复既有轮询及失联检测。
        useWait=false;job=null;
      }finally{clearTimeout(timer);op.waitController=null;}
    }
    if(!useWait){await poll();job=latestJobs.find(job=>String(job.id)===String(op.forwardId));}
    if(op.cancelled)return;
    if(job&&(useWait||online))observed=Date.now();
    if(job?.state===3)return;
    if(job?.state===4||job?.state===6){stopOutgoingFolder(op,job.error||(job.state===4?'接收端已取消本次传输':'接收端已拒绝本次传输'),job.state===6);return;}
    if(job?.state>=4)throw new Error(job.error||job.stateText||'目标设备未完成接收');
    if(Date.now()-observed>15000)throw new Error('无法确认文件夹接收状态，批次已停止');
    if(!useWait)await new Promise(resolve=>setTimeout(resolve,500));
  }
}
async function poll() {
  if(serverSession.blocked)return;
  if(polling){followPoll=true;return;}polling=true;
  const controller=new AbortController();const timeout=setTimeout(()=>controller.abort(),5000);
  try {
    if(!serverSession.host){await serverSession.initialize(controller.signal);client=serverSession.client;try{const saved=JSON.parse(serverSession.read('limits')||'null');if(saved)limits={files:limitValue(saved.files),bytes:limitValue(saved.bytes)};}catch{}}
    const response=await fetch('/web/api/state',{headers:{...serverSession.headers(),'X-Chuan-Client':client,'X-Chuan-Tab':tab,'X-Chuan-Device':browserDeviceType(),'X-Chuan-Name':'','X-Chuan-Ping-Ack':String(pingAck)},cache:'no-store',signal:controller.signal});
    if(response.status===409)serverSession.fail();
    if(!response.ok)throw new Error(response.status===403?'Web 服务已关闭': '连接失败 ('+response.status+')');
    const state=await response.json();serverSession.check(state);if(token&&token!==state.token)pingAck=0;token=state.token;online=true;hostId=state.hostId;
    if($('android-client'))$('android-client').hidden=state.androidAvailable!==true;
    if($('android-mobile'))$('android-mobile').hidden=state.androidAvailable!==true;
    hostName=state.name;
    webUploadChecksumOptional=state.webUploadChecksumOptional===true;
    webTransferWait=state.webTransferWait===true;
    webBatchFiles=state.webBatchFiles===true;
    webUploadStatus=state.webUploadStatus===true;
    webZipArchives=state.webZipArchives===true;
    webOfferAsync=state.webOfferAsync===true;
    latestOffers=state.offers||[];settleApprovals();
    hostAutoReceive=state.autoReceive;
    deviceName=state.clientName;
    hostLimits={files:state.maxTransferFiles||'0',bytes:state.maxTransferBytes||'0'};
    if(preferencesToken!==token){await savePreferences();preferencesToken=token;}
    renderRequests(state.requests||[]);
    $('identity').textContent='本浏览器：'+state.clientName+' · '+state.clientIp;
    $('host').textContent='服务主机：'+state.name+' · '+location.hostname;
    $('browser-name').textContent=state.clientName;$('browser-ip').textContent=state.clientIp;
    $('service-origin').textContent=location.origin;$('footer-origin').textContent='服务地址：'+location.origin;
    $('service-status').textContent='已连接服务主机';$('service-status').classList.remove('offline');
    $('version').textContent='版本 '+state.version;
    const list=[{id:hostId,name:state.name,ip:location.hostname,browser:false,platform:state.platform,note:state.hostNote||''},...state.peers];
    if(chosen&&!list.some(p=>p.id===chosen))chosen='';
    if(JSON.stringify(peers)!==JSON.stringify(list)){peers=list;renderDevices();}
    latestJobs=state.jobs;inbox=state.inbox||[];
    const remoteUploads=new Map(latestJobs.filter(job=>job.uploadKey).map(job=>[job.uploadKey,job]));
    for(const op of operations){const remote=remoteUploads.get(op.key);if(remote)op.remoteDone=remote.done||0;if(!op.finished&&!op.cancelled){if(remote?.state===4||remote?.state===6)stopOutgoingFolder(op,remote.error||(remote.state===4?'接收端已取消本次传输':'接收端已拒绝本次传输'),remote.state===6);else if(remote?.state===5)failOutgoing(op,remote.error||'接收端未能继续接收');}}
    renderJobs();receiveApproved();updateTarget();showNotice(actionMessage);flushBatchReports();
    if(state.ping>pingAck){
      pingAck=state.ping;flashEffect.play();followPoll=true;
    }
  }catch(error){online=false;if(serverSession.blocked){for(const op of operations){if(!op.finished){op.cancelled=true;op.finished=true;op.status='服务端会话已变化，任务已停止';}op.xhr?.abort();op.waitController?.abort();op.packController?.abort();op.plan?.controller?.abort();}for(const controller of folderDownloadControllers.values())controller.abort();acceptedBatches.clear();}updateTarget();renderRequests(currentRequests);renderJobs();$('service-status').textContent=serverSession.blocked?'需要重新连接':'正在重新连接';$('service-status').classList.add('offline');showNotice(error.name==='AbortError'?'连接超时，正在重连':error.message||'无法连接服务主机');}
  finally{clearTimeout(timeout);polling=false;if(followPoll){followPoll=false;setTimeout(poll,0);}}
}
let limits={files:'0',bytes:'0'}, hostLimits={files:'0',bytes:'0'}, preferencesToken='';
function minimumLimit(a,b){return BigInt(a)&&BigInt(b)?String(BigInt(a)<BigInt(b)?BigInt(a):BigInt(b)):BigInt(a)?a:b;}
async function savePreferences(){await command('/web/api/preferences',{'X-Chuan-Max-Files':limits.files,'X-Chuan-Max-Bytes':limits.bytes});}
function settleApprovals(){
  for(const [batch,waiter] of approvalWaiters){
    const offer=latestOffers.find(offer=>offer.batch===batch&&String(offer.id)===waiter.id);
    if(!offer)continue;
    if(offer.selection?.length===waiter.plan.items.length&&!/[^01-]/.test(offer.selection)&&waiter.plan.selection!==offer.selection){waiter.plan.selection=offer.selection;refreshUploads();runQueue();}
    if(offer.state<3)continue;
    if(offer.state===3||offer.state===6)waiter.resolve(offer.selection);
    else{const error=new Error(offer.error||'接收申请已停止');error.receiverCancelled=offer.state===4;waiter.reject(error);}
  }
}
async function cancelPendingItems(plan){
  if(!plan.requestId||!webOfferAsync)return;
  const mask=plan.items.map((_,index)=>operations.some(op=>op.plan===plan&&op.item===index&&op.cancelled)?'0':'-').join('');
  if(mask.includes('0'))try{await command('/web/api/offer-cancel',{'X-Chuan-Batch':plan.id},mask);await poll();}catch{}
}
async function cancelOffer(plan){
  const id=plan.requestId||latestOffers.find(offer=>offer.batch===plan.id)?.id;
  if(id)try{await command('/web/api/cancel/'+id);await poll();}catch{}
}
async function waitForApproval(plan,signal){
  if(signal.aborted){await cancelOffer(plan);throw new DOMException('已取消','AbortError');}
  let abort;
  try{return await new Promise((resolve,reject)=>{
    approvalWaiters.set(plan.id,{id:plan.requestId,plan,resolve,reject});
    abort=()=>{cancelOffer(plan);reject(new DOMException('已取消','AbortError'));};
    signal.addEventListener('abort',abort,{once:true});settleApprovals();poll();
  });}finally{approvalWaiters.delete(plan.id);signal.removeEventListener('abort',abort);}
}
async function negotiate(plan){
  const controller=new AbortController();plan.controller=controller;
  plan.awaitingApproval=true;refreshUploads();
  try{
    const manifest=plan.items.map(item=>encode(item.name)+'\t'+(item.folder?'D':'F')+'\t'+item.count+'\t'+item.total+'\n').join('');
    const asynchronous=webOfferAsync,headers={'X-Chuan-Batch':plan.id,'X-Chuan-Sender':encode(deviceName),'X-Chuan-Target':encode(plan.target),'X-Chuan-Offer-Wait':'1','Content-Type':'text/plain'};
    if(asynchronous)headers['X-Chuan-Offer-Async']='1';
    let result;
    if(asynchronous){
      const posting=new AbortController(),timeout=setTimeout(()=>posting.abort(),5000);
      try{const response=JSON.parse(await command('/web/api/offer',headers,manifest,posting.signal));if(!/^\d+$/.test(response.id))throw new Error('接收申请响应无效');plan.requestId=String(response.id);}
      catch(error){await poll();await cancelOffer(plan);throw error;}finally{clearTimeout(timeout);}
      await cancelPendingItems(plan);result=await waitForApproval(plan,controller.signal);
    }else result=await command('/web/api/offer',headers,manifest,controller.signal);
    if(result.length!==plan.items.length||/[^01]/.test(result))throw new Error('批次审批响应无效');
    plan.selection=result;
    return result;
  }finally{plan.controller=null;plan.awaitingApproval=false;refreshUploads();}
}
const folderDestinations=new Map(), acceptedBatches=new Set(), downloaded=new Set(),savedFiles=new Set(),writingFiles=new Set();
const folderDownloadControllers=new Map(),cancelledFolderBatches=new Set(),folderSaveErrors=new Map();let downloading=false;
async function chooseDestination(batch){
  if(!directoryReceiveAvailable())throw new Error('当前页面无法直接写入目录。保留文件夹结构接收需要使用 Windows 客户端，或支持目录写入的浏览器安全页面。');
  const handle=await window.showDirectoryPicker({mode:'readwrite'});
  cancelledFolderBatches.delete(batch);folderSaveErrors.delete(batch);
  for(const job of latestJobs)if(job.batchId===batch){downloaded.delete(job.id);savedFiles.delete(String(job.id));}
  folderDestinations.set(batch,handle);acceptedBatches.add(batch);
}
async function receiveApproved(){
  if(downloading)return;downloading=true;
  try{
    for(const job of latestJobs){
      if(!job.receiving||!acceptedBatches.has(job.batchId)||downloaded.has(job.id)||!inbox.some(file=>String(file.id)===String(job.id)))continue;
      if(!job.folderId)continue; // 单文件由用户在状态视图中发起下载。
      if(job.state>=4)continue;
      downloaded.add(job.id);
      writingFiles.add(String(job.id));
      const controller=new AbortController();folderDownloadControllers.set(String(job.id),controller);
      try{
        if(job.folderId){
          let directory=folderDestinations.get(job.batchId);if(!directory){downloaded.delete(job.id);continue;}
          const parts=job.name.replaceAll('\\','/').split('/');
          for(const name of parts.slice(0,-1))directory=await directory.getDirectoryHandle(name,{create:true});
          let filename=parts.at(-1), suffix=0; const dot=filename.lastIndexOf('.'), stem=dot>0?filename.slice(0,dot):filename, extension=dot>0?filename.slice(dot):'';
          for(;;){try{await directory.getFileHandle(filename);filename=stem+' ('+(++suffix)+')'+extension;}catch(error){if(error.name==='NotFoundError')break;throw error;}}
          const file=await directory.getFileHandle(filename,{create:true});const writable=await file.createWritable();
          try{const response=await fetch(serverSession.download(job.id,token),{signal:controller.signal});if(!response.ok)throw new Error('文件夹接收失败');await response.body.pipeTo(writable,{signal:controller.signal});}catch(error){await writable.abort().catch(()=>{});throw error;}
          savedFiles.add(String(job.id));
        }else{
          const link=document.createElement('a');link.href=serverSession.download(job.id,token);link.download=job.name;document.body.append(link);link.click();link.remove();
        }
      }catch(error){if(error.name!=='AbortError')folderSaveErrors.set(job.batchId,error.message);notice(error.name==='AbortError'?'目录保存已取消':error.message);downloaded.delete(job.id);acceptedBatches.delete(job.batchId);folderDestinations.delete(job.batchId);}
      finally{writingFiles.delete(String(job.id));folderDownloadControllers.delete(String(job.id));renderJobs();}
    }
  }finally{downloading=false;}
}
const approvalPanel=createApprovalPanel($('requests'),{bytes,glyph:icon,available:item=>!item.folder||directoryReceiveAvailable(),notice,
  async decide(request,bits){
    if(request.items.some((item,index)=>item.folder&&bits[index]==='1'))await chooseDestination(request.batch);
    await command('/web/api/decision/'+request.id,{},bits);
    if(bits.includes('1'))acceptedBatches.add(request.batch);
    await poll();
  }
});
function renderRequests(requests){currentRequests=requests;approvalPanel.update(requests,online);}

$('limits').onclick=()=>{$('limit-files').value=limits.files;$('limit-mib').value=String(BigInt(limits.bytes)/1048576n);$('limits-error').textContent='';$('limits-dialog').showModal();};
$('limits-form').onsubmit=async event=>{
  if(event.submitter.value!=='save')return;event.preventDefault();
  try{const next={files:limitValue($('limit-files').value),bytes:limitValue($('limit-mib').value,1048576n)};await command('/web/api/preferences',{'X-Chuan-Max-Files':next.files,'X-Chuan-Max-Bytes':next.bytes});limits=next;try{serverSession.write('limits',JSON.stringify(limits));}catch{}$('limits-dialog').close();notice('传输上限已保存',3000);}catch(error){$('limits-error').textContent=error.message;}
};
for(const id of ['picker','folder-picker']){
  $(id).onchange=e=>{try{enqueue(selectedFiles(e.target.files,id==='folder-picker'),e.target.dataset.retryTarget||chosen);}catch(error){notice(error.message);}finally{e.target.value='';delete e.target.dataset.retryTarget;}};
  $(id).oncancel=e=>{delete e.target.dataset.retryTarget;};
}
$('choose-files').onclick=()=>{delete $('picker').dataset.retryTarget;$('picker').click();};
$('choose-folder').onclick=()=>{
  if(!('webkitdirectory' in $('folder-picker'))){notice('当前浏览器不支持文件夹选择。请将文件夹压缩后通过“选择文件”发送。');return;}
  delete $('folder-picker').dataset.retryTarget;$('folder-picker').click();
};
function updateTarget(){
  $('receive-mode').textContent=online&&chosen===hostId?(hostAutoReceive?'自动接收：开':'接收方确认后传输'):'';
  const peer=peers.find(item=>item.id===chosen),enabled=online&&!!peer;
  $('ping').disabled=!enabled;$('note').disabled=!enabled;$('picker').disabled=!enabled;
  $('drop').classList.toggle('disabled',!enabled);$('drop').setAttribute('aria-disabled',String(!enabled));$('drop').tabIndex=enabled?0:-1;
  $('choose-files').disabled=!enabled;
  const folderSelectable='webkitdirectory' in $('folder-picker');
  $('folder-picker').disabled=!enabled||!folderSelectable;$('choose-folder').disabled=!enabled||!folderSelectable;
  $('folder-picker-help').hidden=folderSelectable;$('folder-picker-help').textContent='当前文件选择器不支持文件夹，请压缩后发送。';
  $('drop-title').textContent=enabled?(compactView()?'选择要发送的文件':'拖入文件或文件夹'):'请先选择目标设备';
  $('drop-detail').textContent=enabled?'发送至 '+peer.name:'选择在线设备后启用发送';
  $('target-name').textContent=peer?peer.name+' · '+deviceLabel(peer):'请选择设备';$('target-ip').textContent=peer?.ip||'';
  $('target-glyph').setAttribute('href','#i-'+(peer?.browser?'browser':'computer'));
  updateViews();
}

$('ping').onclick=async()=>{const target=chosen;$('ping').disabled=true;try{await command('/web/api/ping',{'X-Chuan-Peer':encode(target)});notice('Ping 已送达',3000);}catch(e){notice(e.message,5000);}finally{updateTarget();}};
$('note').onclick=async()=>{const peer=peers.find(p=>p.id===chosen);if(!peer)return;const note=await ask('IP 临时备注','IP '+peer.ip+'\n备注仅在当前客户端可见。',peer.note||'');if(note===null)return;try{await command('/web/api/note',{'X-Chuan-Address':peer.ip,'X-Chuan-Note':encode(note.trim())});await poll();}catch(e){notice(e.message);}};
$('clear').onclick=async()=>{try{await command('/web/api/clear');for(const op of operations)if(!outgoingActive(op))op.hidden=true;await poll();}catch(e){notice(e.message);}};
$('refresh').onclick=async()=>{try{await command('/web/api/refresh');await poll();}catch(error){notice(error.message);}};
$('drop').onclick=event=>{if(event.target.closest('button'))return;if(!$('picker').disabled)$('picker').click();};
$('drop').onkeydown=e=>{if(e.target!==$('drop'))return;if((e.key==='Enter'||e.key===' ')&&!$('picker').disabled){e.preventDefault();$('picker').click();}};
$('drop').ondragover=e=>{e.preventDefault();e.dataTransfer.dropEffect=$('picker').disabled?'none':'copy';if(!$('picker').disabled)$('drop').classList.add('dragover');};
$('drop').ondragleave=()=>$('drop').classList.remove('dragover');
$('drop').ondrop=async e=>{e.preventDefault();$('drop').classList.remove('dragover');if($('picker').disabled)return;const target=chosen;try{notice('正在读取文件夹…');enqueue(await droppedFiles(e.dataTransfer),target);}catch(error){notice('读取失败：'+error.message);}};
window.addEventListener('pagehide',event=>{if(event.persisted)return;if(token&&!serverSession.blocked)fetch('/web/api/leave',{method:'POST',keepalive:true,headers:{...serverSession.headers(),'X-Chuan-Token':token,'X-Chuan-Client':client,'X-Chuan-Tab':tab}}).catch(()=>{});});
window.addEventListener('pageshow',()=>poll());
document.addEventListener('visibilitychange',()=>{if(!document.hidden){flashEffect.resume();poll();}});
window.addEventListener('online',()=>poll());

$('switch-device').onclick=openDeviceSheet;
$('device-backdrop').onclick=()=>{closeDeviceSheet();$('switch-device').focus();};
$('close-device-sheet').onclick=()=>{closeDeviceSheet();$('switch-device').focus();};
document.querySelectorAll('[data-job-filter]').forEach(button=>button.onclick=()=>{jobFilter=button.dataset.jobFilter;document.querySelectorAll('[data-job-filter]').forEach(item=>{item.classList.toggle('active',item===button);item.setAttribute('aria-pressed',String(item===button));});renderJobs();});
window.addEventListener('resize',()=>{if(!compactView())closeDeviceSheet();updateTarget();renderJobs();});
document.addEventListener('keydown',event=>{
  if(!$('peer-sidebar').classList.contains('sheet-open'))return;
  if(event.key==='Escape'){event.preventDefault();closeDeviceSheet();$('switch-device').focus();}
  if(event.key==='Tab'){
    const controls=[...$('peer-sidebar').querySelectorAll('button:not(:disabled)')].filter(control=>control.getClientRects().length);
    if(!controls.length)return;const first=controls[0],last=controls.at(-1);
    if(event.shiftKey&&document.activeElement===first){event.preventDefault();last.focus();}
    else if(!event.shiftKey&&document.activeElement===last){event.preventDefault();first.focus();}
  }
});
updateViews();

poll();setInterval(poll,2000);
