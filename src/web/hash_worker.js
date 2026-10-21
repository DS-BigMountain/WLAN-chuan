import {Sha256} from './sha256.js';
self.onmessage=async({data})=>{const file=data.file||data;
  try{
    const hash=new Sha256();
    for(let offset=0;offset<file.size;offset+=1048576){
      const expected=Math.min(1048576,file.size-offset),bytes=new Uint8Array(await file.slice(offset,offset+expected).arrayBuffer());
      if(bytes.length!==expected)throw Error('源文件长度已变化');
      hash.update(bytes);
    }
    const state=data.withState?{h:hash.h,block:hash.block,used:hash.used,bytes:hash.bytes}:undefined;const digest=hash.clone().digest();self.postMessage({digest,state},[digest.buffer]);
  }catch(error){self.postMessage({error:error.message||'无法读取源文件'});}
};
