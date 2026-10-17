package local.chuan.app;

import android.content.Context;
import android.database.Cursor;
import android.net.Uri;
import android.provider.DocumentsContract;
import java.io.*;
import java.util.*;
import local.chuan.core.*;

/** Persistent received files remain accessible after the in-memory transfer session ends. */
final class ReceivedFiles {
    final Context context;
    ReceivedFiles(Context context){this.context=context;}
    String relative(String value){return value.isEmpty()?"":Protocol.safePath(value);}
    File file(String value)throws IOException {
        File root=ChuanService.root(context).getCanonicalFile(),file=value.isEmpty()?root:new File(root,relative(value)).getCanonicalFile();
        if(!file.equals(root)&&!file.getPath().startsWith(root.getPath()+File.separator))throw new IOException("接收文件路径越界");return file;
    }
    Uri tree(){String selected=ChuanService.prefs(context).getString("receiveTree","");return selected.isEmpty()?null:Uri.parse(selected);}
    Uri document(Uri tree,String value)throws IOException {
        Uri document=DocumentsContract.buildDocumentUriUsingTree(tree,DocumentsContract.getTreeDocumentId(tree));
        if(!value.isEmpty())for(String part:relative(value).split("/")){document=new StoragePublisher(context).child(tree,document,part);if(document==null)throw new FileNotFoundException("接收文件已移除");}return document;
    }
    String list(String requested,int offset)throws IOException {
        String path=relative(requested);Uri tree=tree();List<Map<String,Object>> entries=new ArrayList<>();if(offset<0||offset>Integer.MAX_VALUE-200)throw new IOException("页码无效");
        if(tree==null){File directory=file(path);if(!directory.isDirectory())throw new IOException("接收目录不存在");File[] children=directory.listFiles();if(children==null)throw new IOException("无法读取接收目录");
            for(File child:children){if(child.getName().startsWith(".chuan-")&&child.getName().endsWith(".part"))continue;entries.add(Json.map("name",child.getName(),"path",path.isEmpty()?child.getName():path+"/"+child.getName(),"folder",child.isDirectory(),"size",child.length()));}
        }else{
            Uri parent=document(tree,path),children=DocumentsContract.buildChildDocumentsUriUsingTree(tree,DocumentsContract.getDocumentId(parent));
            try(Cursor rows=context.getContentResolver().query(children,new String[]{DocumentsContract.Document.COLUMN_DISPLAY_NAME,DocumentsContract.Document.COLUMN_MIME_TYPE,DocumentsContract.Document.COLUMN_SIZE},null,null,null)){
                if(rows==null)throw new IOException("无法读取已授权接收目录");while(rows.moveToNext()){String name=rows.getString(0);if(name.startsWith(".chuan-")&&name.endsWith(".part"))continue;entries.add(Json.map("name",name,"path",path.isEmpty()?name:path+"/"+name,"folder",DocumentsContract.Document.MIME_TYPE_DIR.equals(rows.getString(1)),"size",rows.isNull(2)?0:rows.getLong(2)));}
            }catch(SecurityException error){throw new IOException("目录授权已失效，请重新选择接收目录",error);}
        }
        entries.sort((a,b)->{int folders=Boolean.compare(Boolean.TRUE.equals(b.get("folder")),Boolean.TRUE.equals(a.get("folder")));return folders!=0?folders:String.valueOf(a.get("name")).compareToIgnoreCase(String.valueOf(b.get("name")));});
        int slash=path.lastIndexOf('/');return Json.stringify(Json.map("path",path,"parent",path.isEmpty()?null:slash<0?"":path.substring(0,slash),"entries",entries.subList(Math.min(offset,entries.size()),Math.min(offset+200,entries.size())),"offset",offset,"next",offset+200<entries.size()?offset+200:-1,"total",entries.size()));
    }
    Uri uri(String path)throws IOException {if(relative(path).isEmpty())throw new IOException("请选择接收文件");Uri tree=tree();return tree==null?ChuanProvider.uri(context,file(path)):document(tree,path);}
}
