package local.chuan.app;

import android.content.*;
import android.database.*;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;
import android.webkit.MimeTypeMap;
import java.io.*;

/** Read-only URI grants for received files; no broad storage or exported provider access. */
public final class ChuanProvider extends ContentProvider {
    @Override public boolean onCreate(){return true;}
    static Uri uri(Context context,File file)throws IOException {String root=ChuanService.root(context).getCanonicalPath()+File.separator,path=file.getCanonicalPath();if(!path.startsWith(root))throw new IOException("接收文件路径越界");Uri.Builder uri=new Uri.Builder().scheme("content").authority("local.chuan.files").appendPath("received");for(String part:path.substring(root.length()).split(java.util.regex.Pattern.quote(File.separator)))uri.appendPath(part);return uri.build();}
    File file(Uri uri)throws FileNotFoundException {try{if(!"local.chuan.files".equals(uri.getAuthority())||uri.getPathSegments().size()<2||!uri.getPathSegments().get(0).equals("received"))throw new IOException("URI 无效");File root=ChuanService.root(getContext()),file=root;for(int i=1;i<uri.getPathSegments().size();i++)file=new File(file,uri.getPathSegments().get(i));if(!file.getCanonicalPath().startsWith(root.getCanonicalPath()+File.separator)||!file.isFile())throw new IOException("文件不存在");return file;}catch(IOException error){throw new FileNotFoundException(error.getMessage());}}
    @Override public ParcelFileDescriptor openFile(Uri uri,String mode)throws FileNotFoundException {if(!mode.equals("r"))throw new FileNotFoundException("仅允许读取");return ParcelFileDescriptor.open(file(uri),ParcelFileDescriptor.MODE_READ_ONLY);}
    @Override public Cursor query(Uri uri,String[] projection,String selection,String[] selectionArgs,String sortOrder){try{File file=file(uri);String[] names=projection==null?new String[]{OpenableColumns.DISPLAY_NAME,OpenableColumns.SIZE}:projection;MatrixCursor cursor=new MatrixCursor(names);Object[] row=new Object[names.length];for(int i=0;i<names.length;i++)row[i]=names[i].equals(OpenableColumns.DISPLAY_NAME)?file.getName():names[i].equals(OpenableColumns.SIZE)?file.length():null;cursor.addRow(row);return cursor;}catch(IOException error){return null;}}
    @Override public String getType(Uri uri){String name=uri.getLastPathSegment();String type=MimeTypeMap.getSingleton().getMimeTypeFromExtension(name!=null&&name.contains(".")?name.substring(name.lastIndexOf('.')+1).toLowerCase(java.util.Locale.ROOT):"");return type==null?"application/octet-stream":type;}
    @Override public Uri insert(Uri uri,ContentValues values){throw new UnsupportedOperationException();}
    @Override public int update(Uri uri,ContentValues values,String selection,String[] args){throw new UnsupportedOperationException();}
    @Override public int delete(Uri uri,String selection,String[] args){throw new UnsupportedOperationException();}
}
