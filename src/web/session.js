// A browser page is bound to one host and one process lifetime. Task IDs are local to that lifetime.
export class ServerSession {
  constructor({fetcher=(...args)=>globalThis.fetch(...args),storage=undefined,random=()=>Array.from(crypto.getRandomValues(new Uint8Array(16)),n=>n.toString(16).padStart(2,'0')).join('')}={}){
    this.fetcher=fetcher;this.random=random;try{this.storage=storage===undefined?localStorage:storage;}catch{this.storage=null;}
    this.host='';this.instance='';this.client='';this.blocked=false;
  }
  key(name){return 'chuan:'+this.host+':'+name;}
  read(name){try{return this.storage?.getItem(this.key(name));}catch{return null;}}
  write(name,value){try{this.storage?.setItem(this.key(name),value);}catch{}}
  async initialize(signal){
    const response=await this.fetcher('/api/info',{cache:'no-store',signal});
    if(!response.ok)throw new Error('无法确认服务端身份 ('+response.status+')');
    const info=await response.json();if(info.protocol!==1||typeof info.id!=='string'||!info.id||info.id.length>64)throw new Error('服务端身份响应无效');
    this.host=info.id;this.instance=info.instance||'';const previous=this.read('client');this.client=/^[0-9a-f]{32}$/i.test(previous||'')?previous:this.random();this.write('client',this.client);
  }
  fail(){this.blocked=true;throw new Error('服务端身份或运行会话已变化。当前任务已停止，请重新连接后重新发送。');}
  check(state){this.assert();if(state.hostId!==this.host||(this.instance&&state.serverInstance!==this.instance))this.fail();if(!this.instance)this.instance=state.serverInstance||'';}
  assert(){if(this.blocked)throw new Error('当前页面与原服务端的会话已失效，请重新连接。');}
  headers(){this.assert();return this.host?{'X-Chuan-Server':this.host,...(this.instance?{'X-Chuan-Instance':this.instance}:{})}:{};}
  download(id,token){this.assert();const query=new URLSearchParams({client:this.client,server:this.host,instance:this.instance,ticket:token});return '/web/files/'+encodeURIComponent(id)+'?'+query;}
}
export function deviceLabel(peer){if(peer.browser)return peer.platform==='mobile-web'?'手机 Web':peer.platform==='desktop-web'?'电脑 Web':'Web';return peer.platform==='windows'?'Win 客户端':peer.platform==='android'?'Android 客户端':'客户端';}
export function browserDeviceType(){const nav=globalThis.navigator||{};return nav.userAgentData?.mobile||/Android|iPhone|iPad|iPod|Mobile/i.test(nav.userAgent||'')||/Macintosh/.test(nav.userAgent||'')&&nav.maxTouchPoints>1?'mobile-web':'desktop-web';}
