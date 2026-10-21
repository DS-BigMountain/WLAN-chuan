package local.chuan.core;
import java.io.*;
import java.net.*;
import java.nio.file.*;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.*;

/** Source reads and remote preparation are observed on a real loopback transfer. */
public final class TransferReadTest {
    static void require(boolean value,String message){if(!value)throw new AssertionError(message);}
    static int port(boolean udp)throws IOException{if(udp)try(DatagramSocket socket=new DatagramSocket(0)){return socket.getLocalPort();}else try(ServerSocket socket=new ServerSocket(0)){return socket.getLocalPort();}}
    static Engine host(Path root)throws Exception{
        Engine.Config config=new Engine.Config(root.resolve("received").toFile());config.port=port(false);config.discoveryPort=port(true);
        Engine engine=new Engine(config,new Engine.Platform(){public InputStream asset(String name)throws IOException{throw new FileNotFoundException();}public File apk(){return null;}public String saved(Engine.Job job){return "";}public void deleted(Engine.Job job){}public void settingsChanged(Engine.Config c){}});engine.start();return engine;
    }
    static void finished(Engine engine)throws Exception{long until=System.nanoTime()+TimeUnit.SECONDS.toNanos(15);for(;;){synchronized(engine.lock){if(!engine.jobs.isEmpty()&&engine.jobs.values().stream().allMatch(job->job.state>=3))return;}if(System.nanoTime()>until)throw new AssertionError("transfer timeout");Thread.sleep(20);}}
    public static void main(String[] args)throws Exception{
        Path root=Paths.get(args[0]).resolve(UUID.randomUUID().toString());Engine receiver=host(root.resolve("receiver")),sender=host(root.resolve("sender"));
        try{
            String peer=sender.probe("127.0.0.1",receiver.port()).id;
            byte[] bytes=new byte[2*1024*1024];Arrays.fill(bytes,(byte)19);AtomicInteger opens=new AtomicInteger();AtomicBoolean hold=new AtomicBoolean();CountDownLatch reading=new CountDownLatch(1),release=new CountDownLatch(1);
            Engine.Source source=new Engine.Source("资料/a.bin",bytes.length,()->{opens.incrementAndGet();if(hold.get()){reading.countDown();try{if(!release.await(10,TimeUnit.SECONDS))throw new IOException("gate timeout");}catch(InterruptedException error){throw new IOException(error);}}return new ByteArrayInputStream(bytes);});
            sender.send(peer,Arrays.asList(Engine.Source.directory("资料"),source));finished(sender);
            require(opens.get()==1,"new files must be read once, without a checksum pre-pass");
            require(Arrays.equals(bytes,Files.readAllBytes(root.resolve("receiver/received/资料/a.bin"))),"byte identity");
            hold.set(true);sender.send(peer,Arrays.asList(Engine.Source.directory("资料"),source));require(reading.await(5,TimeUnit.SECONDS),"source verification began");
            synchronized(receiver.lock){require(receiver.jobs.values().stream().anyMatch(job->job.name.equals("资料/a.bin")&&job.state<3&&job.activity.equals("等待发送端核对同名文件")),"receiver identifies the actual file and sender checksum phase");}
            release.countDown();finished(sender);require(opens.get()==2,"repeat copy reads source once for equality, without uploading payload again");
            synchronized(receiver.lock){require(receiver.jobs.values().stream().anyMatch(job->job.name.equals("资料/a.bin")&&job.skipped),"identical file skipped");}
            System.out.println("PASS one source read for a new file, streamed byte identity, remote checksum phase, verified repeat skip");
        }finally{sender.close();receiver.close();}
    }
}
