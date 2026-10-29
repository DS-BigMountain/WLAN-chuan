package local.chuan.app;

import android.content.*;
import android.content.pm.*;
import android.os.Build;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.*;
import java.util.concurrent.*;
import local.chuan.core.*;

/** Application-scoped updater: Activity recreation does not discard an in-progress download. */
final class AppUpdater {
    private static AppUpdater instance;
    static synchronized AppUpdater get(Context context){if(instance==null)instance=new AppUpdater(context.getApplicationContext());return instance;}
    private final Context context;
    private final ExecutorService worker=Executors.newSingleThreadExecutor();
    private volatile boolean cancel;
    private String phase="idle",message="当前版本 V"+Engine.VERSION;
    private long done,total;
    private UpdateRelease release;
    private String metadata="";
    private String noticeVersion="",settingsSeen="",updateSeen="";
    static File directory(Context context){return new File(context.getFilesDir(),"update");}
    static File apk(Context context){return new File(directory(context),"Chuan.apk");}
    private AppUpdater(Context c){context=c;File saved=new File(directory(c),"release.json");if(saved.isFile()&&apk(c).isFile()){phase="checking";message="正在检查已下载的更新…";worker.execute(()->{try{String body=read(saved);UpdateRelease value=UpdateRelease.parse(body,"android.apk");if(UpdateRelease.compare(value.version,Engine.VERSION)<=0){clear();set("idle","当前版本 V"+Engine.VERSION);return;}verify(value);synchronized(this){release=value;metadata=body;noticeVersion=value.version;done=total=value.size;}set("ready","下载完成，需要安装更新。");}catch(Exception e){clear();set("idle","当前版本 V"+Engine.VERSION);}finally{if(!"ready".equals(state().get("phase")))check();}});}else check();}
    private static String read(File file)throws IOException {if(file.length()>1048576)throw new IOException("更新信息过长");try(InputStream in=new FileInputStream(file);ByteArrayOutputStream out=new ByteArrayOutputStream()){byte[] buffer=new byte[8192];int n;while((n=in.read(buffer))!=-1){if(out.size()+n>1048576)throw new IOException("更新信息过长");out.write(buffer,0,n);}return new String(out.toByteArray(),StandardCharsets.UTF_8);}}
    private void clear(){apk(context).delete();new File(directory(context),"package.part").delete();new File(directory(context),"release.json").delete();}
    private synchronized void set(String value,String detail){phase=value;message=detail;}
    synchronized Map<String,Object> state(){return Json.map("phase",phase,"message",message,"done",done,"total",total,"version",release==null?"":release.version,"notes",release==null?"":release.notes,"settingsUnread",!noticeVersion.isEmpty()&&!noticeVersion.equals(settingsSeen),"updateUnread",!noticeVersion.isEmpty()&&!noticeVersion.equals(updateSeen));}
    synchronized void readNotice(boolean detail){settingsSeen=noticeVersion;if(detail)updateSeen=noticeVersion;}
    synchronized boolean busy(){return phase.equals("checking")||phase.equals("downloading");}
    synchronized void checkOnEntry(){if(!busy()&&!phase.equals("ready"))check();}
    synchronized void check(){if(busy())return;cancel=false;set("checking","正在检查 GitHub 更新…");worker.execute(()->{try{String body=UpdateRelease.fetch(()->cancel);String tag=UpdateRelease.tag(body);if(UpdateRelease.compare(tag,Engine.VERSION)<=0){synchronized(this){noticeVersion="";}set("current","当前已是最新正式版本 V"+Engine.VERSION);return;}UpdateRelease value=UpdateRelease.parse(body,"android.apk");synchronized(this){release=value;metadata=body;noticeVersion=value.version;done=0;total=value.size;}set("available","发现新版本 "+value.version);}catch(Exception e){set("error",detail(e));}});}
    synchronized void download(){if(!phase.equals("available")||release==null)return;final UpdateRelease value=release;cancel=false;done=0;set("downloading","正在下载更新…");worker.execute(()->{try{File dir=directory(context);if(!dir.isDirectory()&&!dir.mkdirs())throw new IOException("无法创建更新缓存目录");if(dir.getUsableSpace()<value.size+8L*1024*1024)throw new IOException("存储空间不足");File partial=new File(dir,"package.part");value.download(partial,()->cancel,(count,size)->{synchronized(this){done=count;total=size;}});if(cancel)throw new IOException("更新操作已取消");set("downloading","正在校验更新包…");if(!UpdateRelease.hash(partial).equals(value.digest))throw new IOException("更新包 SHA-256 校验失败，请重试");if(apk(context).exists()&&!apk(context).delete())throw new IOException("无法清理旧更新包");if(!partial.renameTo(apk(context)))throw new IOException("无法保存更新包");verify(value);if(cancel)throw new IOException("更新操作已取消");try(FileOutputStream out=new FileOutputStream(new File(dir,"release.json"))){out.write(metadata.getBytes(StandardCharsets.UTF_8));out.getFD().sync();}set("ready","下载完成，需要安装更新。");}catch(Exception e){clear();set("error",detail(e));}});}
    void cancel(){cancel=true;}
    private static String detail(Exception e){String text=e.getMessage();return text==null?"更新失败，请检查网络后重试":text;}
    private static Set<String> certificates(PackageInfo info)throws IOException {
        android.content.pm.Signature[] signatures;
        if(Build.VERSION.SDK_INT>=28){if(info.signingInfo==null)throw new IOException("无法读取 APK 签名");signatures=info.signingInfo.getApkContentsSigners();}else signatures=info.signatures;
        if(signatures==null||signatures.length==0)throw new IOException("APK 缺少签名");Set<String> result=new HashSet<>();for(android.content.pm.Signature signature:signatures)result.add(signature.toCharsString());return result;
    }
    @SuppressWarnings("deprecation") private void verify(UpdateRelease value)throws Exception {
        File file=apk(context);if(file.length()!=value.size||!UpdateRelease.hash(file).equals(value.digest))throw new IOException("更新包校验失败，请重新下载");
        PackageManager manager=context.getPackageManager();int flags=Build.VERSION.SDK_INT>=28?PackageManager.GET_SIGNING_CERTIFICATES:PackageManager.GET_SIGNATURES;
        PackageInfo incoming=manager.getPackageArchiveInfo(file.getAbsolutePath(),flags),current=manager.getPackageInfo(context.getPackageName(),flags);
        if(incoming==null||!context.getPackageName().equals(incoming.packageName)||!value.version.substring(1).equals(incoming.versionName)||UpdateRelease.compare(incoming.versionName,Engine.VERSION)<=0)throw new IOException("APK 标识或版本无效");
        long oldCode=Build.VERSION.SDK_INT>=28?current.getLongVersionCode():current.versionCode,newCode=Build.VERSION.SDK_INT>=28?incoming.getLongVersionCode():incoming.versionCode;
        if(newCode<=oldCode)throw new IOException("APK 内部版本号未递增，无法覆盖更新");
        if(!certificates(current).equals(certificates(incoming)))throw new IOException("APK 签名与已安装应用不一致，无法覆盖更新");
    }
    void prepareInstall(Runnable ready,java.util.function.Consumer<String> failure){final UpdateRelease value;synchronized(this){if(!phase.equals("ready")||release==null){failure.accept("请先下载更新包");return;}value=release;set("checking","正在验证安装文件…");}worker.execute(()->{try{verify(value);set("ready","下载完成，需要安装更新。");ready.run();}catch(Exception e){clear();set("error",detail(e));failure.accept(detail(e));}});}
}
