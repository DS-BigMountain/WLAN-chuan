// Directed ZIP64 format and packing-failure checks; no user payloads are read.
import assert from 'node:assert/strict';
import {createHash,randomBytes} from 'node:crypto';
import {mkdir,writeFile} from 'node:fs/promises';
import {archiveSelection,buildZip,crc32,zipPlan,zipLocal,zipCentral,zipEnd} from '../src/web/zip_store.js';
const data=randomBytes(1024*1024+13),crc=crc32(data),middle=1337;
assert.equal(crc,crc32(data.subarray(middle),crc32(data.subarray(0,middle))));
assert.equal((crc32(new TextEncoder().encode('123456789'))^0xffffffff)>>>0,0xcbf43926);
const ordinary=new File(['ordinary'],'plain.txt'),input=[{file:ordinary,path:'plain.txt'},{file:new File([data],'数据.bin'),path:'资料/子目录/数据.bin'},{file:new File([],'空.txt'),path:'资料/空.txt'}];
const selection=archiveSelection(input);assert.equal(selection.length,2);assert.equal(selection[0].file,ordinary);assert.equal(selection[1].path,'资料.zip');
const packed=await buildZip(selection[1].archive),bytes=new Uint8Array(await packed.file.arrayBuffer());assert.equal(bytes.length,selection[1].file.size);assert.deepEqual(Buffer.from(packed.digest),createHash('sha256').update(bytes).digest());
await mkdir('build/zip-check',{recursive:true});await writeFile('build/zip-check/browser.zip',bytes);await writeFile('build/zip-check/browser-source.bin',data);
const large=zipPlan([{file:{size:6*1024**3},path:'HT/大文件.bin'},{file:{size:9},path:'HT/last.txt'}],'HT');assert(large.size>0xffffffff);const entry=large.entries.at(-1),local=zipLocal(large.entries.find(item=>item.size>0xffffffff),large),central=zipCentral(entry,large,0),end=zipEnd(large);assert.equal(new DataView(local.buffer).getBigUint64(local.length-16,true),6n*1024n**3n);assert.equal(new DataView(central.buffer).getBigUint64(central.length-8,true),BigInt(entry.offset));assert.equal(new DataView(end.buffer).getBigUint64(48,true),BigInt(large.centralOffset));
const bad=zipPlan([{path:'坏目录/不可读.txt',file:{size:1,slice(){return {arrayBuffer(){throw Error('locked');}};}}}],'坏目录');await assert.rejects(()=>buildZip(bad),/坏目录\/不可读.txt/);
await assert.rejects(()=>buildZip(selection[1].archive,()=>{},()=>true),{name:'AbortError'});
assert.equal(archiveSelection([{file:new File([],'资料.zip'),path:'资料.zip'},...input.slice(1)])[1].path,'资料 (2).zip');
console.log('PASS STORE ZIP64, CRC32, SHA256, Unicode hierarchy, zero-length file, 6 GiB headers, unreadable source, cancellation, plain-file identity');
