package local.chuan.core;

import java.io.*;
import java.net.*;
import java.security.*;
import java.util.*;

/** GitHub release metadata and bounded HTTPS downloads shared with the JVM test harness. */
public final class UpdateRelease {
    public static final String API="https://api.github.com/repos/DS-BigMountain/WLAN-chuan/releases/latest";
    public static final String PAGE="https://github.com/DS-BigMountain/WLAN-chuan/releases/latest";
    public final String version,notes,url,digest,name;
    public final long size;
    public interface Progress { void receive(long done,long total)throws IOException; }
    public interface Cancellation { boolean cancelled(); }
    private UpdateRelease(String version,String notes,String url,String digest,String name,long size){this.version=version;this.notes=notes;this.url=url;this.digest=digest;this.name=name;this.size=size;}
    public static int[] version(String value)throws IOException {
        if(value.startsWith("V"))value=value.substring(1);
        String[] parts=value.split("\\.",-1);if(parts.length!=3)throw new IOException("版本号格式无效");int[] numbers=new int[3];
        for(int i=0;i<3;i++){if(!parts[i].matches("[0-9]{1,5}"))throw new IOException("版本号格式无效");numbers[i]=Integer.parseInt(parts[i]);if(numbers[i]>65535)throw new IOException("版本号格式无效");}return numbers;
    }
    public static int compare(String a,String b)throws IOException {int[] x=version(a),y=version(b);for(int i=0;i<3;i++)if(x[i]!=y[i])return Integer.compare(x[i],y[i]);return 0;}
    public static String tag(String body)throws IOException {Map<String,Object> root=Json.object(body);if(Boolean.TRUE.equals(root.get("draft"))||Boolean.TRUE.equals(root.get("prerelease")))throw new IOException("未找到正式发行版本");String tag=Json.string(root,"tag_name","");if(!tag.startsWith("V"))throw new IOException("发行版本须采用 Vx.x.x 格式");version(tag);return tag;}
    public static UpdateRelease parse(String body,String platform)throws IOException {
        Map<String,Object> root=Json.object(body);String tag=tag(body),name="Chuan-"+tag.substring(1)+"-"+platform;
        Object assets=root.get("assets");if(!(assets instanceof List))throw new IOException("此版本缺少更新资源");Map<?,?> found=null;
        for(Object item:(List<?>)assets)if(item instanceof Map&&name.equals(((Map<?,?>)item).get("name"))){if(found!=null)throw new IOException("更新资源名称重复");found=(Map<?,?>)item;}
        if(found==null)throw new IOException("此版本缺少对应平台的更新包");
        String url=String.valueOf(found.get("browser_download_url")),digest=String.valueOf(found.get("digest"));Object rawSize=found.get("size");
        if(!(rawSize instanceof Long)||((Long)rawSize)<=0||((Long)rawSize)>512L*1024*1024)throw new IOException("更新包大小超出允许范围");
        if(!url.equals("https://github.com/DS-BigMountain/WLAN-chuan/releases/download/"+tag+"/"+name))throw new IOException("更新下载地址无效");
        if(!digest.matches("sha256:[0-9a-fA-F]{64}"))throw new IOException("更新包缺少有效的 SHA-256 摘要，请从发布页下载");
        return new UpdateRelease(tag,Json.string(root,"body",""),url,digest.substring(7).toLowerCase(Locale.ROOT),name,(Long)rawSize);
    }
    private static HttpsURLConnectionHolder connect(String address)throws IOException {
        URL url=new URL(address);
        for(int redirect=0;redirect<6;redirect++){
            String host=url.getHost();
            if(!url.getProtocol().equals("https")||(url.getPort()!=-1&&url.getPort()!=443)||url.getUserInfo()!=null||!(host.equals("api.github.com")||host.equals("github.com")||host.equals("release-assets.githubusercontent.com")||host.equals("objects.githubusercontent.com")))throw new IOException("更新下载地址无效");
            javax.net.ssl.HttpsURLConnection connection=(javax.net.ssl.HttpsURLConnection)url.openConnection();connection.setInstanceFollowRedirects(false);connection.setConnectTimeout(15000);connection.setReadTimeout(15000);connection.setRequestProperty("User-Agent","Chuan-Updater/2");connection.setRequestProperty("Accept",host.equals("api.github.com")?"application/vnd.github+json":"application/octet-stream");connection.setRequestProperty("Accept-Encoding","identity");
            int code;try{code=connection.getResponseCode();}catch(IOException e){connection.disconnect();throw e;}
            if(code==301||code==302||code==303||code==307||code==308){String location=connection.getHeaderField("Location");connection.disconnect();if(location==null)throw new IOException("更新重定向地址缺失");url=new URL(url,location);continue;}
            if(code!=200){connection.disconnect();throw new IOException(code==403||code==429?"GitHub 请求受限，请稍后重试或打开发布页":"GitHub 未返回可用更新资源");}
            return new HttpsURLConnectionHolder(connection);
        }throw new IOException("更新下载重定向次数过多");
    }
    private static final class HttpsURLConnectionHolder implements Closeable {final javax.net.ssl.HttpsURLConnection connection;HttpsURLConnectionHolder(javax.net.ssl.HttpsURLConnection c){connection=c;}public void close(){connection.disconnect();}}
    public static String fetch(Cancellation cancel)throws IOException {ByteArrayOutputStream out=new ByteArrayOutputStream();transfer(API,out,1048576,false,cancel,(done,total)->{});return new String(out.toByteArray(),java.nio.charset.StandardCharsets.UTF_8);}
    public void download(File file,Cancellation cancel,Progress progress)throws IOException {try(FileOutputStream out=new FileOutputStream(file)){transfer(url,out,size,true,cancel,progress);out.getFD().sync();}catch(IOException e){file.delete();throw e;}}
    private static void transfer(String url,OutputStream out,long limit,boolean exact,Cancellation cancel,Progress progress)throws IOException {
        try(HttpsURLConnectionHolder holder=connect(url)){long length=holder.connection.getContentLengthLong();if(length>limit||(exact&&length>=0&&length!=limit))throw new IOException("更新响应大小无效");try(InputStream in=holder.connection.getInputStream()){byte[] buffer=new byte[131072];long done=0;for(;;){if(cancel.cancelled())throw new IOException("更新操作已取消");int count=in.read(buffer);if(count<0)break;if(count==0)continue;done+=count;if(done>limit)throw new IOException("更新响应超出大小限制");out.write(buffer,0,count);progress.receive(done,exact?limit:length);}if(exact&&done!=limit)throw new IOException("更新包下载不完整，请重试");}}
    }
    public static String hash(File file)throws IOException {try{MessageDigest hash=MessageDigest.getInstance("SHA-256");try(InputStream in=new FileInputStream(file)){byte[] buffer=new byte[131072];int count;while((count=in.read(buffer))!=-1)hash.update(buffer,0,count);}StringBuilder out=new StringBuilder();for(byte b:hash.digest())out.append(String.format(Locale.ROOT,"%02x",b&255));return out.toString();}catch(NoSuchAlgorithmException e){throw new IOException("无法初始化更新校验",e);}}
}
