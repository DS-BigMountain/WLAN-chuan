package local.chuan.core;
import java.io.*;
import java.net.*;
import java.nio.charset.StandardCharsets;
import java.util.*;
import java.util.concurrent.*;
import static local.chuan.core.Engine.*;
import static local.chuan.core.Protocol.*;

/** Each browser stream has one bounded pipe or one direct native upload socket. */
final class WebArchive implements Closeable {
    final Engine e;final Map<String,Stream> streams=new ConcurrentHashMap<>();
    static final class Stream implements Closeable {
        final Engine engine;Stream(Engine engine){this.engine=engine;}
        String client,tab;Socket socket;OutputStream writer;InputStream response;Job job;
        long total,received,offset;volatile long touched=now();CompletableFuture<byte[]> result;
        public void close(){try{if(socket!=null)socket.close();}catch(IOException ignored){}try{if(writer!=null)writer.close();}catch(IOException ignored){}if(socket!=null){engine.sockets.remove(socket);if(job!=null)job.sockets.remove(socket);}}
    }
    WebArchive(Engine engine){e=engine;}
    public void close(){for(Stream stream:streams.values())stream.close();streams.clear();}
    void handle(InputStream input,OutputStream output,Header h,String ip,String path)throws IOException {
        long length=h.length();String client=h.get("x-chuan-client"),tab=h.get("x-chuan-tab");
        if(!h.get("transfer-encoding").isEmpty())throw new IOException("流式请求格式无效");
        if(path.equals("/web/api/archive/open")){
            long total=number(h.get("x-chuan-size")),offset=h.get("x-chuan-offset").isEmpty()?0:number(h.get("x-chuan-offset"));int index=(int)number(h.get("x-chuan-item"));String name=safePath(unhex(h.get("x-chuan-path"))),target=unhex(h.get("x-chuan-target"));Peer peer=null;
            if(length!=0||!key(h.get("x-chuan-upload"))||!name.endsWith(".zip")||total>9007199254740959L||offset>total||!h.get("x-chuan-folder").isEmpty())throw new IOException("ZIP 流参数无效");
            synchronized(e.lock){
                Batch batch=e.batches.get(e.transfers.batchKey(h,ip,true));
                if(batch==null||index<0||index>=batch.items.size()||index>=batch.selection.length()||batch.selection.charAt(index)!='1'||batch.cancelledItems.contains(index)||!batch.target.equals(h.get("x-chuan-target")))throw new IOException("ZIP 没有对应批准范围");
                Item item=batch.items.get(index);if(item.folder||!item.name.equals(name)||item.total!=total)throw new IOException("ZIP 与批准项目不一致");
                if(!target.isEmpty()&&!target.equals(e.config.id))peer=e.peer(target);
            }
            for(Map.Entry<String,Stream> entry:streams.entrySet())if(now()-entry.getValue().touched>45000){entry.getValue().close();streams.remove(entry.getKey());}
            Stream stream=new Stream(e);stream.client=client;stream.tab=tab;stream.total=total-offset+32;stream.offset=offset;
            String id=random();synchronized(streams){long owned=streams.values().stream().filter(s->s.client.equals(client)).count();if(streams.size()>=16||owned>=4)throw new IOException("ZIP 流通道已满");streams.put(id,stream);}
            boolean ready=false;
            try{
                if(peer!=null&&(!peer.browser||!peer.owner.isEmpty())){
                    Job job=null;synchronized(e.lock){for(Job previous:e.jobs.values())if(previous.owner.equals(client)&&previous.upload.equals(h.get("x-chuan-upload"))&&!previous.receiving){if(previous.state<3)throw new IOException("该 ZIP 已在发送");job=previous;break;}}
                    if(job!=null)e.request(peer,"POST","/api/retry-upload/"+h.get("x-chuan-upload"),Collections.singletonMap("X-Chuan-Batch",h.get("x-chuan-batch")),"",5000);
                    if(job==null)job=e.job(name,peer.name,total,false);stream.job=job;
                    job.owner=client;job.webTab=tab;job.upload=h.get("x-chuan-upload");job.batchId=h.get("x-chuan-batch");job.item=index;job.target=target;job.activity="边打包边转发 ZIP";e.state(job,1,"");
                    Socket socket=e.connect(peer.gateway.isEmpty()?peer.ip:peer.gateway,peer.port,150000);stream.socket=socket;e.sockets.add(socket);job.sockets.add(socket);stream.writer=new BufferedOutputStream(socket.getOutputStream());stream.response=new BufferedInputStream(socket.getInputStream());
                    Map<String,String> fields=e.identity(peer);fields.put("X-Chuan-Path",h.get("x-chuan-path"));fields.put("X-Chuan-Size",String.valueOf(total));fields.put("X-Chuan-Display-Sender",h.get("x-chuan-sender"));fields.put("X-Chuan-Batch",job.batchId);fields.put("X-Chuan-Item",String.valueOf(index));fields.put("X-Chuan-Upload",job.upload);fields.put("X-Chuan-Recipient",peer.browser?hex(peer.id):"");fields.put("X-Chuan-Resume-Key",h.get("x-chuan-resume-key"));fields.put("X-Chuan-Offset",String.valueOf(offset));fields.put("X-Chuan-Prefix",h.get("x-chuan-prefix"));fields.put("Content-Length",String.valueOf(stream.total));
                    StringBuilder request=new StringBuilder("POST /api/upload HTTP/1.1\r\nHost: "+peer.ip+":"+peer.port+"\r\nConnection: close\r\nExpect: 100-continue\r\n");for(Map.Entry<String,String> field:fields.entrySet())request.append(field.getKey()).append(": ").append(field.getValue()).append("\r\n");ascii(stream.writer,request+"\r\n");
                    Header answer;do{answer=header(stream.response);}while(answer.status()==102);if(answer.status()!=100)throw new IOException("接收端未允许 ZIP 传输："+new String(read(stream.response,answer.length(),4096),StandardCharsets.UTF_8));e.state(job,2,"");
                }else{
                    PipedInputStream consumer=new PipedInputStream(1024*1024);PipedOutputStream producer=new PipedOutputStream(consumer);stream.writer=producer;ByteArrayOutputStream result=new ByteArrayOutputStream();
                    Socket pipeSocket=new Socket(){@Override public synchronized void close()throws IOException{consumer.close();producer.close();super.close();}};stream.socket=pipeSocket;e.sockets.add(pipeSocket);
                    Header inner=new Header();inner.fields.putAll(h.fields);inner.fields.put("content-length",String.valueOf(stream.total));inner.fields.put("x-chuan-checksum","sha256");
                    stream.result=new CompletableFuture<>();Thread worker=new Thread(()->{try{e.transfers.upload(pipeSocket,consumer,result,inner,ip,true);stream.result.complete(result.toByteArray());}catch(Exception error){stream.result.completeExceptionally(error);}finally{e.sockets.remove(pipeSocket);try{consumer.close();}catch(IOException ignored){}}},"Chuan browser ZIP stream");worker.setDaemon(true);worker.start();
                }
                e.diagnostics.write(Json.map("event","web_archive_stream_open","client_id",client,"upload_id",h.get("x-chuan-upload"),"direct",stream.job!=null));
                response(output,200,"OK",Json.stringify(Json.map("stream",id,"forwarded",stream.job!=null,"id",stream.job==null?"":String.valueOf(stream.job.id))),"application/json");ready=true;
            }finally{if(!ready){stream.close();streams.remove(id);if(stream.job!=null)e.state(stream.job,5,"ZIP 流建立失败");}}
            return;
        }
        String id=path.substring(path.lastIndexOf('/')+1);Stream stream=streams.get(id);
        if(stream==null||!stream.client.equals(client)||!stream.tab.equals(tab))throw new IOException("ZIP 流不属于当前页面");
        if(path.startsWith("/web/api/archive/abort/")){if(length!=0)throw new IOException("取消请求无效");if(stream.job!=null){boolean paused=h.get("x-chuan-paused").equals("1"),failed=paused||h.get("x-chuan-abort-failed").equals("1");if(paused)e.pause(stream.job.id,true);stream.job.cancelled=!failed;String reason=unhex(h.get("x-chuan-error"));e.state(stream.job,failed?5:4,reason.length()>1024?"ZIP 流中断":reason);}stream.close();streams.remove(id);response(output,200,"OK","","text/plain");return;}
        if(!path.startsWith("/web/api/archive/chunk/"))throw new IOException("ZIP 流命令无效");
        synchronized(stream){
            if(number(h.get("x-chuan-offset"))!=stream.received||length<1||length>1024*1024||length>stream.total-stream.received)throw new IOException("ZIP 分块偏移或大小无效");
            boolean complete=false,failed=false;
            try{
                byte[] bytes=read(input,length,1024*1024);stream.writer.write(bytes);stream.writer.flush();stream.received+=length;stream.touched=now();if(stream.job!=null)e.progress(stream.job,stream.offset+Math.min(stream.received,stream.total-32));
                if(stream.received<stream.total){response(output,202,"Accepted","{}","application/json");return;}
                InputStream reply=stream.response;if(stream.result!=null){stream.writer.close();reply=new ByteArrayInputStream(stream.result.get(150,TimeUnit.SECONDS));}
                Header answer;do{answer=header(reply);}while(answer.status()==102);String body=new String(read(reply,answer.length(),131072),StandardCharsets.UTF_8);
                if(answer.status()!=201)throw new IOException("接收端未确认 ZIP 保存："+body);
                if(stream.job!=null){e.state(stream.job,3,"");body=Json.stringify(Json.map("id",String.valueOf(stream.job.id),"forwarded",true));}
                response(output,201,"Created",body,"application/json");complete=true;
            }catch(Exception error){failed=true;if(stream.job!=null){if(!stream.job.cancelled)try{String state=e.request(e.peer(stream.job.target),"POST","/api/upload-status/"+stream.job.upload,Collections.singletonMap("X-Chuan-Batch",stream.job.batchId),"",5000);if(state.equals("4"))stream.job.cancelled=true;}catch(IOException ignored){}e.state(stream.job,stream.job.cancelled?4:5,error.getMessage());if(stream.job.cancelled){response(output,410,"Gone","接收端已取消本次传输","text/plain; charset=utf-8");return;}}throw new IOException("ZIP 流式上传未完成："+error.getMessage(),error);}
            finally{if(failed||complete||stream.received==stream.total||stream.job!=null&&stream.job.state>=3){stream.close();streams.remove(id);if(stream.socket!=null){e.sockets.remove(stream.socket);if(stream.job!=null)stream.job.sockets.remove(stream.socket);}}}
        }
    }
}
