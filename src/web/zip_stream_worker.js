import {zipChunks} from './zip_store.js';
import {Sha256} from './sha256.js';
let acknowledge;
self.onmessage=async({data})=>{
  if(data.ack){acknowledge?.();return;}
  try{
    const hash=new Sha256();let pending=new Uint8Array(1048576),used=0,written=0;
    const send=async bytes=>{const ack=new Promise(resolve=>acknowledge=resolve);self.postMessage({chunk:bytes.buffer},[bytes.buffer]);await ack;};
    for await(const bytes of zipChunks(data.archive)){
      hash.update(bytes);written+=bytes.length;let offset=0;
      while(offset<bytes.length){const count=Math.min(bytes.length-offset,pending.length-used);pending.set(bytes.subarray(offset,offset+count),used);used+=count;offset+=count;if(used===pending.length){await send(pending);pending=new Uint8Array(1048576);used=0;}}
    }
    if(written!==data.archive.size)throw Error('ZIP 流长度不一致');
    const digest=hash.digest();let offset=0;while(offset<digest.length){const count=Math.min(digest.length-offset,pending.length-used);pending.set(digest.subarray(offset,offset+count),used);offset+=count;used+=count;if(used===pending.length){await send(pending);pending=new Uint8Array(1048576);used=0;}}
    if(used)await send(pending.slice(0,used));self.postMessage({done:true});
  }catch(error){self.postMessage({error:error.message||'ZIP 流生成失败'});}
};
