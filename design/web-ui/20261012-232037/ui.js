'use strict';
const $ = id => document.getElementById(id);
const mobile = document.body.dataset.layout === 'mobile';
const compactLayout = () => mobile || window.innerWidth <= 760;
const peers = [
  {id:'host',name:'BIGMOUNTAIN',ip:'192.168.1.12',browser:false},
  {id:'desktop',name:'DESKTOP-02',ip:'192.168.1.20',browser:false},
  {id:'browser',name:mobile?'浏览器-01':'手机浏览器',ip:mobile?'192.168.1.40':'192.168.1.35',browser:true}
];
const state = {
  online:true,chosen:'desktop',tab:'send',filter:'all',requestPending:true,selected:false,
  notes:new Map(),maxFiles:0n,maxBytes:0n,jobs:[
    {id:1,name:mobile?'会议资料.zip':'项目资料',folder:!mobile,size:mobile?'86 MB':'1.28 GB',
      count:mobile?1:24,peer:'DESKTOP-02',targetId:'desktop',receiving:false,status:'sending',percent:65,speed:mobile?'8.6 MB/s':'112 MB/s'},
    {id:2,name:'操作说明.pdf',folder:false,size:'3.2 MB',count:1,peer:'BIGMOUNTAIN',receiving:true,status:'ready',percent:0}
  ]
};
let nextJobId = 3;
function element(tag,content='',className='') {
  const node = document.createElement(tag);node.textContent=content;node.className=className;return node;
}
function icon(name) {
  const svg=document.createElementNS('http://www.w3.org/2000/svg','svg');svg.classList.add('icon');svg.setAttribute('aria-hidden','true');
  const use=document.createElementNS(svg.namespaceURI,'use');use.setAttribute('href','#i-'+name);svg.append(use);return svg;
}
function notify(message) { $('notice').textContent=message;$('notice').hidden=!message; }
function setTab(tab) { state.tab=tab;renderViews(); }
function renderViews() {
  const compact=compactLayout();
  document.querySelectorAll('[data-view]').forEach(node=>{
    const visible=!compact||node.dataset.view===state.tab;
    if(node.id==='receive-section')node.hidden=!visible||!state.requestPending;
    else if(node.id==='receive-empty')node.hidden=!visible||state.requestPending;
    else node.hidden=!visible;
  });
  document.querySelectorAll('[data-tab]').forEach(button=>{
    const active=button.dataset.tab===state.tab;button.classList.toggle('active',active);button.setAttribute('aria-pressed',String(active));
  });
  $('target-bar').hidden=compact&&state.tab!=='send';
}
function makeDevice(peer,sheet=false) {
  const button=element('button','','device'+(state.chosen===peer.id?' selected':''));button.type='button';
  button.setAttribute('aria-pressed',String(state.chosen===peer.id));button.append(icon(peer.browser?'browser':'computer'));
  const copy=element('span','','device-copy');copy.append(element('strong',peer.name));
  copy.append(element('small',peer.ip+(state.notes.get(peer.ip)?' · '+state.notes.get(peer.ip):'')));
  copy.append(element('small',peer.browser?'在线 · 浏览器':'在线 · Windows'));button.append(copy,element('span','','online-dot'));
  button.onclick=()=>{state.chosen=peer.id;renderDevices();renderTarget();if(sheet){$('device-dialog').close();setTab('send');}};
  return button;
}
function renderDevices() {
  $('device-list').replaceChildren(...peers.map(peer=>makeDevice(peer)));
  $('sheet-device-list').replaceChildren(...peers.map(peer=>makeDevice(peer,true)));
}
function renderTarget() {
  const peer=peers.find(item=>item.id===state.chosen),enabled=state.online&&!!peer;
  $('target-name').textContent=peer?.name||'请选择设备';$('target-ip').textContent=peer?.ip||'';
  $('target-glyph').setAttribute('href','#i-'+(peer?.browser?'browser':'computer'));
  $('drop-title').textContent=enabled?(compactLayout()?'选择要发送的文件':'拖入文件或文件夹'):'请先选择在线设备';
  $('drop-destination').textContent=enabled?'发送至 '+peer.name:'选择目标后启用发送';
  $('target-mode').textContent=peer?.id==='host'?'主机自动接收：关':'';
  $('route-label').textContent=peer?.id==='host'?'直接上传至服务主机':'经 BIGMOUNTAIN 转发';
  $('send-route').textContent=peer?.id==='host'?'浏览器 → BIGMOUNTAIN':'浏览器 → BIGMOUNTAIN → '+(peer?.name||'目标设备');
  for(const id of ['choose-files','ping','note'])$(id).disabled=!enabled;
  $('choose-folder').disabled=!enabled||mobile;
  $('connection-label').textContent=state.online?'已连接服务主机':'正在重新连接';
  document.querySelector('.directory-help').hidden=!mobile;
}
function jobCard(job) {
  const card=element('div','','job');card.dataset.job=String(job.id);card.append(icon(job.folder?'folder':'file'));
  const body=element('div','','job-body'),title=element('div','','job-title');
  const name=element('strong');name.append(element('span',job.receiving?'接收':'发送','direction'),document.createTextNode(job.name));
  const labels={sending:'发送至目标设备',waiting:'等待接收方确认',ready:'可下载',browser:'已交给浏览器',cancelled:'已取消',completed:'已送达目标设备'};
  title.append(name,element('span',labels[job.status]||job.status,'job-status'+(job.status==='completed'?' green':'')));body.append(title);
  const detail=(job.receiving?'来自 ':'目标：')+job.peer+' · '+(job.folder?job.count+' 个文件 · ':'')+job.size;
  body.append(element('p',detail));
  if(job.status==='sending'||job.status==='waiting') {
    const progress=element('div','','job-progress'),track=element('div','','progress-track'),fill=element('span');fill.style.width=job.percent+'%';track.append(fill);
    progress.append(track,element('small',job.percent+'%'),element('small',job.speed||'0 B/s'));body.append(progress);
    const stages=element('div','','stages');stages.append(
      element('span',job.status==='waiting'?'接收方确认':'上传至主机：已完成',job.status==='waiting'?'current-stage':'finished-stage'),
      element('span','→'),element('span',job.status==='waiting'?'上传至服务主机':'目标设备：发送中',job.status==='sending'?'current-stage':''));
    body.append(stages);
  } else if(job.status==='browser') body.append(element('p','请在浏览器下载列表中核对保存结果。'));
  const actions=element('div','','job-actions'),button=element('button');button.type='button';
  if(job.receiving&&(job.status==='ready'||job.status==='browser')) {
    button.disabled=!state.online;
    button.append(icon('download'),document.createTextNode(job.status==='browser'?'重新下载':'下载'));
    button.onclick=()=>{if(!state.online)return;job.status='browser';renderJobs();notify('文件已交给浏览器，请在下载列表中核对保存结果。');};
  } else if(job.status==='sending'||job.status==='waiting') {
    button.append(icon('stop'),document.createTextNode('取消'));
    button.onclick=()=>{job.status='cancelled';renderJobs();notify('已取消此任务；目标设备保持不变。');};
  } else {
    button.append(icon('delete'),document.createTextNode('移除记录'));
    button.onclick=()=>{state.jobs=state.jobs.filter(item=>item.id!==job.id);renderJobs();};
  }
  actions.append(button);card.append(body,actions);return card;
}
function renderJobs() {
  const visible=state.jobs.filter(job=>state.filter==='all'||(state.filter==='receive')===job.receiving);
  $('jobs').replaceChildren(...visible.map(jobCard));
  if(!visible.length)$('jobs').append(element('p','暂无传输任务','empty-job'));
  const active=state.jobs.find(job=>!job.receiving&&(job.status==='sending'||job.status==='waiting'));
  $('mobile-current-job').replaceChildren(active?jobCard(active):element('p','暂无进行中的发送任务','empty-job'));
  $('job-count').textContent=String(state.jobs.length);
  $('clear-finished').disabled=!state.jobs.some(job=>['browser','cancelled','completed'].includes(job.status));
}
function renderSelection() {
  state.selected=$('receive-file').checked;
  $('selection-summary').textContent=state.selected?'已选择 1 个项目 · 3.2 MB':'尚未选择接收项目';
  $('accept-selected').disabled=!state.selected||!state.online;
  $('reject-request').disabled=!state.online;
}
function finishRequest(accept) {
  if(!state.online)return;
  if(accept&&!state.selected)return;
  state.requestPending=false;$('request-count').textContent='0';$('request-count').hidden=true;
  if(accept)state.jobs.push({id:nextJobId++,name:'会议纪要.pdf',folder:false,size:'3.2 MB',count:1,peer:'BIGMOUNTAIN',receiving:true,status:'ready',percent:0});
  renderViews();renderJobs();notify(accept?'已接受会议纪要.pdf；文件夹未接受。请在状态中下载。':'已拒绝此批次。');
  if(mobile&&accept)setTab('status');
}
function addOutgoing(folder) {
  const peer=peers.find(item=>item.id===state.chosen);if(!state.online||!peer)return;
  if(folder&&mobile)return;
  const count=folder?6n:1n,total=folder?24n*1048576n:3355443n;
  if((state.maxFiles&&count>state.maxFiles)||(state.maxBytes&&total>state.maxBytes)){notify('超过本浏览器设置的单批文件数量或大小上限。');return;}
  const job={id:nextJobId++,name:folder?'项目文档':'会议纪要.pdf',folder,size:folder?'24 MB':'3.2 MB',count:folder?6:1,
    peer:peer.name,targetId:peer.id,receiving:false,status:'waiting',percent:0,speed:'0 B/s'};
  state.jobs.push(job);renderJobs();notify('已提交批次，等待 '+peer.name+' 确认。');if(mobile)setTab('status');
}
$('client-identity').textContent=mobile?'本浏览器：手机浏览器 · 192.168.1.35':'本浏览器：浏览器-01 · 192.168.1.40';
$('sidebar-identity').textContent=mobile?'手机浏览器':'浏览器-01';$('sidebar-ip').textContent=mobile?'192.168.1.35':'192.168.1.40';
$('switch-device').onclick=()=>$('device-dialog').showModal();
$('receive-file').onchange=renderSelection;$('accept-selected').onclick=()=>finishRequest(true);$('reject-request').onclick=()=>finishRequest(false);
$('choose-files').onclick=()=>addOutgoing(false);$('choose-folder').onclick=()=>addOutgoing(true);
$('show-status').onclick=()=>setTab('status');$('receive-show-status').onclick=()=>setTab('status');
$('refresh').onclick=()=>notify('已刷新当前在线设备。');
$('ping').onclick=()=>notify('Ping 已送达 '+peers.find(peer=>peer.id===state.chosen).name+'。');
$('note').onclick=()=>{const peer=peers.find(item=>item.id===state.chosen);$('note-ip').textContent=peer.ip;$('note-value').value=state.notes.get(peer.ip)||'';$('note-dialog').showModal();};
$('note-form').onsubmit=event=>{event.preventDefault();const peer=peers.find(item=>item.id===state.chosen);state.notes.set(peer.ip,$('note-value').value.trim());$('note-dialog').close();renderDevices();notify('IP 临时备注已保存。');};
$('limits-button').onclick=()=>$('limits-dialog').showModal();
$('limits-form').onsubmit=event=>{
  event.preventDefault();const values=[$('limit-files').value,$('limit-size').value];
  if(values.some(value=>!/^\d+$/.test(value))){$('limits-error').textContent='上限必须为非负整数。';return;}
  if(BigInt(values[0])>18446744073709551615n||BigInt(values[1])*1048576n>18446744073709551615n){$('limits-error').textContent='设置值超出协议可表示的范围。';return;}
  state.maxFiles=BigInt(values[0]);state.maxBytes=BigInt(values[1])*1048576n;
  $('limits-error').textContent='';$('limits-dialog').close();notify('传输上限已保存；实际限制取服务主机与接收方允许的范围。');
};
$('native-client').onclick=()=>notify('Windows 客户端入口对应服务主机的 /download/Chuan.exe。');
$('clear-finished').onclick=()=>{state.jobs=state.jobs.filter(job=>!['browser','cancelled','completed'].includes(job.status));renderJobs();notify('已清除本次会话中已结束的显示记录。');};
document.querySelectorAll('[data-tab]').forEach(button=>button.onclick=()=>setTab(button.dataset.tab));
document.querySelectorAll('[data-filter]').forEach(button=>button.onclick=()=>{
  state.filter=button.dataset.filter;document.querySelectorAll('[data-filter]').forEach(item=>item.classList.toggle('active',item===button));renderJobs();
});
document.querySelectorAll('[data-close]').forEach(button=>button.onclick=()=>$(button.dataset.close).close());
document.querySelectorAll('dialog').forEach(dialog=>dialog.addEventListener('click',event=>{if(event.target===dialog)dialog.close();}));
window.addEventListener('resize',()=>{renderTarget();renderViews();});
window.chuanPrototype={
  getState:()=>({chosen:state.chosen,online:state.online,tab:state.tab,requestPending:state.requestPending,jobs:state.jobs.map(job=>({...job}))}),
  setOnline:value=>{state.online=value;renderTarget();renderSelection();renderJobs();notify(value?'已恢复连接服务主机。':'连接中断，正在重连。发送与接收确认暂时不可用。');}
};
renderDevices();renderTarget();renderJobs();renderSelection();renderViews();
