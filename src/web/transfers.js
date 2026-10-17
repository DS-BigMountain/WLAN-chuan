// Display groups use the transfer batch, direction and peer; names alone are not identities.
export function transferGroups(items) {
  const rows=[], groups=new Map();
  for(const item of items) {
    if(!item.folderId){if(!item.removed)rows.push(item);continue;}
    const key=JSON.stringify([item.folderId,item.batchId||'',item.folderName||'',item.groupScope||'',!!item.receiving,item.peer||'']);
    let group=groups.get(key);
    if(!group){group={group:true,key,name:item.folderName||item.name?.replaceAll('\\','/').split('/')[0]||'文件夹',receiving:item.receiving,peer:item.peer,items:[],total:0,done:0,speed:0,count:0,completed:0,failed:0,cancelled:0,error:''};groups.set(key,group);rows.push(group);}
    group.items.push(item);
    group.total=Math.max(group.total,item.folderTotal||0);group.count=Math.max(group.count,item.folderCount||0);
    group.done+=Math.min(item.done||0,item.total||0);
    if(item.state===2)group.speed+=item.speed||0;
    if(item.state===3)group.completed++;
    if(item.state===5){group.failed++;if(!group.error)group.error=item.name+'：'+(item.error||item.stateText||'传输失败');}
    if(item.state===4)group.cancelled++;
  }
  for(const group of groups.values()) {
    group.done=Math.min(group.done,group.total);
    const active=group.items.filter(item=>item.state<3), failure=group.failed?5:group.items.reduce((state,item)=>Math.max(state,item.state),3);
    group.state=active.length?(active.some(item=>item.state===2)?2:active.some(item=>item.state===1)?1:0):failure>3?failure:group.completed>=group.count?3:1;
    group.awaitingApproval=!group.receiving&&group.state===1&&active.some(item=>item.awaitingApproval===true||(item.awaitingApproval===undefined&&item.state===1&&item.stage!=='staging'));
    group.stateText=group.awaitingApproval?'等待对方确认':group.state===5?(group.completed?'部分失败':'传输失败'):['等待发送','等待接收或后续文件','传输中','已完成','已取消','','已拒绝'][group.state];
    group.percent=group.total?Math.floor(group.done*1000/group.total)/10:Math.floor(group.completed*100/Math.max(1,group.count));
    group.items=group.items.filter(item=>!item.removed);
  }
  return rows.filter(row=>!row.group||row.items.length);
}

export function folderProgress(group,formatBytes){
  const percent=group.done>0&&group.percent===0?'<0.1':String(group.percent);
  return group.completed+'/'+group.count+' 个文件 · '+formatBytes(group.done)+' / '+formatBytes(group.total)+' · '+percent+'%'+(group.failed?' · '+group.failed+' 个失败':'')+(group.cancelled?' · '+group.cancelled+' 个未完成':'');
}
export function transferTree(items,rootName){
  const root={name:rootName,path:rootName,directory:true,total:0,done:0,count:0,children:[]},nodes=new Map([[rootName,root]]);
  for(const item of items){
    const parts=String(item.name||'').replaceAll('\\','/').split('/').filter(Boolean);
    if(parts[0]===rootName)parts.shift();if(!parts.length)continue;
    let parent=root;
    for(let i=0;i<parts.length;i++){
      const path=parent.path+'/'+parts[i],directory=i<parts.length-1;
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
