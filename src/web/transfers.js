// Display groups use the transfer batch, direction and peer; names alone are not identities.
// Deferred work contains references only. Full verification/packing never holds
// a metadata slot; the ready queue and all active stages have separate budgets.
export const FILE_PIPELINE_LIMITS=Object.freeze({preparation:15,verification:15,transfer:4,ready:32});
export function createFilePipeline(prepare,transfer,failed=error=>console.error(error),observe=()=>{},verify=prepare){
  const limits=FILE_PIPELINE_LIMITS;
  const pending=[],deferred=[],ready=[],waiters=[];let preparing=0,verifying=0,transferring=0;
  const report=(phase,item)=>{try{observe(phase,item,{pending:pending.length,verification_pending:deferred.length,ready:ready.length,preparing,verifying,transferring,preparation_limit:limits.preparation,verification_limit:limits.verification,transfer_limit:limits.transfer,ready_limit:limits.ready});}catch{}};
  function pump(){
    while(ready.length&&transferring<limits.transfer){
      const item=ready.shift();transferring++;report("transfer_start",item);
      Promise.resolve().then(()=>transfer(item)).catch(error=>failed(error,item)).finally(()=>{transferring--;report("transfer_end",item);pump();});
    }
    while(pending.length&&preparing<limits.preparation&&ready.length+preparing+verifying<limits.ready){
      const item=pending.shift();preparing++;report("prepare_start",item);
      Promise.resolve().then(()=>prepare(item)).then(result=>{if(result==='verify'){deferred.push(item);report('verification_queued',item);}else if(result===true){ready.push(item);report("ready",item);}}).catch(error=>failed(error,item)).finally(()=>{preparing--;report("prepare_end",item);pump();});
    }
    while(deferred.length&&verifying<limits.verification&&ready.length+preparing+verifying<limits.ready){
      const item=deferred.shift();verifying++;report('verification_start',item);
      Promise.resolve().then(()=>verify(item)).then(result=>{if(result===true){ready.push(item);report('ready',item);}}).catch(error=>failed(error,item)).finally(()=>{verifying--;report('verification_end',item);pump();});
    }
    if(!pending.length&&!deferred.length&&!ready.length&&!preparing&&!verifying&&!transferring)for(const resolve of waiters.splice(0))resolve();
  }
  return {push(item){pending.push(item);report("queued",item);pump();},idle(){return new Promise(resolve=>{waiters.push(resolve);pump();});},
    state(){return {pending:pending.length,verification_pending:deferred.length,ready:ready.length,preparing,verifying,transferring};}};
}
const phaseNames=new Map([
  ['正在发送文件','发送文件'],['发送中','发送文件'],['正在接收文件','接收文件'],['接收中','接收文件'],
  ['正在保存文件','写入磁盘'],['正在校验接收内容','校验接收数据'],
  ['正在核对文件内容','核对同名文件'],['正在校验已有文件','核对同名文件'],
  ['正在核对目标文件','等待接收端核对同名文件'],['等待发送端核对同名文件','发送端正在核对同名文件'],
  ['准备接收文件','准备接收'],['正在打包 ZIP','打包文件'],['正在检查目标目录','检查接收位置'],
  ['等待接收端确认','等待接收端响应'],['等待接收端保存确认','等待接收端保存完成'],
  ['独立通道自动重试中','连接中断，正在重试'],['等待浏览器提交文件','等待浏览器保存确认'],
  ['等待发送页面恢复','发送页面暂时离线'],['等待接收页面恢复','接收页面暂时离线']
]);
const phase=item=>item.activity||(item.op&&!item.op.finished&&!item.awaitingApproval?item.op.status:'')||'';
const processing=item=>!item.removed&&item.state<3&&(item.state===2||!!phase(item)&&!['等待核对同名文件','等待打包文件','已就绪，等待传输','等待发送','等待接收','等待对方确认','等待发送端打包 ZIP','等待重试数据','等待接收方确认','等待浏览器接收','等待发送页面恢复','等待接收页面恢复'].includes(phase(item)));
export function transferAction(item){
  if(item.paused)return "已暂停，已接收数据已保留";
  if(item.state>=3)return item.stateText||['','','','已完成','已取消','传输失败','已拒绝'][item.state];
  const value=phase(item);if(value)return phaseNames.get(value)||value;
  if(item.awaitingApproval)return '等待接收方确认';
  return item.state===2?(item.directory?'创建目录':item.receiving?'接收文件':'发送文件'):item.receiving?'等待发送端开始下一项':'排队等待';
}
export function currentOperation(group,formatBytes){
  if(group.state>=3)return '';
  return group.currentAction+(group.currentFile?'：'+group.currentFile:'')+
    (group.currentTransferring?' · '+formatBytes(group.currentDone)+' / '+formatBytes(group.currentTotal):'')+
    (group.stageSummary?' · '+group.stageSummary:'');
}
const transferringFile=item=>item.state===2&&!item.directory&&!item.paused&&
  (!phase(item)||['发送文件','接收文件','多连接并行发送','多连接并行接收','边打包边发送 ZIP'].includes(transferAction(item)));
