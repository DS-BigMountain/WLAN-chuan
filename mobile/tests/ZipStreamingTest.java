package local.chuan.core;
import java.io.*;import java.nio.file.*;import java.util.*;import java.util.concurrent.*;import java.util.concurrent.atomic.*;import java.util.zip.*;
public final class ZipStreamingTest {
 public static void main(String[] args)throws Exception{
  Path root=Paths.get(args[0]).resolve(UUID.randomUUID().toString());Engine sender=TransferReadTest.host(root.resolve("sender")),receiver=TransferReadTest.host(root.resolve("receiver"));CountDownLatch late=new CountDownLatch(1),release=new CountDownLatch(1);AtomicInteger reads=new AtomicInteger();
  try{sender.config.zipFolders=true;byte[] large=new byte[8*1024*1024];Arrays.fill(large,(byte)31);
   Engine.Source first=new Engine.Source("流式/a.bin",large.length,()->{reads.incrementAndGet();return new ByteArrayInputStream(large);});
   Engine.Source last=new Engine.Source("流式/late.txt",4,()->{late.countDown();PipelineTransferTest.await(release);return new ByteArrayInputStream(new byte[]{1,2,3,4});});
   sender.send(sender.probe("127.0.0.1",receiver.port()).id,Arrays.asList(Engine.Source.directory("流式"),first,last));
   TransferReadTest.require(late.await(8,TimeUnit.SECONDS),"last source has not completed");
   synchronized(receiver.lock){TransferReadTest.require(receiver.jobs.values().stream().anyMatch(j->j.done>1024*1024&&j.state==2),"receiver already has ZIP bytes while last source remains blocked");}
   TransferReadTest.require(!Files.exists(root.resolve("receiver/received/流式.zip")),"archive not committed early");release.countDown();TransferReadTest.finished(sender);
   try(ZipFile zip=new ZipFile(root.resolve("receiver/received/流式.zip").toFile())){TransferReadTest.require(zip.getEntry("流式/a.bin").getSize()==large.length,"ZIP source size");try(InputStream input=zip.getInputStream(zip.getEntry("流式/late.txt"))){TransferReadTest.require(input.read()==1,"last file content");}}
   TransferReadTest.require(reads.get()==1,"source content read once");TransferReadTest.require(!Files.exists(root.resolve("sender/relay/archive-send")),"no archive-sized sender cache");
   System.out.println("PASS native ZIP sends before the last source opens; one source read; no ZIP cache; final archive verified");
  }finally{release.countDown();sender.close();receiver.close();}
 }
}
