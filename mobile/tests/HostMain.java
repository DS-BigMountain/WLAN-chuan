package local.chuan.test;

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;
import local.chuan.core.*;

/** Console harness for real Windows/JVM/browser interoperability checks; never packaged. */
public final class HostMain {
    public static void main(String[] args)throws Exception {
        Map<String,String> options=new HashMap<>();for(int i=0;i<args.length;i+=2)options.put(args[i],args[i+1]);
        File root=new File(options.get("--root")),assets=new File(options.getOrDefault("--assets","src/web"));root.mkdirs();Engine.Config config=new Engine.Config(new File(root,"received"));config.port=Integer.parseInt(options.getOrDefault("--port","0"));config.discoveryPort=Integer.parseInt(options.get("--discovery-port"));config.id=options.getOrDefault("--id",Protocol.random());config.name=options.getOrDefault("--name","Android Protocol Host");config.autoReceive=!options.getOrDefault("--auto-receive","true").equals("false");
        Engine engine=new Engine(config,new Engine.Platform(){
            public InputStream asset(String name)throws IOException{return new FileInputStream(new File(assets,name));}
            public File apk(){return options.containsKey("--apk")?new File(options.get("--apk")):null;}
            public boolean externalStorage(){return options.containsKey("--external");}
            public String existing(String name,boolean directory,long size,String digest,Engine.Check check)throws IOException {
                if(!externalStorage())return null;File file=new File(options.get("--external"),name);if(!file.exists())return "";
                if(directory){if(!file.isDirectory())throw new IOException("type conflict");return file.toURI().toString();}
                if(digest==null){if(!file.isFile()||file.length()!=size)throw new IOException("content conflict");return file.toURI().toString();}
                java.security.MessageDigest hash=Protocol.sha();long count=0;try(InputStream input=new FileInputStream(file)){byte[] buffer=new byte[65536];int n;while((n=input.read(buffer))!=-1){check.run();hash.update(buffer,0,n);count+=n;}}
                if(count!=size||!Protocol.hex(hash.digest()).equals(digest))throw new IOException("content conflict");return file.toURI().toString();
            }
            public String saved(Engine.Job job)throws IOException {if(!externalStorage())return job.file.toURI().toString();Path target=Paths.get(options.get("--external"),job.name);if(job.directory)Files.createDirectories(target);else{Files.createDirectories(target.getParent());Files.copy(job.file.toPath(),target);}return target.toUri().toString();}
            public void deleted(Engine.Job job){}public void settingsChanged(Engine.Config config){}
        });
        Runtime.getRuntime().addShutdownHook(new Thread(engine::close));engine.start();System.out.println("READY "+Json.stringify(Json.map("port",engine.port(),"id",config.id,"instance",engine.instance)));System.out.flush();
        try(BufferedReader reader=new BufferedReader(new InputStreamReader(System.in,StandardCharsets.UTF_8))){String line;while((line=reader.readLine())!=null){Map<String,Object> command=Json.object(line);Object answer=Json.map();try{String kind=Json.string(command,"command","");switch(kind){case "state":answer=engine.snapshot();break;case "connect":engine.addPeer(Json.string(command,"address",""));break;case "send":{List<Engine.Source> files=new ArrayList<>();for(Object path:(List<?>)command.get("files")){Path file=Paths.get(path.toString());if(Files.isDirectory(file)){try(java.util.stream.Stream<Path> children=Files.walk(file)){children.forEach(child->{String relative=child.equals(file)?file.getFileName().toString():file.getFileName()+"/"+file.relativize(child).toString().replace('\\','/');files.add(Files.isDirectory(child)?Engine.Source.directory(relative):Engine.Source.file(child.toFile(),relative));});}}else files.add(Engine.Source.file(file.toFile(),file.getFileName().toString()));}engine.send(Json.string(command,"target",""),files);break;}case "approve":engine.decide(Json.number(command,"id",0),Json.string(command,"selection",""));break;case "cancel":engine.cancel(Json.number(command,"id",0));break;case "retry":engine.retry(Json.number(command,"id",0));break;case "settings":config.zipFolders=Boolean.TRUE.equals(command.get("zipFolders"));engine.configure(config.name,Boolean.TRUE.equals(command.get("autoReceive")),true,Json.number(command,"maxFiles",0),Json.number(command,"maxBytes",0));break;case "stop":engine.close();return;default:throw new IllegalArgumentException("Unknown command");}}catch(Exception error){answer=Json.map("error",error.toString());}System.out.println("ANSWER "+Json.stringify(Json.map("request",command.get("request"),"result",answer)));System.out.flush();}}
        finally{engine.close();}
    }
}
