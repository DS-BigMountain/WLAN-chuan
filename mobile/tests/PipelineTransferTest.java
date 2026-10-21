package local.chuan.core;
import java.io.*;
import java.nio.file.*;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.*;

/** A slow existing-file comparison must not occupy any of the four payload slots. */
public final class PipelineTransferTest {
    public static void main(String[] args)throws Exception {
        Path root=Paths.get(args[0]).resolve(UUID.randomUUID().toString());
        Engine receiver=TransferReadTest.host(root.resolve("receiver")),sender=TransferReadTest.host(root.resolve("sender"));
        CountDownLatch hashStarted=new CountDownLatch(15),releaseHash=new CountDownLatch(1),payloads=new CountDownLatch(4),releasePayloads=new CountDownLatch(1);
        try{
            byte[] bytes=new byte[65536];Arrays.fill(bytes,(byte)23);
            Path destination=root.resolve("receiver/received/流水线");Files.createDirectories(destination);for(int i=0;i<17;i++)Files.write(destination.resolve("slow-"+i+".bin"),bytes);
            List<Engine.Source> sources=new ArrayList<>();sources.add(Engine.Source.directory("流水线"));
            for(int i=0;i<17;i++)sources.add(new Engine.Source("流水线/slow-"+i+".bin",bytes.length,()->{
                hashStarted.countDown();await(releaseHash);return new ByteArrayInputStream(bytes);
            }));
            AtomicInteger opens=new AtomicInteger();
            for(int i=0;i<4;i++)sources.add(new Engine.Source("流水线/new-"+i+".bin",bytes.length,()->{
                opens.incrementAndGet();payloads.countDown();await(releasePayloads);return new ByteArrayInputStream(bytes);
            }));
            sender.send(sender.probe("127.0.0.1",receiver.port()).id,sources);
            TransferReadTest.require(hashStarted.await(5,TimeUnit.SECONDS),"existing-file comparison started");
            TransferReadTest.require(payloads.await(8,TimeUnit.SECONDS),"four payloads start while all 15 verification slots remain blocked");
            releasePayloads.countDown();
            long deadline=System.nanoTime()+TimeUnit.SECONDS.toNanos(8);
            for(int i=0;i<4;i++){
                Path file=destination.resolve("new-"+i+".bin");
                while(!Files.exists(file)&&System.nanoTime()<deadline)Thread.sleep(20);
                TransferReadTest.require(Arrays.equals(Files.readAllBytes(file),bytes),"ready file committed ahead of slow comparison");
            }
            TransferReadTest.require(opens.get()==4,"new files read once each");releaseHash.countDown();TransferReadTest.finished(sender);
            System.out.println("PASS four concurrent payloads complete ahead of 15 blocked verifications; source read once; byte identity");
        }finally{releaseHash.countDown();releasePayloads.countDown();sender.close();receiver.close();}
    }
    static void await(CountDownLatch latch)throws IOException{try{if(!latch.await(15,TimeUnit.SECONDS))throw new IOException("gate timeout");}catch(InterruptedException error){Thread.currentThread().interrupt();throw new IOException(error);}}
}
