import {Sha256} from './sha256.js';
self.onmessage=async({data})=>{const file=data.file||data;
  try{
    const started=performance.now();let readMs=0,hashMs=0;
    const hash=new Sha256();
    for(let offset=0;offset<file.size;offset+=1048576){
      const reading=performance.now(),expected=Math.min(1048576,file.size-offset),bytes=new Uint8Array(await file.slice(offset,offset+expected).arrayBuffer());readMs+=performance.now()-reading;
      if(bytes.length!==expected)throw Error('源文件长度已变化');
      const hashing=performance.now();hash.update(bytes);hashMs+=performance.now()-hashing;
    }
    const state=data.withState?{h:hash.h,block:hash.block,used:hash.used,bytes:hash.bytes}:undefined;const digest=hash.clone().digest();self.postMessage({digest,state,metrics:{source_read_ms:readMs,hash_compute_ms:hashMs,worker_elapsed_ms:performance.now()-started}},[digest.buffer]);
  }catch(error){self.postMessage({error:error.message||'无法读取源文件'});}
};
