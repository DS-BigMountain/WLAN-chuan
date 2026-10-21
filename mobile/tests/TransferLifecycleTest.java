package local.chuan.core;
import java.io.*;import java.net.*;import java.util.*;import java.util.concurrent.*;import java.util.concurrent.atomic.*;import java.util.function.BooleanSupplier;

/** Focused lifecycle checks against real loopback HTTP connections. */
public final class TransferLifecycleTest {
    static void require(boolean value,String message){if(!value)throw new AssertionError(message);}
    static void until(BooleanSupplier condition,String message)throws Exception{long end=System.currentTimeMillis()+40000;while(System.currentTimeMillis()<end){if(condition.getAsBoolean())return;Thread.sleep(30);}throw new AssertionError(message);}
    static Engine host(File root)throws Exception{Engine.Config c=new Engine.Config(root);try(DatagramSocket socket=new DatagramSocket(0)){c.discoveryPort=socket.getLocalPort();}c.port=0;c.id=Protocol.random();c.name="Lifecycle";return new Engine(c,new Engine.Platform(){public InputStream asset(String name)throws IOException{throw new FileNotFoundException(name);}public File apk(){return null;}public String saved(Engine.Job job){return job.file.toURI().toString();}public void deleted(Engine.Job job){}public void settingsChanged(Engine.Config config){}});}
    static List<Engine.Job> visible(Engine e){synchronized(e.lock){List<Engine.Job> result=new ArrayList<>();for(Engine.Job job:e.jobs.values())if(!job.requestOnly&&!job.nativeHidden)result.add(job);return result;}}
    public static void main(String[] args)throws Exception{
        File root=new File(args[0]);root.mkdirs();Engine sender=host(new File(root,"sender")),receiver=host(new File(root,"receiver"));
        try{sender.start();receiver.start();sender.addPeer("127.0.0.1:"+receiver.port());until(()->{synchronized(sender.lock){return sender.peers.containsKey(receiver.config.id);}},"peer");
            CountDownLatch blocked=new CountDownLatch(1),release=new CountDownLatch(1);AtomicBoolean once=new AtomicBoolean();
            Engine.Source slow=new Engine.Source("目录/b.bin",3,()->{if(once.compareAndSet(false,true)){blocked.countDown();try{if(!release.await(15,TimeUnit.SECONDS))throw new IOException("test timeout");}catch(InterruptedException e){throw new IOException(e);}}return new ByteArrayInputStream(new byte[]{1,2,3});});
            sender.send(receiver.config.id,Arrays.asList(Engine.Source.directory("目录"),new Engine.Source("目录/a.txt",1,()->new ByteArrayInputStream(new byte[]{7})),slow));require(blocked.await(15,TimeUnit.SECONDS),"second file must be held");
            require(visible(receiver).size()==3,"receiver needs full directory manifest before payload");sender.clear();receiver.clear();require(visible(sender).size()==3&&visible(receiver).size()==3,"clear removed members of active batch");release.countDown();until(()->visible(sender).stream().allMatch(j->j.state==3),"clear interrupted active transfer");require(new File(receiver.config.receiveDirectory,"目录/b.bin").length()==3,"missing received file");
            System.out.println("PASS Android native: clear retains completed children and approval state during real directory transfer");
            AtomicBoolean fail=new AtomicBoolean(true);sender.send(receiver.config.id,Arrays.asList(Engine.Source.directory("重试"),new Engine.Source("重试/good.txt",1,()->new ByteArrayInputStream(new byte[]{9})),new Engine.Source("重试/fail.bin",2,()->{if(fail.get())throw new IOException("temporary source read failure");return new ByteArrayInputStream(new byte[]{4,5});})));
            until(()->visible(sender).stream().anyMatch(j->j.name.equals("重试/fail.bin")&&j.state==5&&j.retryExpired&&!j.retryPending),"30 second failure");Engine.Job old=visible(sender).stream().filter(j->j.name.equals("重试/fail.bin")).findFirst().get();long saved=new File(receiver.config.receiveDirectory,"重试/good.txt").lastModified();fail.set(false);sender.retry(old.id);
            until(()->visible(sender).stream().anyMatch(j->j.id!=old.id&&j.name.equals("重试/fail.bin")&&j.state==3),"individual manual retry");require(new File(receiver.config.receiveDirectory,"重试/good.txt").lastModified()==saved,"manual retry rewrote completed sibling");require(new File(receiver.config.receiveDirectory,"重试/fail.bin").length()==2,"manual retry lost relative path");require(visible(sender).stream().filter(j->j.folderName.equals("重试")).count()==3,"replacement duplicated group members");
            System.out.println("PASS Android native: timed failure re-adds only failed file with original relative path");
        }finally{sender.close();receiver.close();}
    }
}
