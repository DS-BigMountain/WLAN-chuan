import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import local.chuan.core.*;

public final class UpdateReleaseTest {
    static int checks;
    static void check(boolean value,String message){if(!value)throw new AssertionError(message);checks++;}
    interface Attempt {void run()throws Exception;}
    static void reject(Attempt action,String message)throws Exception {try{action.run();}catch(Exception error){checks++;return;}throw new AssertionError(message);}
    public static void main(String[] args)throws Exception {
        String body=new String(Files.readAllBytes(Paths.get(args[0])),StandardCharsets.UTF_8);
        UpdateRelease apk=UpdateRelease.parse(body,"android.apk");
        check(apk.version.equals(UpdateRelease.tag(body))&&apk.size>0&&apk.digest.length()==64,"Android release metadata");
        check(UpdateRelease.parse(body,"windows-x64.zip").name.equals("Chuan-"+apk.version.substring(1)+"-windows-x64.zip"),"platform selection");
        check(UpdateRelease.compare("V2.10.0","2.9.9")>0,"numeric version ordering");
        check(UpdateRelease.compare("V2.2.0","2.2.0")==0,"same version");
        for(String bad:new String[]{"2.2","V2.2.0.1","V2.-1.0","V65536.0.0","V2.2.0-beta","V2.2.0 "})reject(()->UpdateRelease.version(bad),"invalid version accepted");
        reject(()->UpdateRelease.parse(body,"linux.zip"),"missing platform accepted");
        reject(()->UpdateRelease.parse(body.replace("sha256:","md5:"),"android.apk"),"bad digest accepted");
        reject(()->UpdateRelease.parse(body.replace("https://github.com/DS-BigMountain/WLAN-chuan/releases/download/","https://example.org/"),"android.apk"),"foreign URL accepted");
        reject(()->UpdateRelease.parse(body.replaceAll("\"prerelease\"\\s*:\\s*false","\"prerelease\":true"),"android.apk"),"prerelease accepted");
        File sample=File.createTempFile("chuan-hash-",".bin",new File(args[1]));try{Files.write(sample.toPath(),"abc".getBytes(StandardCharsets.UTF_8));check(UpdateRelease.hash(sample).equals("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"),"SHA-256");}finally{sample.delete();}
        if(args.length>2&&args[2].equals("--live")){String live=UpdateRelease.fetch(()->false);check(UpdateRelease.compare(UpdateRelease.tag(live),apk.version)>=0,"live GitHub check");File download=new File(args[1],"current-release.apk");UpdateRelease current=UpdateRelease.parse(live,"android.apk");current.download(download,()->false,(done,total)->{});check(download.length()==current.size&&UpdateRelease.hash(download).equals(current.digest),"live download and digest");File cancelled=new File(args[1],"cancelled.part");reject(()->current.download(cancelled,()->true,(done,total)->{}),"cancelled download succeeded");check(!cancelled.exists(),"partial file retained");}
        System.out.println(checks+" Android update checks passed");
    }
}
