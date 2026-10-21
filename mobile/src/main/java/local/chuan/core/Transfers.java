package local.chuan.core;

import java.io.*;
import java.net.*;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.*;
import static local.chuan.core.Protocol.*;
import static local.chuan.core.Engine.*;

/** Batch consent, streamed payloads, SHA-256 trailers and browser delivery. */
final class Transfers {
    final Engine e;
    Transfers(Engine engine){e=engine;}
    void phase(InputStream input,OutputStream output,Header h,String ip,String upload,boolean browser)throws IOException {
        String phase=new String(read(input,h.length(),16),StandardCharsets.UTF_8);
        if(!key(upload)||!(phase.equals("pack")||phase.equals("hash")))throw new IOException("处理阶段无效");
        synchronized(e.lock){Batch batch=e.batches.get(batchKey(h,ip,browser));if(batch==null)throw new IOException("批次不存在");
            for(Job job:e.jobs.values())if(job.announced&&job.upload.equals(upload)&&job.batchId.equals(h.get("x-chuan-batch"))&&(browser?job.owner:job.nativeOwner).equals(h.get(browser?"x-chuan-client":"x-chuan-id"))){
                if(job.cancelled||batch.cancelledItems.contains(job.item)){response(output,410,"Gone","","text/plain");return;}
                job.activity=phase.equals("pack")?"发送端正在打包文件":"等待发送端核对同名文件";e.trace(job);batch.touched=now();response(output,200,"OK","","text/plain");return;
            }
        }response(output,404,"Not Found","","text/plain");
    }
    void retryUpload(OutputStream output,Header h,String ip,String upload,boolean browser)throws IOException {
        synchronized(e.lock){Batch batch=e.batches.get(batchKey(h,ip,browser));if(batch==null){response(output,404,"Not Found","", "text/plain");return;}
            String owner=h.get(browser?"x-chuan-client":"x-chuan-id");
            for(Job job:e.jobs.values())if(job.receiving&&job.batchId.equals(batch.id)&&job.upload.equals(upload)&&(browser?job.owner:job.nativeOwner).equals(owner)){
                if(job.cancelled||batch.cancelledItems.contains(job.item)||job.state==6){response(output,410,"Gone","", "text/plain");return;}
                if(job.paused){batch.touched=now();response(output,200,"OK","7","text/plain");return;}if(job.state==3){response(output,200,"OK","3", "text/plain");return;}
                if(!job.recipient.isEmpty()&&job.source!=null&&job.state!=5){batch.touched=now();response(output,200,"OK","8","text/plain");return;}
                if(job.state==5){if(batch.paths.remove(job.item+":"+job.name.toLowerCase(Locale.ROOT))){batch.counts[job.item]--;batch.bytes[job.item]-=job.total;}job.state=0;job.done=0;job.error="";job.activity="等待重试数据";e.trace(job);job.announced=true;job.nativeHidden=false;job.hiddenClients.clear();batch.touched=now();}
                response(output,job.state==0?200:409,job.state==0?"OK":"Conflict","0", "text/plain");return;
            }response(output,200,"OK","0", "text/plain");
        }
    }
    static boolean permanent(String message){return message!=null&&(message.contains("同名文件")||message.contains("类型冲突")||message.contains("原文件已保留")||message.contains("源文件")||message.contains("文件长度发生变化")||message.contains("文件路径已变更"));}
    void queueRetry(Peer peer,Job job){if(permanent(job.error)){try{e.request(peer,"POST","/api/batch-status",Collections.singletonMap("X-Chuan-Batch",job.batchId),job.upload+"\t5\t"+hex(job.error)+"\n",4000);}catch(IOException ignored){}return;}synchronized(e.lock){if(job.cancelled||job.retryDeadline!=0||!e.running)return;job.retryDeadline=now()+30000;job.retryExpired=false;job.retryPending=true;job.state=1;job.error="";job.activity="独立通道自动重试中";e.trace(job);}
        e.scheduler.schedule(()->{synchronized(e.lock){if(job.retryDeadline!=0&&now()>=job.retryDeadline&&job.state!=3&&!job.cancelled&&!job.paused){job.retryExpired=true;for(Socket socket:job.sockets)try{socket.close();}catch(IOException ignored){}e.state(job,5,"传输失败超时（自动重试已达 30 秒）");}}},30,java.util.concurrent.TimeUnit.SECONDS);
        e.retries.execute(()->retryAttempt(peer,job));
    }
    void retryAttempt(Peer peer,Job job){
        if(!e.running||job.cancelled){job.retryDeadline=0;job.retryPending=false;return;}
        if(job.paused&&!job.remotePaused){job.retryDeadline=now()+30000;e.retries.schedule(()->retryAttempt(peer,job),400,java.util.concurrent.TimeUnit.MILLISECONDS);return;}if(job.remotePaused)job.retryDeadline=now()+30000;
        if(now()>=job.retryDeadline)job.retryExpired=true;
        if(!job.retryExpired)try{String state=e.request(peer,"POST","/api/retry-upload/"+job.upload,Collections.singletonMap("X-Chuan-Batch",job.batchId),"",4000,job);if(state.equals("7")){job.remotePaused=true;job.paused=true;job.retryDeadline=now()+30000;}else if(state.equals("3")){job.done=job.total;e.state(job,3,"");}else if(state.equals("8")){job.remotePaused=false;job.paused=false;job.retryDeadline=now()+30000;job.activity="等待浏览器继续保存";}else{if(job.remotePaused){job.remotePaused=false;job.paused=false;job.retryDeadline=now()+30000;}sendOne(peer,job);}}catch(IOException error){if(error.getMessage()!=null&&error.getMessage().contains("410")){job.cancelled=true;e.state(job,4,"接收端已取消本次传输");}else e.state(job,5,error.getMessage());}
        synchronized(e.lock){if(job.state==3||job.cancelled||job.retryExpired||permanent(job.error)){job.retryDeadline=0;job.retryPending=false;job.activity="";e.trace(job);if(job.retryExpired&&job.state!=3)e.state(job,5,"传输失败超时（自动重试已达 30 秒）");}
            else{job.state=1;job.activity="独立通道自动重试中";e.trace(job);e.retries.schedule(()->retryAttempt(peer,job),400,java.util.concurrent.TimeUnit.MILLISECONDS);return;}}
        if(job.retryExpired||permanent(job.error))try{e.request(peer,"POST","/api/batch-status",Collections.singletonMap("X-Chuan-Batch",job.batchId),job.upload+"\t5\t"+hex(job.error)+"\n",4000);}catch(IOException ignored){}
    }
    void batchFiles(InputStream input,OutputStream output,Header h,String ip,Session session,boolean status)throws IOException {
        boolean browser=session!=null;String owner=browser?session.client:h.get("x-chuan-id"),display=browser?session.name:sender(h);
        String batchId=h.get("x-chuan-batch");if(!key(batchId))throw new IOException("批次标识无效");
        String text=new String(read(input,h.length(),16*1024*1024),StandardCharsets.UTF_8);List<String[]> entries=new ArrayList<>();
        for(String line:text.split("\n")){String[] fields=line.split("\t",-1);if(status?fields.length!=3:fields.length!=5&&fields.length!=6)throw new IOException("批次清单格式无效");if(!status&&fields.length==6&&!(fields[5].equals("A")&&unhex(fields[3]).endsWith(".zip")||fields[5].equals("D")&&number(fields[4])==0))throw new IOException("ZIP 清单格式无效");entries.add(fields);}if(entries.isEmpty()||entries.size()>50000)throw new IOException("批次清单数量无效");
        synchronized(e.lock){Batch batch=e.batches.get((browser?"web:":"native:")+ip+":"+owner+":"+batchId);if(batch==null||batch.selection.length()!=batch.items.size()||!batch.target.isEmpty()&&!unhex(batch.target).equals(e.config.id))throw new IOException("没有对应的本机批次授权");
            if(status){Map<String,Job> owned=new HashMap<>();for(Job job:e.jobs.values())if((browser?job.owner:job.nativeOwner).equals(owner)&&job.batchId.equals(batchId))owned.put(job.upload,job);
                for(String[] fields:entries){long state=number(fields[1]);String error=unhex(fields[2]);if(!owned.containsKey(fields[0])||(state!=4&&state!=5)||error.isEmpty()||error.length()>1024||error.indexOf('\r')>=0||error.indexOf('\n')>=0)throw new IOException("批次状态或任务归属无效");}
                for(String[] fields:entries){Job job=owned.get(fields[0]);if(job.state!=3&&!job.cancelled){job.announced=false;if(number(fields[1])==4){job.cancelled=true;batch.cancelledItems.add(job.item);}e.state(job,(int)number(fields[1]),unhex(fields[2]));}}response(output,200,"OK","{}","application/json; charset=utf-8");return;
            }
            if(e.jobs.size()+entries.size()>100000)throw new IOException("清单已登记或任务数量超过上限");
            long[] counts=new long[batch.items.size()],sizes=new long[batch.items.size()];Set<String> paths=new HashSet<>(),uploads=new HashSet<>();Map<Integer,String> folders=new HashMap<>();
            for(String[] fields:entries){int index=(int)number(fields[0]);if(index<0||index>=batch.items.size()||batch.selection.charAt(index)!='1'||batch.announcedItems.contains(index)||!key(fields[1]))throw new IOException("清单超出批准范围");Item item=batch.items.get(index);String name=safePath(unhex(fields[3]));long size=number(fields[4]);
                if(!paths.add(name)||!uploads.add(fields[1])||size>item.total-sizes[index]||(item.folder?!key(fields[2])||!(name.startsWith(item.name+"/")||fields.length==6&&fields[5].equals("D")&&name.equals(item.name)):!fields[2].isEmpty()||!name.equals(item.name)))throw new IOException("文件路径、数量或大小不匹配");
                if(item.folder&&folders.putIfAbsent(index,fields[2])!=null&&!folders.get(index).equals(fields[2]))throw new IOException("文件夹标识不一致");counts[index]++;sizes[index]+=size;
                if(fields.length==6&&(fields[5].equals("A")&&item.folder||fields[5].equals("D")&&!item.folder))throw new IOException("ZIP 必须作为单文件传输");
            }
            for(int i=0;i<batch.items.size();i++)if(counts[i]>0&&(counts[i]!=batch.items.get(i).count||sizes[i]!=batch.items.get(i).total))throw new IOException("清单总数量或总大小不匹配");
            for(String[] fields:entries){int index=(int)number(fields[0]);Item item=batch.items.get(index);Job job=e.job(safePath(unhex(fields[3])),display,number(fields[4]),true);job.owner=browser?owner:"";job.webTab=h.get("x-chuan-tab").isEmpty()?owner:h.get("x-chuan-tab");job.nativeOwner=browser?"":owner;job.upload=fields[1];job.batchId=batchId;job.item=index;job.target=e.config.id;job.displaySender=display;job.announced=true;job.stage=browser?"upload":"";job.storageKey=batch.key+":";if(fields.length==6){job.directory=fields[5].equals("D");}
                if(item.folder){job.folderId=fields[2];job.folderName=item.name;job.folderCount=item.count;job.folderTotal=item.total;}}
            for(String[] fields:entries)batch.announcedItems.add((int)number(fields[0]));
            batch.announced=true;response(output,200,"OK","{}","application/json; charset=utf-8");
        }
    }
    String manifest(List<Item> items){StringBuilder out=new StringBuilder();for(Item item:items)out.append(hex(item.name)).append('\t').append(item.folder?'D':'F').append('\t').append(item.count).append('\t').append(item.total).append('\n');return out.toString();}
    List<Item> parseManifest(String body)throws IOException {
        List<Item> out=new ArrayList<>();Set<String> roots=new HashSet<>();long count=0,total=0;
        if(!body.endsWith("\n"))throw new IOException("批次清单不完整");
        for(String line:body.split("\n")){String[] fields=line.split("\t",-1);if(fields.length!=4||(!fields[1].equals("D")&&!fields[1].equals("F")))throw new IOException("批次清单无效");String name=safePath(unhex(fields[0]));long files=number(fields[2]),bytes=number(fields[3]);boolean folder=fields[1].equals("D");if(name.contains("/")||files<1||(!folder&&files!=1)||!roots.add(name.toLowerCase(Locale.ROOT)))throw new IOException("批次项目无效或名称重复");count=Math.addExact(count,files);total=Math.addExact(total,bytes);out.add(new Item(name,folder,files,bytes));}
        if(out.isEmpty())throw new IOException("批次为空");limits(count,total,e.config.maxFiles,e.config.maxBytes);return out;
    }
    void limits(long count,long total,long maxFiles,long maxBytes)throws IOException{if((maxFiles>0&&count>maxFiles)||(maxBytes>0&&total>maxBytes))throw new LimitException();}
    static final class LimitException extends IOException {LimitException(){super("超过单批文件数量或总大小上限");}}
    interface SelectionHandler {void changed(String mask)throws IOException;}
    interface Cancellation {String mask();}
    String sendOffer(Peer peer,String batch,List<Item> items,String display,Job control)throws IOException {return sendOffer(peer,batch,items,display,control,null,null,null);}
    String sendOffer(Peer peer,String batch,List<Item> items,String display,Job control,OutputStream waitingOutput)throws IOException {return sendOffer(peer,batch,items,display,control,waitingOutput,null,null);}
    String sendOffer(Peer peer,String batch,List<Item> items,String display,Job control,OutputStream waitingOutput,SelectionHandler progress,Cancellation cancelled)throws IOException {
        Peer route=peer.copy();if(peer.browser&&peer.owner.isEmpty()){route.ip="127.0.0.1";route.port=e.config.port;route.gateway="";route.owner=e.config.id;}
        Map<String,String> headers=new LinkedHashMap<>();headers.put("X-Chuan-Batch",batch);headers.put("X-Chuan-Recipient",peer.browser?hex(peer.id):"");headers.put("X-Chuan-Display-Sender",hex(display));headers.put("X-Chuan-Offer-Wait","1");headers.put("X-Chuan-Offer-Async","1");
        String selected=e.request(route,"POST","/api/offer",headers,manifest(items),130000,control,waitingOutput);
        if(!selected.startsWith("{")){if(selected.length()!=items.size()||!selected.matches("[01]+"))throw new IOException("接收方的批次审批响应无效");if(progress!=null)progress.changed(selected);return selected;}
        String last="",sentCancel="";long failedAt=0,keepalive=now();
        while(e.running){
            String mask=control!=null&&control.cancelled?repeat('0',items.size()):cancelled==null?"":cancelled.mask();
            if(!mask.isEmpty()&&!mask.equals(sentCancel))try{e.request(route,"POST","/api/offer-cancel",Collections.singletonMap("X-Chuan-Batch",batch),mask,5000);sentCancel=mask;}catch(IOException ignored){}
            if(control!=null&&control.cancelled)throw new IOException("发送已取消");
            String answer;
            try{answer=e.request(route,"POST","/api/offer-status",Collections.singletonMap("X-Chuan-Batch",batch),"",5000,control);failedAt=0;}
            catch(IOException error){if(failedAt==0)failedAt=now();if(now()-failedAt>15000)throw new IOException("无法获取接收确认，请检查连接后重试",error);answer="";}
            if(!answer.isEmpty()){
                String[] fields=answer.split("\t",-1);if(fields.length!=3||fields[1].length()!=items.size()||!fields[1].matches("[01-]+"))throw new IOException("批次审批响应无效");int state=Integer.parseInt(fields[0]);selected=fields[1];
                if(state==4||state==5)throw new IOException(fields[2].isEmpty()?"接收申请已停止":unhex(fields[2]));
                if(!last.equals(selected)){last=selected;if(progress!=null)progress.changed(selected);}if(selected.indexOf('-')<0)return selected;
            }
            if(waitingOutput!=null&&now()-keepalive>10000){ascii(waitingOutput,"HTTP/1.1 102 Processing\r\n\r\n");keepalive=now();}
            try{Thread.sleep(250);}catch(InterruptedException error){Thread.currentThread().interrupt();throw new IOException(error);}
        }throw new IOException("服务已停止");
    }
    void offerStatus(InputStream input,OutputStream output,Header h,String ip,boolean browser,boolean cancel)throws IOException {
        String key=batchKey(h,ip,browser),mask=new String(read(input,h.length(),65536),StandardCharsets.US_ASCII);List<Long> cancelled=new ArrayList<>();String answer;
        synchronized(e.lock){Job found=null;for(Job job:e.jobs.values())if(job.asyncOfferKey.equals(key)){found=job;break;}if(found==null){response(output,404,"Not Found","","text/plain");return;}final Job job=found;
            if(cancel){if(mask.length()!=job.offerItems.size()||!mask.matches("[0-]+"))throw new IOException("取消范围无效");completeOffer(job,mask);for(int i=0;i<mask.length();i++)if(mask.charAt(i)=='0'){e.batches.get(key).cancelledItems.add(i);for(Job item:e.jobs.values())if(!item.requestOnly&&item.batchId.equals(job.batchId)&&item.item==i&&(browser?item.owner:item.nativeOwner).equals(h.get(browser?"x-chuan-client":"x-chuan-id"))){item.folderStopped=true;if(item.state<3)cancelled.add(item.id);}}}
            e.batches.get(key).touched=now();answer=job.state+"\t"+job.selection+"\t"+hex(job.error);
        }for(long id:cancelled)e.cancel(id);response(output,200,"OK",answer,"text/plain; charset=utf-8");
    }
    String batchKey(Header h,String ip,boolean browser){return (browser?"web:":"native:")+ip+":"+h.get(browser?"x-chuan-client":"x-chuan-id")+":"+h.get("x-chuan-batch");}
    String sender(Header h){return unhex(h.get(h.get("x-chuan-display-sender").isEmpty()?"x-chuan-sender":"x-chuan-display-sender"));}
    String recipient(String id)throws IOException {if(id.isEmpty())return "";synchronized(e.lock){for(Session session:e.sessions.values())if(session.id.equals(id)&&!session.tabs.isEmpty())return session.client;}throw new IOException("目标浏览器已离线");}
    void completeOffer(Job job,String selected){synchronized(e.lock){
        if(job.state>=3)return;Batch batch=e.batches.get(job.asyncOfferKey);if(batch==null){e.state(job,5,"接收申请已失效，请重试");return;}
        String previous=batch.selection.length()==batch.items.size()?batch.selection:repeat('-',batch.items.size());StringBuilder merged=new StringBuilder(previous);
        for(int i=0;i<previous.length();i++)if(previous.charAt(i)=='-'&&(job.cancelled||selected.charAt(i)!='-'))merged.setCharAt(i,job.cancelled?'0':selected.charAt(i));
        batch.selection=merged.toString();batch.touched=now();job.selection=batch.selection;
        if(job.cancelled||job.selection.indexOf('-')<0)e.state(job,job.cancelled?4:job.selection.indexOf('1')>=0?3:6,"");e.lock.notifyAll();
    }}
    void offer(Socket socket,InputStream input,OutputStream output,Header h,String ip,boolean browser)throws IOException {
        String batchId=h.get("x-chuan-batch"),owner=h.get(browser?"x-chuan-client":"x-chuan-id"),sender=sender(h);
        boolean asynchronous=h.get("x-chuan-offer-async").equals("1");
        if(!key(batchId)||owner.isEmpty()||sender.isEmpty()||sender.length()>64||sender.matches(".*[\\r\\n].*"))throw new IOException("批次身份无效");
        List<Item> items=parseManifest(new String(read(input,h.length(),1048576),StandardCharsets.UTF_8));
        String target=unhex(h.get("x-chuan-target")),recipientId=unhex(h.get("x-chuan-recipient"));if(browser&&!recipientId.isEmpty())throw new IOException("浏览器不能指定代理接收身份");
        Peer route=browser&&!target.isEmpty()&&!target.equals(e.config.id)?e.peer(target):null;
        boolean localBrowser=asynchronous&&route!=null&&route.browser&&route.owner.isEmpty();
        String recipient=recipient(localBrowser?route.id:recipientId);final Peer relay=localBrowser?null:route;
        if(!recipient.isEmpty()){synchronized(e.lock){Session session=e.sessions.get(recipient);long count=0,total=0;for(Item item:items){count=Math.addExact(count,item.count);total=Math.addExact(total,item.total);}limits(count,total,session.maxFiles,session.maxBytes);}}
        Batch batch=new Batch();batch.key=batchKey(h,ip,browser);batch.id=batchId;batch.owner=owner;batch.target=h.get("x-chuan-target");batch.recipient=h.get("x-chuan-recipient");batch.items=items;batch.selection=repeat('-',items.size());batch.counts=new long[items.size()];batch.bytes=new long[items.size()];
        synchronized(e.lock){if(e.batches.size()>=4096||e.batches.containsKey(batch.key)){response(output,409,"Conflict","批次标识已使用","text/plain; charset=utf-8");return;}e.batches.put(batch.key,batch);}
        long total=0;for(Item item:items)total=Math.addExact(total,item.total);Job job=e.job(items.size()==1?items.get(0).name:items.size()+" 个项目",sender,total,true);
        synchronized(e.lock){job.requestOnly=true;job.offerItems=items;job.nativeOwner=browser?"":owner;job.selection=repeat('-',items.size());job.batchId=batchId;job.recipient=recipient;job.owner=browser?owner:"";job.webTab=h.get("x-chuan-tab").isEmpty()?owner:h.get("x-chuan-tab");job.displaySender=sender;job.state=1;job.asyncOfferKey=asynchronous?batch.key:"";job.awaitingApproval=relay==null&&(!e.config.autoReceive||!recipient.isEmpty());}
        if(asynchronous){
            if(relay!=null)try{e.approvals.execute(()->{try{completeOffer(job,sendOffer(relay,batchId,items,sender,job,null,mask->completeOffer(job,mask),()->{synchronized(e.lock){return job.selection.replace('1','-');}}));}catch(IOException error){e.state(job,job.cancelled?4:5,error.getMessage());}});}catch(java.util.concurrent.RejectedExecutionException busy){e.state(job,5,"服务繁忙，请稍后重试");}
            else if(e.config.autoReceive&&recipient.isEmpty())completeOffer(job,repeat('1',items.size()));
            response(output,202,"Accepted",Json.stringify(Json.map("id",String.valueOf(job.id))),"application/json; charset=utf-8");return;
        }
        job.sockets.add(socket);
        String selected=repeat('1',items.size());
        try{
            if(relay!=null){selected=sendOffer(relay,batchId,items,sender,job,h.get("x-chuan-offer-wait").equals("1")?output:null);}else if(!e.config.autoReceive||!recipient.isEmpty()){
                job.awaitingApproval=true;awaitSelection(socket,input,job,h.get("x-chuan-offer-wait").equals("1")?output:null);selected=job.selection==null||job.cancelled?repeat('0',items.size()):job.selection;
            }
            if(job.cancelled)selected=repeat('0',items.size());synchronized(e.lock){batch.selection=selected;batch.touched=now();}e.state(job,job.cancelled?4:selected.indexOf('1')>=0?3:6,"");response(output,200,"OK",selected,"text/plain; charset=utf-8");
        }catch(IOException error){e.state(job,job.cancelled?4:5,error.getMessage());throw error;}finally{job.sockets.remove(socket);}
    }
    void awaitSelection(Socket socket,InputStream input,Job job,OutputStream waitingOutput)throws IOException {
        long keepalive=now();int timeout=socket.getSoTimeout();try{socket.setSoTimeout(2);
            while((job.selection==null||job.selection.indexOf('-')>=0)&&!job.cancelled&&e.running){synchronized(e.lock){try{e.lock.wait(250);}catch(InterruptedException error){Thread.currentThread().interrupt();job.cancelled=true;break;}}
                if((job.selection==null||job.selection.indexOf('-')>=0)&&!job.cancelled)try{if(input.read()<0){job.cancelled=true;break;}throw new IOException("审批请求包含额外数据");}catch(SocketTimeoutException idle){}
                if((job.selection==null||job.selection.indexOf('-')>=0)&&!job.cancelled&&waitingOutput!=null&&now()-keepalive>=10000){ascii(waitingOutput,"HTTP/1.1 102 Processing\r\n\r\n");keepalive=now();}
            }
        }finally{if(!socket.isClosed())socket.setSoTimeout(timeout);}
    }
    void upload(Socket socket,InputStream input,OutputStream output,Header h,String ip,boolean browser)throws IOException {
        upload(socket,input,output,h,ip,browser,false);
    }
    void upload(Socket socket,InputStream input,OutputStream output,Header h,String ip,boolean browser,boolean probe)throws IOException {
        boolean directory=h.get("x-chuan-directory").equals("1"),copying=h.get("x-chuan-copy").equals("1");
        String name=safePath(unhex(h.get("x-chuan-path"))),sender=sender(h);long total=number(h.get("x-chuan-size"));
        boolean checksum=!(browser&&h.get("x-chuan-checksum").equals("none"));
        String resumeKey=h.get("x-chuan-resume-key");long resumeOffset=h.get("x-chuan-offset").isEmpty()?0:number(h.get("x-chuan-offset"));
        if((!resumeKey.isEmpty()&&!Resume.keyValid(resumeKey))||resumeOffset>total||resumeOffset>0&&resumeKey.isEmpty())throw new IOException("续传参数无效");
        if(sender.isEmpty()||sender.length()>64||sender.matches(".*[\\r\\n].*")||total>Long.MAX_VALUE-32||h.length()!=(probe?0:total-resumeOffset+(checksum?32:0))||(!browser&&!probe&&!h.get("expect").equals("100-continue"))||(browser&&(!key(h.get("x-chuan-upload"))||!h.get("expect").isEmpty()))||(!h.get("x-chuan-checksum").isEmpty()&&!h.get("x-chuan-checksum").equals(checksum?"sha256":"none")))throw new IOException("文件请求参数无效");
        String target=unhex(h.get("x-chuan-target")),recipientId=unhex(h.get("x-chuan-recipient")),recipient=recipient(recipientId);if(browser&&!recipientId.isEmpty())throw new IOException("接收身份无效");
        boolean staging=browser&&!target.isEmpty()&&!target.equals(e.config.id);if(staging)e.peer(target);
        String folder=h.get("x-chuan-folder"),folderName=unhex(h.get("x-chuan-folder-name"));long folderCount=folder.isEmpty()?0:number(h.get("x-chuan-folder-count")),folderTotal=folder.isEmpty()?0:number(h.get("x-chuan-folder-size"));
        if(!folder.isEmpty()&&(!key(folder)||!(name.startsWith(safePath(folderName)+"/")||directory&&name.equals(folderName))||folderName.contains("/")||folderCount<1||folderTotal<total))throw new IOException("文件夹批次无效");
        limits(folder.isEmpty()?1:folderCount,folder.isEmpty()?total:folderTotal,e.config.maxFiles,e.config.maxBytes);
        File base=staging||!recipient.isEmpty()?e.config.relayDirectory:e.config.receiveDirectory;base.mkdirs();
        boolean matched=false;String existingUri=null,copyError="";
        if(copying||probe||directory){
            synchronized(e.lock){Batch approved=e.batches.get(batchKey(h,ip,browser));int index=(int)number(h.get("x-chuan-item"));
                if(approved!=null&&approved.cancelledItems.contains(index)){response(output,410,"Gone","接收端已取消本次传输","text/plain; charset=utf-8");return;}
                if(!copying||approved==null||index<0||index>=approved.items.size()||approved.selection.length()!=approved.items.size()||approved.selection.charAt(index)!='1'||approved.cancelledItems.contains(index)||!approved.target.equals(h.get("x-chuan-target"))||!approved.recipient.equals(h.get("x-chuan-recipient")))throw new IOException("目录复制没有有效授权");
                Item item=approved.items.get(index);if(!item.folder||!(name.startsWith(item.name+"/")||directory&&name.equals(item.name))||total>item.total||directory&&(!probe||total!=0))throw new IOException("目录复制范围无效");approved.touched=now();
            }
            try { File targetFile=new File(base,name);safeDestination(base,targetFile);
            if(probe){
                long[] tick={now()};Engine.Check check=()->{if(!e.running)throw new IOException("服务已停止");if(now()-tick[0]>2000){ascii(output,"HTTP/1.1 102 Processing\r\n\r\n");tick[0]=now();}};
                boolean metadata=h.get("x-chuan-metadata").equals("1")&&!directory;
                if(!staging&&recipient.isEmpty())existingUri=e.platform.existing(name,directory,total,metadata?null:h.get("x-chuan-sha256"),check);
                if(metadata){
                    boolean exists=!staging&&recipient.isEmpty()&&(existingUri!=null?!existingUri.isEmpty():targetFile.exists());
                    if(existingUri==null&&exists&&(!targetFile.isFile()||targetFile.length()!=total))throw new IOException("同名文件类型或大小不同，原文件已保留");
                    synchronized(e.lock){for(Job candidate:e.jobs.values())if(candidate.announced&&!candidate.cancelled&&candidate.state<3&&candidate.upload.equals(h.get("x-chuan-upload"))&&candidate.batchId.equals(h.get("x-chuan-batch"))&&(browser?candidate.owner:candidate.nativeOwner).equals(h.get(browser?"x-chuan-client":"x-chuan-id"))){candidate.activity=exists?"等待发送端核对同名文件":"准备接收文件";e.trace(candidate);break;}}
                    response(output,200,"OK",exists?"{\"verify\":true}":"{\"upload\":true}","application/json");return;
                }
                if(existingUri!=null)matched=!existingUri.isEmpty();
                else if(directory){if(targetFile.exists()&&!targetFile.isDirectory())throw new IOException("目标路径类型冲突，原文件已保留");matched=true;}
                else if(!staging&&recipient.isEmpty()&&targetFile.exists()){
                    if(!targetFile.isFile()||targetFile.length()!=total)throw new IOException("同名文件内容不同，原文件已保留");
                    Job checking=null;synchronized(e.lock){for(Job candidate:e.jobs.values())if(candidate.announced&&!candidate.cancelled&&candidate.upload.equals(h.get("x-chuan-upload"))&&candidate.batchId.equals(h.get("x-chuan-batch"))&&(browser?candidate.owner:candidate.nativeOwner).equals(h.get(browser?"x-chuan-client":"x-chuan-id"))){checking=candidate;checking.activity="核对同名文件";e.trace(checking);break;}}
                    try(InputStream file=new FileInputStream(targetFile)){matched=digest(file,check::run).equals(h.get("x-chuan-sha256"));}
                    finally{if(checking!=null)synchronized(e.lock){checking.activity="";e.trace(checking);}}
                    if(!matched)throw new IOException("同名文件内容不同，原文件已保留");
                }
                if(directory)matched=true;
                if(!matched){response(output,200,"OK","{\"upload\":true}","application/json");return;}
            }
            }catch(IOException error){copyError=error.getMessage()==null?"无法核对目标文件":error.getMessage();}
        }
        Batch batch=null;int item=0;File folderRoot=null;
        if(!h.get("x-chuan-batch").isEmpty()){
            synchronized(e.lock){batch=e.batches.get(batchKey(h,ip,browser));item=(int)number(h.get("x-chuan-item"));
                if(batch==null||item<0||item>=batch.items.size()||batch.selection.length()!=batch.items.size()||batch.selection.charAt(item)!='1'||!batch.target.equals(h.get("x-chuan-target"))||!batch.recipient.equals(h.get("x-chuan-recipient")))throw new IOException("文件没有对应的批次授权");
                if(batch.cancelledItems.contains(item)){response(output,410,"Gone","接收端已取消本次传输","text/plain; charset=utf-8");return;}
                Item authorized=batch.items.get(item);String pathKey=item+":"+name.toLowerCase(Locale.ROOT);long nextCount=batch.counts[item]+1,nextBytes=Math.addExact(batch.bytes[item],total);
                if((authorized.folder?!(name.startsWith(authorized.name+"/")||directory&&name.equals(authorized.name)):!name.equals(authorized.name))||nextCount>authorized.count||nextBytes>authorized.total||(nextCount==authorized.count&&nextBytes!=authorized.total)||batch.paths.contains(pathKey)||authorized.folder!=!folder.isEmpty()||(authorized.folder&&(!authorized.name.equals(folderName)||authorized.count!=folderCount||authorized.total!=folderTotal)))throw new IOException("文件超出已批准项目的范围");
                batch.counts[item]=nextCount;batch.bytes[item]=nextBytes;batch.paths.add(pathKey);batch.touched=now();
                if(authorized.folder&&copyError.isEmpty()){folderRoot=batch.roots.get(item);if(folderRoot==null){folderRoot=copying?new File(base,authorized.name):unique(base,authorized.name,true);if(!folderRoot.isDirectory()&&!folderRoot.mkdirs())throw new IOException("无法创建接收目录");batch.roots.put(item,folderRoot);}}
            }
        }
        if(!recipient.isEmpty()&&batch==null)throw new IOException("浏览器接收文件需要批次审批");
        Job announced=null;synchronized(e.lock){Batch declared=e.batches.get(batchKey(h,ip,browser));if(declared!=null){for(Job candidate:e.jobs.values())if(candidate.announced&&candidate.state==0&&(browser?candidate.owner:candidate.nativeOwner).equals(h.get(browser?"x-chuan-client":"x-chuan-id"))&&candidate.batchId.equals(h.get("x-chuan-batch"))&&candidate.upload.equals(h.get("x-chuan-upload"))&&candidate.name.equals(name)&&candidate.total==total){announced=candidate;candidate.announced=false;candidate.activity="";e.trace(candidate);candidate.browserPaused=false;candidate.resumeActivity="";break;}if(announced==null&&declared.announcedItems.contains(item))throw new IOException("文件与已登记清单不一致");}}
        Job job=announced!=null?announced:e.job(name,sender,total,true);job.directory=directory;job.nativeOwner=browser?"":h.get("x-chuan-id");job.owner=browser?h.get("x-chuan-client"):"";job.webTab=h.get("x-chuan-tab").isEmpty()?job.owner:h.get("x-chuan-tab");job.upload=h.get("x-chuan-upload");job.recipient=recipient;job.target=target;job.displaySender=sender;job.staging=staging||!recipient.isEmpty();job.stage=browser?(staging?"staging":"upload"):"";job.batchId=h.get("x-chuan-batch");job.storageKey=batchKey(h,ip,browser)+":"+recipient;job.item=item;job.folderId=folder;job.folderName=folderName;job.folderCount=folderCount;job.folderTotal=folderTotal;job.sockets.add(socket);
        if(!copyError.isEmpty()){e.state(job,5,copyError);job.sockets.remove(socket);response(output,409,"Conflict",copyError,"text/plain; charset=utf-8");return;}
        if(batch==null&&!e.config.autoReceive&&!job.staging){job.state=1;job.awaitingApproval=true;job.offerItems=Collections.singletonList(new Item(name,false,1,total));awaitSelection(socket,input,job,null);if(!"1".equals(job.selection)||job.cancelled){e.state(job,job.cancelled?4:6,"");response(output,417,"Expectation Failed","接收请求已拒绝","text/plain; charset=utf-8");job.sockets.remove(socket);return;}}
        if(probe&&matched){
            File destination=new File(base,name);
            if(directory&&!destination.isDirectory()&&!destination.mkdirs())throw new IOException("无法创建目录");
            job.file=destination;job.source=directory?Source.directory(name):destination.isFile()?Source.file(destination,name):null;
            job.publicUri=existingUri==null?"":existingUri;job.skipped=!directory;job.done=total;
            if(directory&&!job.staging)job.publicUri=e.publish(job);
            if(!recipient.isEmpty()){job.state=1;waitBrowser(job,output,208);job.sockets.remove(socket);return;}
            e.state(job,3,"");job.sockets.remove(socket);response(output,208,"Already Reported",Json.stringify(Json.map("id",String.valueOf(job.id),"skipped",true)),"application/json");return;
        }
        long available=base.getUsableSpace();if(total-resumeOffset>available||available-(total-resumeOffset)<2L*1024*1024){e.state(job,5,"接收空间不足");response(output,507,"Insufficient Storage","接收空间不足","text/plain");return;}
        File temporary=resumeKey.isEmpty()?File.createTempFile(".chuan-",".part",base):Resume.file(base,h.get(browser?"x-chuan-client":"x-chuan-id"),name,total,resumeKey),destination;
        try{
            File parent;if(folderRoot!=null){String suffix=name.substring(name.indexOf('/')+1);parent=new File(folderRoot,suffix).getParentFile();}else parent=new File(base,name).getParentFile();
            if(!parent.isDirectory()&&!parent.mkdirs())throw new IOException("无法创建接收目录");if(!parent.getCanonicalPath().startsWith(base.getCanonicalPath()+File.separator)&&!parent.getCanonicalFile().equals(base.getCanonicalFile()))throw new IOException("文件路径越界");
            safeDestination(base,temporary);MessageDigest hash=checksum?sha():null;
            try(RandomAccessFile data=new RandomAccessFile(temporary,"rw");java.nio.channels.FileLock lease=data.getChannel().tryLock()){
                if(lease==null||data.length()!=resumeOffset)throw new IOException("续传数据正在使用或偏移已变化");
                if(resumeOffset>0){synchronized(e.lock){job.activity="核验已接收部分，准备续传";e.trace(job);}MessageDigest prefix=sha();byte[] bytes=new byte[1024*1024];long checked=0,tick=now();while(checked<resumeOffset){if(job.cancelled||job.paused||!e.running)throw new IOException("传输已暂停或取消");int n=data.read(bytes,0,(int)Math.min(bytes.length,resumeOffset-checked));if(n<0)throw new EOFException();prefix.update(bytes,0,n);if(hash!=null)hash.update(bytes,0,n);checked+=n;if(!browser&&now()-tick>2000){ascii(output,"HTTP/1.1 102 Processing\r\n\r\n");tick=now();}}if(!hex(prefix.digest()).equals(h.get("x-chuan-prefix")))throw new IOException("续传内容不一致，需从头接收该文件");}
                if(!browser)ascii(output,"HTTP/1.1 100 Continue\r\n\r\n");e.state(job,2,"");job.progressBytes=resumeOffset;job.progressAt=now();e.progress(job,resumeOffset);
                OutputStream destinationStream=new OutputStream(){public void write(int value)throws IOException{data.write(value);}public void write(byte[] bytes,int at,int count)throws IOException{if(job.paused)throw new IOException("传输已暂停");data.write(bytes,at,count);}};
                e.diagnostics.copy(job,"network_to_disk",input,destinationStream,total-resumeOffset,hash,bytes->e.progress(job,resumeOffset+bytes));
                synchronized(e.lock){job.activity="正在校验接收内容";e.trace(job);}if(checksum&&!MessageDigest.isEqual(hash.digest(),read(input,32,32)))throw new IOException("SHA-256 完整性校验失败");synchronized(e.lock){job.activity="正在保存文件";e.trace(job);}data.getFD().sync();
            }
            boolean exact=copying&&!job.staging&&!e.platform.externalStorage();
            synchronized(e.lock){destination=exact?new File(parent,name.substring(name.lastIndexOf('/')+1)):unique(parent,name.substring(name.lastIndexOf('/')+1),false);if(exact&&destination.exists())throw new IOException("目标文件在核对后已变化，原文件已保留");if(job.cancelled||job.retryExpired||!e.running)throw new IOException("传输已取消");if(!temporary.renameTo(destination))throw new IOException("无法提交接收文件");job.file=destination;job.source=Source.file(destination,name);}
            if(!job.staging)job.publicUri=e.publish(job);
            if(!recipient.isEmpty()){job.state=1;waitBrowser(job,output);return;}
            e.state(job,3,"");response(output,201,"Created",browser?Json.stringify(Json.map("id",String.valueOf(job.id))):destination.getName(),browser?"application/json; charset=utf-8":"text/plain; charset=utf-8");
        }catch(Exception error){if(resumeKey.isEmpty()||String.valueOf(error.getMessage()).contains("SHA-256"))temporary.delete();e.state(job,job.paused?5:job.cancelled?4:5,job.cancelled?"接收端已取消本次传输":error.getMessage()==null?"传输中断":error.getMessage());response(output,422,"Unprocessable Content",job.error,"text/plain; charset=utf-8");}finally{job.sockets.remove(socket);}
    }
    void waitBrowser(Job job,OutputStream output)throws IOException {waitBrowser(job,output,201);}
    void waitBrowser(Job job,OutputStream output,int success)throws IOException {long alive=0;while(e.running&&!job.cancelled&&job.state<3){synchronized(e.lock){try{e.lock.wait(250);}catch(InterruptedException error){Thread.currentThread().interrupt();break;}}if(now()-alive>2000){ascii(output,"HTTP/1.1 102 Processing\r\n\r\n");alive=now();}}if(job.state<3)e.state(job,4,"浏览器未完成接收");response(output,job.state==3?success:409,job.state==3?"Created":"Conflict",job.error,"text/plain; charset=utf-8");}
    static File unique(File parent,String name,boolean directory)throws IOException {int dot=name.lastIndexOf('.');String stem=dot>0&&!directory?name.substring(0,dot):name,extension=dot>0&&!directory?name.substring(dot):"";for(int i=0;i<10000;i++){File target=new File(parent,i==0?name:stem+" ("+i+")"+extension);if(!target.exists()){if(directory&&!target.mkdir())continue;return target;}}throw new IOException("同名文件数量过多");}
    void send(Peer peer,List<Source> sources){send(peer,sources,false);}
    void send(Peer peer,List<Source> sources,boolean forceZip){final boolean zipFolders=e.config.zipFolders||forceZip;List<Job> prepared=new ArrayList<>();try{
        if(sources.isEmpty())throw new IOException("未选择文件");long total=0;for(Source source:sources)total=Math.addExact(total,source.size);limits(sources.size(),total,e.config.maxFiles,e.config.maxBytes);
        Map<String,ZipStore.Plan> archives=new LinkedHashMap<>();Set<String> used=new HashSet<>();for(Source source:sources)if(!source.directory&&source.path.indexOf('/')<0&&!used.add(source.path.toLowerCase(Locale.ROOT)))throw new IOException("所选文件包含重复名称");
        for(ZipStore.Plan plan:zipFolders?ZipStore.plans(sources):Collections.<ZipStore.Plan>emptyList()){String root=plan.name.substring(0,plan.name.length()-4),name=plan.name;int suffix=2;while(!used.add(name.toLowerCase(Locale.ROOT)))name=root+" ("+(suffix++)+").zip";plan.name=name;archives.put(root,plan);}
        List<Source> packed=new ArrayList<>();Map<String,ZipStore.Plan> plans=new HashMap<>();Set<String> emitted=new HashSet<>();for(Source source:sources){int slash=source.path.indexOf('/');if(!zipFolders||slash<0&&!source.directory)packed.add(source);else{String root=slash<0?source.path:source.path.substring(0,slash);if(emitted.add(root)){ZipStore.Plan plan=archives.get(root);packed.add(plan.metadata());plans.put(plan.name,plan);}}}
        long wireBytes=0;for(Source source:packed)wireBytes=Math.addExact(wireBytes,source.size);limits(packed.size(),wireBytes,e.config.maxFiles,e.config.maxBytes);List<Item> items=new ArrayList<>();String batch=random();Map<String,Integer> roots=new LinkedHashMap<>();Map<String,String> folderIds=new HashMap<>();
        synchronized(e.lock){for(Source source:packed){
            Job job=e.job(source.path,peer.name,source.size,false);job.source=source;job.directory=source.directory;job.target=peer.id;job.batchId=batch;job.storageKey="send:"+batch+":"+peer.id;job.displaySender=e.config.name;job.upload=random();
            boolean folder=!zipFolders&&(source.directory||source.path.contains("/"));
            if(folder){String root=source.path.split("/",2)[0];Integer index=roots.get(root);if(index==null){index=items.size();roots.put(root,index);folderIds.put(root,random());items.add(new Item(root,true,0,0));}job.item=index;Item item=items.get(index);item.count++;item.total=Math.addExact(item.total,source.size);job.folderId=folderIds.get(root);job.folderName=root;}
            else{job.item=items.size();items.add(new Item(source.path,false,1,source.size));}
            ZipStore.Plan plan=plans.get(job.name);if(plan!=null){job.archiveSources=plan.sources;job.source=ZipStore.source(plan,()->{if(!e.running)throw new IOException("服务已停止");});}prepared.add(job);e.state(job,1,"");
        }
        for(Job job:prepared)if(!job.folderId.isEmpty()){Item item=items.get(job.item);job.folderCount=item.count;job.folderTotal=item.total;}}
        Set<Integer> declaredItems=new HashSet<>();
        Job control=new Job(0,"",peer.name,0,false);Set<Long> processed=new HashSet<>();
        sendOffer(peer,batch,items,e.config.name,control,null,selected->{
        for(Job entry:prepared)if(selected.charAt(entry.item)=='1'&&!processed.contains(entry.id))e.state(entry,0,"");
        List<Job> selectedJobs=new ArrayList<>();Set<Long> registeredJobs=new HashSet<>();
        for(Job job:prepared){if(selected.charAt(job.item)=='-'||!processed.add(job.id))continue;
            if(selected.charAt(job.item)!='1'){e.state(job,job.cancelled?4:6,"接收方未选择此项");continue;}
            boolean registered=declaredItems.contains(job.item);
            if(!peer.browser&&!registered&&!job.cancelled){StringBuilder manifest=new StringBuilder();for(Job entry:prepared)if(entry.item==job.item)manifest.append(entry.item).append("\t").append(entry.upload).append("\t").append(entry.folderId).append("\t").append(hex(entry.name)).append("\t").append(entry.total).append(plans.containsKey(entry.name)?"\tA":entry.directory?"\tD":"").append("\n");try{e.request(peer,"POST","/api/batch-files",Collections.singletonMap("X-Chuan-Batch",batch),manifest.toString(),5000,job);registered=true;declaredItems.add(job.item);}catch(IOException error){e.state(job,5,"接收端未确认传输清单");continue;}}
            if(registered)registeredJobs.add(job.id);e.state(job,0,"");selectedJobs.add(job);
        }
        Map<Long,File> artifacts=new java.util.concurrent.ConcurrentHashMap<>();
        java.util.function.Consumer<Job> finish=job->{
            boolean registered=registeredJobs.contains(job.id);
            if(registered&&job.state>=4&&!job.cancelled)try{String state=e.request(peer,"POST","/api/upload-status/"+job.upload,Collections.singletonMap("X-Chuan-Batch",batch),"",5000);if(state.equals("4")){job.cancelled=true;e.state(job,4,"接收端已取消本次传输");}else if(state.equals("3")){job.done=job.total;e.state(job,3,"");}}catch(IOException ignored){}
            File artifact=artifacts.remove(job.id);if(artifact!=null&&job.state!=5&&!(peer.browser&&peer.owner.isEmpty()))artifact.delete();
            if(job.state==5&&!job.cancelled){queueRetry(peer,job);return;}
            if(registered&&job.state>=4)try{String error=job.error.isEmpty()?"发送未完成":job.error;e.request(peer,"POST","/api/batch-status",Collections.singletonMap("X-Chuan-Batch",batch),job.upload+"\t"+(job.state==4?4:5)+"\t"+hex(error.replace('\n',' ').replace('\r',' '))+"\n",5000);}catch(IOException ignored){}
        };
        try{FilePipeline.run(e,selectedJobs,job->{
            if(job.cancelled||!e.running){e.state(job,4,"发送已取消");finish.accept(job);return FilePipeline.DONE;}

            if(!prepareOne(peer,job,true)){finish.accept(job);return FilePipeline.DONE;}
            if(job.verificationPending)return FilePipeline.VERIFY;
            e.state(job,0,"");synchronized(e.lock){if(job.state<3)job.activity="已就绪，等待传输";e.trace(job);}return FilePipeline.READY;
        },job->{
            boolean registered=registeredJobs.contains(job.id);File archive=null;
            try{
                if(job.cancelled||!e.running){e.state(job,4,"发送已取消");finish.accept(job);return false;}
                if(archive!=null)artifacts.put(job.id,archive);
                if(!prepareOne(peer,job)){finish.accept(job);return false;}
                e.state(job,0,"");synchronized(e.lock){if(job.state<3)job.activity="已就绪，等待传输";e.trace(job);}return true;
            }catch(Exception error){if(archive!=null)artifacts.put(job.id,archive);e.state(job,job.cancelled?4:5,error.getMessage());finish.accept(job);return false;}
        },job->{try{transmitOne(peer,job);}catch(Exception error){e.state(job,job.cancelled?4:5,error.getMessage());}finish.accept(job);});}
        catch(InterruptedException error){Thread.currentThread().interrupt();throw new IOException("传输已停止",error);}
        },()->{synchronized(e.lock){StringBuilder mask=new StringBuilder(repeat('-',items.size()));boolean all=true;for(Job job:prepared){if(job.cancelled)mask.setCharAt(job.item,'0');else all=false;}if(all)control.cancelled=true;return mask.toString();}});

    }catch(Exception error){e.notice="发送失败："+error.getMessage();for(Job job:prepared)if(job.state<3)e.state(job,job.cancelled?4:5,error.getMessage());}}
    boolean prepareOne(Peer peer,Job job){return prepareOne(peer,job,false);}
    boolean prepareOne(Peer peer,Job job,boolean metadataOnly){
        boolean verifying=job.verificationPending&&!metadataOnly;job.verificationPending=false;
        if(job.cancelled||!e.running){e.state(job,4,"发送已取消");return false;}
        if(peer.browser&&peer.owner.isEmpty())return true;
        try {
            if(!job.folderId.isEmpty()){
                synchronized(e.lock){job.activity="检查接收位置";e.trace(job);}
                Map<String,String> fields=e.identity(peer);fields.put("X-Chuan-Path",hex(job.name));fields.put("X-Chuan-Size",String.valueOf(job.total));fields.put("X-Chuan-Batch",job.batchId);fields.put("X-Chuan-Item",String.valueOf(job.item));fields.put("X-Chuan-Upload",job.upload);fields.put("X-Chuan-Folder",job.folderId);fields.put("X-Chuan-Folder-Name",hex(job.folderName));fields.put("X-Chuan-Folder-Count",String.valueOf(job.folderCount));fields.put("X-Chuan-Folder-Size",String.valueOf(job.folderTotal));fields.put("X-Chuan-Copy","1");fields.put("X-Chuan-Directory",job.directory?"1":"0");fields.put("X-Chuan-Sha256","");fields.put("X-Chuan-Recipient",peer.browser?hex(peer.id):"");
                String result;boolean legacy=false;
                try{result=verifying?"{\"verify\":true}":e.request(peer,"POST","/api/copy-check",fields,"",130000,job);}catch(IOException error){if(error.getMessage()==null||!(error.getMessage().startsWith("HTTP 404")||error.getMessage().startsWith("HTTP 405")))throw error;legacy=true;result="";}
                if(legacy||result.contains("\"verify\":true")){
                    if(metadataOnly){job.verificationPending=true;synchronized(e.lock){job.activity="等待核对同名文件";e.trace(job);}return true;}
                    synchronized(e.lock){job.activity="核对同名文件";e.trace(job);}final long[] tick={now()};
                    String digest=job.directory?"":digest(job.source.opener.open(),()->{if(job.cancelled||job.retryExpired||!e.running)throw new IOException("传输已取消");if(now()-tick[0]>2000){String state=e.request(peer,"POST","/api/upload-status/"+job.upload,Collections.singletonMap("X-Chuan-Batch",job.batchId),"",4000,job);if(state.equals("4")){job.cancelled=true;throw new IOException("接收端已取消本次传输");}tick[0]=now();}});
                    fields.put("X-Chuan-Sha256",digest);synchronized(e.lock){job.activity="等待接收端核对同名文件";e.trace(job);}
                    result=e.request(peer,"POST","/api/copy-probe",fields,"",130000,job);
                }
                if(!result.contains("\"upload\":true")){job.skipped=!job.directory;job.done=job.total;e.state(job,3,"");return false;}
            }
        }catch(Exception error){if(error.getMessage()!=null&&error.getMessage().startsWith("HTTP 410"))job.cancelled=true;e.state(job,job.cancelled?4:5,error.getMessage());return false;}
        synchronized(e.lock){job.activity="";e.trace(job);}return !job.cancelled&&e.running;
    }
    void sendOne(Peer peer,Job job){if(prepareOne(peer,job))transmitOne(peer,job);}
    void transmitOne(Peer peer,Job job){
        if(job.paused){e.state(job,5,"");return;}
        if(job.cancelled||!e.running){e.state(job,4,"发送已取消");return;}
        if(peer.browser&&peer.owner.isEmpty()){try{job.recipient=recipient(peer.id);e.state(job,1,"");synchronized(e.lock){job.activity="等待浏览器接收";e.trace(job);}}catch(IOException error){e.state(job,5,error.getMessage());}return;}
        Socket connection=null;
        try{
            Map<String,String> headers=e.identity(peer);headers.put("X-Chuan-Path",hex(job.name));headers.put("X-Chuan-Size",String.valueOf(job.total));headers.put("X-Chuan-Recipient",peer.browser?hex(peer.id):"");headers.put("X-Chuan-Display-Sender",hex(job.displaySender));headers.put("X-Chuan-Upload",job.upload);headers.put("X-Chuan-Batch",job.batchId);headers.put("X-Chuan-Item",String.valueOf(job.item));headers.put("X-Chuan-Folder",job.folderId);headers.put("X-Chuan-Folder-Name",hex(job.folderName));headers.put("X-Chuan-Folder-Count",String.valueOf(job.folderCount));headers.put("X-Chuan-Folder-Size",String.valueOf(job.folderTotal));if(!job.folderId.isEmpty())headers.put("X-Chuan-Copy","1");
            long offset=0;if(!job.directory){headers.put("X-Chuan-Resume-Key",Resume.fingerprint(job.source));headers.put("X-Chuan-Resume-Reset",job.resumeReset?"1":"0");try{offset=((Number)Json.object(e.request(peer,"POST","/api/resume",headers,"",150000,job)).get("offset")).longValue();job.resumeReset=false;}catch(IOException error){if(error.getMessage()==null||!error.getMessage().startsWith("HTTP 404"))throw error;headers.remove("X-Chuan-Resume-Key");}if(offset<0||offset>job.total)throw new IOException("续传偏移无效");}
            final long start=offset;MessageDigest hash=sha();
            try(InputStream file=job.source.opener.open()){
                if(start>0){synchronized(e.lock){job.activity="核验已接收部分，准备续传";e.trace(job);}headers.put("X-Chuan-Prefix",Resume.prefix(file,start,hash,()->{if(job.cancelled||job.paused||!e.running)throw new IOException("传输已暂停或取消");if(job.retryPending)job.retryDeadline=now()+30000;}));}
                headers.put("X-Chuan-Offset",String.valueOf(start));headers.put("Content-Length",String.valueOf(job.total-start+32));
                String address=peer.gateway.isEmpty()?peer.ip:peer.gateway;connection=e.connect(address,peer.port,150000);job.sockets.add(connection);e.sockets.add(connection);e.state(job,1,"");
                StringBuilder request=new StringBuilder("POST /api/upload HTTP/1.1\r\nHost: "+address+":"+peer.port+"\r\nConnection: close\r\nExpect: 100-continue\r\n");for(Map.Entry<String,String> entry:headers.entrySet())request.append(entry.getKey()).append(": ").append(entry.getValue()).append("\r\n");OutputStream output=new BufferedOutputStream(connection.getOutputStream());ascii(output,request+"\r\n");InputStream input=new BufferedInputStream(connection.getInputStream());Header reply;do{reply=header(input);if(job.retryPending)job.retryDeadline=now()+30000;}while(reply.status()==102);
                if(reply.status()!=100){String reason=new String(read(input,reply.length(),131072),StandardCharsets.UTF_8);if(reason.contains("续传内容不一致"))job.resumeReset=true;throw new IOException("接收端拒绝文件：HTTP "+reply.status()+" "+reason);}
                e.state(job,2,"");job.progressBytes=start;job.progressAt=now();e.progress(job,start);synchronized(e.lock){job.activity=job.archiveSources.isEmpty()?"正在发送文件":"边打包边发送 ZIP";e.trace(job);}
                e.diagnostics.copy(job,"disk_to_network",file,output,job.total-start,hash,bytes->{if(job.paused)throw new IOException("传输已暂停");e.progress(job,start+bytes);});if(file.read()!=-1)throw new IOException("发送期间文件长度发生变化");output.write(hash.digest());output.flush();synchronized(e.lock){job.activity="等待接收端保存确认";e.trace(job);}
                do{reply=header(input);}while(reply.status()==102&&!job.cancelled);if(reply.status()!=201){String reason=new String(read(input,reply.length(),131072),StandardCharsets.UTF_8);throw new IOException("接收端未确认保存：HTTP "+reply.status()+" "+reason);}e.state(job,3,"");
            }
        }catch(Exception error){e.state(job,job.cancelled?4:5,job.cancelled?"接收端已取消本次传输":error.getMessage()==null?"传输中断":error.getMessage());}
        finally{if(connection!=null){job.sockets.remove(connection);e.sockets.remove(connection);try{connection.close();}catch(IOException ignored){}}}
    }
    Job forward(Session session,long id,String target,String upload)throws IOException {
        synchronized(e.lock){for(Job existing:e.jobs.values())if(existing.owner.equals(session.client)&&existing.stage.equals("forward")&&existing.upload.equals(upload))return existing;}
        Job file;synchronized(e.lock){file=e.jobs.get(id);if(file==null||!file.owner.equals(session.client)||file.source==null||!file.target.equals(target)||!file.upload.equals(upload))throw new IOException("转发文件与任务归属不匹配");}
        Peer peer=e.peer(target);Job job=e.job(file.name,peer.name,file.total,false);job.source=file.source;job.directory=file.directory;job.file=file.file;job.staging=true;job.stage="forward";job.owner=session.client;job.upload=upload;job.target=target;job.displaySender=file.displaySender;job.batchId=file.batchId;job.storageKey=file.storageKey;job.item=file.item;job.folderId=file.folderId;job.folderName=file.folderName;job.folderCount=file.folderCount;job.folderTotal=file.folderTotal;e.filePreparation.execute(()->{
            if(!prepareOne(peer,job,true)){if(job.state==5)queueRetry(peer,job);return;}
            Runnable ready=()->{e.state(job,0,"");synchronized(e.lock){if(job.state>=3)return;job.activity="已就绪，等待传输";e.trace(job);}e.fileTransfers.execute(()->{transmitOne(peer,job);if(job.state==5)queueRetry(peer,job);});};
            if(job.verificationPending)e.fileVerification.execute(()->{if(prepareOne(peer,job))ready.run();else if(job.state==5)queueRetry(peer,job);});else ready.run();
        });return job;
    }
    void download(Session session,Job job,OutputStream output,boolean checksum,long offset,String prefix,boolean range)throws IOException {
        if(job==null||job.removed||job.source==null||(!job.owner.equals(session.client)&&!job.recipient.equals(session.client)))throw new FileNotFoundException("文件不属于当前浏览器会话");
        boolean delivery=job.recipient.equals(session.client);if(job.paused||delivery&&job.state==4)throw new IOException("传输已暂停或取消");if(offset<0||offset>job.total||range&&(checksum||offset>=job.total)||!prefix.isEmpty()&&!Resume.keyValid(prefix))throw new IOException("续传范围无效");
        try(InputStream input=job.source.opener.open()){
            MessageDigest hash=sha();if(offset>0){if(checksum||!prefix.isEmpty()){String actual=Resume.prefix(input,offset,hash,()->{if(job.paused||job.cancelled||!e.running)throw new IOException("传输已暂停或取消");});if(!prefix.isEmpty()&&!actual.equals(prefix))throw new IOException("续传内容不一致");}else Resume.skip(input,offset);}
            if(delivery){e.state(job,2,"");job.progressBytes=offset;job.progressAt=now();e.progress(job,offset);synchronized(e.lock){job.activity=job.archiveSources.isEmpty()?"正在发送文件":"边打包边发送 ZIP";e.trace(job);}}
            ascii(output,(range?"HTTP/1.1 206 Partial Content\r\nContent-Range: bytes "+offset+"-"+(job.total-1)+"/"+job.total+"\r\n":"HTTP/1.1 200 OK\r\n")+"Connection: close\r\nCache-Control: no-store\r\nAccept-Ranges: bytes\r\nX-Content-Type-Options: nosniff\r\nContent-Type: application/octet-stream\r\nContent-Disposition: attachment; filename=\"download\"; filename*=UTF-8''"+encodeName(job.name.substring(job.name.lastIndexOf('/')+1))+"\r\nContent-Length: "+Math.addExact(job.total-offset,checksum?32:0)+"\r\n\r\n");
            e.diagnostics.copy(job,"disk_to_browser",input,output,job.total-offset,checksum?hash:null,bytes->{if(job.paused)throw new IOException("传输已暂停");if(delivery)e.progress(job,offset+bytes);});if(checksum)output.write(hash.digest());output.flush();if(delivery){if(job.folderId.isEmpty())e.state(job,3,"");else synchronized(e.lock){job.state=1;job.activity="等待浏览器保存确认";e.trace(job);}}
        }catch(IOException error){if(delivery)e.state(job,job.cancelled?4:5,error.getMessage());throw error;}
    }
    static void safeDestination(File base,File path)throws IOException {
        File absolute=path.getAbsoluteFile();if(!absolute.getCanonicalFile().equals(absolute)||!absolute.toPath().normalize().startsWith(base.getAbsoluteFile().toPath().normalize()))throw new IOException("目标路径越界或包含链接");
    }
    interface Check {void run()throws IOException;}
    static String digest(InputStream stream,Check check)throws IOException {try(InputStream input=stream){MessageDigest hash=sha();byte[] buffer=new byte[256*1024];int count;while((count=input.read(buffer))!=-1){check.run();hash.update(buffer,0,count);}return hex(hash.digest());}}
    static String repeat(char c,int count){char[] value=new char[count];Arrays.fill(value,c);return new String(value);}
}
