package local.chuan.core;
import java.io.*;import java.nio.charset.StandardCharsets;import java.security.MessageDigest;import java.util.*;
import static local.chuan.core.Engine.*;import static local.chuan.core.Protocol.*;

/** Persistent candidate data is accepted only after its full prefix is verified. */
final class Resume {
    static boolean keyValid(String key){return key.matches("[0-9a-f]{64}");}
    static File file(File base,String owner,String name,long total,String key){return new File(base,".chuan-resume-"+hex(sha().digest((owner+":"+name+":"+total+":"+key).getBytes(StandardCharsets.UTF_8)))+".part");}
    static String fingerprint(Source source)throws IOException{if(!source.fingerprint.isEmpty())return source.fingerprint;return hex(sha().digest(("chuan-resume-v1:"+source.size).getBytes(StandardCharsets.UTF_8)));}
    static void skip(InputStream input,long count)throws IOException{while(count>0){long step=input.skip(count);if(step==0){if(input.read()<0)throw new EOFException("源文件长度发生变化");step=1;}count-=step;}}
    static String prefix(InputStream input,long count,MessageDigest full,Engine.Check check)throws IOException{MessageDigest hash=sha();byte[] bytes=new byte[1024*1024];while(count>0){check.run();int n=input.read(bytes,0,(int)Math.min(bytes.length,count));if(n<0)throw new EOFException("源文件长度发生变化");if(n==0)continue;hash.update(bytes,0,n);if(full!=null)full.update(bytes,0,n);count-=n;}return hex(hash.digest());}
    static String probe(Engine e,Header h,String ip,boolean browser)throws IOException{
        String key=h.get("x-chuan-resume-key"),owner=h.get(browser?"x-chuan-client":"x-chuan-id"),name=safePath(unhex(h.get("x-chuan-path")));long total=number(h.get("x-chuan-size")),index=number(h.get("x-chuan-item"));
        if(h.length()!=0||!keyValid(key)||name.isEmpty())throw new IOException("续传参数无效");
        synchronized(e.lock){Batch batch=e.batches.get(e.transfers.batchKey(h,ip,browser));if(batch==null||index>=batch.items.size()||batch.selection.charAt((int)index)!='1'||batch.cancelledItems.contains((int)index)||!batch.target.equals(h.get("x-chuan-target")))throw new IOException("续传没有对应批准范围");Item item=batch.items.get((int)index);if(!(item.folder?name.startsWith(item.name+"/"):name.equals(item.name))||total>item.total)throw new IOException("续传与批准项目不一致");batch.touched=now();}
        String target=unhex(h.get("x-chuan-target"));if(browser&&h.get("x-chuan-resume-direct").equals("1")&&!target.isEmpty()&&!target.equals(e.config.id)){Peer peer=e.peer(target);if(!peer.browser||!peer.owner.isEmpty()){Map<String,String> fields=new LinkedHashMap<>();for(String field:Arrays.asList("x-chuan-resume-key","x-chuan-resume-reset","x-chuan-path","x-chuan-size","x-chuan-batch","x-chuan-item","x-chuan-upload"))fields.put(field,h.get(field));return e.request(peer,"POST","/api/resume",fields,"",150000);}}
        File base=browser&&!target.isEmpty()&&!target.equals(e.config.id)?e.config.relayDirectory:e.config.receiveDirectory;if(!base.isDirectory()&&!base.mkdirs())throw new IOException("无法创建接收目录");File part=file(base,owner,name,total,key);Transfers.safeDestination(base,part);long offset=part.exists()?part.length():0;if(offset>total)throw new IOException("续传长度无效");
        if(h.get("x-chuan-resume-reset").equals("1")&&part.exists())try(RandomAccessFile data=new RandomAccessFile(part,"rw");java.nio.channels.FileLock lock=data.getChannel().tryLock()){if(lock==null)throw new IOException("续传数据正在使用");data.setLength(0);offset=0;}
        boolean paused=false;synchronized(e.lock){for(Job job:e.jobs.values())if(job.batchId.equals(h.get("x-chuan-batch"))&&job.upload.equals(h.get("x-chuan-upload"))&&(browser?job.owner:job.nativeOwner).equals(owner))paused=job.paused;}return Json.stringify(Json.map("offset",offset,"paused",paused));
    }
}
