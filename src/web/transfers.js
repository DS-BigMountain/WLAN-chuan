// Display groups use the transfer batch, direction and peer; names alone are not identities.
export function transferGroups(items) {
  const rows=[], groups=new Map();
  for(const item of items) {
    if(!item.folderId){if(!item.removed)rows.push(item);continue;}
    const key=JSON.stringify([item.folderId,!!item.receiving,item.peer||'']);
    let group=groups.get(key);
    if(!group){group={group:true,key,name:item.folderName,receiving:item.receiving,peer:item.peer,items:[],total:0,done:0,speed:0,count:0,completed:0};groups.set(key,group);rows.push(group);}
    group.items.push(item);
    group.total=Math.max(group.total,item.folderTotal||0);group.count=Math.max(group.count,item.folderCount||0);
    group.done+=Math.min(item.done||0,item.total||0);
    if(item.state===2)group.speed+=item.speed||0;
    if(item.state===3)group.completed++;
  }
  for(const group of groups.values()) {
    group.done=Math.min(group.done,group.total);
    const active=group.items.filter(item=>item.state<3), failure=group.items.reduce((state,item)=>Math.max(state,item.state),3);
    group.state=active.length?(active.some(item=>item.state===2)?2:active.some(item=>item.state===1)?1:0):failure>3?failure:group.completed>=group.count?3:1;
    group.stateText=['等待发送','等待接收或后续文件','传输中','已完成','已取消','部分失败','已拒绝'][group.state];
    group.percent=group.total?Math.floor(group.done*100/group.total):Math.floor(group.completed*100/Math.max(1,group.count));
    group.items=group.items.filter(item=>!item.removed);
  }
  return rows.filter(row=>!row.group||row.items.length);
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
