// Keep the relative path separate from File.name, including across relay uploads.
export function relativePath(value) {
  const path=value.replaceAll('\\','/');
  if(!path||path.startsWith('/')||path.split('/').some(part=>!part||part==='.'||part==='..'||part.includes(':')))
    throw new Error('文件相对路径无效');
  return path;
}
export function selectedFiles(files,directory=false) {
  return Array.from(files,file=>{
    if(directory&&!file.webkitRelativePath)throw new Error('当前系统文件选择器未返回文件夹层级。请使用支持目录选择的浏览器，或将文件夹压缩后通过“选择文件”发送。');
    return {file,path:relativePath(file.webkitRelativePath||file.name)};
  });
}
export function folderBatches(items,makeId) {
  const groups=new Map(), paths=new Set();
  for(const item of items) {
    if(paths.has(item.path))throw new Error('同一批次存在重复路径，请分别发送同名文件夹。');
    paths.add(item.path);
    if(!item.path.includes('/'))continue;
    const root=item.path.split('/')[0];
    if(!groups.has(root))groups.set(root,{id:makeId(),name:root,count:0,total:0,totalExact:'0'});
    const group=groups.get(root);group.count++;group.totalExact=String(BigInt(group.totalExact)+BigInt(item.file.size));group.total=Number(group.totalExact);
    if(!Number.isFinite(group.total))throw new Error('无法表示文件夹总大小。');
  }
  return items.map(item=>({...item,batch:groups.get(item.path.split('/')[0])}));
}
export async function droppedFiles(transfer) {
  // Read entries synchronously while the drop event still owns the data store.
  const items=Array.from(transfer.items||[]).filter(item=>item.kind==='file');
  const entries=items.map(item=>item.webkitGetAsEntry?.()||null);
  const fallback=Array.from(transfer.files||[]);
  if(!entries.some(Boolean))return selectedFiles(fallback);
  if(entries.some(entry=>!entry))throw new Error('部分拖放项目无法读取，请使用文件或文件夹选择按钮。');
  const result=[];
  async function visit(entry,parent='') {
    const path=relativePath(parent+entry.name);
    if(entry.isFile) {
      const file=await new Promise((resolve,reject)=>entry.file(resolve,reject));
      result.push({file,path});
    } else if(entry.isDirectory) {
      const reader=entry.createReader();
      for(;;) {
        const batch=await new Promise((resolve,reject)=>reader.readEntries(resolve,reject));
        if(!batch.length)break;
        for(const child of batch)await visit(child,path+'/');
      }
    }
  }
  for(const entry of entries)await visit(entry);
  return result;
}

export function batchPlan(files,makeId,maxFiles='0',maxBytes='0') {
  const entries=folderBatches(files,makeId), items=[], folders=new Map();
  let total=0n;
  for(const entry of entries) {
    total+=BigInt(entry.file.size);
    if(entry.batch) {
      if(!folders.has(entry.batch.id)) { folders.set(entry.batch.id,items.length);items.push({name:entry.batch.name,folder:true,count:String(entry.batch.count),total:entry.batch.totalExact}); }
      entry.item=folders.get(entry.batch.id);
    } else { entry.item=items.length;items.push({name:entry.path,folder:false,count:'1',total:String(entry.file.size)}); }
  }
  if((BigInt(maxFiles)&&BigInt(entries.length)>BigInt(maxFiles))||(BigInt(maxBytes)&&total>BigInt(maxBytes)))throw new Error('超过设置的单批文件数量或总大小上限。');
  if(total>18446744073709551615n)throw new Error('总大小超过协议可表示的整数范围。');
  return {id:makeId(),entries,items,total:String(total)};
}
export function limitValue(value,multiplier=1n) {
  if(!/^\d+$/.test(value))throw new Error('上限必须为非负整数；0 表示无限制。');
  const result=BigInt(value)*multiplier;
  if(result>18446744073709551615n)throw new Error('设置值超出可表示的整数范围。');
  return String(result);
}
