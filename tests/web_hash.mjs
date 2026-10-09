import {readFileSync} from 'node:fs';
import {createHash,randomBytes} from 'node:crypto';
import assert from 'node:assert/strict';
const source=readFileSync(new URL('../src/web/sha256.js',import.meta.url));
const {Sha256}=await import(`data:text/javascript;base64,${source.toString('base64')}`);
let count=0;
for(const size of [0,1,3,55,56,63,64,65,119,120,127,128,129,1048593,10485767]) {
  const data=randomBytes(size),expected=createHash('sha256').update(data).digest('hex');
  for(const chunk of [1,37,65536]) {
    if(size>1048576&&chunk===1)continue;
    const hash=new Sha256();for(let i=0;i<size;i+=chunk)hash.update(data.subarray(i,i+chunk));
    assert.equal(Buffer.from(hash.digest()).toString('hex'),expected,`size=${size},chunk=${chunk}`);count++;
  }
}
assert.equal(Buffer.from(new Sha256().update(new TextEncoder().encode('abc')).digest()).toString('hex'),'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad');
console.log(`${count+1} incremental SHA-256 checks passed`);