export function transferGroups(items) {
  const rows=[], groups=new Map();
  for(const item of items) {
    if(!item.folderId){if(!item.removed)rows.push(item);continue;}
    const key=JSON.stringify([item.folderId,item.batchId||'',item.folderName||'',item.groupScope||'',!!item.receiving,item.peer||'']);
    let group=groups.get(key);
    if(!group){group={group:true,key,name:item.folderName||item.name?.replaceAll('\\','/').split('/')[0]||'文件夹',receiving:item.receiving,peer:item.peer,items:[],total:0,done:0,speed:0,count:0,completed:0,skipped:0,failed:0,cancelled:0,error:''};groups.set(key,group);rows.push(group);}
    group.items.push(item);
    group.total=Math.max(group.total,item.folderTotal||0);group.count=Math.max(group.count,item.folderCount||0);
    group.done+=Math.min(item.done||0,item.total||0);
    if(item.state===2)group.speed+=item.speed||0;
    if(item.state===3)group.completed++;
    if(item.skipped)group.skipped++;
    if(item.state===5){group.failed++;if(!group.error)group.error=item.name+'：'+(item.error||item.stateText||'传输失败');}
    if(item.state===4)group.cancelled++;
  }
  for(const group of groups.values()) {
    group.done=Math.min(group.done,group.total);
    group.paused=group.items.some(item=>item.state<3)&&group.items.filter(item=>item.state<3).every(item=>item.paused);
    const active=group.items.filter(item=>item.state<3), failure=group.failed?5:group.items.reduce((state,item)=>Math.max(state,item.state),3);
    const transferring=active.filter(transferringFile).length;
    group.transferringFiles=transferring;
    const probing=active.filter(item=>transferAction(item)==='检查接收位置').length;
    const verifying=active.filter(item=>['核对同名文件','发送端正在核对同名文件','等待接收端核对同名文件','打包文件'].includes(transferAction(item))).length;
    const waiting=active.filter(item=>['等待核对同名文件','等待打包文件'].includes(phase(item))).length;
    const ready=active.filter(item=>phase(item)==='已就绪，等待传输').length;
    group.stageSummary=transferring+probing+verifying+waiting+ready>1?'检查 '+probing+' · 核对 '+verifying+' · 传输 '+transferring+' · 待传 '+ready+(waiting?' · 待核对 '+waiting:''):'';
    group.state=active.length?(active.some(item=>item.state===2)?2:active.some(item=>item.state===1)?1:0):failure>3?failure:group.completed>=group.count?3:1;
    if(!active.length&&group.completed<group.count&&group.items.some(item=>item.folderStopped)&&group.state!==5){group.state=4;group.cancelled=Math.max(group.cancelled,group.count-group.completed);}
    group.awaitingApproval=!group.receiving&&group.state===1&&active.some(item=>item.awaitingApproval===true||(item.awaitingApproval===undefined&&item.state===1&&item.stage!=='staging'&&!phase(item)&&!(item.done>0)));
    group.stateText=group.awaitingApproval?'等待对方确认':group.state===5?(group.completed?'部分失败':'传输失败'):['等待发送','等待接收或后续文件','传输中','已完成','已取消','','已拒绝'][group.state];
    group.percent=group.total?Math.floor(group.done*1000/group.total)/10:Math.floor(group.completed*100/Math.max(1,group.count));
    const current=active.filter(processing).sort((a,b)=>Number(!!phase(b))-Number(!!phase(a))||Number(!!a.directory)-Number(!!b.directory));
    group.currentCount=current.length;group.currentFile='';group.currentAction='';group.currentTransferring=false;group.currentDone=0;group.currentTotal=0;
    if(current.length){const item=current[0];group.currentFile=String(item.name||'').replaceAll('\\','/');if(group.currentFile.startsWith(group.name+'/'))group.currentFile=group.currentFile.slice(group.name.length+1);group.currentAction=transferAction(item);group.currentDone=item.done||0;group.currentTotal=item.total||0;group.currentTransferring=transferringFile(item);}
    else if(group.state<3){const paused=active.find(item=>['等待发送页面恢复','等待接收页面恢复','等待核对同名文件','等待打包文件'].includes(phase(item)));group.currentAction=paused?transferAction(paused):active.some(item=>phase(item)==='等待浏览器接收')?'等待浏览器开始接收':group.awaitingApproval?'等待接收方确认':group.receiving?'等待发送端开始下一项':'排队中，等待前一项完成';}
    group.items=group.items.filter(item=>!item.removed);
  }
  return rows.filter(row=>!row.group||row.items.length);
}

