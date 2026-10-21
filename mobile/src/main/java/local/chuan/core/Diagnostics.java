package local.chuan.core;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.time.Instant;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.*;

/** Bounded asynchronous JSONL writer. Export reports omissions rather than silently hiding them. */
final class Diagnostics implements Closeable {
    final File directory;final Object io=new Object();final ArrayBlockingQueue<Object> queue=new ArrayBlockingQueue<>(16384);
    final AtomicLong dropped=new AtomicLong(),storageErrors=new AtomicLong();final String session=System.currentTimeMillis()+"-"+UUID.randomUUID();
    volatile boolean closed;Thread writer;OutputStream output;long bytes;int segment;
    Diagnostics(File directory){this.directory=directory;try{directory.mkdirs();open();writer=new Thread(this::run,"Chuan diagnostics");writer.setDaemon(true);writer.start();}catch(IOException error){dropped.incrementAndGet();storageErrors.incrementAndGet();}}
    void open()throws IOException{output=new BufferedOutputStream(new FileOutputStream(new File(directory,"transfer-"+session+"-"+(segment++)+".jsonl")));bytes=0;}
    void write(Map<String,Object> fields){try{fields.put("utc",Instant.now().toString());fields.put("mono_ms",System.nanoTime()/1000000.0);fields.put("platform","android");fields.put("diagnostic_session",session);if(writer==null||closed||!queue.offer(Json.stringify(fields)+"\n"))dropped.incrementAndGet();}catch(Exception error){dropped.incrementAndGet();}}
    void run(){try{while(!closed||!queue.isEmpty()){
        Object value=queue.poll(250,TimeUnit.MILLISECONDS);
        synchronized(io){try{
            if(value instanceof String){byte[] data=((String)value).getBytes(StandardCharsets.UTF_8);if(bytes>=16*1024*1024){output.close();open();}output.write(data);bytes+=data.length;}
            if(value==null||value instanceof CountDownLatch||queue.isEmpty())output.flush();
        }catch(IOException error){dropped.incrementAndGet();storageErrors.incrementAndGet();}finally{if(value instanceof CountDownLatch)((CountDownLatch)value).countDown();}}
    }}catch(InterruptedException error){Thread.currentThread().interrupt();}finally{synchronized(io){try{output.close();}catch(IOException ignored){}}}}
    void flush()throws IOException{if(writer==null)return;CountDownLatch fence=new CountDownLatch(1);try{if(!queue.offer(fence,5,TimeUnit.SECONDS)||!fence.await(10,TimeUnit.SECONDS))throw new IOException("日志刷新超时");}catch(InterruptedException error){Thread.currentThread().interrupt();throw new IOException(error);}}
    void export(File target,String client)throws IOException{
        flush();synchronized(io){File[] found=directory.listFiles(file->file.isFile()&&file.getName().startsWith("transfer-")&&file.getName().endsWith(".jsonl"));List<File> files=new ArrayList<>(found==null?Collections.emptyList():Arrays.asList(found));
            files.sort(Comparator.comparingLong(File::lastModified).reversed().thenComparing(File::getName));long size=0;int selected=0;for(File file:files){if(size+file.length()>256L*1024*1024)break;size+=file.length();selected++;}
            try(Writer result=new BufferedWriter(new OutputStreamWriter(new FileOutputStream(target),StandardCharsets.UTF_8))){
                result.write(Json.stringify(Json.map("event","export_manifest","diagnostic_schema",2,"platform","android","dropped_events",dropped.get(),"storage_errors",storageErrors.get(),"omitted_segments",files.size()-selected,"selected_segments",selected))+"\n");
                String owner="\"client_id\":"+Json.stringify(client),recipient="\"recipient_id\":"+Json.stringify(client);
                for(int i=selected-1;i>=0;i--)try(BufferedReader input=new BufferedReader(new InputStreamReader(new FileInputStream(files.get(i)),StandardCharsets.UTF_8))){String line;while((line=input.readLine())!=null)if(client.isEmpty()||line.contains("\"event\":\"session_start\"")||line.contains("\"event\":\"resources\"")||line.contains(owner)||line.contains(recipient)){result.write(line);result.write('\n');}}
            }
        }
    }
    void copy(Engine.Job job,String direction,InputStream input,OutputStream output,long count,java.security.MessageDigest hash,Protocol.Progress progress)throws IOException{
        long started=System.nanoTime(),sample=started,done=0,readNanos=0,writeNanos=0,hashNanos=0;String result="failed",error="";
        byte[] buffer=new byte[128*1024];
        try{while(done<count){
            long tick=System.nanoTime();int length=input.read(buffer,0,(int)Math.min(buffer.length,count-done));readNanos+=System.nanoTime()-tick;
            if(length<0)throw new EOFException("连接中断或文件长度已变更");if(length==0)continue;
            tick=System.nanoTime();output.write(buffer,0,length);writeNanos+=System.nanoTime()-tick;
            if(hash!=null){tick=System.nanoTime();hash.update(buffer,0,length);hashNanos+=System.nanoTime()-tick;}
            done+=length;if(progress!=null)progress.update(done);
            if(System.nanoTime()-sample>=1000000000L){ioEvent(job,direction,"payload_sample",done,started,readNanos,writeNanos,hashNanos,"running","");sample=System.nanoTime();}
        }result="payload_copied";}catch(IOException failure){error=failure.getMessage();throw failure;}
        finally{ioEvent(job,direction,"payload_end",done,started,readNanos,writeNanos,hashNanos,result,error);}
    }
    void ioEvent(Engine.Job job,String direction,String event,long done,long started,long read,long written,long hash,String result,String error){
        write(Json.map("event",event,"direction",direction,"id",job.id,"batch_id",job.batchId,"upload_id",job.upload,"path",job.name,"client_id",job.owner,"recipient_id",job.recipient,"done_bytes",done,"total_bytes",job.total,"elapsed_ms",(System.nanoTime()-started)/1000000.0,"read_call_ms",read/1000000.0,"write_call_ms",written/1000000.0,"hash_ms",hash/1000000.0,"result",result,"error",error));
    }
    public void close(){write(Json.map("event","session_end","dropped_events",dropped.get(),"storage_errors",storageErrors.get()));try{flush();}catch(IOException ignored){}closed=true;if(writer!=null)try{writer.join(1500);}catch(InterruptedException error){Thread.currentThread().interrupt();}}
}
