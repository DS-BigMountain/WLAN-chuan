// 验证实际上传队列的完成等待、旧服务端兼容、单项失败后继续及取消传播。
import {readFileSync} from 'node:fs';
import vm from 'node:vm';
import assert from 'node:assert/strict';
const source=readFileSync(new URL('../src/web/app.js',import.meta.url),'utf8');
const run=source.slice(source.indexOf('function stopOutgoingFolder('),source.indexOf('async function poll()'));
const cancel=source.slice(source.indexOf('async function cancel(op)'),source.indexOf('function enqueue('));
for(const scenario of ['complete','legacy','unavailable','timeout','malformed','failed','cancelled']){
  const batch={id:'folder',count:2,total:2,totalExact:'2',name:'folder'},plan={id:'plan',promise:Promise.resolve('1')};
  const operations=[0,1].map(i=>({key:'upload-'+i,file:new Blob(['x']),path:'folder/'+i,target:'receiver',item:0,batch,plan,status:'等待',cancelled:false}));
  let uploads=0,waits=0,polls=0;const commands=[],sleeps=[];
  class Xhr{
    constructor(){this.upload={};this.status=201;this.responseText='{"id":"7"}';}
    open(){}setRequestHeader(){}abort(){this.onabort?.();}
    send(){uploads++;queueMicrotask(()=>{this.onload();this.onloadend();});}
  }
  const context={operations,queue:[...operations],queueRunning:false,webBatchFiles:false,webUploadChecksumOptional:true,webUploadStatus:false,webTransferWait:scenario!=='legacy',
    serverSession:{headers:()=>({})},peers:[{id:'receiver',browser:false}],online:true,hostId:'host',deviceName:'Test',token:'token',client:'client',tab:'tab',
    XMLHttpRequest:Xhr,Blob,AbortController,performance,Date,clearTimeout,encode:s=>s,refreshUploads(){},latestJobs:[],
    setTimeout(fn,ms){if(ms===500){sleeps.push(ms);queueMicrotask(fn);return 0;}if(ms===5000&&scenario==='timeout'){queueMicrotask(fn);return 0;}return setTimeout(fn,ms);},
    async poll(){polls++;context.latestJobs=[{id:'8',state:polls%2===0?3:2}];},
    async command(url,headers,body,signal){
      commands.push(url);
      if(url.startsWith('/web/api/forward/'))return '{"id":"8"}';
      if(!url.startsWith('/web/api/wait/'))return '';
      waits++;
      if(scenario==='unavailable')throw new Error('request failed');
      if(scenario==='timeout')return new Promise((resolve,reject)=>signal.addEventListener('abort',()=>reject(new DOMException('Aborted','AbortError')),{once:true}));
      if(scenario==='malformed')return '{"state":null}';
      if(scenario==='failed')return '{"state":5,"error":"目标写入失败"}';
      if(scenario==='cancelled')return new Promise((resolve,reject)=>{
        signal.addEventListener('abort',()=>reject(new DOMException('Aborted','AbortError')),{once:true});
        queueMicrotask(()=>vm.runInContext('cancel(operations[0])',sandbox));
      });
      return JSON.stringify({state:waits%2===0?3:2,error:''});
    }
  };
  const sandbox=vm.createContext(context);await vm.runInContext(cancel+'\n'+run+'\nrunQueue()',sandbox);await Promise.resolve();
  for(let i=0;i<100&&(context.queueRunning||operations.some(op=>!op.finished));i++)await new Promise(resolve=>setTimeout(resolve,1));
  assert.equal(context.queueRunning,false);assert(operations.every(op=>op.finished));assert(!operations[0].waitController);
  if(scenario==='failed'||scenario==='cancelled'){
    assert.equal(uploads,scenario==='failed'?2:1);
    if(scenario==='cancelled')assert(operations[1].cancelled,'user cancellation stops remaining files');
    if(scenario==='failed'){assert(operations[0].error);assert(operations[0].status.includes('目标写入失败'));}
    else{assert(operations[0].cancelled);assert(!operations[0].error);assert(commands.includes('/web/api/cancel/8'));}
  }else{
    assert.equal(uploads,2);assert(operations.every(op=>!op.error));
    if(scenario==='complete'){assert.equal(waits,4);assert.deepEqual(sleeps,[],'completion notification has no fixed polling sleep');}
    else{assert(polls>0);assert(sleeps.length>0,'legacy polling retained');}
    if(scenario==='legacy')assert.equal(waits,0,'older servers never receive the new endpoint');
  }
  console.log('PASS',scenario);
}
