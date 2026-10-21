// One bounded incremental worker per active verification; the File is cloned by
// reference and file-sized ArrayBuffers are never sent through the UI thread.
export function hashFile(file,signal,withState=false){return new Promise((resolve,reject)=>{
  if(signal?.aborted){reject(new DOMException('核对已取消','AbortError'));return;}
  const worker=new Worker(new URL('./hash_worker.js',import.meta.url),{type:'module'});
  const close=()=>{signal?.removeEventListener('abort',abort);worker.terminate();};
  const abort=()=>{close();reject(new DOMException('核对已取消','AbortError'));};
  signal?.addEventListener('abort',abort,{once:true});
  worker.onmessage=({data})=>{close();data.error?reject(Error(data.error)):resolve(withState?{digest:data.digest,state:data.state}:data.digest);};
  worker.onerror=event=>{close();reject(Error(event.message||'摘要线程启动失败'));};
  try{worker.postMessage(withState?{file,withState:true}:file);}catch(error){close();reject(error);}
});}
