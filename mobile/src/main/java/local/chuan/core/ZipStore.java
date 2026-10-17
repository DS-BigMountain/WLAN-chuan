package local.chuan.core;

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.*;
import java.util.zip.CRC32;
import static local.chuan.core.Engine.*;
import static local.chuan.core.Protocol.*;

/** One uncompressed ZIP64 file; no payload is opened while planning consent. */
final class ZipStore {
    static final class Entry {String path;Source source;byte[] name;long size,offset,crc;boolean directory;Entry(String path,Source source){this.path=path;this.source=source;directory=source==null;size=directory?0:source.size;name=path.getBytes(StandardCharsets.UTF_8);if(name.length>65535)throw new IllegalArgumentException("ZIP 路径过长："+path);}}
    static final class Plan {
        String name;List<Source> sources;List<Entry> entries=new ArrayList<>();long size,total,count,centralOffset,centralSize;int time,date;
        Plan(String root,List<Source> sources){
            this.name=root+".zip";this.sources=new ArrayList<>(sources);Set<String> directories=new TreeSet<>(),paths=new HashSet<>();directories.add(root+"/");
            for(Source source:sources){if(!source.path.startsWith(root+"/")||!paths.add(source.path.toLowerCase(Locale.ROOT)))throw new IllegalArgumentException("ZIP 源路径重复或超出目录："+source.path);int at=source.path.indexOf('/');while(at>=0){directories.add(source.path.substring(0,at+1));at=source.path.indexOf('/',at+1);}total=Math.addExact(total,source.size);count++;}
            for(String path:directories)entries.add(new Entry(path,null));for(Source source:sources)entries.add(new Entry(source.path,source));
            long offset=0;for(Entry entry:entries){entry.offset=offset;offset=Math.addExact(offset,Math.addExact(entry.size,74L+entry.name.length));centralSize=Math.addExact(centralSize,74L+entry.name.length);}centralOffset=offset;size=Math.addExact(Math.addExact(offset,centralSize),98);
            Calendar now=Calendar.getInstance();time=now.get(Calendar.HOUR_OF_DAY)<<11|now.get(Calendar.MINUTE)<<5|now.get(Calendar.SECOND)/2;date=(Math.max(1980,Math.min(2107,now.get(Calendar.YEAR)))-1980)<<9|(now.get(Calendar.MONTH)+1)<<5|now.get(Calendar.DAY_OF_MONTH);
        }
        Source metadata(){return new Source(name,size,()->{throw new IOException("ZIP 尚未打包");});}
    }
    static List<Plan> plans(List<Source> sources){Map<String,List<Source>> roots=new LinkedHashMap<>();for(Source source:sources){int at=source.path.indexOf('/');if(at>=0)roots.computeIfAbsent(source.path.substring(0,at),key->new ArrayList<>()).add(source);}List<Plan> result=new ArrayList<>();for(Map.Entry<String,List<Source>> root:roots.entrySet())result.add(new Plan(root.getKey(),root.getValue()));return result;}
    interface Check {void run()throws IOException;}
    interface Progress {void update(long done)throws IOException;}
    static void put(byte[] bytes,int at,long value,int width){for(int i=0;i<width;i++)bytes[at+i]=(byte)(value>>>(8*i));}
    static File build(Plan plan,File cache,Progress progress,Check check)throws IOException {
        check.run();if(!cache.isDirectory()&&!cache.mkdirs())throw new IOException("无法创建 ZIP 打包缓存");long available=cache.getUsableSpace();if(plan.size>available||available-plan.size<2L*1024*1024)throw new IOException("打包 ZIP 所需的磁盘空间不足");
        File zip=File.createTempFile("archive-",".zip",cache);boolean success=false;long done=0,last=0,written=0;byte[] buffer=new byte[1024*1024];
        try(FileOutputStream file=new FileOutputStream(zip);OutputStream output=new BufferedOutputStream(file,1024*1024)){
            for(Entry entry:plan.entries){check.run();byte[] local=new byte[50+entry.name.length];put(local,0,0x04034b50,4);put(local,4,45,2);put(local,6,0x808,2);put(local,10,plan.time,2);put(local,12,plan.date,2);put(local,18,0xffffffffL,4);put(local,22,0xffffffffL,4);put(local,26,entry.name.length,2);put(local,28,20,2);System.arraycopy(entry.name,0,local,30,entry.name.length);int extra=30+entry.name.length;put(local,extra,1,2);put(local,extra+2,16,2);put(local,extra+4,entry.size,8);put(local,extra+12,entry.size,8);output.write(local);written+=local.length;CRC32 crc=new CRC32();
                if(!entry.directory)try(InputStream input=entry.source.opener.open()){
                    long read=0;while(read<entry.size){check.run();int count=input.read(buffer,0,(int)Math.min(buffer.length,entry.size-read));if(count<0)throw new IOException("文件长度发生变化");if(count==0)continue;crc.update(buffer,0,count);output.write(buffer,0,count);read+=count;done+=count;written+=count;if(now()-last>=100){progress.update(done);last=now();}}if(input.read()!=-1)throw new IOException("文件长度发生变化");
                }catch(IOException error){throw new IOException("无法打包源文件："+entry.path+"；"+error.getMessage(),error);}
                entry.crc=crc.getValue();byte[] descriptor=new byte[24];put(descriptor,0,0x08074b50,4);put(descriptor,4,entry.crc,4);put(descriptor,8,entry.size,8);put(descriptor,16,entry.size,8);output.write(descriptor);written+=descriptor.length;
            }
            for(Entry entry:plan.entries){check.run();byte[] central=new byte[74+entry.name.length];put(central,0,0x02014b50,4);put(central,4,45,2);put(central,6,45,2);put(central,8,0x808,2);put(central,12,plan.time,2);put(central,14,plan.date,2);put(central,16,entry.crc,4);put(central,20,0xffffffffL,4);put(central,24,0xffffffffL,4);put(central,28,entry.name.length,2);put(central,30,28,2);put(central,38,entry.directory?16:0,4);put(central,42,0xffffffffL,4);System.arraycopy(entry.name,0,central,46,entry.name.length);int extra=46+entry.name.length;put(central,extra,1,2);put(central,extra+2,24,2);put(central,extra+4,entry.size,8);put(central,extra+12,entry.size,8);put(central,extra+20,entry.offset,8);output.write(central);written+=central.length;}
            byte[] end=new byte[98];put(end,0,0x06064b50,4);put(end,4,44,8);put(end,12,45,2);put(end,14,45,2);put(end,24,plan.entries.size(),8);put(end,32,plan.entries.size(),8);put(end,40,plan.centralSize,8);put(end,48,plan.centralOffset,8);put(end,56,0x07064b50,4);put(end,64,plan.centralOffset+plan.centralSize,8);put(end,72,1,4);put(end,76,0x06054b50,4);put(end,84,65535,2);put(end,86,65535,2);put(end,88,0xffffffffL,4);put(end,92,0xffffffffL,4);output.write(end);written+=end.length;output.flush();file.getFD().sync();if(written!=plan.size)throw new IOException("ZIP 文件长度不一致");progress.update(plan.total);check.run();success=true;return zip;
        }finally{if(!success)zip.delete();}
    }
}
