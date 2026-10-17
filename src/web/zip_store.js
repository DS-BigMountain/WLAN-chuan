import {Sha256} from './sha256.js';
import {relativePath} from './files.js';
const encoder=new TextEncoder(),tables=new Uint32Array(2048);
for(let n=0;n<256;n++){let c=n;for(let bit=0;bit<8;bit++)c=c&1?0xedb88320^(c>>>1):c>>>1;tables[n]=c;}
for(let n=256;n<2048;n++){const c=tables[n-256];tables[n]=(c>>>8)^tables[c&255];}
export function crc32(bytes,crc=0xffffffff){
  let i=0;for(;i+8<=bytes.length;i+=8){crc^=bytes[i]|bytes[i+1]<<8|bytes[i+2]<<16|bytes[i+3]<<24;crc=tables[1792+(crc&255)]^tables[1536+(crc>>>8&255)]^tables[1280+(crc>>>16&255)]^tables[1024+(crc>>>24)]^tables[768+bytes[i+4]]^tables[512+bytes[i+5]]^tables[256+bytes[i+6]]^tables[bytes[i+7]];}
  for(;i<bytes.length;i++)crc=(crc>>>8)^tables[(crc^bytes[i])&255];return crc>>>0;
}
function record(length){const bytes=new Uint8Array(length),view=new DataView(bytes.buffer);return {bytes,u16:(at,n)=>view.setUint16(at,n,true),u32:(at,n)=>view.setUint32(at,n,true),u64:(at,n)=>view.setBigUint64(at,BigInt(n),true)};}
export function zipPlan(sources,root,stamp=Date.now()){
  relativePath(root);if(root.includes('/'))throw Error('文件夹名称无效');
  const dirs=new Set([root+'/']),files=[],seen=new Set();let total=0n;
  for(const source of sources){const path=relativePath(source.path);if(!path.startsWith(root+'/')||seen.has(path))throw Error('文件夹路径重复或超出原目录：'+path);seen.add(path);const parts=path.split('/');for(let i=1;i<parts.length;i++)dirs.add(parts.slice(0,i).join('/')+'/');const size=BigInt(source.file.size);if(size<0n)throw Error('文件大小无效');files.push({...source,size:Number(size),directory:false});total+=size;}
  const entries=[...[...dirs].sort().map(path=>({path,size:0,directory:true})),...files];let offset=0n;
  for(const entry of entries){entry.name=encoder.encode(entry.path);if(entry.name.length>65535)throw Error('ZIP 文件路径过长：'+entry.path);entry.offset=Number(offset);entry.dataOffset=Number(offset+50n+BigInt(entry.name.length));offset+=74n+BigInt(entry.name.length)+BigInt(entry.size);}
  const centralOffset=offset,centralSize=entries.reduce((sum,entry)=>sum+74n+BigInt(entry.name.length),0n),size=offset+centralSize+98n;
  if(size>BigInt(Number.MAX_SAFE_INTEGER))throw Error('ZIP 大小超过浏览器可精确表示的范围');
  const date=new Date(stamp),year=Math.min(2107,Math.max(1980,date.getFullYear()));
  return {root,sources,entries,size:Number(size),total:Number(total),count:files.length,centralOffset:Number(centralOffset),centralSize:Number(centralSize),time:date.getHours()<<11|date.getMinutes()<<5|date.getSeconds()>>>1,date:(year-1980)<<9|(date.getMonth()+1)<<5|date.getDate(),stamp};
}
export function zipLocal(entry,plan){const r=record(50+entry.name.length);r.u32(0,0x04034b50);r.u16(4,45);r.u16(6,0x808);r.u16(10,plan.time);r.u16(12,plan.date);r.u32(18,0xffffffff);r.u32(22,0xffffffff);r.u16(26,entry.name.length);r.u16(28,20);r.bytes.set(entry.name,30);const at=30+entry.name.length;r.u16(at,1);r.u16(at+2,16);r.u64(at+4,entry.size);r.u64(at+12,entry.size);return r.bytes;}
export function zipDescriptor(entry,crc){const r=record(24);r.u32(0,0x08074b50);r.u32(4,crc);r.u64(8,entry.size);r.u64(16,entry.size);return r.bytes;}
export function zipCentral(entry,plan,crc){const r=record(74+entry.name.length);r.u32(0,0x02014b50);r.u16(4,45);r.u16(6,45);r.u16(8,0x808);r.u16(12,plan.time);r.u16(14,plan.date);r.u32(16,crc);r.u32(20,0xffffffff);r.u32(24,0xffffffff);r.u16(28,entry.name.length);r.u16(30,28);r.u32(38,entry.directory?16:0);r.u32(42,0xffffffff);r.bytes.set(entry.name,46);const at=46+entry.name.length;r.u16(at,1);r.u16(at+2,24);r.u64(at+4,entry.size);r.u64(at+12,entry.size);r.u64(at+20,entry.offset);return r.bytes;}
export function zipEnd(plan){const r=record(98),count=plan.entries.length,end=plan.centralOffset+plan.centralSize;r.u32(0,0x06064b50);r.u64(4,44);r.u16(12,45);r.u16(14,45);r.u64(24,count);r.u64(32,count);r.u64(40,plan.centralSize);r.u64(48,plan.centralOffset);r.u32(56,0x07064b50);r.u64(64,end);r.u32(72,1);r.u32(76,0x06054b50);r.u16(84,65535);r.u16(86,65535);r.u32(88,0xffffffff);r.u32(92,0xffffffff);return r.bytes;}
export function archiveSelection(items){
  const groups=new Map(),output=[],used=new Set(items.filter(item=>!item.path.includes('/')).map(item=>item.path));
  for(const item of items){if(!item.path.includes('/')){output.push(item);continue;}const root=item.path.split('/')[0];if(!groups.has(root)){const sources=[];groups.set(root,sources);output.push({root,sources});}groups.get(root).push(item);}
  return output.map(item=>{if(!item.sources)return item;const archive=zipPlan(item.sources,item.root);let name=item.root+'.zip',index=2;while(used.has(name))name=item.root+' ('+(index++)+').zip';used.add(name);archive.name=name;return {archive,file:{name,size:archive.size,lastModified:archive.stamp},path:name};});
}
export async function buildZip(archive,progress=()=>{},cancelled=()=>false){
  const parts=[],central=[],sha=new Sha256();let done=0,complete=0,last=0;
  const emit=bytes=>{parts.push(bytes);sha.update(bytes);};
  for(const entry of archive.entries){if(cancelled())throw new DOMException('打包已取消','AbortError');emit(zipLocal(entry,archive));let crc=0xffffffff;
    if(!entry.directory){try{if(!entry.size)await entry.file.slice(0,0).arrayBuffer();for(let offset=0;offset<entry.size;offset+=1048576){if(cancelled())throw new DOMException('打包已取消','AbortError');const data=new Uint8Array(await entry.file.slice(offset,Math.min(entry.size,offset+1048576)).arrayBuffer());if(data.length!==Math.min(1048576,entry.size-offset))throw Error('文件长度已变化');crc=crc32(data,crc);sha.update(data);done+=data.length;if(Date.now()-last>=100){progress({done,complete,path:entry.path});last=Date.now();}}}catch(error){if(error.name==='AbortError')throw error;throw Error('无法打包源文件：'+entry.path+'；'+(error.message||'文件已移动、被占用或已变化'));}parts.push(entry.file);complete++;}
    const checksum=(crc^0xffffffff)>>>0;emit(zipDescriptor(entry,checksum));central.push(zipCentral(entry,archive,checksum));
  }
  for(const bytes of central)emit(bytes);emit(zipEnd(archive));progress({done,complete,path:''});
  const file=new File(parts,archive.name||archive.root+'.zip',{type:'application/zip',lastModified:archive.stamp});if(file.size!==archive.size)throw Error('ZIP 文件长度不一致');return {file,digest:sha.digest()};
}
export function packZip(archive,progress,signal){return new Promise((resolve,reject)=>{
  if(signal?.aborted){reject(new DOMException('打包已取消','AbortError'));return;}
  const worker=new Worker(new URL('./zip_worker.js',import.meta.url),{type:'module'}),close=()=>{signal?.removeEventListener('abort',abort);worker.terminate();},abort=()=>{close();reject(new DOMException('打包已取消','AbortError'));};
  signal?.addEventListener('abort',abort,{once:true});worker.onmessage=event=>{if(event.data.progress){progress(event.data.progress);return;}close();event.data.error?reject(Error(event.data.error)):resolve(event.data);};worker.onerror=event=>{close();reject(Error(event.message||'ZIP 打包线程启动失败'));};worker.postMessage(archive);
});}
