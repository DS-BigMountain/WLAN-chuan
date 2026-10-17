// Pending selections are keyed by request and original item index, never by row position.
export function createApprovalPanel(host,{bytes,glyph,decide,available=()=>true,notice=()=>{}}){
  const make=(tag,value='',cls='')=>{const node=document.createElement(tag);node.textContent=value;node.className=cls;return node;};
  const panel=make('section','','approval-panel receive-section'),toolbar=make('div','','approval-toolbar'),title=make('strong','接收申请'),actions=make('div','','approval-actions');
  const allLabel=make('label','','approval-select-all'),all=document.createElement('input');all.type='checkbox';allLabel.append(all,document.createTextNode('全选'));
  const reject=make('button','拒绝'),accept=make('button','接收','primary'),groupsHost=make('div','','approval-groups');reject.type=accept.type='button';
  actions.append(allLabel,reject,accept);toolbar.append(title,actions);panel.append(toolbar,groupsHost);host.replaceChildren(panel);
  const records=new Map(),groups=new Map(),busy=new Set(),resolved=new Set();let requests=[],online=true;
  const key=(request,index)=>String(request.id)+':'+index;
  const pending=(request,index)=>!resolved.has(key(request,index))&&(!request.selection||request.selection[index]==='-');
  function selection(){const leaves=[...records.values()].filter(row=>row.active&&!row.check.disabled);const count=leaves.filter(row=>row.check.checked).length,unsupported=leaves.some(row=>row.check.checked&&!row.available);all.checked=!!leaves.length&&count===leaves.length;all.indeterminate=count>0&&count<leaves.length;all.disabled=!online||!leaves.length;reject.disabled=!online||!count;accept.disabled=reject.disabled||unsupported;accept.title=unsupported?'所选文件夹需要使用客户端接收，或取消勾选该文件夹。':'';for(const group of groups.values()){const items=leaves.filter(row=>row.sender===group.sender);group.count.textContent=items.filter(row=>row.check.checked).length+' / '+group.liveCount+' 项';}}
  all.onchange=()=>{for(const row of records.values())if(row.active&&!row.check.disabled)row.check.checked=all.checked;selection();};
  async function submit(value){
    const chosen=requests.map(request=>({request,bits:request.items.map((_,index)=>{const row=records.get(key(request,index));return row?.active&&!row.check.disabled&&row.check.checked?value:'-';}).join('')})).filter(entry=>entry.bits.includes(value));
    for(const {request} of chosen)busy.add(String(request.id));render();
    try{for(const {request,bits} of chosen){try{await decide(request,bits);for(let i=0;i<bits.length;i++)if(bits[i]!=='-')resolved.add(key(request,i));}catch(error){notice(error.message||'接收操作未完成');}}}
    finally{for(const {request} of chosen)busy.delete(String(request.id));render();}
  }
  accept.onclick=()=>submit('1');reject.onclick=()=>submit('0');
  function render(){
    const live=new Set(),groupLive=new Set();let total=0;
    for(const row of records.values())row.active=false;
    for(const group of groups.values())group.liveCount=0;
    for(const request of requests){
      const items=request.items.map((item,index)=>({item,index})).filter(({index})=>pending(request,index));if(!items.length)continue;
      const sender=request.sender||'发送方';groupLive.add(sender);let group=groups.get(sender);
      if(!group){const element=make('details','','approval-sender'),summary=make('summary'),name=make('strong',sender),count=make('span','','approval-count'),list=make('div','','approval-files');element.open=true;summary.append(glyph('computer'),name,count);element.append(summary,list);group={element,list,count,sender,liveCount:0};groups.set(sender,group);groupsHost.append(element);}
      for(const {item,index} of items){
        const id=key(request,index);live.add(id);total++;group.liveCount++;let row=records.get(id);
        if(!row){const element=make('label','','approval-file'),copy=make('span','','approval-file-copy'),name=make('strong'),size=make('small'),check=document.createElement('input');check.type='checkbox';check.checked=true;check.onchange=selection;copy.append(name,size);element.append(glyph(item.folder?'folder':'file'),copy,check);row={element,name,size,check,sender};records.set(id,row);group.list.append(element);}
        row.active=true;row.available=available(item);row.name.textContent=item.name;row.size.textContent=bytes(Number(item.total))+(item.folder?' · '+item.count+' 个文件':'');row.check.disabled=!online||busy.has(String(request.id));row.check.setAttribute('aria-label',item.name);row.element.title=item.name+(!row.available?'：当前页面无法接收文件夹目录':'');row.element.classList.toggle('unavailable',!row.available);
      }
    }
    for(const [id,row] of records)if(!live.has(id)){row.element.remove();records.delete(id);}
    for(const [sender,group] of groups)if(!groupLive.has(sender)){group.element.remove();groups.delete(sender);}
    host.hidden=!total;title.textContent='接收申请 ('+total+')';selection();
  }
  return {
    reset(){records.clear();groups.clear();resolved.clear();busy.clear();groupsHost.replaceChildren();},
    update(next,connected=true){const ids=new Set(next.map(request=>String(request.id)));for(const id of resolved)if(!ids.has(id.slice(0,id.indexOf(':'))))resolved.delete(id);requests=next;online=connected;render();}
  };
}
