import {createApprovalPanel} from '/web/approvals.js';
import {Sha256} from '/web/sha256.js';
import {hashFile} from '/web/hash_file.js';
import {selectedFiles,droppedFiles,batchPlan,limitValue} from '/web/files.js';
import {transferGroups,pingEffect,timedNotice,folderProgress,createFolderDisclosure,transferAction,currentOperation,createFilePipeline,createDiagnostics} from '/web/transfers.js';
import {ServerSession,deviceLabel,browserDeviceType} from '/web/session.js';
import {archiveSelection,packZip,streamZip} from '/web/zip_store.js';
const serverSession=new ServerSession();
const $ = id => document.getElementById(id);
const hex = bytes => Array.from(bytes,b=>b.toString(16).padStart(2,'0')).join('');
const encode = text => hex(new TextEncoder().encode(text));
const random = () => hex(crypto.getRandomValues(new Uint8Array(16)));
let client=random(), deviceName='';
const tab=random();
let chosen='', peers=[], latestJobs=[], inbox=[], pingAck=0, followPoll=false;
let token='', online=false, polling=false, queueRunning=false, actionMessage='', hostId='';
let webUploadChecksumOptional=false, webCopyCheck=false, webDownloadChecksum=false, webTransferWait=false, webBatchFiles=false, webUploadStatus=false, webZipArchives=false, webZipStream=false,webResume=false, webOfferAsync=false, hostAutoReceive=false;
let latestOffers=[],zipFolders=false;
const approvalWaiters=new Map();
let hostName='',jobFilter='all',currentRequests=[];
const queue=[], operations=[], signatures=new Map();
const trace=createDiagnostics(()=>({client_id:client,host_id:hostId,server_instance:serverSession.instance||''}));
const pipelineTrace=direction=>(phase,item,state)=>trace.write('pipeline',{direction,phase,...state,upload_id:item.key||item.uploadKey||'',batch_id:item.plan?.id||item.batchId||'',path:item.path||item.name||''});
const retrying=new Set();
const batchReports=new Set();let reportingBatch=false;
const jobRows=new Map();
let uploadFrame=0;
let sourceReselection=null;
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
  if(op.retryPending&&!op.cancelled&&!op.rejected){state=1;status="独立通道自动重试中";}
  const paused=op.paused||op.remotePaused;if(paused){state=1;status=op.remotePaused?'对方已暂停，已接收数据已保留':'已暂停，已接收数据已保留';}
  const metrics=stage==='forward'?forward:source;
  const done=op.packing?op.packDone||0:state===3?op.file.size:metrics?Math.max(metrics.done||0,op.remoteDone||0):op.remoteDone??(stage==='upload'?op.uploadDone||0:0);
  return {id:source?.id??op.key,name:op.path,total:op.packing?op.archive.total:op.file.size,done,
    speed:paused?0:metrics?.speed??(stage==='upload'?op.speed||0:0),paused:!!paused,state,stateText:status,peer:peerNames.get(op.target)||'目标设备',receiving:false,op,directory:!!op.directory,skipped:!!op.skipped,
    error:op.error?op.status:forward?.error||source?.error||'',activity:stage==='forward'?forward?.activity||'':!op.finished&&!awaitingApproval?op.status:'',uploadKey:source?.uploadKey,stage,awaitingApproval,canDelete:!!source?.canDelete||!!forward?.canDelete,folderId:op.batch?.id,folderName:op.batch?.name,folderCount:op.batch?.count,folderTotal:op.batch?.total};
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
function showNotice(message) {
  $('connection').textContent=message;$('connection').hidden=!message;
  if(serverSession.blocked)$('connection').append(button('重新连接',()=>location.reload()));
  else if(message&&sourceReselection){
    const {target,folder}=sourceReselection;
    $('connection').append(button(folder?'重新选择文件夹':'重新选择文件',event=>{
      if(!event.isTrusted)return;
      if(!online||!peers.some(peer=>peer.id===target)){notice('原目标设备已离线，请在设备重新上线后重试。');return;}
      chosen=target;renderDevices();updateTarget();
      const picker=$(folder?'folder-picker':'picker');picker.dataset.retryTarget=target;picker.click();
    }));
  }
}
const notice=timedNotice(message=>{sourceReselection=null;actionMessage=message;showNotice(message);});
function requestSourceReselection(target,folder,message){
  notice(message);sourceReselection={target,folder};showNotice(message);
}
function row(name,detail) {
  const r=text('div','','row'), d=text('div','','details'),heading=text('div','','job-title');
  heading.append(text('span','','direction'),text('strong',name,'name'),text('span','','job-status'));
  d.append(heading,text('div',detail,'detail'));
  const a=text('div','','actions');r.append(icon('file'),d,a);return {r,d,a};
}
async function command(path,extra={},body=null,signal=undefined) {
  serverSession.assert();
  const started=performance.now();let status=0;
  try{
  const response=await fetch(path,{method:'POST',headers:{...serverSession.headers(),'X-Chuan-Token':token,'X-Chuan-Client':client,'X-Chuan-Tab':tab,...extra},body,signal});
  status=response.status;const message=await response.text();if(!response.ok){const error=new Error(message||`请求失败 (${response.status})`);error.receiverCancelled=response.status===410;throw error;}
  return message;
  }finally{trace.write('request',{path,status,elapsed_ms:performance.now()-started});}
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
  items=items.filter(item=>item.state>=4);
  if(!items.length)return;
  const originals=items.map(item=>item.op).filter(Boolean),target=originals[0]?.target||items[0]?.target;
  folder=folder||originals.some(op=>op.archive);
  if(!online){notice('服务主机未连接，请连接后重试。');return;}
  if(!target||!peers.some(peer=>peer.id===target)){notice('原目标设备已离线，请在设备重新上线后重试。');return;}
  const retryKey=JSON.stringify(items.map(item=>String(item.id)));
  if(retrying.has(retryKey))return;retrying.add(retryKey);
  try{
  if(originals.length===items.length&&originals.some(op=>op.archive)){
    const sources=originals.flatMap(op=>op.archive?op.archive.sources:[{file:op.file,path:op.path}]);
    for(const source of sources)try{await source.file.slice(0,Math.min(1,source.file.size)).arrayBuffer();}catch{requestSourceReselection(target,true,'原文件已变化或无法读取，请重新选择文件夹后重试。');return;}
    enqueue(sources,target);
    renderJobs();return;
  }
  if(originals.length===items.length&&originals.every(op=>op.file instanceof File)){
    try{
      for(const op of originals)await op.file.slice(0,Math.min(1,op.file.size)).arrayBuffer();
      enqueue(originals.map(op=>({file:op.file,path:op.path,directory:op.directory})),target);
      renderJobs();return;
    }catch{requestSourceReselection(target,folder,'原文件已变化或无法读取，请重新选择后重试。');return;}
  }
  requestSourceReselection(target,folder,'浏览器已释放原文件，请重新选择后重试。');
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
    return {...job,activity:folderActivities.get(String(job.id))||job.activity,serverState:job.state,state:staging?1:receivingFolder&&saved?3:cancelled?4:failed?5:saving&&job.state===3&&!saved?(writingFiles.has(String(job.id))?2:1):job.state,
      done:staging||cancelled||failed?0:receivingFolder&&saved?job.total:saving&&job.state===3&&!saved&&!writingFiles.has(String(job.id))?0:job.done,speed:staging?0:job.speed,
      awaitingApproval:!job.receiving&&job.state===1&&!waitingForBrowser&&!job.activity&&!(job.done>0),
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
  async function pause(item,paused){if(item.receiving&&item.folderId){for(const member of latestJobs.filter(job=>job.receiving&&job.batchId===item.batchId&&job.folderId===item.folderId&&job.state!==3)){const id=String(member.id);if(paused){incomingPaused.add(id);const context=incomingContexts.get(id);if(context)context.pauseRequested=true;folderDownloadControllers.get(id)?.abort();}else{incomingPaused.delete(id);folderSaveErrors.delete(member.batchId);}}}if(item.op)await pauseOperation(item.op,paused);else await command('/web/api/'+(paused?'pause/':'resume-job/')+item.id);await poll();renderJobs();}
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
    if(active){if(!job.receiving||job.folderId){const toggle=button(job.paused?'继续':'暂停',()=>pause(job,!job.paused).catch(e=>notice(e.message)));toggle.className='pause';a.append(toggle);}a.append(button('取消',async()=>{try{await stop(job);await poll();renderJobs();}catch(e){notice(e.message);}}));}
    else {if(!job.receiving&&job.state>=4){const retry=button('重试',()=>retrySending([job]));retry.className='retry';a.append(retry);}a.append(button('删除',async()=>{
      if(job.canDelete&&!await ask('删除传输记录','将删除此记录及对应服务端副本。浏览器电脑上的原文件不受影响。\n\n'+job.name))return;
      try{await remove(job);await poll();renderJobs();}catch(e){notice(e.message);}
    }));}
    return {r,update(next){
      job=next;const toggle=a.querySelector('.pause');if(toggle)toggle.textContent=job.paused?'继续':'暂停';r.classList.toggle('lifecycle-active',job.state<3);r.classList.toggle('lifecycle-ended',job.state>=3);const percent=job.total?Math.min(100,job.done*100/job.total):0;
      setText(d.querySelector('.name'),job.name);
      const ready=job.receiving&&file&&!file.received&&job.state!==2;
      const status=job.receiving&&file?(file.received?'已交给浏览器，请核对下载结果':job.state===2?'正在交给浏览器':'可下载'):transferAction(job);
      setText(detail,(job.receiving?'来自 ':'目标：')+job.peer+' · '+bytes(job.total)+(job.error?' · '+job.error:''));
      setText(statusLabel,(job.state<3?'进行中 · ':'已结束 · ')+status);statusLabel.classList.toggle('green',job.state===3);
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
    const {r,d,a}=row('',''),tree=createFolderDisclosure(group,bytes,(slot,file)=>{const signature=String(file.id)+":"+file.state;if(slot.dataset.signature===signature)return;slot.dataset.signature=signature;slot.replaceChildren();if(!file.receiving&&file.state===5)slot.append(button("重试此文件",()=>retrySending([file])));}),name=tree.button,detail=d.querySelector('.detail'),statusLabel=d.querySelector('.job-status');
    d.querySelector('.name').replaceWith(name);
    d.querySelector('.direction').textContent=group.receiving?'接收':'发送';
    r.classList.add('folder-row');r.firstChild.replaceWith(icon('folder'));
    const available=group.items.some(item=>filesById.has(String(item.id)));
    if(group.receiving&&available&&!folderDestinations.has(group.items[0].batchId))a.append(button('保存文件夹',async()=>{
      try{await chooseDestination(group.items[0].batchId);await receiveApproved();}catch(error){notice(error.message);}
    }));
    const progress=document.createElement('progress'),metrics=text('div','','job-progress'),percentLabel=text('small','','progress-metrics');
    progress.max=100;progress.setAttribute('aria-label','文件夹传输进度');metrics.append(progress,percentLabel);d.append(metrics);
    const currentLine=text('div','','current-operation');d.append(currentLine);
    if(group.state<3){const toggle=button(group.paused?'继续':'暂停',async()=>{try{const active=group.items.filter(item=>item.state<3);for(const item of active.filter(item=>item.op))await pauseOperation(item.op,!group.paused);const server=active.find(item=>!item.op);if(server)await pause(server,!group.paused);await poll();renderJobs();}catch(e){notice(e.message);}});toggle.className='pause';a.append(toggle);a.append(button('取消',async()=>{try{for(const item of group.items)if(item.state<3)await stop(item);await poll();renderJobs();}catch(e){notice(e.message);}}));}
    else {if(!group.receiving&&group.state>=4){const retry=button('重试',()=>retrySending(group.items,true));retry.className='retry';a.append(retry);}a.append(button('删除',async()=>{
      if(!await ask('删除文件夹批次','删除此批次的记录及可删除的服务端副本。浏览器电脑上的原文件不受影响。\n\n'+group.name))return;
      try{for(const item of group.items)await remove(item);await poll();renderJobs();}catch(e){notice(e.message);}
    }));}
    section.append(r);
    section.append(tree.element);
    return {r:section,update(next){
      group=next;const toggle=a.querySelector('.pause');if(toggle)toggle.textContent=group.paused?'继续':'暂停';section.classList.toggle('lifecycle-active',group.state<3);section.classList.toggle('lifecycle-ended',group.state>=3);
      setText(name,group.name);
      const saved=group.receiving&&group.items.every(item=>savedFiles.has(String(item.id)))&&group.completed>=group.count;
      const status=group.receiving?(saved?'已保存到所选目录':group.state===4?'目录保存已取消':group.state===5?'目录保存失败，可重新选择目录':group.state===3?'已交给浏览器，请核对本地目录':folderDestinations.has(group.items[0].batchId)?'保存至所选目录中':'等待选择保存目录'):group.stateText;
      setText(detail,(group.receiving?'来自 ':'目标：')+group.peer+' · '+folderProgress(group,bytes)+(group.error?'\n'+group.error:''));
      setText(statusLabel,(group.state<3?'进行中 · ':'已结束 · ')+(group.state<3&&(!group.receiving||folderDestinations.has(group.items[0].batchId))?group.currentAction:status));statusLabel.classList.toggle('green',saved||(!group.receiving&&group.state===3));
      const needsDirectory=group.receiving&&group.state<3&&!folderDestinations.has(group.items[0].batchId);
      setText(currentLine,needsDirectory?'请选择保存目录以继续接收':currentOperation(group,bytes));currentLine.hidden=group.state>=3;currentLine.title=currentLine.textContent;
      statusLabel.classList.toggle('waiting',!!group.awaitingApproval);
      metrics.hidden=!!group.awaitingApproval;setText(percentLabel,(group.done>0&&group.percent===0?'<0.1':group.percent)+'%'+(group.currentTransferring?'  '+bytes(Math.round(group.speed))+'/s':''));
      if(progress.value!==group.percent)progress.value=group.percent;
      const save=[...a.querySelectorAll('button')].find(button=>button.textContent==='保存文件夹');if(save){save.disabled=!online||!directoryReceiveAvailable();save.title=directoryReceiveAvailable()?'':'此页面不支持目录写入，请使用 Windows 客户端或先压缩后发送。';}
      const retry=a.querySelector('.retry');if(retry)retry.disabled=!online;
      tree.update(group);
    }};
  }
  trace.observe([...jobs,...local]);
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
  op.paused=false;op.remotePaused=false;op.pauseAbort=false;op.cancelled=true;op.status='已取消';const activeRequest=op.xhr;refreshUploads();
  clearTimeout(op.retryTimer);op.retryPending=false;
  op.packController?.abort();op.hashController?.abort();
  op.waitController?.abort();
  if(op.batch)for(const queued of queue)if(queued.batch===op.batch){queued.cancelled=true;queued.finished=true;queued.status='文件夹批次已取消';}
  if(activeRequest) {
    // Cancel the receive task as well when a manual confirmation holds its socket.
    try {await command(`/web/api/cancel/${op.key}`);} catch {}
    activeRequest.abort();
  }
  if(op.forwardId)try{await command('/web/api/cancel/'+op.forwardId);}catch{}
  if(op.plan?.requestId)await cancelPendingItems(op.plan);
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
function enqueue(files,target=chosen,forceZip=false) {
  if(!online){notice('Web服务未连接，无法发送文件。');return;}
  if(!target||!peers.some(peer=>peer.id===target)){notice('请选择在线的目标设备。');return;}
  if(!files.length){notice('未找到可发送文件；空文件夹不会传输。');return;}
  const destination=peers.find(peer=>peer.id===target);
  if(!zipFolders&&!forceZip&&destination?.browser&&!destination.directoryReceive&&files.some(item=>item.directory||item.path?.includes('/'))){
    const dialog=document.createElement('dialog'),title=text('h2','接收端无法接收文件夹'),description=text('p','目标 Web 页面不支持直接保存目录。建议打包为 ZIP 后发送；此操作仅对本次传输生效。');
    dialog.append(title,description,button('取消',()=>dialog.close()),button('打包传输',()=>{dialog.close();enqueue(files,target,true);}));dialog.addEventListener('close',()=>dialog.remove(),{once:true});document.body.append(dialog);dialog.showModal();return;
  }
  notice('');
  let plan;
  try{batchPlan(files,random,minimumLimit(limits.files,hostLimits.files),minimumLimit(limits.bytes,hostLimits.bytes));plan=batchPlan(zipFolders||forceZip?archiveSelection(files):files,random,'0',minimumLimit(limits.bytes,hostLimits.bytes));}catch(error){notice(error.message);return;}
  plan.target=target;
  for(const {file,path,batch,item,archive,directory} of plan.entries)if(file instanceof File||archive) {
    const op={file,path,batch,plan,item,archive,directory,target,key:random(),status:'等待发送',percent:0,cancelled:false,finished:false};operations.push(op);queue.push(op);
  }
  preparePlan(plan);refreshUploads();runQueue();return true;
}
function stopOutgoingFolder(op,message,rejected=false){
  const related=op.batch?operations.filter(item=>item.batch===op.batch):[op];
  for(const item of related)if((!item.fileId&&!item.finished)||item===op){
    item.error=false;item.cancelled=!rejected;item.rejected=rejected;item.status=message;item.finished=true;
    clearTimeout(item.retryTimer);item.retryPending=false;
    item.xhr?.abort();item.waitController?.abort();item.packController?.abort();item.hashController?.abort();
  }
}
function failOutgoing(op,message){
  if(op.finished||op.cancelled||op.rejected||op.retryPending)return;
  op.error=true;op.finished=true;op.status='发送失败：'+message;
  op.packController?.abort();op.hashController?.abort();op.xhr?.abort();op.waitController?.abort();
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
  if(result?.paused){op.remotePaused=true;error.remotePaused=true;return false;}
  if(result?.state===3){op.fileId=String(result.id);try{await deliverUpload(op);op.error=false;op.percent=100;return true;}catch(failure){error.message=failure.message;return false;}}
  if(result?.state===4||result?.state===6){stopOutgoingFolder(op,result.error||(result.state===4?'接收端已取消本次传输':'接收端已拒绝本次传输'),result.state===6);return true;}
  if(result?.state===5&&result.error){error.message=result.error;if(result.error.includes("续传内容不一致"))op.resumeReset=true;}
  return false;
}
async function deliverUpload(op){
  if(op.streamForwarded){op.status='已完成';return;}
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
  const registrations=op.plan.registrations??=new Map();
  if(!registrations.has(op.item))registrations.set(op.item,registerOperationOnce(op).catch(error=>{registrations.delete(op.item);throw error;}));
  await registrations.get(op.item);
}
async function registerOperationOnce(op){
  if(op.registered||!webBatchFiles||op.target!==hostId||!op.plan.items.some(item=>item.folder)&&!op.plan.entries.some(entry=>entry.archive))return;
  const ops=operations.filter(item=>item.plan===op.plan&&item.item===op.item&&!item.registered);
  const body=ops.map(item=>item.item+'\t'+item.key+'\t'+(item.batch?.id||'')+'\t'+encode(item.path)+'\t'+item.file.size+(item.archive&&webZipArchives?'\tA':item.directory?'\tD':'')+'\n').join('');
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
      sendPipeline.push(op);
    }
  }finally{queueRunning=false;}
}
const retryQueue=[];let retryLaneRunning=false;
function scheduleRetry(op){
  if(op.retryPending||op.paused)return;
  if(/同名文件|类型冲突|原文件已保留|源文件|无法读取源文件/.test(op.status)){op.finished=true;return;}
  op.retryPending=true;op.finished=false;op.error=false;op.retryDeadline=Date.now()+30000;op.retryStarted=Date.now();op.retryActivity=Date.now();op.status='独立通道自动重试中';retryQueue.push(op);
  if(!op.remotePaused)op.retryTimer=setTimeout(function checkRetry(){if(op.paused||op.remotePaused)return;const idle=Date.now()-(op.retryActivity||op.retryStarted||0);if(op.processing&&idle<30000){op.retryTimer=setTimeout(checkRetry,Math.max(250,30000-idle));return;}op.retryExpired=true;op.retryPending=false;op.finished=true;op.error=true;op.status='传输失败超时（自动重试已达 30 秒）';op.xhr?.abort();op.waitController?.abort();op.packController?.abort();op.hashController?.abort();if(op.plan?.registered){batchReports.add(op.plan);flushBatchReports();}refreshUploads();},30000);
  setTimeout(runRetryLane,0);
}
async function runRetryLane(){
  if(retryLaneRunning)return;retryLaneRunning=true;
  try{while(retryQueue.length){
    const op=retryQueue.shift();
    if(op.paused){op.retryPending=false;continue;}if(op.cancelled||op.rejected||op.retryExpired){clearTimeout(op.retryTimer);op.retryPending=false;continue;}
    try{
      op.waitController=new AbortController();
      const state=await command('/web/api/retry-upload/'+op.key,{'X-Chuan-Batch':op.plan.id},null,op.waitController.signal);op.waitController=null;
      if(state==='3'&&await confirmUpload(op,{})){op.retryPending=false;op.finished=true;}
      else{op.error=false;await processOperation(op);if(!op.error&&!op.retryExpired&&!op.cancelled){op.retryPending=false;op.finished=true;}}
    }catch(error){op.error=true;}
    if(op.retryPending&&!op.cancelled&&!op.rejected&&!op.retryExpired){op.error=false;op.status='独立通道自动重试中';await new Promise(resolve=>setTimeout(resolve,400));retryQueue.push(op);}
    else{clearTimeout(op.retryTimer);op.retryPending=false;op.finished=true;refreshUploads();if(op.plan?.registered){batchReports.add(op.plan);await flushBatchReports();}}
  }}finally{retryLaneRunning=false;}
}
const sendPipeline=createFilePipeline(op=>processOperation(op,'probe'),op=>processOperation(op,'transfer'),(error,op)=>{trace.write('pipeline_error',{path:op.path,error:error.message});},pipelineTrace('send'),op=>processOperation(op,'verify'));
async function resumeFingerprint(op){
  if(op.resumeKey)return op.resumeKey;const hash=new Sha256();
  if(op.streamingZip){for(const entry of op.archive.entries)hash.update(new TextEncoder().encode(entry.path+':'+entry.size+'\n'));}
  else hash.update(new TextEncoder().encode('chuan-resume-v1:'+op.file.size));
  return op.resumeKey=hex(hash.digest());
}
async function pauseOperation(op,paused){
  if(paused){op.paused=true;op.pauseAbort=true;op.status='已暂停，已接收数据已保留';op.finished=false;clearTimeout(op.retryTimer);op.retryPending=false;
    await command('/web/api/pause/'+(op.forwardId||op.key),{}).catch(()=>{});op.xhr?.abort();op.packController?.abort();op.hashController?.abort();op.waitController?.abort();
  }else{await command('/web/api/resume-job/'+(op.forwardId||op.key),{}).catch(()=>{});await command('/web/api/retry-upload/'+op.key,{'X-Chuan-Batch':op.plan.id},null).catch(()=>{});op.paused=false;op.remotePaused=false;op.error=false;op.retryExpired=false;op.status='准备断点续传';op.resumeRequested=true;if(!op.processing){op.pauseAbort=false;op.resumeRequested=false;sendPipeline.push(op);}}
  refreshUploads();
}
async function processOperation(op,stage='all'){
  if(op.processing||op.paused)return false;op.processing=true;
  do {
      try {
        if(stage!=='transfer'){
        op.preparationDeferred=false;
        const defer=()=>{op.preparationDeferred=true;op.status=op.archive&&!op.packed?'等待打包文件':'等待核对同名文件';refreshUploads();return 'verify';};
        if(op.plan.failure)throw op.plan.failure;
        const selection=op.plan.selection||await op.plan.promise;
        if(selection[op.item]!=='1'){op.status='接收方未选择此项';op.rejected=true;continue;}
        if(op.cancelled||op.retryExpired)continue;
        await registerOperation(op);
        op.streamingZip=!!op.archive&&webZipStream;
        if(op.archive&&!op.packed&&!op.streamingZip){
          if(stage==='probe')return defer();
          op.packing=true;op.packDone=0;op.packComplete=0;op.status='正在打包 ZIP';op.packController=new AbortController();refreshUploads();
          if(op.registered&&webCopyCheck)await command('/web/api/file-phase/'+op.key,{'X-Chuan-Batch':op.plan.id},'pack');
          let watching=false,checkedAt=0;
          const check=async()=>{if(watching||!op.registered||Date.now()-checkedAt<900)return;watching=true;checkedAt=Date.now();try{const remote=await uploadResult(op);if(remote?.state===4||remote?.state===6)stopOutgoingFolder(op,remote.error||'接收端已取消本次传输',remote.state===6);else if(remote?.state===5)failOutgoing(op,remote.error||'接收端未能继续接收');}finally{watching=false;}};
          const timer=setInterval(check,1000);
          try{const packed=await packZip(op.archive,progress=>{op.packDone=progress.done;op.packComplete=progress.complete;op.packPath=progress.path;refreshUploads();check();},op.packController.signal);op.file=packed.file;op.digest=packed.digest;op.packed=true;await check();}
          finally{clearInterval(timer);op.packing=false;op.packController=null;}
        }
        if(op.cancelled||op.error||op.rejected)continue;
        const size=op.file.size;
        op.hashMs=0;
        const ensureDigest=async()=>{
          if(op.digest)return;
          op.status='核对同名文件';refreshUploads();
          const hashStarted=performance.now();op.hashController=new AbortController();let watching=false;
          const check=async()=>{if(watching||!webUploadStatus||!op.registered)return;watching=true;try{const remote=await uploadResult(op);if(remote?.state===4||remote?.state===6)stopOutgoingFolder(op,remote.error||'接收端已终止本次传输',remote.state===6);}catch{}finally{watching=false;}};
          const timer=setInterval(check,2000);
          try{op.digest=await hashFile(op.file,op.hashController.signal);op.hashMs+=Math.round(performance.now()-hashStarted);}
          finally{clearInterval(timer);op.hashController=null;}
        };
        if(op.cancelled||op.retryExpired)continue;
        if(!online||!peers.some(p=>p.id===op.target)){
          const deadline=Date.now()+15000;
          while(!op.cancelled&&!serverSession.blocked&&Date.now()<deadline&&(!online||!peers.some(p=>p.id===op.target))){op.status='连接中断，正在重连';refreshUploads();await poll();if(online&&peers.some(p=>p.id===op.target))break;await new Promise(resolve=>setTimeout(resolve,500));}
        }
        if(op.cancelled||op.retryExpired)continue;
        if(!online||!peers.some(p=>p.id===op.target)){op.connectionLost=true;throw new Error('目标设备已离线，未完成文件可在重连后重试');}
        if(op.batch){
          op.status='检查接收位置';refreshUploads();
          const probe=async(check)=>{op.waitController=new AbortController();try{return JSON.parse(await command(check?'/web/api/copy-check':'/web/api/copy-probe',{
            'X-Chuan-Copy':'1','X-Chuan-Directory':op.directory?'1':'0','X-Chuan-Sha256':op.digest?hex(op.digest):'',
            'X-Chuan-Target':encode(op.target),'X-Chuan-Upload':op.key,'X-Chuan-Path':encode(op.path),'X-Chuan-Sender':encode(deviceName||'Web 浏览器'),
            'X-Chuan-Batch':op.plan.id,'X-Chuan-Item':String(op.item),'X-Chuan-Size':String(size),
            'X-Chuan-Folder':op.batch.id,'X-Chuan-Folder-Name':encode(op.batch.name),'X-Chuan-Folder-Count':String(op.batch.count),'X-Chuan-Folder-Size':op.batch.totalExact
          },null,op.waitController.signal));}finally{op.waitController=null;}};
          if(!webCopyCheck&&!op.directory){if(stage==='probe'&&!op.digest)return defer();await ensureDigest();}
          let result=stage==='verify'&&op.copyVerification?{verify:true}:await probe(webCopyCheck);
          if(result.verify){if(stage==='probe'){op.copyVerification=true;return defer();}await ensureDigest();op.status='等待接收端核对同名文件';refreshUploads();result=await probe(false);op.copyVerification=false;}
          if(result.skipped){op.fileId=result.id;op.skipped=!op.directory;await deliverUpload(op);op.status=op.directory?'目录已保存':'内容相同，已跳过';continue;}
        }
        if(!webUploadChecksumOptional&&!op.digest){if(stage==='probe')return defer();await ensureDigest();}
        if(stage==='probe'||stage==='verify'){op.prepared=true;op.status='已就绪，等待传输';refreshUploads();return true;}
        }
        if(op.cancelled||op.retryExpired||op.rejected||op.error)continue;
        op.prepared=false;
        const size=op.file.size,hash=null;
        op.status='发送中';refreshUploads();
        const resumeFields={};let resumeOffset=0;
        if(webResume&&!op.directory){
          op.streamingZip=!!op.archive&&webZipStream;
          const fields={'X-Chuan-Resume-Key':await resumeFingerprint(op),'X-Chuan-Resume-Reset':op.resumeReset?'1':'0','X-Chuan-Resume-Direct':op.streamingZip?'1':'0','X-Chuan-Target':encode(op.target),'X-Chuan-Upload':op.key,'X-Chuan-Path':encode(op.path),'X-Chuan-Batch':op.plan.id,'X-Chuan-Item':String(op.item),'X-Chuan-Size':String(size)};
          const candidate=JSON.parse(await command('/web/api/resume',fields,null));
          if(candidate.paused){clearTimeout(op.retryTimer);op.retryDeadline=Infinity;op.remotePaused=true;const error=Error('对方已暂停，已接收数据已保留');error.remotePaused=true;throw error;}op.remotePaused=false;
          resumeOffset=candidate.offset;if(resumeOffset)clearTimeout(op.retryTimer);if(!Number.isSafeInteger(resumeOffset)||resumeOffset<0||resumeOffset>size)throw Error('续传偏移无效');op.resumeReset=false;
          Object.assign(resumeFields,{'X-Chuan-Resume-Key':op.resumeKey,'X-Chuan-Offset':String(resumeOffset)});
          if(resumeOffset&&!op.streamingZip){clearTimeout(op.retryTimer);op.status='核验已接收部分，准备续传';op.hashController=new AbortController();refreshUploads();try{resumeFields['X-Chuan-Prefix']=hex(await hashFile(op.file.slice(0,resumeOffset),op.hashController.signal));}finally{op.hashController=null;}}
          op.uploadDone=resumeOffset;op.percent=Math.floor(resumeOffset*100/Math.max(1,size));refreshUploads();
        }
        if(op.paused||op.pauseAbort)continue;
        if(op.streamingZip){
          op.packController=new AbortController();op.xhr={abort:()=>op.packController?.abort()};let stream='',lastResult=null;
          try{
            const fields={...resumeFields,'X-Chuan-Target':encode(op.target),'X-Chuan-Upload':op.key,'X-Chuan-Path':encode(op.path),'X-Chuan-Sender':encode(deviceName||'Web 浏览器'),'X-Chuan-Batch':op.plan.id,'X-Chuan-Item':String(op.item),'X-Chuan-Size':String(size)};
            const open=async()=>{const opened=JSON.parse(await command('/web/api/archive/open',fields,null,op.packController.signal));stream=opened.stream;op.streamForwarded=opened.forwarded===true;if(opened.id)op.forwardId=opened.id;};
            const prefixHash=new Sha256();let skipped=0;if(!resumeOffset)await open();
            let sent=0;op.status='边打包边发送 ZIP';op.hashMs=0;refreshUploads();
            await streamZip(op.archive,async bytes=>{
              if(op.cancelled||op.retryExpired)throw new DOMException('已取消','AbortError');
              if(skipped<resumeOffset){const take=Math.min(bytes.length,resumeOffset-skipped);prefixHash.update(bytes.subarray(0,take));skipped+=take;bytes=bytes.subarray(take);if(skipped===resumeOffset){fields['X-Chuan-Prefix']=hex(prefixHash.digest());await open();}if(!bytes.length)return;}
              lastResult=JSON.parse(await command('/web/api/archive/chunk/'+stream,{'X-Chuan-Offset':String(sent),'Content-Type':'application/octet-stream'},bytes,op.packController.signal));sent+=bytes.length;op.retryActivity=Date.now();
              const now=performance.now(),done=Math.min(size,resumeOffset+sent);op.speed=op.progressAt?(done-(op.uploadDone||0))*1000/Math.max(1,now-op.progressAt):0;op.progressAt=now;op.uploadDone=done;op.percent=Math.floor(done*100/Math.max(1,size));refreshUploads();
            },op.packController.signal);
            op.fileId=lastResult.id;trace.write('archive_stream_end',{path:op.path,wire_bytes:sent,forwarded:op.streamForwarded});
          }catch(error){if(String(error.message).includes('续传内容不一致'))op.resumeReset=true;if(stream)await command('/web/api/archive/abort/'+stream,{'X-Chuan-Abort-Failed':op.cancelled&&!op.paused?'0':'1','X-Chuan-Error':encode(String(error.message||'ZIP 流中断').slice(0,512)),'X-Chuan-Paused':op.paused?'1':'0'},null).catch(()=>{});throw error;}
          finally{op.packController=null;op.xhr=null;}
        }else{
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
          for(const [key,value] of Object.entries({...serverSession.headers(),...resumeFields,'Content-Type':'application/octet-stream','X-Chuan-Token':token,'X-Chuan-Client':client,'X-Chuan-Tab':tab,'X-Chuan-Target':encode(op.target),'X-Chuan-Upload':op.key,'X-Chuan-Path':encode(op.path),'X-Chuan-Sender':encode(deviceName||'Web 浏览器'),'X-Chuan-Batch':op.plan.id,'X-Chuan-Item':String(op.item),'X-Chuan-Size':String(size)}))xhr.setRequestHeader(key,value);
          if(op.batch)for(const [key,value] of Object.entries({'X-Chuan-Copy':'1','X-Chuan-Folder':op.batch.id,'X-Chuan-Folder-Name':encode(op.batch.name),'X-Chuan-Folder-Count':String(op.batch.count),'X-Chuan-Folder-Size':op.batch.totalExact}))xhr.setRequestHeader(key,value);
          xhr.timeout=0; // Transfer lifetime is bounded by transport activity, not file size.
          xhr.upload.onprogress=e=>{if(op.cancelled)return;op.retryActivity=Date.now();const now=performance.now(),done=Math.min(size,resumeOffset+e.loaded);op.speed=op.progressAt?(done-(op.uploadDone||0))*1000/Math.max(1,now-op.progressAt):0;op.progressAt=now;op.uploadDone=done;op.percent=e.lengthComputable?Math.floor((resumeOffset+e.loaded)*100/Math.max(1,size)):0;op.status=op.percent===100?'等待接收确认':'发送中';refreshUploads();};
          xhr.onload=()=>{if(xhr.responseText.includes('续传内容不一致'))op.resumeReset=true;if(xhr.status===201){op.fileId=JSON.parse(xhr.responseText).id;resolve();}else {const error=new Error(xhr.responseText||`发送失败 (${xhr.status})`);error.receiverCancelled=xhr.status===410;reject(error);}};
          xhr.onerror=async()=>{try{await op.file.slice(0,Math.min(1,size)).arrayBuffer();const error=new Error('网络连接中断，未收到接收端保存确认');error.transportError=true;reject(error);}catch{const error=new Error('无法读取源文件，文件可能已移动、被占用或已变更');error.sourceError=true;reject(error);}};xhr.ontimeout=()=>reject(new Error('请求超时'));xhr.onabort=()=>reject(new Error('已取消'));
          xhr.send(hash||op.digest?new Blob([op.file.slice(resumeOffset),op.digest||hash.digest()],{type:'application/octet-stream'}):op.file.slice(resumeOffset));
        });
        }
        if(op.cancelled||op.retryExpired)continue;
        await deliverUpload(op);
        op.percent=100;
        if(op.batch)op.batch.networkFailures=0;
      }catch(error){
        if(!op.paused&&!op.pauseAbort&&!op.cancelled&&!op.rejected&&!await confirmUpload(op,error)){
          op.error=true;op.status='发送失败：'+error.message;
          if(!serverSession.blocked&&!op.retryExpired)scheduleRetry(op);
        }
      }finally{
        op.processing=false;
        if(op.resumeRequested&&!op.paused){op.resumeRequested=false;op.pauseAbort=false;sendPipeline.push(op);}
        if((stage==='probe'||stage==='verify')&&(op.prepared||op.preparationDeferred)&&!op.cancelled&&!op.error&&!op.rejected){refreshUploads();}
        else{
        op.finished=!op.retryPending&&!op.paused;op.xhr=null;
        if(op.retryExpired&&!op.cancelled){op.error=true;op.status='传输失败超时（自动重试已达 30 秒）';}
        if(op.finished&&op.plan?.registered){batchReports.add(op.plan);await flushBatchReports();}refreshUploads();poll();
        }
      }
  }while(false);
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
    if(!serverSession.host){await serverSession.initialize(controller.signal);client=serverSession.client;try{const saved=JSON.parse(serverSession.read('limits')||'null');if(saved)limits={files:limitValue(saved.files),bytes:limitValue(saved.bytes)};zipFolders=serverSession.read('zipFolders')==='true';}catch{}}
    const response=await fetch('/web/api/state',{headers:{...serverSession.headers(),'X-Chuan-Client':client,'X-Chuan-Tab':tab,'X-Chuan-Device':browserDeviceType(),'X-Chuan-Directory-Receive':directoryReceiveAvailable()?'1':'0','X-Chuan-Name':'','X-Chuan-Ping-Ack':String(pingAck)},cache:'no-store',signal:controller.signal});
    if(response.status===409)serverSession.fail();
    if(!response.ok)throw new Error(response.status===403?'Web 服务已关闭': '连接失败 ('+response.status+')');
    const state=await response.json();serverSession.check(state);if(token&&token!==state.token)pingAck=0;token=state.token;online=true;hostId=state.hostId;
    if($('android-client'))$('android-client').hidden=state.androidAvailable!==true;
    if($('android-mobile'))$('android-mobile').hidden=state.androidAvailable!==true;
    hostName=state.name;
    webUploadChecksumOptional=state.webUploadChecksumOptional===true;
    webCopyCheck=state.webCopyCheck===true;
    webDownloadChecksum=state.webDownloadChecksum===true;
    webTransferWait=state.webTransferWait===true;
    webBatchFiles=state.webBatchFiles===true;
    webUploadStatus=state.webUploadStatus===true;
    webZipArchives=state.webZipArchives===true;webZipStream=state.webZipStream===true;webResume=state.webResume===true;
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
  }catch(error){online=false;if(serverSession.blocked){for(const op of operations){if(!op.finished){op.cancelled=true;op.finished=true;op.status='服务端会话已变化，任务已停止';}op.xhr?.abort();op.waitController?.abort();op.packController?.abort();op.hashController?.abort();op.plan?.controller?.abort();}for(const controller of folderDownloadControllers.values())controller.abort();acceptedBatches.clear();}updateTarget();renderRequests(currentRequests);renderJobs();$('service-status').textContent=serverSession.blocked?'需要重新连接':'正在重新连接';$('service-status').classList.add('offline');showNotice(error.name==='AbortError'?'连接超时，正在重连':error.message||'无法连接服务主机');}
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
const folderDownloadControllers=new Map(),cancelledFolderBatches=new Set(),folderSaveErrors=new Map(),folderActivities=new Map();
async function chooseDestination(batch){
  if(!directoryReceiveAvailable())throw new Error('当前页面无法直接写入目录。保留文件夹结构接收需要使用 Windows 客户端，或支持目录写入的浏览器安全页面。');
  const handle=await window.showDirectoryPicker({mode:'readwrite'});
  cancelledFolderBatches.delete(batch);folderSaveErrors.delete(batch);
  for(const job of latestJobs)if(job.batchId===batch){downloaded.delete(job.id);savedFiles.delete(String(job.id));}
  folderDestinations.set(batch,handle);acceptedBatches.add(batch);
}
const incomingContexts=new Map(),incomingPaused=new Set(),incomingRetry=new Map();
function finishIncoming(job){
  writingFiles.delete(String(job.id));folderActivities.delete(String(job.id));folderDownloadControllers.delete(String(job.id));incomingContexts.delete(String(job.id));renderJobs();
}
function incomingMarker(filename,total){return '.chuan-browser-'+hex(new Sha256().update(new TextEncoder().encode(filename+':'+total)).digest())+'.json';}
let incomingMarkerSecret;
function incomingMarkerTag(record){
  if(!incomingMarkerSecret){let key;try{key=localStorage.getItem('chuan-receive-marker-key');if(!/^[0-9a-f]{32}$/.test(key||'')){key=random();localStorage.setItem('chuan-receive-marker-key',key);}}catch{key=random();}incomingMarkerSecret=Uint8Array.from(key.match(/../g),byte=>parseInt(byte,16));}
  const inner=new Uint8Array(64).fill(0x36),outer=new Uint8Array(64).fill(0x5c);for(let i=0;i<incomingMarkerSecret.length;i++){inner[i]^=incomingMarkerSecret[i];outer[i]^=incomingMarkerSecret[i];}
  const data=new TextEncoder().encode(JSON.stringify({path:record.path,total:record.total,records:record.records}));return hex(new Sha256().update(outer).update(new Sha256().update(inner).update(data).digest()).digest());
}
async function writeIncomingMarker(directory,name,record){const handle=await directory.getFileHandle(name,{create:true}),stream=await handle.createWritable();try{await stream.write(JSON.stringify({...record,tag:incomingMarkerTag(record)}));await stream.close();}catch(error){await stream.abort().catch(()=>{});throw error;}}
async function failIncoming(job,error){
  const context=incomingContexts.get(String(job.id));downloaded.delete(job.id);
  if(context?.pauseRequested||incomingPaused.has(String(job.id))){finishIncoming(job);return;}
  if(error.sourceMismatch&&context){await context.directory.removeEntry(context.filename).catch(()=>{});await context.directory.removeEntry(incomingMarker(context.filename,job.total)).catch(()=>{});folderSaveErrors.delete(job.batchId);incomingRetry.set(String(job.id),{job,at:Date.now()+500});notice('源文件内容已变化，正在重新接收该文件');}
  else if(error.name==='QuotaExceededError'||error.name==='NotAllowedError'){incomingPaused.add(String(job.id));await command('/web/api/pause/'+job.id).catch(()=>{});notice('接收目录无法继续写入，已暂停并保留进度：'+error.message);}
  else if(error.name==='AbortError'){acceptedBatches.delete(job.batchId);folderDestinations.delete(job.batchId);notice('目录保存已取消，已接收部分已保留');}
  else if(/原文件已保留|同名文件|续传记录/.test(error.message)){downloaded.add(job.id);incomingRetry.delete(String(job.id));folderSaveErrors.set(job.batchId,error.message);await command('/web/api/file-failed/'+job.id).catch(()=>{});notice(error.message);}
  else{folderSaveErrors.set(job.batchId,error.message);notice(error.message+'；已接收部分已保留');if(acceptedBatches.has(job.batchId))incomingRetry.set(String(job.id),{job,at:Date.now()+1500});}
  finishIncoming(job);
}
const receivePipeline=createFilePipeline(async job=>{
  const controller=new AbortController();folderDownloadControllers.set(String(job.id),controller);
  const activity=value=>{folderActivities.set(String(job.id),value);renderJobs();};
  try{
    if(!acceptedBatches.has(job.batchId)||cancelledFolderBatches.has(job.batchId))throw new DOMException('已取消','AbortError');
    let directory=folderDestinations.get(job.batchId);if(!directory){downloaded.delete(job.id);finishIncoming(job);return false;}
    writingFiles.add(String(job.id));activity('检查接收位置');
    const parts=job.name.replaceAll('\\','/').split('/');
    for(const name of parts.slice(0,-1))directory=await directory.getDirectoryHandle(name,{create:true});
    const info=webDownloadChecksum?{directory:!!job.directory}:JSON.parse(await command('/web/api/file-info/'+job.id,{},null,controller.signal));
    const filename=parts.at(-1);let existing=null;
    if(info.directory)await directory.getDirectoryHandle(filename,{create:true});
    else{
      try{existing=await directory.getFileHandle(filename);}catch(error){if(error.name!=='NotFoundError')throw error;}
      if(existing){
        const file=await existing.getFile();let marker=null;
        try{const record=await directory.getFileHandle(incomingMarker(filename,job.total));marker=JSON.parse(await(await record.getFile()).text());}catch(error){if(error.name!=='NotFoundError')throw Error('续传记录无法读取，原文件已保留：'+job.name);}
        if(marker&&(marker.tag!==incomingMarkerTag(marker)||marker.path!==job.name||marker.total!==job.total||!Array.isArray(marker.records)))throw Error('续传记录与目标不匹配，原文件已保留：'+job.name);
        if(!marker&&file.size!==job.total)throw Error('同名文件内容不同，原文件已保留：'+job.name);
        incomingContexts.set(String(job.id),{directory,filename,info,existing,controller,activity,file,marker});activity('等待核对同名文件');return 'verify';
      }
    }
    if(controller.signal.aborted)throw new DOMException('已取消','AbortError');
    incomingContexts.set(String(job.id),{directory,filename,info,existing,controller,activity});activity('已就绪，等待传输');return true;
  }catch(error){await failIncoming(job,error);return false;}
},async job=>{
  const context=incomingContexts.get(String(job.id));if(!context)return;
  const {directory,filename,info,existing,controller,activity}=context;
  try{
    if(controller.signal.aborted||!acceptedBatches.has(job.batchId))throw new DOMException('已取消','AbortError');
    if(!info.directory&&(!existing||context.resume)){
      let file=existing;
      if(!file){let appeared=false;try{await directory.getFileHandle(filename);appeared=true;}catch(error){if(error.name!=='NotFoundError')throw error;}if(appeared)throw Error('目标文件在核对后已变化，原文件已保留：'+job.name);file=await directory.getFileHandle(filename,{create:true});}
      const start=context.resume?.offset||0,hash=new Sha256();if(context.resume){hash.h.set(context.resume.state.h);hash.block.set(context.resume.state.block);hash.used=context.resume.state.used;hash.bytes=context.resume.state.bytes;}
      const previous={bytes:start,prefix:hex(hash.clone().digest())},markerName=incomingMarker(filename,job.total);
      await writeIncomingMarker(directory,markerName,{path:job.name,total:job.total,records:[previous]});
      const writable=await file.createWritable({keepExistingData:!!start});await writable.seek(start);let count=start,tail=0,verified=false;
      try{
        activity(start?'正在续传文件':'正在接收文件');const headers=start?{'X-Chuan-Offset':String(start),'X-Chuan-Prefix':previous.prefix}:{};
        const response=await fetch(serverSession.download(job.id,token)+(webDownloadChecksum?'&checksum=trailer':''),{headers,signal:controller.signal});
        if(!response.ok||!response.body){const reason=await response.text();const error=Error(reason||'文件夹接收失败');error.sourceMismatch=reason.includes('续传内容不一致');throw error;}
        const reader=response.body.getReader(),trailer=new Uint8Array(32);
        for(;;){
          if(controller.signal.aborted)throw new DOMException('已暂停或取消','AbortError');const next=await reader.read();if(next.done)break;
          const length=webDownloadChecksum?Math.min(next.value.length,Math.max(0,job.total-count)):next.value.length,data=next.value.subarray(0,length);
          if(length){await writable.write(data);hash.update(data);count+=length;}
          const rest=next.value.subarray(length);if(rest.length){if(!webDownloadChecksum||tail+rest.length>32)throw Error('接收数据长度异常');trailer.set(rest,tail);tail+=rest.length;}
        }
        if(count!==job.total||(webDownloadChecksum&&tail!==32))throw Error('接收数据不完整');
        if(hex(hash.clone().digest())!==(webDownloadChecksum?hex(trailer):info.sha256)){const error=Error('接收文件完整性校验失败');error.sourceMismatch=true;throw error;}
        activity('正在保存文件');verified=true;
        await writeIncomingMarker(directory,markerName,{path:job.name,total:job.total,records:[previous,{bytes:count,prefix:hex(hash.clone().digest())}]});await writable.close();await directory.removeEntry(markerName);
      }catch(error){
        if(error.sourceMismatch){await writable.abort().catch(()=>{});if(!existing){await directory.removeEntry(filename).catch(()=>{});await directory.removeEntry(markerName).catch(()=>{});}}
        else{try{await writeIncomingMarker(directory,markerName,{path:job.name,total:job.total,records:[previous,{bytes:count,prefix:hex(hash.clone().digest())}]});await writable.close();}catch{await writable.abort().catch(()=>{});}}
        throw error;
      }
    }
    await command('/web/api/file-saved/'+job.id,{},null,controller.signal);savedFiles.add(String(job.id));finishIncoming(job);
  }catch(error){await failIncoming(job,error);}
},error=>trace.write('pipeline_error',{error:error.message}),pipelineTrace('receive'),async job=>{
  const context=incomingContexts.get(String(job.id));if(!context)return false;
  const {info,file,controller,activity,directory,filename}=context;
  try{
    if(controller.signal.aborted||!acceptedBatches.has(job.batchId))throw new DOMException('已取消','AbortError');
    if(context.marker){activity('核验已接收部分，准备续传');const checked=await hashFile(file,controller.signal,true),digest=hex(checked.digest);if(!context.marker.records.some(record=>record.bytes===file.size&&record.prefix===digest)||file.size>job.total)throw Error('续传文件已被修改，原文件已保留：'+job.name);if(file.size===job.total){if(!info.sha256)Object.assign(info,JSON.parse(await command('/web/api/file-info/'+job.id,{},null,controller.signal)));if(digest!==info.sha256)throw Error('同名文件内容不同，原文件已保留：'+job.name);await directory.removeEntry(incomingMarker(filename,job.total));activity('内容相同，已保存');return true;}context.resume={offset:file.size,state:checked.state};activity('已就绪，等待续传');return true;}
    activity('核对同名文件');
    if(!info.sha256)Object.assign(info,JSON.parse(await command('/web/api/file-info/'+job.id,{},null,controller.signal)));
    if(hex(await hashFile(file,controller.signal))!==info.sha256)throw Error('同名文件内容不同，原文件已保留：'+job.name);
    activity('已就绪，等待传输');return true;
  }catch(error){await failIncoming(job,error);return false;}
});
async function receiveApproved(){
  for(const [id,pending] of incomingRetry)if(online&&Date.now()>=pending.at&&!incomingPaused.has(id)){pending.at=Date.now()+2000;try{await command('/web/api/resume-job/'+id);folderSaveErrors.delete(pending.job.batchId);incomingRetry.delete(id);}catch{}}
  for(const job of latestJobs){
    if(job.paused||incomingPaused.has(String(job.id))||writingFiles.has(String(job.id))||!job.receiving||!job.folderId||job.state>=4||!acceptedBatches.has(job.batchId)||downloaded.has(job.id)||!inbox.some(file=>String(file.id)===String(job.id)))continue;
    downloaded.add(job.id);receivePipeline.push(job);
  }
  await receivePipeline.idle();
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

$('limits').onclick=()=>{$('zip-folders').checked=zipFolders;$('limit-files').value=limits.files;$('limit-mib').value=String(BigInt(limits.bytes)/1048576n);$('limits-error').textContent='';$('limits-dialog').showModal();};
$('limits-form').onsubmit=async event=>{
  if(event.submitter.value!=='save')return;event.preventDefault();
  try{const next={files:limitValue($('limit-files').value),bytes:limitValue($('limit-mib').value,1048576n)};await command('/web/api/preferences',{'X-Chuan-Max-Files':next.files,'X-Chuan-Max-Bytes':next.bytes});limits=next;zipFolders=$('zip-folders').checked;try{serverSession.write('zipFolders',String(zipFolders));serverSession.write('limits',JSON.stringify(limits));}catch{}$('limits-dialog').close();notice('传输设置已保存',3000);}catch(error){$('limits-error').textContent=error.message;}
};
for(const id of ['picker','folder-picker']){
  $(id).onchange=e=>{try{enqueue(selectedFiles(e.target.files,id==='folder-picker'),e.target.dataset.retryTarget||chosen);}catch(error){notice(error.message);}finally{e.target.value='';delete e.target.dataset.retryTarget;}};
  $(id).oncancel=e=>{delete e.target.dataset.retryTarget;};
}
$('choose-files').onclick=()=>{delete $('picker').dataset.retryTarget;$('picker').click();};
$('choose-folder').onclick=async()=>{
  if(directoryReceiveAvailable()){
    try{const handle=await window.showDirectoryPicker({mode:'read'}),files=[];
      async function walk(dir,parent){files.push({path:parent,file:new File([],dir.name),directory:true});for await(const entry of dir.values()){const path=parent+'/'+entry.name;if(entry.kind==='directory')await walk(entry,path);else files.push({path,file:await entry.getFile()});}}
      await walk(handle,handle.name);enqueue(files);return;
    }catch(error){if(error.name!=='AbortError')notice(error.message);return;}
  }
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
$('clear').onclick=async()=>{try{await command('/web/api/clear');const busy=new Set(operations.filter(outgoingActive).map(op=>op.plan));for(const op of operations)if(!outgoingActive(op)&&!busy.has(op.plan))op.hidden=true;await poll();}catch(e){notice(e.message);}};
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

$('export-diagnostics').onclick=async()=>{
  const control=$('export-diagnostics');control.disabled=true;$('limits-dialog').close();notice('正在导出诊断日志');
  try{
    trace.write('export_requested',{send_pipeline:sendPipeline.state(),receive_pipeline:receivePipeline.state()});
    const exportController=new AbortController(),exportTimer=setTimeout(()=>exportController.abort(),15000);
    let server=new Blob();try{const response=await fetch('/web/api/diagnostics-export',{signal:exportController.signal,method:'POST',headers:{...serverSession.headers(),'X-Chuan-Client':client,'X-Chuan-Tab':tab,'X-Chuan-Token':token}});if(!response.ok)throw Error('服务端日志导出失败 ('+response.status+')');server=await response.blob();}catch(error){trace.write('server_export_failed',{error:error.message});notice('已导出页面日志；'+error.message);}finally{clearTimeout(exportTimer);}
    const blob=new Blob([server,await trace.blob()],{type:'application/x-ndjson'}),url=URL.createObjectURL(blob),link=document.createElement('a');link.href=url;link.download='Chuan-Web-diagnostics-'+new Date().toISOString().replaceAll(':','-')+'.jsonl';document.body.append(link);link.click();link.remove();setTimeout(()=>URL.revokeObjectURL(url),60000);
  }catch(error){notice(error.message);}finally{control.disabled=false;}
};
