package local.chuan.app;

import android.Manifest;
import android.app.*;
import android.content.*;
import android.database.Cursor;
import android.graphics.Color;
import android.net.Uri;
import android.os.*;
import android.provider.*;
import android.view.*;
import android.webkit.*;
import android.widget.Toast;
import java.io.*;
import java.util.*;
import java.util.concurrent.*;
import local.chuan.core.*;

public final class MainActivity extends Activity {
    boolean pendingUpdatePermission=false;
    void installUpdate(){
        try {
            Engine active=ChuanService.active;if(active!=null)for(Object item:(List<?>)active.snapshot().get("jobs"))if(((Number)((Map<?,?>)item).get("state")).intValue()<3)throw new IOException("当前存在未结束的传输，请结束任务后安装更新");
            if(!getPackageManager().canRequestPackageInstalls()){pendingUpdatePermission=true;startActivityForResult(new Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES,Uri.parse("package:"+getPackageName())),701);return;}
            AppUpdater.get(this).prepareInstall(()->runOnUiThread(()->{if(isFinishing()||isDestroyed())return;try{
                Engine value=ChuanService.active;if(value!=null)for(Object item:(List<?>)value.snapshot().get("jobs"))if(((Number)((Map<?,?>)item).get("state")).intValue()<3)throw new IOException("当前存在未结束的传输，请结束任务后安装更新");
                Uri uri=ChuanProvider.updateUri();Intent intent=new Intent(Intent.ACTION_VIEW).setDataAndType(uri,"application/vnd.android.package-archive").addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);intent.setClipData(ClipData.newUri(getContentResolver(),"传更新包",uri));startActivity(intent);
            }catch(Exception error){message(error.getMessage());}}),this::message);
        }catch(Exception error){message(error.getMessage());}
    }
    WebView web;final ExecutorService files=Executors.newSingleThreadExecutor();String pickerTarget="";final List<Uri> shared=Collections.synchronizedList(new ArrayList<>());
    void message(String value){runOnUiThread(()->Toast.makeText(this,value,Toast.LENGTH_LONG).show());}
    Engine engine(){Engine value=ChuanService.active;if(value==null)throw new IllegalStateException(ChuanService.status);return value;}
    void startHost(){startForegroundService(new Intent(this,ChuanService.class));}
    @Override public void onCreate(Bundle state){super.onCreate(state);getWindow().setStatusBarColor(Color.WHITE);getWindow().setNavigationBarColor(Color.rgb(245,246,248));getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_LIGHT_STATUS_BAR|View.SYSTEM_UI_FLAG_LIGHT_NAVIGATION_BAR);
        web=new WebView(this);web.setBackgroundColor(Color.WHITE);android.widget.FrameLayout content=new android.widget.FrameLayout(this);content.addView(web,new android.widget.FrameLayout.LayoutParams(-1,-1));if(Build.VERSION.SDK_INT>=30)getWindow().setDecorFitsSystemWindows(false);setContentView(content);content.setOnApplyWindowInsetsListener((view,insets)->{if(Build.VERSION.SDK_INT>=30){android.graphics.Insets bars=insets.getInsets(WindowInsets.Type.systemBars()|WindowInsets.Type.ime());view.setPadding(bars.left,bars.top,bars.right,bars.bottom);}return insets;});content.requestApplyInsets();
        WebSettings settings=web.getSettings();settings.setJavaScriptEnabled(true);settings.setCacheMode(WebSettings.LOAD_NO_CACHE);settings.setDomStorageEnabled(true);settings.setAllowFileAccess(false);settings.setAllowContentAccess(false);settings.setMixedContentMode(WebSettings.MIXED_CONTENT_NEVER_ALLOW);settings.setTextZoom(100);
        web.addJavascriptInterface(new Bridge(),"ChuanNative");web.setWebViewClient(new WebViewClient(){
            @Override public WebResourceResponse shouldInterceptRequest(WebView view,WebResourceRequest request){Uri uri=request.getUrl();try{if(!uri.getScheme().equals("https")||!uri.getHost().equals("app.chuan.local")||uri.getPath().contains(".."))return new WebResourceResponse("text/plain","UTF-8",403,"Forbidden",Collections.emptyMap(),new ByteArrayInputStream(new byte[0]));String name=uri.getPath().substring(1);return new WebResourceResponse(Protocol.mime(name).split(";")[0],"UTF-8",getAssets().open(name));}catch(Exception error){return new WebResourceResponse("text/plain","UTF-8",404,"Not Found",Collections.emptyMap(),new ByteArrayInputStream(new byte[0]));}}
            @Override public boolean shouldOverrideUrlLoading(WebView view,WebResourceRequest request){return !"app.chuan.local".equals(request.getUrl().getHost());}
        });
        AppUpdater.get(this);if(state!=null)pendingUpdatePermission=state.getBoolean("pendingUpdatePermission",false);
        collectShared(getIntent());startHost();web.loadUrl("https://app.chuan.local/native/app.html");
        if(Build.VERSION.SDK_INT>=33&&checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS)!=android.content.pm.PackageManager.PERMISSION_GRANTED)requestPermissions(new String[]{Manifest.permission.POST_NOTIFICATIONS},41);
    }
    @Override protected void onNewIntent(Intent intent){super.onNewIntent(intent);setIntent(intent);collectShared(intent);}
    @Override protected void onStart(){super.onStart();AppUpdater.get(this).checkOnEntry();}
    @Override protected void onSaveInstanceState(Bundle state){state.putBoolean("pendingUpdatePermission",pendingUpdatePermission);super.onSaveInstanceState(state);}
    void collectShared(Intent intent){if(intent==null)return;String action=intent.getAction();if(Intent.ACTION_SEND.equals(action)){Uri uri=intent.getParcelableExtra(Intent.EXTRA_STREAM);if(uri!=null)shared.add(uri);}else if(Intent.ACTION_SEND_MULTIPLE.equals(action)){ArrayList<Uri> uris=intent.getParcelableArrayListExtra(Intent.EXTRA_STREAM);if(uris!=null)shared.addAll(uris);}}
    String name(Uri uri)throws IOException {try(Cursor row=getContentResolver().query(uri,new String[]{OpenableColumns.DISPLAY_NAME},null,null,null)){if(row!=null&&row.moveToFirst()&&!row.isNull(0))return row.getString(0);}String path=uri.getLastPathSegment();if(path==null)throw new IOException("无法读取文件名");return path;}
    void persist(Uri uri,int flags){try{getContentResolver().takePersistableUriPermission(uri,flags&(Intent.FLAG_GRANT_READ_URI_PERMISSION|Intent.FLAG_GRANT_WRITE_URI_PERMISSION));}catch(SecurityException ignored){}}
    Engine.Source source(Uri uri,String relative)throws IOException {
        long size=-1;try(Cursor row=getContentResolver().query(uri,new String[]{OpenableColumns.SIZE},null,null,null)){if(row!=null&&row.moveToFirst()&&!row.isNull(0))size=row.getLong(0);}
        if(size<0)try(ParcelFileDescriptor descriptor=getContentResolver().openFileDescriptor(uri,"r")){if(descriptor!=null)size=descriptor.getStatSize();}
        if(size<0){File cache=new File(getCacheDir(),"outgoing");cache.mkdirs();File file=File.createTempFile("source-",".bin",cache);try(InputStream input=getContentResolver().openInputStream(uri);FileOutputStream output=new FileOutputStream(file)){if(input==null)throw new IOException("无法读取所选文件");byte[] buffer=new byte[128*1024];long done=0;int read;while((read=input.read(buffer))>=0){if(read>cache.getUsableSpace()-2L*1024*1024)throw new IOException("缓存空间不足");done=Math.addExact(done,read);Engine engine=engine();if(engine.config.maxBytes>0&&done>engine.config.maxBytes)throw new IOException("超过发送大小上限");output.write(buffer,0,read);}output.getFD().sync();}catch(Exception error){file.delete();throw new IOException(error.getMessage(),error);}return Engine.Source.file(file,relative);}
        final Uri selected=uri;return new Engine.Source(relative,size,()->{InputStream stream=getContentResolver().openInputStream(selected);if(stream==null)throw new IOException("无法打开所选文件");return stream;});
    }
    void walk(Uri tree,Uri document,String relative,List<Engine.Source> out,int depth)throws IOException {
        out.add(Engine.Source.directory(relative));
        if(depth>64)throw new IOException("文件夹层级超过 64 层");Uri children=DocumentsContract.buildChildDocumentsUriUsingTree(tree,DocumentsContract.getDocumentId(document));
        try(Cursor rows=getContentResolver().query(children,new String[]{DocumentsContract.Document.COLUMN_DOCUMENT_ID,DocumentsContract.Document.COLUMN_DISPLAY_NAME,DocumentsContract.Document.COLUMN_MIME_TYPE},null,null,null)){
            if(rows==null)throw new IOException("无法读取文件夹");while(rows.moveToNext()){Uri child=DocumentsContract.buildDocumentUriUsingTree(tree,rows.getString(0));String path=Protocol.safePath(relative+"/"+rows.getString(1));if(rows.getString(2).equals(DocumentsContract.Document.MIME_TYPE_DIR))walk(tree,child,path,out,depth+1);else{out.add(source(child,path));long limit=engine().config.maxFiles;if(limit>0&&out.size()>limit)throw new IOException("超过单批文件数量上限");}}
        }
    }
    @Override protected void onActivityResult(int request,int result,Intent data){super.onActivityResult(request,result,data);if(request==701){boolean resume=pendingUpdatePermission;pendingUpdatePermission=false;if(resume&&getPackageManager().canRequestPackageInstalls())installUpdate();else message("安装授权未开启，已保留下载的更新包");return;}if(result!=RESULT_OK||data==null)return;
        if(request==6){Uri target=data.getData();if(target!=null)files.execute(()->{File log=null;try{log=engine().exportDiagnostics();try(InputStream input=new FileInputStream(log);OutputStream output=getContentResolver().openOutputStream(target,"wt")){if(output==null)throw new IOException("无法写入日志文件");byte[] buffer=new byte[65536];int count;while((count=input.read(buffer))!=-1)output.write(buffer,0,count);}message("诊断日志已导出");}catch(Exception error){message("日志导出失败："+error.getMessage());}finally{if(log!=null)log.delete();}});return;}
        if(request==3){Uri tree=data.getData();if(tree==null)return;try{getContentResolver().takePersistableUriPermission(tree,Intent.FLAG_GRANT_READ_URI_PERMISSION|Intent.FLAG_GRANT_WRITE_URI_PERMISSION);ChuanService.prefs(this).edit().putString("receiveTree",tree.toString()).apply();message("接收目录已保存");}catch(Exception error){message("无法保存目录授权："+error.getMessage());}return;}
        String target=pickerTarget;
        files.execute(()->{try{List<Engine.Source> selected=new ArrayList<>();if(request==2){Uri tree=data.getData();if(tree==null)return;persist(tree,data.getFlags());Uri root=DocumentsContract.buildDocumentUriUsingTree(tree,DocumentsContract.getTreeDocumentId(tree));walk(tree,root,Protocol.safePath(name(root)),selected,0);}else{List<Uri> uris=new ArrayList<>();if(data.getClipData()!=null)for(int i=0;i<data.getClipData().getItemCount();i++)uris.add(data.getClipData().getItemAt(i).getUri());else if(data.getData()!=null)uris.add(data.getData());for(Uri uri:uris){persist(uri,data.getFlags());selected.add(source(uri,name(uri)));}}
            if(selected.isEmpty())throw new IOException("所选文件夹为空");
            if(request==2&&engine().requiresArchive(target))runOnUiThread(()->new android.app.AlertDialog.Builder(this).setTitle("接收端无法接收文件夹").setMessage("目标 Web 页面不支持直接保存目录。建议打包为 ZIP 后发送；此操作仅对本次传输生效。").setNegativeButton("取消",null).setPositiveButton("打包传输",(dialog,which)->engine().send(target,selected,true)).show());
            else engine().send(target,selected);
        }catch(Exception error){message(error.getMessage());}});
    }
    void showFile(Uri uri,String label,boolean share){runOnUiThread(()->{try{String mime=getContentResolver().getType(uri);Intent intent=share?new Intent(Intent.ACTION_SEND).setType(mime==null?"application/octet-stream":mime).putExtra(Intent.EXTRA_STREAM,uri):new Intent(Intent.ACTION_VIEW).setDataAndType(uri,mime==null?"application/octet-stream":mime);intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);intent.setClipData(ClipData.newUri(getContentResolver(),label,uri));startActivity(Intent.createChooser(intent,share?"分享接收文件":"打开接收文件"));}catch(Exception error){message(error.getMessage());}});}
    void open(long id,boolean share){try{Engine.Job job=engine().find(id);if(job==null||job.publicUri.isEmpty())throw new IOException("当前记录没有可打开的接收文件");showFile(Uri.parse(job.publicUri),job.name,share);}catch(Exception error){message(error.getMessage());}}
    final class Bridge {
        @JavascriptInterface public void checkUpdate(){AppUpdater.get(MainActivity.this).check();}
        @JavascriptInterface public void readUpdateNotice(boolean detail){AppUpdater.get(MainActivity.this).readNotice(detail);}
        @JavascriptInterface public void downloadUpdate(){AppUpdater.get(MainActivity.this).download();}
        @JavascriptInterface public void cancelUpdate(){AppUpdater.get(MainActivity.this).cancel();}
        @JavascriptInterface public void installUpdate(){runOnUiThread(()->MainActivity.this.installUpdate());}
        @JavascriptInterface public void updatePage(){runOnUiThread(()->{try{startActivity(new Intent(Intent.ACTION_VIEW,Uri.parse(UpdateRelease.PAGE)));}catch(Exception error){message(error.getMessage());}});}
        @JavascriptInterface public String state(){try{Engine value=ChuanService.active;Map<String,Object> state=value==null?Json.map("running",false,"notice",ChuanService.status,"name",ChuanService.prefs(MainActivity.this).getString("name",Build.MODEL),"version",Engine.VERSION,"peers",Collections.emptyList(),"jobs",Collections.emptyList(),"requests",Collections.emptyList(),"addresses",Collections.emptyList()):value.snapshot();state.put("update",AppUpdater.get(MainActivity.this).state());state.put("sharedCount",shared.size());state.put("receiveTree",ChuanService.prefs(MainActivity.this).getString("receiveTree","").isEmpty()?"应用接收目录":"已授权的系统目录");return Json.stringify(state);}catch(Exception error){return Json.stringify(Json.map("error",error.getMessage()));}}
        @JavascriptInterface public void pickFiles(String target){runOnUiThread(()->{pickerTarget=target;Intent intent=new Intent(Intent.ACTION_OPEN_DOCUMENT).setType("*/*").addCategory(Intent.CATEGORY_OPENABLE).putExtra(Intent.EXTRA_ALLOW_MULTIPLE,true);startActivityForResult(intent,1);});}
        @JavascriptInterface public void pickFolder(String target){runOnUiThread(()->{pickerTarget=target;startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE).addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION|Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION),2);});}
        @JavascriptInterface public void sendShared(String target){List<Uri> pending;synchronized(shared){pending=new ArrayList<>(shared);shared.clear();}files.execute(()->{try{List<Engine.Source> selected=new ArrayList<>();for(Uri uri:pending)selected.add(source(uri,name(uri)));engine().send(target,selected);}catch(Exception error){message(error.getMessage());}});}
        @JavascriptInterface public void chooseReceive(){runOnUiThread(()->startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE).addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION|Intent.FLAG_GRANT_WRITE_URI_PERMISSION|Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION),3));}
        @JavascriptInterface public void exportDiagnostics(){runOnUiThread(()->startActivityForResult(new Intent(Intent.ACTION_CREATE_DOCUMENT).setType("application/x-ndjson").addCategory(Intent.CATEGORY_OPENABLE).putExtra(Intent.EXTRA_TITLE,"Chuan-Android-diagnostics-"+System.currentTimeMillis()+".jsonl"),6));}
        @JavascriptInterface public void resetReceive(){ChuanService.prefs(MainActivity.this).edit().remove("receiveTree").apply();message("已使用应用接收目录");}
        @JavascriptInterface public String configure(String body){try{Map<String,Object> values=Json.object(body);String name=Json.string(values,"name","");long maxFiles=Protocol.number(Json.string(values,"maxFiles","0")),maxBytes=Math.multiplyExact(Protocol.number(Json.string(values,"maxMiB","0")),1048576);long requestedPort=Protocol.number(Json.string(values,"port","45871"));if(requestedPort<1024||requestedPort>65535)throw new IOException("端口必须为 1024 至 65535");int port=(int)requestedPort;Engine engine=engine();if(port!=engine.port()){for(Object item:(List<?>)engine.snapshot().get("jobs"))if(((Number)((Map<?,?>)item).get("state")).intValue()<3)throw new IOException("当前有未结束任务，结束任务后可更改端口");}engine.config.zipFolders=Boolean.TRUE.equals(values.get("zipFolders"));engine.configure(name,Boolean.TRUE.equals(values.get("autoReceive")),Boolean.TRUE.equals(values.get("shareSoftware")),maxFiles,maxBytes);if(port!=engine.port()){ChuanService.prefs(MainActivity.this).edit().putInt("port",port).apply();runOnUiThread(()->{stopService(new Intent(MainActivity.this,ChuanService.class));new Handler(Looper.getMainLooper()).postDelayed(()->startHost(),350);});}return "{}";}catch(Exception error){return Json.stringify(Json.map("error",error.getMessage()));}}
        @JavascriptInterface public void connect(String address){try{engine().addPeer(address);}catch(Exception error){message(error.getMessage());}}
        @JavascriptInterface public void decide(String id,String bits){try{engine().decide(Long.parseLong(id),bits);}catch(Exception error){message(error.getMessage());}}
        @JavascriptInterface public void pause(String id,boolean paused){try{engine().pause(Long.parseLong(id),paused);}catch(Exception error){message(error.getMessage());}}
        @JavascriptInterface public void cancel(String id){try{engine().cancel(Long.parseLong(id));}catch(Exception error){message(error.getMessage());}}
        @JavascriptInterface public void retry(String id){try{engine().retry(Long.parseLong(id));}catch(Exception error){message(error.getMessage());}}
        @JavascriptInterface public void delete(String id){try{engine().delete(Long.parseLong(id));}catch(Exception error){message(error.getMessage());}}
        @JavascriptInterface public void clear(){try{engine().clear();}catch(Exception error){message(error.getMessage());}}
        @JavascriptInterface public void ping(String target){try{engine().ping(target);}catch(Exception error){message(error.getMessage());}}
        @JavascriptInterface public void note(String ip,String note){try{engine().note(ip,note);}catch(Exception error){message(error.getMessage());}}
        @JavascriptInterface public void refresh(){try{engine().refresh();}catch(Exception ignored){}}
        @JavascriptInterface public String listReceived(String path,int offset){try{return new ReceivedFiles(MainActivity.this).list(path,offset);}catch(Exception error){return Json.stringify(Json.map("error",error.getMessage()));}}
        @JavascriptInterface public void openStored(String path){try{showFile(new ReceivedFiles(MainActivity.this).uri(path),path,false);}catch(Exception error){message(error.getMessage());}}
        @JavascriptInterface public void shareStored(String path){try{showFile(new ReceivedFiles(MainActivity.this).uri(path),path,true);}catch(Exception error){message(error.getMessage());}}
        @JavascriptInterface public void openFile(String id){open(Long.parseLong(id),false);}
        @JavascriptInterface public void shareFile(String id){open(Long.parseLong(id),true);}
        @JavascriptInterface public void copyLink(){try{Engine engine=engine();List<String> ips=engine.addresses();String url="http://"+(ips.isEmpty()?"127.0.0.1":ips.get(0))+":"+engine.port()+"/";runOnUiThread(()->{((ClipboardManager)getSystemService(CLIPBOARD_SERVICE)).setPrimaryClip(ClipData.newPlainText("传 Web 服务",url));message("Web 地址已复制");});}catch(Exception error){message(error.getMessage());}}
        @JavascriptInterface public void openWeb(){try{String url="http://127.0.0.1:"+engine().port()+"/";runOnUiThread(()->startActivity(new Intent(Intent.ACTION_VIEW,Uri.parse(url))));}catch(Exception error){message(error.getMessage());}}
        @JavascriptInterface public void stop(){runOnUiThread(()->stopService(new Intent(MainActivity.this,ChuanService.class)));}
        @JavascriptInterface public void start(){runOnUiThread(()->startHost());}
    }
    @Override protected void onDestroy(){web.removeJavascriptInterface("ChuanNative");web.destroy();files.shutdown();super.onDestroy();}
}