export function folderProgress(group,formatBytes){
  const percent=group.done>0&&group.percent===0?'<0.1':String(group.percent);
  return group.completed+'/'+group.count+' 项 · '+formatBytes(group.done)+' / '+formatBytes(group.total)+' · '+percent+'%'+(group.skipped?' · '+group.skipped+' 个文件已跳过':'')+(group.failed?' · '+group.failed+' 个失败':'')+(group.cancelled?' · '+group.cancelled+' 个未完成':'');
}
export function transferTree(items,rootName){
  const root={name:rootName,path:rootName,directory:true,total:0,done:0,count:0,children:[]},nodes=new Map([[rootName,root]]);
  for(const item of items){
    const parts=String(item.name||'').replaceAll('\\','/').split('/').filter(Boolean);
    if(parts[0]===rootName)parts.shift();if(!parts.length)continue;
    let parent=root;
    for(let i=0;i<parts.length;i++){
      const path=parent.path+'/'+parts[i],directory=i<parts.length-1||!!item.directory;
      let node=nodes.get(path);if(!node){node={name:parts[i],path,directory,total:0,done:0,count:0,children:[]};nodes.set(path,node);parent.children.push(node);}
      if(!directory){node.item=item;node.total=item.total||0;node.done=item.done||0;node.count=1;node.completed=Number(item.state===3);node.failed=Number(item.state===5);}
      parent=node;
    }
  }
  const sum=node=>{if(!node.directory)return;node.completed=0;node.failed=0;node.children.sort((a,b)=>Number(b.directory)-Number(a.directory)||a.name.localeCompare(b.name,'zh-CN'));for(const child of node.children){sum(child);node.total+=child.total;node.done+=child.done;node.count+=child.count;node.completed+=child.completed;node.failed+=child.failed;}};sum(root);return root;
}
export function createFolderTree(initial,formatBytes,decorate=null){
  const element=document.createElement('details'),summary=document.createElement('summary'),list=document.createElement('ul'),entries=new Map();
  element.className='folder-tree';element.append(summary,list);let group=initial;
  const put=(node,value)=>{if(node.textContent!==value)node.textContent=value;};
  function render(){
    put(summary,group.name+' · '+group.count+' 个文件 · '+formatBytes(group.total));element.setAttribute('aria-label',group.name+' 文件夹目录');
    if(!element.open)return;
    const model=transferTree(group.items,group.name),live=new Set();
    function walk(parent,children){let cursor=parent.firstChild;for(const node of children){
      live.add(node.path);let entry=entries.get(node.path);
      if(!entry){const row=document.createElement('li'),container=node.directory?document.createElement('details'):document.createElement('div'),line=node.directory?document.createElement('summary'):document.createElement('div'),name=document.createElement('span'),size=document.createElement('small'),status=document.createElement('span'),children=document.createElement('ul');
        const actions=document.createElement('span'),metrics=document.createElement('span'),progress=document.createElement('progress'),percent=document.createElement('small'),error=document.createElement('div');
        actions.className='tree-actions';metrics.className='tree-metrics';error.className='tree-error';progress.max=100;progress.setAttribute('aria-label',node.name+' 传输进度');metrics.append(progress,percent);row.dataset.path=node.path;name.className='tree-name';size.className='tree-size';status.className='tree-status';line.className='tree-line';line.append(name,size,metrics,status);if(!node.directory){line.append(actions);container.append(line,error);}else container.append(line,children);container.classList.add('tree-entry');row.append(container);entry={row,name,size,status,children,actions,metrics,progress,percent,error};entries.set(node.path,entry);}
      put(entry.name,node.name);put(entry.size,formatBytes(node.total)+(node.directory?' · '+node.count+' 个文件':''));
      const item=node.item;put(entry.status,item?['等待发送','等待接收','传输中','已完成','已取消','传输失败','已拒绝'][item.state]||item.stateText||'':node.completed+'/'+node.count);
      entry.status.dataset.state=String(item?.state??(node.failed?5:node.completed===node.count?3:''));entry.row.title=node.path+(item?.error?'\n'+item.error:'');
      entry.metrics.hidden=!item||item.state!==2;const percent=item?.total?Math.min(100,100*(item.done||0)/item.total):0;entry.progress.value=percent;put(entry.percent,(item?.done&&percent<.1?'<0.1':Math.floor(percent))+'% · '+formatBytes(Math.round(item?.speed||0))+'/s');
      put(entry.error,item?.error||'');entry.error.hidden=!item?.error||item.state===4;
      if(item&&decorate)decorate(entry.actions,item);
      if(entry.row!==cursor)parent.insertBefore(entry.row,cursor);cursor=entry.row.nextSibling;
      if(node.directory)walk(entry.children,node.children);
    }while(cursor){const next=cursor.nextSibling;cursor.remove();cursor=next;}}
    walk(list,model.children);for(const [key,entry] of entries)if(!live.has(key)){entry.row.remove();entries.delete(key);}
  }
  element.addEventListener('toggle',render);
  return {element,update(next){group=next;render();}};
}
let disclosureId=0;
export function createFolderDisclosure(initial,formatBytes,decorate=null){
  const tree=createFolderTree(initial,formatBytes,decorate),button=document.createElement('button');let group=initial;
  tree.element.classList.add('inline-tree');tree.element.id='folder-contents-'+(++disclosureId);button.type='button';button.className='name folder-toggle';button.setAttribute('aria-controls',tree.element.id);
  const refresh=()=>{button.textContent=group.name;button.setAttribute('aria-expanded',String(tree.element.open));tree.update(group);};
  button.onclick=()=>{tree.element.open=!tree.element.open;refresh();};tree.element.addEventListener('toggle',refresh);refresh();
  return {element:tree.element,button,update(next){group=next;refresh();}};
}

