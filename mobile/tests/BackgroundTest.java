package local.chuan.core;
import java.io.*;
import static local.chuan.core.Engine.*;
public final class BackgroundTest {
    static void check(boolean value,String message){if(!value)throw new AssertionError(message);}
    public static void main(String[] args)throws Exception {
        Config cfg=new Config(new File(args[0],"received"));Engine e=new Engine(cfg,new Platform(){public InputStream asset(String name)throws IOException{throw new FileNotFoundException();}public File apk(){return null;}public String saved(Job job){return "";}public void deleted(Job job){}public void settingsChanged(Config config){}});
        try{long now=now();Session sender=new Session();sender.client="sender";sender.id="web:sender";sender.tabs.put("tab",now-65000);sender.openTabs.add("tab");e.sessions.put("sender",sender);
            Peer peer=new Peer(sender.id,"A1","127.0.0.1",45871);peer.browser=true;peer.platform="desktop-web";peer.seen=now-65000;e.peers.put(peer.id,peer);Peer nativePeer=new Peer("native","Native","127.0.0.1",45872);nativePeer.seen=now-10000;e.peers.put(nativePeer.id,nativePeer);
            Job job=e.job("HT.zip","A1",19685133076L,true);job.owner="sender";job.webTab="tab";job.announced=true;job.started=now-600000;job.activity="等待发送端打包 ZIP";
            Session recipient=new Session();recipient.client="recipient";recipient.id="web:recipient";recipient.tabs.put("receive",now-600000);recipient.openTabs.add("receive");e.sessions.put("recipient",recipient);Job waiting=e.job("Waiting.zip","Sender",10,true);waiting.recipient="recipient";waiting.state=1;waiting.awaitingApproval=true;waiting.requestOnly=true;
            e.expire();check(job.state==0&&job.announced,"background packing stays queued");check(e.peers.size()==1,"native and browser have separate grace");check(waiting.state==1&&waiting.awaitingApproval,"receiving page suspension preserves consent");
            sender.tabs.put("tab",now-600000);peer.seen=now-600000;e.expire();check(job.state==0&&job.announced&&job.browserPaused,"long suspension pauses instead of failing");check(job.activity.equals("等待发送页面恢复"),"accurate paused state");
            sender.tabs.put("tab",now());recipient.tabs.put("receive",now());e.expire();check(!job.browserPaused&&job.activity.equals("等待发送端打包 ZIP"),"resume restores previous phase");check(waiting.state==1&&!waiting.browserPaused,"receiving page resumes");
            sender.openTabs.add("other");sender.tabs.put("other",now());Job other=e.job("Other.zip","A1",10,true);other.owner="sender";other.webTab="other";other.announced=true;sender.openTabs.remove("tab");sender.tabs.remove("tab");e.expire();check(job.state==5&&job.error.contains("已关闭"),"explicit source tab closure ends its task");check(other.state==0&&other.announced,"other tab stays valid");recipient.openTabs.clear();recipient.tabs.clear();e.expire();check(waiting.state==4,"explicit receiving page closure cancels");
            System.out.println("PASS Android background grace, paused queue/resume, explicit closure, tab isolation and receiving consent");
        }finally{e.close();}
    }
}
