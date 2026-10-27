package local.chuan.core;

import java.io.*;
import java.net.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;
import static local.chuan.core.Protocol.*;

/** Real HTTP approval, interrupted relay upload and byte-exact resumed delivery. */
public final class ResumeRelayTest {
    static void require(boolean value,String message){if(!value)throw new AssertionError(message);}
    interface Condition {boolean ready()throws Exception;}
    static void until(Condition condition)throws Exception{long end=System.currentTimeMillis()+5000;while(!condition.ready()){if(System.currentTimeMillis()>end)throw new AssertionError("condition timeout");Thread.sleep(10);}}
    static final class Connection implements AutoCloseable {
        final Socket socket;final InputStream input;final OutputStream output;
        Connection(int port)throws IOException{socket=new Socket("127.0.0.1",port);socket.setSoTimeout(5000);input=new BufferedInputStream(socket.getInputStream());output=socket.getOutputStream();}
        void send(String text)throws IOException{output.write(text.getBytes(StandardCharsets.ISO_8859_1));output.flush();}
        public void close()throws IOException{socket.close();}
    }
    static String request(Engine engine,String method,String path,String fields,String body)throws IOException {
        try(Connection connection=new Connection(engine.port())){
            connection.send(method+" "+path+host(engine)+fields+"Content-Length: "+body.getBytes(StandardCharsets.UTF_8).length+"\r\nConnection: close\r\n\r\n");
            connection.output.write(body.getBytes(StandardCharsets.UTF_8));connection.output.flush();
            Header response=header(connection.input);
            return response.status()+" "+new String(read(connection.input,response.length(),1048576),StandardCharsets.UTF_8);
        }
    }
    static String host(Engine engine){return " HTTP/1.1\r\nHost: 127.0.0.1:"+engine.port()+"\r\n";}
    static Engine host(Path root)throws Exception{
        Engine.Config config=new Engine.Config(root.resolve("received").toFile());config.port=0;
        try(DatagramSocket socket=new DatagramSocket(0)){config.discoveryPort=socket.getLocalPort();}
        Engine engine=new Engine(config,new Engine.Platform(){
            public InputStream asset(String name)throws IOException{throw new FileNotFoundException(name);}
            public File apk(){return null;}public String saved(Engine.Job job){return job.file.toURI().toString();}
            public void deleted(Engine.Job job){}public void settingsChanged(Engine.Config value){}
        });engine.start();return engine;
    }
    public static void main(String[] args)throws Exception {
        require(args.length==1,"isolated output directory required");
        Path root=Paths.get(args[0]).toAbsolutePath().resolve(UUID.randomUUID().toString());
        try(Engine engine=host(root)){
            engine.config.autoReceive=false;
            String client="aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",batch="bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",upload="cccccccccccccccccccccccccccccccc",key=String.join("",Collections.nCopies(64,"d"));
            require(request(engine,"GET","/web/api/state","X-Chuan-Client: "+client+"\r\n","").startsWith("200 "),"register browser");
            String recipient;synchronized(engine.lock){recipient=engine.sessions.get(client).id;}
            String sender="X-Chuan-Id: "+hex("regression-sender")+"\r\nX-Chuan-Sender: "+hex("review")+"\r\n",route="X-Chuan-Recipient: "+hex(recipient)+"\r\n";
            String fields=sender+route+"X-Chuan-Batch: "+batch+"\r\nX-Chuan-Offer-Async: 1\r\n";
            require(request(engine,"POST","/api/offer",fields,hex("partial.bin")+"\tF\t1\t10\n").startsWith("202 "),"offer");
            fields=sender+route+"X-Chuan-Batch: "+batch+"\r\nX-Chuan-Item: 0\r\nX-Chuan-Upload: "+upload+"\r\nX-Chuan-Path: "+hex("partial.bin")+"\r\nX-Chuan-Size: 10\r\nX-Chuan-Resume-Key: "+key+"\r\n";
            require(request(engine,"POST","/api/resume",fields,"").startsWith("400 "),"pending batch cannot resume");
            synchronized(engine.lock){for(Engine.Job job:new ArrayList<>(engine.jobs.values()))if(job.requestOnly&&job.awaitingApproval)engine.decide(job.id,"1");}
            try(Connection connection=new Connection(engine.port())){
                connection.send("POST /api/upload"+host(engine)+fields+"Expect: 100-continue\r\nContent-Length: 42\r\n\r\n");
                require(header(connection.input).status()==100,"approved payload");connection.send("abc");connection.socket.shutdownOutput();
                require(header(connection.input).status()==422,"interrupted upload fails");
            }
            File partial=Resume.file(engine.config.relayDirectory,hex("regression-sender"),"partial.bin",10,key);
            require(partial.length()==3,"relay retains interrupted prefix");
            require(request(engine,"POST","/api/resume",fields,"").contains("\"offset\":3"),"probe must read relay prefix");
            require(request(engine,"POST","/api/resume",fields.replace(route,"")+"X-Chuan-Resume-Reset: 1\r\n","").startsWith("400 "),"resume route must match consent");
            require(partial.length()==3,"unauthorized reset preserves prefix");
            require(request(engine,"POST","/api/retry-upload/"+upload,fields,"").startsWith("200 "),"prepare retry");
            try(Connection connection=new Connection(engine.port())){
                connection.send("POST /api/upload"+host(engine)+fields+"X-Chuan-Offset: 3\r\nX-Chuan-Prefix: "+hex(sha().digest("abc".getBytes(StandardCharsets.US_ASCII)))+"\r\nExpect: 100-continue\r\nContent-Length: 39\r\n\r\n");
                require(header(connection.input).status()==100,"accept resumed suffix");connection.send("defghij");connection.output.write(sha().digest("abcdefghij".getBytes(StandardCharsets.US_ASCII)));connection.output.flush();
                long[] id={0};until(()->{synchronized(engine.lock){for(Engine.Job job:engine.jobs.values())if(!job.requestOnly&&job.name.equals("partial.bin")&&job.state==1&&job.source!=null){id[0]=job.id;return true;}return false;}});
                require(request(engine,"GET","/web/files/"+id[0]+"?client="+client,"","").equals("200 abcdefghij"),"browser receives exact resumed contents");
                Header response;do{response=header(connection.input);}while(response.status()==102);require(response.status()==201,"sender receives saved confirmation");
            }
            System.out.println("PASS Android relay: retained offset; route-bound consent; exact resumed contents and sender confirmation");
            try(Engine gateway=host(root.resolve("gateway"))){
                gateway.syncMesh(gateway.probe("127.0.0.1",engine.port()));
                String gatewayClient="eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee",forwardBatch="ffffffffffffffffffffffffffffffff";
                String state=request(gateway,"GET","/web/api/state","X-Chuan-Client: "+gatewayClient+"\r\n","");
                String token=Json.string(Json.object(state.substring(4)),"token","");
                String browserFields="X-Chuan-Client: "+gatewayClient+"\r\nX-Chuan-Token: "+token+"\r\nX-Chuan-Sender: "+hex("review")+"\r\nX-Chuan-Target: "+hex(recipient)+"\r\nX-Chuan-Batch: "+forwardBatch+"\r\n";
                require(request(gateway,"POST","/web/api/offer",browserFields+"X-Chuan-Offer-Async: 1\r\n",hex("forwarded.zip")+"\tF\t1\t10\n").startsWith("202 "),"forwarded offer");
                until(()->{synchronized(engine.lock){for(Engine.Job job:engine.jobs.values())if(job.requestOnly&&job.awaitingApproval&&job.batchId.equals(forwardBatch)){engine.decide(job.id,"1");return true;}return false;}});
                until(()->{synchronized(gateway.lock){for(Engine.Job job:gateway.jobs.values())if(job.requestOnly&&job.batchId.equals(forwardBatch)&&"1".equals(job.selection))return true;return false;}});
                String probe=browserFields+"X-Chuan-Item: 0\r\nX-Chuan-Upload: "+upload+"\r\nX-Chuan-Path: "+hex("forwarded.zip")+"\r\nX-Chuan-Size: 10\r\nX-Chuan-Resume-Key: "+key+"\r\nX-Chuan-Resume-Direct: 1\r\n";
                String result=request(gateway,"POST","/web/api/resume",probe,"");
                require(result.startsWith("200 ")&&result.contains("\"offset\":0"),"forwarded resume carries approved browser recipient");
                System.out.println("PASS Android gateway forwards the approved browser recipient in direct resume probes");
            }
        }
    }
}
