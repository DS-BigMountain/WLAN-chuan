package local.chuan.core;

import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.*;
import java.util.function.*;

/** Metadata, expensive verification and transport use independent executors. */
final class FilePipeline {
    static final int DONE=0,READY=1,VERIFY=2;
    static final int PROBE_LIMIT=15,VERIFICATION_LIMIT=15,TRANSFER_LIMIT=4,READY_LIMIT=32;
    static void run(Engine engine,List<Engine.Job> files,Predicate<Engine.Job> prepare,Consumer<Engine.Job> transfer)throws InterruptedException {
        run(engine,files,job->VERIFY,prepare,transfer);
    }
    static void run(Engine engine,List<Engine.Job> files,ToIntFunction<Engine.Job> probe,Predicate<Engine.Job> verify,Consumer<Engine.Job> transfer)throws InterruptedException {
        if(files.isEmpty())return;
        AtomicInteger next=new AtomicInteger();Semaphore ready=new Semaphore(READY_LIMIT);
        CountDownLatch finished=new CountDownLatch(files.size());
        Consumer<Engine.Job> publish=job->{
            boolean acquired=false;
            try{
                ready.acquire();acquired=true;engine.pipeline(job,"ready",READY_LIMIT-ready.availablePermits());
                engine.fileTransfers.execute(()->{
                    ready.release();engine.diagnosticTransferring.incrementAndGet();engine.pipeline(job,"transfer_start",READY_LIMIT-ready.availablePermits());
                    try{transfer.accept(job);}finally{engine.diagnosticTransferring.decrementAndGet();engine.pipeline(job,"transfer_end",READY_LIMIT-ready.availablePermits());finished.countDown();}
                });
            }catch(InterruptedException error){if(acquired)ready.release();finished.countDown();Thread.currentThread().interrupt();}
            catch(RejectedExecutionException error){if(acquired)ready.release();finished.countDown();}
        };
        for(int i=0;i<Math.min(PROBE_LIMIT,files.size());i++)engine.filePreparation.execute(()->{
            for(int index;engine.running&&(index=next.getAndIncrement())<files.size();){
                Engine.Job job=files.get(index);engine.diagnosticPreparing.incrementAndGet();engine.pipeline(job,"probe_start",READY_LIMIT-ready.availablePermits());
                int route=DONE;
                try{route=probe.applyAsInt(job);}catch(Exception error){engine.state(job,job.cancelled?4:5,error.getMessage());}
                finally{engine.diagnosticPreparing.decrementAndGet();engine.pipeline(job,"probe_end",READY_LIMIT-ready.availablePermits());}
                if(route==READY)publish.accept(job);
                else if(route==VERIFY){
                    engine.diagnosticVerificationPending.incrementAndGet();engine.pipeline(job,"verification_queued",READY_LIMIT-ready.availablePermits());
                    try{engine.fileVerification.execute(()->{
                        engine.diagnosticVerificationPending.decrementAndGet();engine.diagnosticVerifying.incrementAndGet();engine.pipeline(job,"verification_start",READY_LIMIT-ready.availablePermits());boolean prepared=false;
                        try{prepared=verify.test(job);}catch(Exception error){engine.state(job,job.cancelled?4:5,error.getMessage());}
                        finally{engine.diagnosticVerifying.decrementAndGet();engine.pipeline(job,"verification_end",READY_LIMIT-ready.availablePermits());}
                        if(prepared)publish.accept(job);else finished.countDown();
                    });}catch(RejectedExecutionException error){engine.diagnosticVerificationPending.decrementAndGet();finished.countDown();}
                }else finished.countDown();
            }
        });
        while(engine.running&&!finished.await(100,TimeUnit.MILLISECONDS)){}
    }
}
