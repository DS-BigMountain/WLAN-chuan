import assert from 'node:assert/strict';
import {createFilePipeline,FILE_PIPELINE_LIMITS} from '../src/web/transfers.js';
const gate=()=>{let release;return {promise:new Promise(resolve=>release=resolve),release:()=>release()};};
const until=async(fn,label)=>{const end=Date.now()+5000;while(!fn()){if(Date.now()>end)throw Error(label);await new Promise(resolve=>setImmediate(resolve));}};
assert.deepEqual(FILE_PIPELINE_LIMITS,{preparation:15,verification:15,transfer:4,ready:32});
const probe=gate(),verification=gate(),payload=gate();let probing=0,maxProbing=0,verifying=0,maxVerifying=0,transferring=0,maxTransferring=0,completed=0,maxReady=0;
const errors=[],snapshots=[];
const pipeline=createFilePipeline(async index=>{
  maxProbing=Math.max(maxProbing,++probing);await probe.promise;probing--;return index<48?'verify':true;
},async()=>{
  maxTransferring=Math.max(maxTransferring,++transferring);await payload.promise;transferring--;completed++;
},error=>errors.push(error),(phase,item,state)=>{maxReady=Math.max(maxReady,state.ready);snapshots.push(state);},async()=>{
  maxVerifying=Math.max(maxVerifying,++verifying);await verification.promise;verifying--;return false;
});
try{
  for(let i=0;i<80;i++)pipeline.push(i);
  await until(()=>probing===15,'metadata did not reach 15 concurrent tasks');assert.equal(verifying,0);assert.equal(transferring,0);
  probe.release();await until(()=>verifying===15&&transferring===4,'15 verifications must overlap four payloads');
  assert(pipeline.state().verification_pending>=33);assert(snapshots.every(s=>s.preparing<=15&&s.verifying<=15&&s.transferring<=4&&s.ready<=32));
  payload.release();await until(()=>completed===32,'verification backlog starved new files');assert.equal(verifying,15);
}finally{probe.release();payload.release();verification.release();await pipeline.idle();}
assert.equal(maxProbing,15);assert.equal(maxVerifying,15);assert.equal(maxTransferring,4);assert(maxReady<=32);assert.deepEqual(errors,[]);
console.log('PASS Web: 15 metadata tasks, 15 blocked verifications, four payloads, bounded 32-reference handoff, all missing files finish independently');
