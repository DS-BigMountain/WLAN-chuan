package local.chuan.core;
import java.nio.file.*;
import java.nio.charset.StandardCharsets;
import java.util.*;

/** Production engine records, asynchronous export fence, and browser ownership filtering. */
public final class DiagnosticsTest {
    public static void main(String[] args)throws Exception{
        Path root=Paths.get(args[0]).resolve(UUID.randomUUID().toString());
        Engine receiver=TransferReadTest.host(root.resolve("receiver")),sender=TransferReadTest.host(root.resolve("sender"));
        try{
            byte[] bytes=new byte[262144];Arrays.fill(bytes,(byte)31);
            sender.send(sender.probe("127.0.0.1",receiver.port()).id,Arrays.asList(Engine.Source.directory("日志样本"),new Engine.Source("日志样本/private.bin",bytes.length,()->new java.io.ByteArrayInputStream(bytes))));
            TransferReadTest.finished(sender);
            List<String> sent=Files.readAllLines(sender.exportDiagnostics().toPath(),StandardCharsets.UTF_8),received=Files.readAllLines(receiver.exportDiagnostics().toPath(),StandardCharsets.UTF_8);
            for(String line:sent)Json.object(line);for(String line:received)Json.object(line);
            TransferReadTest.require(sent.stream().anyMatch(line->line.contains("\"event\":\"pipeline\"")),"queue snapshots exported");
            TransferReadTest.require(sent.stream().anyMatch(line->line.contains("\"event\":\"payload_end\"")),"sender I/O timings exported");
            TransferReadTest.require(received.stream().anyMatch(line->line.contains("\"direction\":\"network_to_disk\"")),"receiver I/O timings exported");
            TransferReadTest.require(received.stream().anyMatch(line->line.contains("\"state\":3")&&line.contains("private.bin")),"terminal state persisted");
            String filtered=new String(Files.readAllBytes(receiver.exportDiagnostics("unrelated-browser").toPath()),StandardCharsets.UTF_8);
            TransferReadTest.require(!filtered.contains("private.bin"),"browser export excludes other owners");
            System.out.println("PASS Android diagnostics: valid JSONL, phase and I/O timings, pipeline snapshots, final state, export fence, owner filtering");
        }finally{sender.close();receiver.close();}
    }
}
