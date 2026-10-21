package local.chuan.core;

import java.io.*;
import java.nio.file.*;
import java.util.*;

/** Focused ownership check: a copy must not delete directory trees or skipped originals. */
public final class CopyDeletionTest {
    static void require(boolean value,String message){if(!value)throw new AssertionError(message);}
    public static void main(String[] args)throws Exception {
        Path base=Paths.get(args[0]).resolve(UUID.randomUUID().toString());Files.createDirectories(base);
        for(boolean external:new boolean[]{false,true}){
            Path root=base.resolve(external?"external":"private"),directory=root.resolve("folder");Files.createDirectories(directory);
            Path original=directory.resolve("existing.txt"),received=directory.resolve("new.txt"),unrelated=directory.resolve("unrelated.txt");
            Files.write(original,new byte[]{1});Files.write(received,new byte[]{2});Files.write(unrelated,new byte[]{3});
            List<Long> providerDeletes=Collections.synchronizedList(new ArrayList<>());
            Engine engine=new Engine(new Engine.Config(root.toFile()),new Engine.Platform(){
                public InputStream asset(String name)throws IOException{throw new FileNotFoundException(name);}
                public File apk(){return null;}public String saved(Engine.Job job){return "";}
                public void settingsChanged(Engine.Config config){}
                public void deleted(Engine.Job job)throws IOException{
                    require(!job.directory&&!job.skipped,"provider must never receive a directory or skipped-file deletion");providerDeletes.add(job.id);
                    if(external)Files.deleteIfExists(job.file.toPath());
                }
            });
            try{
                Engine.Job dir=engine.job("folder","sender",0,true),old=engine.job("folder/existing.txt","sender",1,true),fresh=engine.job("folder/new.txt","sender",1,true);
                for(Engine.Job job:Arrays.asList(dir,old,fresh)){job.folderId="folder-id";job.folderName="folder";job.batchId="batch";job.state=3;}
                dir.directory=true;dir.file=directory.toFile();old.skipped=true;old.file=original.toFile();fresh.file=received.toFile();
                engine.delete(dir.id);long deadline=System.currentTimeMillis()+5000;
                while(!fresh.removed&&System.currentTimeMillis()<deadline)Thread.sleep(10);
                require(dir.removed&&old.removed&&fresh.removed,"all batch records must be removed");
                require(providerDeletes.equals(Collections.singletonList(fresh.id)),"only newly received file belongs to deletion scope");
                require(Files.isDirectory(directory)&&Files.exists(original)&&Files.exists(unrelated),"original directory and unrelated contents must remain");
                require(!Files.exists(received),"newly received file must be deleted");
                System.out.println("PASS "+(external?"external-provider":"private-storage")+": preserve directory, skipped original and unrelated files; delete newly received file");
            }finally{engine.close();}
        }
    }
}
