import {buildZip} from './zip_store.js';
self.onmessage=async event=>{try{const result=await buildZip(event.data,progress=>self.postMessage({progress}));self.postMessage(result);}catch(error){self.postMessage({error:error.message||'ZIP 打包失败'});}};
