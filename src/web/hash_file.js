// Completed workers keep their compiled hashing code for later segments.
// The idle pool is bounded and does not limit independent active verifications.
const idleHashWorkers=[];
let diagnostics=null,activeWorkers=0,workerSequence=0;
export function setHashDiagnostics(callback){diagnostics=callback;}
export function hashFile(file,signal,withState=false,context={}){return new Promise((resolve,reject)=>{
  if(signal?.aborted){reject(new DOMException('核对已取消','AbortError'));return;}
  const idle=idleHashWorkers.pop();if(idle)clearTimeout(idle.timer);
  const started=performance.now(),worker=idle?.worker||new Worker(new URL('./hash_worker.js',import.meta.url),{type:'module'}),workerId=idle?.id??++workerSequence;let released=false;
  activeWorkers++;const report=(event,extra={})=>{try{diagnostics?.({event,worker_id:workerId,worker_reused:!!idle,file_bytes:file.size,active_workers:activeWorkers,idle_workers:idleHashWorkers.length,elapsed_ms:performance.now()-started,...context,...extra});}catch{}};report('hash_start');
  const close=(healthy=false)=>{
    if(released)return;released=true;activeWorkers--;
    signal?.removeEventListener('abort',abort);worker.onmessage=null;worker.onerror=null;
    if(healthy&&idleHashWorkers.length<8){
      const entry={worker,id:workerId,timer:0};entry.timer=setTimeout(()=>{const index=idleHashWorkers.indexOf(entry);if(index>=0){idleHashWorkers.splice(index,1);worker.terminate();}},30000);idleHashWorkers.push(entry);
    }else worker.terminate();
  };
  const abort=()=>{report('hash_abort');close();reject(new DOMException('核对已取消','AbortError'));};
  signal?.addEventListener('abort',abort,{once:true});
  worker.onmessage=({data})=>{report(data.error?'hash_error':'hash_end',{...data.metrics,error:data.error||''});close(!data.error);data.error?reject(Error(data.error)):resolve(withState?{digest:data.digest,state:data.state}:data.digest);};
  worker.onerror=event=>{report('hash_error',{error:event.message});close();reject(Error(event.message||'摘要线程启动失败'));};
  try{worker.postMessage(withState?{file,withState:true}:file);}catch(error){close();reject(error);}
});}
