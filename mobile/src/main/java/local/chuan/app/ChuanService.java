package local.chuan.app;

import android.app.*;
import android.content.*;
import android.content.pm.ServiceInfo;
import android.net.wifi.WifiManager;
import android.os.*;
import java.io.*;
import local.chuan.core.*;

public final class ChuanService extends Service {
    static volatile Engine active;static volatile String status="服务尚未开启";
    WifiManager.MulticastLock multicast;PowerManager.WakeLock awake;
    static File root(Context context){File parent=context.getExternalFilesDir(Environment.DIRECTORY_DOWNLOADS);return new File(parent==null?context.getFilesDir():parent,"传");}
    static android.content.SharedPreferences prefs(Context context){return context.getSharedPreferences("chuan",MODE_PRIVATE);}
    @Override public void onCreate(){super.onCreate();NotificationManager manager=getSystemService(NotificationManager.class);manager.createNotificationChannel(new NotificationChannel("transfer","局域网传输服务",NotificationManager.IMPORTANCE_LOW));
        Intent open=new Intent(this,MainActivity.class);PendingIntent pending=PendingIntent.getActivity(this,1,open,PendingIntent.FLAG_IMMUTABLE|PendingIntent.FLAG_UPDATE_CURRENT);
        Intent stop=new Intent(this,ChuanService.class).setAction("stop");PendingIntent end=PendingIntent.getService(this,2,stop,PendingIntent.FLAG_IMMUTABLE|PendingIntent.FLAG_UPDATE_CURRENT);
        Notification notification=new Notification.Builder(this,"transfer").setContentTitle("传 · 局域网服务已开启").setContentText("可接收文件并提供 Web 服务；点击返回应用。").setSmallIcon(R.drawable.notification).setContentIntent(pending).setOngoing(true).addAction(new Notification.Action.Builder(null,"停止服务",end).build()).build();
        if(Build.VERSION.SDK_INT>=29)startForeground(1,notification,ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE);else startForeground(1,notification);
        try{
            android.content.SharedPreferences saved=prefs(this);Engine.Config config=new Engine.Config(root(this));config.relayDirectory=new File(getCacheDir(),"relay");config.id=saved.getString("id","");if(config.id.isEmpty()){config.id=Protocol.random();saved.edit().putString("id",config.id).apply();}config.name=saved.getString("name",Build.MANUFACTURER+" "+Build.MODEL);config.autoReceive=saved.getBoolean("autoReceive",true);config.shareSoftware=saved.getBoolean("shareSoftware",true);config.maxFiles=saved.getLong("maxFiles",0);config.maxBytes=saved.getLong("maxBytes",0);config.port=saved.getInt("port",45871);
            StoragePublisher storage=new StoragePublisher(this);Engine engine=new Engine(config,new Engine.Platform(){
                public InputStream asset(String name)throws IOException{return getAssets().open("web/"+name);}
                public File apk(){return new File(getApplicationInfo().sourceDir);}
                public String saved(Engine.Job job)throws IOException{return storage.publish(job);}
                public void deleted(Engine.Job job)throws IOException{storage.delete(job);}
                public void settingsChanged(Engine.Config cfg){saved.edit().putString("name",cfg.name).putBoolean("autoReceive",cfg.autoReceive).putBoolean("shareSoftware",cfg.shareSoftware).putLong("maxFiles",cfg.maxFiles).putLong("maxBytes",cfg.maxBytes).apply();}
            });
            engine.start();active=engine;status="局域网服务已开启";
            try{WifiManager wifi=(WifiManager)getApplicationContext().getSystemService(WIFI_SERVICE);multicast=wifi.createMulticastLock("Chuan discovery");multicast.setReferenceCounted(false);multicast.acquire();}catch(Exception ignored){}
            try{PowerManager power=getSystemService(PowerManager.class);awake=power.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK,"chuan:transfer");awake.setReferenceCounted(false);awake.acquire();}catch(Exception ignored){}
        }catch(Exception error){status="服务启动失败："+error.getMessage();stopForeground(true);stopSelf();}
    }
    @Override public int onStartCommand(Intent intent,int flags,int startId){if(intent!=null&&"stop".equals(intent.getAction())){stopSelf();return START_NOT_STICKY;}return START_NOT_STICKY;}
    @Override public void onDestroy(){Engine engine=active;active=null;if(engine!=null)engine.close();if(multicast!=null&&multicast.isHeld())multicast.release();if(awake!=null&&awake.isHeld())awake.release();status="局域网服务已停止";stopForeground(true);super.onDestroy();}
    @Override public IBinder onBind(Intent intent){return null;}
}
