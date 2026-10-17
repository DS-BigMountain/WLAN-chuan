package local.chuan.core;

import java.io.*;
import java.net.*;
import java.nio.charset.StandardCharsets;
import java.security.*;
import java.util.*;

public final class Protocol {
    private Protocol() {}
    public static String hex(String text){return hex(text.getBytes(StandardCharsets.UTF_8));}
    public static String hex(byte[] bytes){StringBuilder out=new StringBuilder(bytes.length*2);for(byte value:bytes)out.append("0123456789abcdef".charAt((value&255)>>>4)).append("0123456789abcdef".charAt(value&15));return out.toString();}
    public static String unhex(String value){if((value.length()&1)!=0)throw new IllegalArgumentException("十六进制编码无效");byte[] out=new byte[value.length()/2];for(int i=0;i<out.length;i++){int a=Character.digit(value.charAt(i*2),16),b=Character.digit(value.charAt(i*2+1),16);if(a<0||b<0)throw new IllegalArgumentException("十六进制编码无效");out[i]=(byte)(a*16+b);}return new String(out,StandardCharsets.UTF_8);}
    public static String random(){return UUID.randomUUID().toString().replace("-","");}
    public static boolean key(String value){return value.matches("[0-9a-fA-F]{32}");}
    public static long number(String value){if(!value.matches("[0-9]{1,19}"))throw new IllegalArgumentException("非负整数无效");return Long.parseLong(value);}
    public static String safePath(String input){
        String value=input.replace('\\','/');if(value.isEmpty()||value.length()>4096||value.startsWith("/")||value.endsWith("/"))throw new IllegalArgumentException("文件路径无效");
        for(String part:value.split("/",-1)){if(part.isEmpty()||part.equals(".")||part.equals("..")||part.length()>240||part.endsWith(".")||part.endsWith(" "))throw new IllegalArgumentException("文件路径无效");for(char c:part.toCharArray())if(c<32||"<>:\"|?*".indexOf(c)>=0)throw new IllegalArgumentException("文件路径包含无效字符");String base=part.split("\\.")[0].toUpperCase(Locale.ROOT);if(base.matches("CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9]"))throw new IllegalArgumentException("文件名与 Windows 保留名称冲突");}
        return value;
    }
    public static byte[] read(InputStream input,long count,int limit)throws IOException {if(count<0||count>limit)throw new IOException("请求数据过长");ByteArrayOutputStream out=new ByteArrayOutputStream((int)count);copy(input,out,count,null,null);return out.toByteArray();}
    public interface Progress {void update(long bytes)throws IOException;}
    public static void copy(InputStream input,OutputStream output,long count,MessageDigest hash,Progress progress)throws IOException {
        byte[] buffer=new byte[128*1024];long done=0;
        while(done<count){int read=input.read(buffer,0,(int)Math.min(buffer.length,count-done));if(read<0)throw new EOFException("连接中断或文件长度已变更");if(read==0)continue;output.write(buffer,0,read);if(hash!=null)hash.update(buffer,0,read);done+=read;if(progress!=null)progress.update(done);}
    }
    public static MessageDigest sha(){try{return MessageDigest.getInstance("SHA-256");}catch(NoSuchAlgorithmException impossible){throw new AssertionError(impossible);}}
    public static final class Header {
        public String first="";public final Map<String,String> fields=new LinkedHashMap<>();
        public String get(String name){return fields.getOrDefault(name.toLowerCase(Locale.ROOT),"");}
        public long length(){return number(get("content-length"));}
        public int status(){return Integer.parseInt(first.split(" ")[1]);}
    }
    public static Header header(InputStream input)throws IOException {
        ByteArrayOutputStream bytes=new ByteArrayOutputStream();int mark=0;
        for(int count=0;count<16384;count++){int c=input.read();if(c<0)throw new EOFException("连接已关闭");bytes.write(c);mark=(mark<<8)|c;if(mark==0x0d0a0d0a){Header out=new Header();String[] lines=new String(bytes.toByteArray(),StandardCharsets.ISO_8859_1).split("\r\n");out.first=lines[0];for(int i=1;i<lines.length;i++){int at=lines[i].indexOf(':');if(at<=0)throw new IOException("HTTP 首部无效");String key=lines[i].substring(0,at).toLowerCase(Locale.ROOT),value=lines[i].substring(at+1).trim();if(out.fields.put(key,value)!=null)throw new IOException("HTTP 首部重复");}if(!out.get("transfer-encoding").isEmpty())throw new IOException("不支持分块请求");return out;}}
        throw new IOException("HTTP 首部过长");
    }
    public static void ascii(OutputStream output,String data)throws IOException{output.write(data.getBytes(StandardCharsets.ISO_8859_1));output.flush();}
    public static void response(OutputStream output,int status,String message,String body,String type)throws IOException {
        byte[] bytes=body.getBytes(StandardCharsets.UTF_8);ascii(output,"HTTP/1.1 "+status+" "+message+"\r\nConnection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nContent-Type: "+type+"\r\nContent-Length: "+bytes.length+"\r\n\r\n");output.write(bytes);output.flush();
    }
    public static String encodeName(String name){try{return URLEncoder.encode(name,"UTF-8").replace("+","%20");}catch(UnsupportedEncodingException impossible){throw new AssertionError(impossible);}}
    public static Map<String,String> query(String target){Map<String,String> out=new LinkedHashMap<>();int at=target.indexOf('?');if(at>=0)for(String pair:target.substring(at+1).split("&")){String[] parts=pair.split("=",2);try{out.put(URLDecoder.decode(parts[0],"UTF-8"),parts.length==2?URLDecoder.decode(parts[1],"UTF-8"):"");}catch(Exception ignored){throw new IllegalArgumentException("查询参数无效");}}return out;}
    public static String mime(String name){return name.endsWith(".css")?"text/css; charset=utf-8":name.endsWith(".js")?"text/javascript; charset=utf-8":name.endsWith(".svg")?"image/svg+xml":name.endsWith(".html")?"text/html; charset=utf-8":"application/octet-stream";}
}
