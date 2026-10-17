package local.chuan.app;

import android.content.*;
import android.database.Cursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;
import java.io.*;
import java.util.*;
import local.chuan.core.*;

final class StoragePublisher {
    final Context context;final Map<String,Uri> folders=new HashMap<>();
    StoragePublisher(Context context){this.context=context;}
    Uri child(Uri tree,Uri parent,String name)throws IOException {
        Uri children=DocumentsContract.buildChildDocumentsUriUsingTree(tree,DocumentsContract.getDocumentId(parent));
        try(Cursor rows=context.getContentResolver().query(children,new String[]{DocumentsContract.Document.COLUMN_DOCUMENT_ID,DocumentsContract.Document.COLUMN_DISPLAY_NAME},null,null,null)){if(rows==null)throw new IOException("无法读取已授权目录");Uri fallback=null;while(rows.moveToNext()){Uri candidate=DocumentsContract.buildDocumentUriUsingTree(tree,rows.getString(0));if(name.equals(rows.getString(1)))return candidate;if(fallback==null&&name.equalsIgnoreCase(rows.getString(1)))fallback=candidate;}return fallback;}catch(Exception error){throw new IOException("无法读取已授权目录："+error.getMessage(),error);}
    }
    String unique(Uri tree,Uri parent,String name,boolean folder)throws IOException {int dot=name.lastIndexOf('.');String stem=dot>0&&!folder?name.substring(0,dot):name,extension=dot>0&&!folder?name.substring(dot):"";for(int i=0;i<10000;i++){String value=i==0?name:stem+" ("+i+")"+extension;if(child(tree,parent,value)==null)return value;}throw new IOException("同名文件数量过多");}
    synchronized String publish(Engine.Job job)throws IOException {
        String selected=ChuanService.prefs(context).getString("receiveTree","");if(selected.isEmpty())return ChuanProvider.uri(context,job.file).toString();
        Uri tree=Uri.parse(selected),parent=DocumentsContract.buildDocumentUriUsingTree(tree,DocumentsContract.getTreeDocumentId(tree)),temporary=null;
        try{
            String[] parts=Protocol.safePath(job.name).split("/");
            if(parts.length>1){String key=job.storageKey+":"+job.folderId+":"+parts[0];Uri root=folders.get(key);if(root==null){root=DocumentsContract.createDocument(context.getContentResolver(),parent,DocumentsContract.Document.MIME_TYPE_DIR,unique(tree,parent,parts[0],true));if(root==null)throw new IOException("无法创建接收文件夹");folders.put(key,root);}parent=root;
                for(int i=1;i<parts.length-1;i++){Uri next=child(tree,parent,parts[i]);if(next==null)next=DocumentsContract.createDocument(context.getContentResolver(),parent,DocumentsContract.Document.MIME_TYPE_DIR,parts[i]);if(next==null)throw new IOException("无法创建子目录");parent=next;}}
            String name=unique(tree,parent,parts[parts.length-1],false);
            temporary=DocumentsContract.createDocument(context.getContentResolver(),parent,"application/octet-stream",".chuan-"+Protocol.random()+".part");if(temporary==null)throw new IOException("无法在接收目录创建文件");
            ParcelFileDescriptor descriptor=context.getContentResolver().openFileDescriptor(temporary,"w");if(descriptor==null)throw new IOException("无法写入接收目录");
            try(ParcelFileDescriptor.AutoCloseOutputStream output=new ParcelFileDescriptor.AutoCloseOutputStream(descriptor);InputStream input=new FileInputStream(job.file)){Protocol.copy(input,output,job.total,null,bytes->{if(job.cancelled)throw new IOException("传输已取消");});output.flush();try{output.getFD().sync();}catch(SyncFailedException ignored){}}
            Uri committed=DocumentsContract.renameDocument(context.getContentResolver(),temporary,name);if(committed==null)throw new IOException("目录提供方不支持提交文件");return committed.toString();
        }catch(Exception error){if(temporary!=null)try{DocumentsContract.deleteDocument(context.getContentResolver(),temporary);}catch(Exception ignored){}throw new IOException("保存到所选目录失败："+error.getMessage(),error);}
    }
    void delete(Engine.Job job)throws IOException {if(job.publicUri.isEmpty()||job.publicUri.startsWith("content://local.chuan.files/"))return;try{if(!DocumentsContract.deleteDocument(context.getContentResolver(),Uri.parse(job.publicUri)))throw new IOException("目录提供方拒绝删除");}catch(Exception error){throw new IOException(error.getMessage(),error);}}
}