// Timer fallback also handles browsers that omit animationend after tab suspension.
export function pingEffect(element,clock=globalThis) {
  let timer=0,until=0;
  const clear=()=>{clock.clearTimeout(timer);timer=0;until=0;element.classList.remove('active');};
  element.onanimationend=clear;
  return {
    play(){clear();until=clock.Date.now()+1800;void element.offsetWidth;element.classList.add('active');timer=clock.setTimeout(clear,1900);},
    resume(){if(until&&clock.Date.now()>=until)clear();},
    clear
  };
}
export function timedNotice(show,clock=globalThis) {
  let timer=0;
  return (message,duration=0)=>{clock.clearTimeout(timer);show(message);if(duration)timer=clock.setTimeout(()=>show(''),duration);};
}

// Browser diagnostics are an ephemeral delivery queue. The server is the sole log store.
export function createDiagnostics(context=()=>({})){
  const run=Date.now().toString(36)+'-'+Math.random().toString(36).slice(2),pending=[],states=new Map(),stages=new Map();
  let dropped=0,failures=0,sequence=0,pendingBytes=0,sink=null,inflight=null,generation=0;
  try{indexedDB.deleteDatabase('chuan-diagnostics-v2');}catch{}
  function write(event,fields={}){try{
    const line=JSON.stringify({...context(),...fields,event,event_sequence:++sequence,platform:'web',diagnostic_session:run,utc:new Date().toISOString(),mono_ms:performance.now(),browser_epoch_ms:performance.timeOrigin+performance.now()});
    const bytes=new TextEncoder().encode(line).length+1;if(bytes>8192){dropped++;return;}
    while(pending.length&&(pending.length>=4096||pendingBytes+bytes>4*1024*1024)){pendingBytes-=pending.shift().bytes;dropped++;}
    pending.push({line,bytes});pendingBytes+=bytes;
  }catch{dropped++;}}
  async function flush(){
    if(inflight)return inflight;if(!sink||!pending.length)return false;
    const batch=[],epoch=generation;let bytes=0;
    for(const item of pending){if(batch.length>=64||bytes+item.bytes>48000)break;batch.push(item);bytes+=item.bytes;}
    inflight=(async()=>{try{
      const accepted=await sink(batch.map(item=>item.line).join('\n')+'\n');
      if(accepted&&epoch===generation){for(const item of batch){const index=pending.indexOf(item);if(index>=0){pending.splice(index,1);pendingBytes-=item.bytes;}}return true;}
      failures++;return false;
    }catch{failures++;return false;}finally{inflight=null;}})();return inflight;
  }
  function reset(){generation++;pending.length=0;pendingBytes=0;states.clear();}
  function span(name,fields={}){
    const started=performance.now(),id=run+'-'+(++sequence),state={span_id:id,stage:name,phase:'start',started,...fields};stages.set(id,state);write('browser_stage_start',state);let ended=false;
    return {id,phase(value){state.phase=value;},end(result={}){if(ended)return;ended=true;stages.delete(id);write('browser_stage_end',{...fields,span_id:id,stage:name,phase:state.phase,elapsed_ms:performance.now()-started,...result});}};
  }
  function observe(jobs){for(const job of jobs){
    const key=String(job.id)+':'+(job.stage||''),now=performance.now(),value=JSON.stringify([job.state,job.activity,job.error,!!job.skipped]),previous=states.get(key);
    const changed=!previous||value!==previous.value;if(!changed&&(job.state!==2||now-previous.sample<1000))continue;
    write(changed?'file_phase':'file_sample',{id:String(job.id),upload_id:job.uploadKey||job.op?.key||'',batch_id:job.batchId||job.op?.plan?.id||'',path:job.name,receiving:!!job.receiving,state:job.state,phase:job.activity||job.stateText||'',previous_ms:previous?now-previous.phaseAt:0,done_bytes:job.done,total_bytes:job.total,speed_bps:job.speed||0,error:job.error||''});
    states.set(key,{value,sample:now,phaseAt:changed?now:previous.phaseAt});
  }}
  let expected=performance.now()+1000;
  const timer=setInterval(()=>{const now=performance.now(),lag=Math.max(0,now-expected);expected=now+1000;
    write('browser_runtime',{event_loop_delay_ms:lag,visibility:document.visibilityState,pending_log_records:pending.length,pending_log_bytes:pendingBytes,dropped_events:dropped,forward_failures:failures,js_heap_used_bytes:performance.memory?.usedJSHeapSize??null,active_stages:[...stages.values()].slice(0,12).map(stage=>({span_id:stage.span_id,stage:stage.stage,phase:stage.phase,id:stage.id,upload_id:stage.upload_id,offset:stage.offset,age_ms:now-stage.started}))});flush();
  },1000);
  globalThis.addEventListener?.('pagehide',()=>{write('browser_pagehide',{pending_log_records:pending.length});flush();});
  document.addEventListener('visibilitychange',()=>{write('browser_visibility',{visibility:document.visibilityState});if(document.hidden)flush();});
  write('browser_environment',{diagnostic_profile:'transport-detail',secure_context:globalThis.isSecureContext,user_agent:navigator.userAgent,hardware_concurrency:navigator.hardwareConcurrency,device_memory_gib:navigator.deviceMemory??null,save_file_picker:typeof globalThis.showSaveFilePicker==='function',directory_picker:typeof globalThis.showDirectoryPicker==='function',connection_effective_type:navigator.connection?.effectiveType??null,connection_downlink_estimate_mbps:navigator.connection?.downlink??null,connection_rtt_estimate_ms:navigator.connection?.rtt??null});
  return {write,span,observe,flush,reset,setForwarder(value){sink=value;},async drain(){for(let i=0;i<128&&pending.length;i++)if(!await flush())break;},stop(){clearInterval(timer);return flush();}};
}
