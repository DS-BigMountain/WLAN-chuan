package local.chuan.core;
import java.nio.file.*;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.*;

/** Actual worker capacity and independence, without disk/network timing assumptions. */
public final class PipelineCapacityTest {
    public static void main(String[] args)throws Exception{
        Engine engine=TransferReadTest.host(Paths.get(args[0]).resolve(UUID.randomUUID().toString()));
        CountDownLatch probes=new CountDownLatch(15),verification=new CountDownLatch(15),payloads=new CountDownLatch(4),completed=new CountDownLatch(32),releaseProbes=new CountDownLatch(1),releaseVerification=new CountDownLatch(1),releasePayloads=new CountDownLatch(1);
        AtomicInteger probing=new AtomicInteger(),verifying=new AtomicInteger(),transferring=new AtomicInteger(),maxProbing=new AtomicInteger(),maxVerifying=new AtomicInteger(),maxTransferring=new AtomicInteger();
        ExecutorService driver=Executors.newSingleThreadExecutor();
        List<Engine.Job> jobs=new ArrayList<>();for(int i=0;i<80;i++)jobs.add(engine.job(String.valueOf(i),"capacity",1,false));
        Future<?> run=driver.submit(()->{try{FilePipeline.run(engine,jobs,job->{
            maxProbing.accumulateAndGet(probing.incrementAndGet(),Math::max);probes.countDown();await(releaseProbes);probing.decrementAndGet();return Integer.parseInt(job.name)<48?FilePipeline.VERIFY:FilePipeline.READY;
        },job->{maxVerifying.accumulateAndGet(verifying.incrementAndGet(),Math::max);verification.countDown();await(releaseVerification);verifying.decrementAndGet();return false;
        },job->{maxTransferring.accumulateAndGet(transferring.incrementAndGet(),Math::max);payloads.countDown();await(releasePayloads);transferring.decrementAndGet();completed.countDown();});}catch(InterruptedException error){Thread.currentThread().interrupt();throw new RuntimeException(error);}});
        try{
            TransferReadTest.require(probes.await(5,TimeUnit.SECONDS),"15 metadata probes started");releaseProbes.countDown();
            TransferReadTest.require(verification.await(5,TimeUnit.SECONDS)&&payloads.await(5,TimeUnit.SECONDS),"15 verifications overlap four payloads");
            releasePayloads.countDown();TransferReadTest.require(completed.await(5,TimeUnit.SECONDS)&&verifying.get()==15,"missing files complete while all 15 verifiers remain blocked");
            releaseVerification.countDown();run.get(5,TimeUnit.SECONDS);
            TransferReadTest.require(maxProbing.get()==15&&maxVerifying.get()==15&&maxTransferring.get()==4,"exact stage capacities");
            System.out.println("PASS Android: 15 metadata probes, 15 verifications, four payloads and independent progress");
        }finally{releaseProbes.countDown();releasePayloads.countDown();releaseVerification.countDown();driver.shutdownNow();engine.close();}
    }
    static void await(CountDownLatch gate){try{if(!gate.await(15,TimeUnit.SECONDS))throw new IllegalStateException("gate timeout");}catch(InterruptedException error){Thread.currentThread().interrupt();throw new IllegalStateException(error);}}
}
