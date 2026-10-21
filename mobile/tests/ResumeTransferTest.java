package local.chuan.core;
import java.io.*;import java.nio.file.*;import java.util.*;import java.util.concurrent.*;

/** Real sockets exercise pause propagation, disconnect recovery and restart reuse. */
public final class ResumeTransferTest {
    interface Condition {boolean ready()throws Exception;}
    static void until(Condition condition,String label)throws Exception{long end=System.nanoTime()+TimeUnit.SECONDS.toNanos(20);while(!condition.ready()){if(System.nanoTime()>end)throw new AssertionError(label);Thread.sleep(10);}}
    static Engine.Job active(Engine engine){synchronized(engine.lock){for(Engine.Job job:engine.jobs.values())if(!job.requestOnly&&job.state<3)return job;return null;}}
    public static void main(String[] args)throws Exception{
        Path root=Paths.get(args[0]).resolve(UUID.randomUUID().toString());Engine sender=TransferReadTest.host(root.resolve("sender"));Engine[] receiver={TransferReadTest.host(root.resolve("receiver"))};
        byte[] bytes=new byte[16*1024*1024];for(int i=0;i<bytes.length;i++)bytes[i]=(byte)(i*17+(i>>>12));
        Engine.Source source=new Engine.Source("large.bin",bytes.length,()->new ByteArrayInputStream(bytes){@Override public synchronized int read(byte[] out,int at,int count){try{Thread.sleep(8);}catch(InterruptedException error){Thread.currentThread().interrupt();}return super.read(out,at,Math.min(count,128*1024));}});
        try{
            String target=sender.probe("127.0.0.1",receiver[0].port()).id;sender.send(target,Collections.singletonList(source));
            until(()->active(receiver[0])!=null&&active(receiver[0]).done>2*1024*1024,"initial bytes");Engine.Job incoming=active(receiver[0]);receiver[0].pause(incoming.id,true);
            until(()->active(sender)!=null&&active(sender).remotePaused,"sender observes receiver pause");long saved=incoming.done;Thread.sleep(500);TransferReadTest.require(incoming.done==saved,"pause preserves offset without advancing");receiver[0].pause(incoming.id,false);
            until(()->Files.exists(root.resolve("receiver/received/large.bin")),"receiver pause resume");TransferReadTest.require(Arrays.equals(bytes,Files.readAllBytes(root.resolve("receiver/received/large.bin"))),"receiver resume contents");TransferReadTest.finished(sender);
            System.out.println("PASS native receiver pause propagates; resumes saved offset; exact contents");
            Files.delete(root.resolve("receiver/received/large.bin"));sender.send(target,Collections.singletonList(source));until(()->active(receiver[0])!=null&&active(receiver[0]).done>2*1024*1024,"second bytes");Engine.Job outgoing=active(sender);sender.pause(outgoing.id,true);until(()->outgoing.retryPending,"paused sender leaves transfer worker");Thread.sleep(200);long before=active(receiver[0])==null?0:active(receiver[0]).done;
            Engine.Config config=receiver[0].config;Engine.Platform platform=receiver[0].platform;receiver[0].close();receiver[0]=new Engine(config,platform);receiver[0].start();
            sender.cancel(outgoing.id);sender.send(sender.probe("127.0.0.1",receiver[0].port()).id,Collections.singletonList(source));
            until(()->Files.exists(root.resolve("receiver/received/large.bin")),"restart reselect completion");TransferReadTest.require(Arrays.equals(bytes,Files.readAllBytes(root.resolve("receiver/received/large.bin"))),"restart contents");
            System.out.println("PASS native reselect after receiver restart reuses persistent partial; exact contents");
        }finally{sender.close();receiver[0].close();}
    }
}
