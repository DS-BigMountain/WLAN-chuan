"""Run real local peers and verify transport and distribution endpoints."""
import argparse
import hashlib
import http.client
import json
import os
from pathlib import Path
import socket
import subprocess
import time
import uuid

parser = argparse.ArgumentParser()
parser.add_argument("exe", type=Path)
parser.add_argument("work", type=Path)
args = parser.parse_args()
exe = args.exe.resolve()
work = args.work.resolve() / uuid.uuid4().hex[:10]
work.mkdir(parents=True)
processes = []
checks = 0

def check(value, label):
    global checks
    if not value:
        raise AssertionError(label)
    checks += 1
    print("PASS", label, flush=True)

def free_port(kind=socket.SOCK_STREAM):
    with socket.socket(socket.AF_INET, kind) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]

def get(port, path, headers=None):
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
    conn.request("GET", path, headers=headers or {})
    response = conn.getresponse()
    status, body = response.status, response.read()
    conn.close()
    return status, body

def start(label, *flags):
    port, udp = free_port(), free_port(socket.SOCK_DGRAM)
    folder = work / label
    folder.mkdir()
    receive = folder / "received"
    process = subprocess.Popen([str(exe), "--headless", "--port", str(port),
        "--discovery-port", str(udp), "--data-dir", str(folder / "config"),
        "--receive-dir", str(receive), "--name", label, "--run-seconds", "180", *flags])
    processes.append(process)
    for _ in range(100):
        if process.poll() is not None:
            raise RuntimeError(f"{label} exited: {process.returncode}")
        try:
            if get(port, "/api/info")[0] == 200:
                return port, udp, folder, receive
        except OSError:
            pass
        time.sleep(0.05)
    raise RuntimeError("server startup timed out")

def header(sock):
    data = b""
    while not data.endswith(b"\r\n\r\n"):
        chunk = sock.recv(1)
        if not chunk:
            break
        data += chunk
        if len(data) > 16384:
            raise AssertionError("oversized response")
    return data

def offer(port, name, payload, digest=None, size=None, declared=None):
    sock = socket.create_connection(("127.0.0.1", port), timeout=5)
    count = len(payload) if size is None else size
    length = count + 32 if declared is None else declared
    request = (f"POST /api/upload HTTP/1.1\r\nHost: 127.0.0.1\r\n"
        f"Expect: 100-continue\r\nX-Chuan-Path: {name.encode().hex()}\r\n"
        f"X-Chuan-Sender: {'测试电脑'.encode().hex()}\r\n"
        f"X-Chuan-Size: {count}\r\nContent-Length: {length}\r\n\r\n")
    sock.sendall(request.encode())
    first = header(sock)
    if first.startswith(b"HTTP/1.1 100 "):
        sock.sendall(payload + (hashlib.sha256(payload).digest() if digest is None else digest))
        result = header(sock)
    else:
        result = first
    sock.close()
    return result

def eventual(predicate):
    for _ in range(100):
        if predicate():
            return True
        time.sleep(0.05)
    return False

client = uuid.uuid4().hex
def state(port, who=client):
    status, body = get(port, '/web/api/state', {'X-Chuan-Client':who})
    assert status == 200
    return json.loads(body)

def web_post(port, path, payload=b'', extra=None, who=client, token=None):
    conn=http.client.HTTPConnection('127.0.0.1', port, timeout=10)
    headers={'X-Chuan-Token': state(port, who)['token'] if token is None else token,'X-Chuan-Client':who}
    headers.update(extra or {})
    conn.request('POST', path, body=payload, headers=headers)
    response=conn.getresponse(); result=response.status,response.read();conn.close();return result

def web_headers(name, payload, key):
    return {'X-Chuan-Path':name.encode().hex(),'X-Chuan-Sender':'Web browser'.encode().hex(),
            'X-Chuan-Size':str(len(payload)),'X-Chuan-Upload':key,'Content-Type':'application/octet-stream'}

try:
    port, udp, folder, receive = start("Receiver")
    status, info = get(port, "/api/info")
    check(status == 200 and json.loads(info)["protocol"] == 1, "protocol info")
    check(get(port, "/")[0] == 200, "download page")
    status, download = get(port, "/download/Chuan.exe")
    check(status == 200 and hashlib.sha256(download).digest() == hashlib.sha256(exe.read_bytes()).digest(), "distributed EXE is byte-identical")
    check(get(port, "/../settings.ini")[0] == 404, "HTTP file exposure restricted")
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as query:
        query.settimeout(5)
        query.sendto(b"CHUAN_DISCOVER1", ("127.0.0.1", udp))
        ad, _ = query.recvfrom(1200)
        check(ad.startswith(b"CHUAN1|") and int(ad.split(b"|")[2]) == port, "UDP discovery replies with service port")
    for name in ["../escape.txt", "a/../../escape.txt", "C:\\escape.txt", "CON.txt", "file:secret", "a//b"]:
        check(offer(port, name, b"test").startswith(b"HTTP/1.1 400 "), "reject unsafe path " + name)
    payload = os.urandom(2 * 1024 * 1024 + 17)
    check(offer(port, "资料/中文 测试.bin", payload).startswith(b"HTTP/1.1 201 "), "Unicode nested file transfer")
    saved = receive / "资料" / "中文 测试.bin"
    check(saved.read_bytes() == payload, "received content verified")
    second = b"duplicate name"
    check(offer(port, "资料/中文 测试.bin", second).startswith(b"HTTP/1.1 201 "), "same-name transfer completes")
    check(saved.read_bytes() == payload and (saved.parent / "中文 测试 (1).bin").read_bytes() == second, "existing file is not overwritten")
    check(offer(port, "空文件.txt", b"").startswith(b"HTTP/1.1 201 ") and (receive / "空文件.txt").stat().st_size == 0, "empty file transfer")
    check(offer(port, "corrupted.bin", b"checksum test", digest=b"\0" * 32).startswith(b"HTTP/1.1 422 "), "checksum mismatch rejected")
    check(not (receive / "corrupted.bin").exists() and not list(receive.glob("*.part")), "bad transfer has no final file or temporary residue")
    check(offer(port, "overflow.bin", b"", size=2**64-1, declared=31).startswith(b"HTTP/1.1 400 "), "length overflow rejected")
    # Exercise the actual application sender, including folder enumeration.
    source = work / "发送目录"
    (source / "子目录").mkdir(parents=True)
    (source / "子目录" / "大文件.bin").write_bytes(os.urandom(8 * 1024 * 1024 + 123))
    (source / "说明.txt").write_text("局域网互传测试", encoding="utf-8")
    (source / "零字节").write_bytes(b"")
    sender_port = free_port()
    send_args = [str(exe), "--send-to", f"127.0.0.1:{port}", "--send", str(source),
        "--data-dir", str(work / "sender-config"), "--receive-dir", str(work / "sender-received"),
        "--port", str(sender_port), "--discovery-port", str(free_port(socket.SOCK_DGRAM)), "--name", "Sender"]
    result = subprocess.run(send_args, timeout=30)
    check(result.returncode == 0, "application sender completes folder batch")
    for f in source.rglob("*"):
        if f.is_file():
            dest = receive / source.name / f.relative_to(source)
            check(dest.read_bytes() == f.read_bytes(), "batch content " + str(f.relative_to(source)))
    # An interrupted payload must never become a final received file.
    sock = socket.create_connection(("127.0.0.1", port), timeout=5)
    req = ("POST /api/upload HTTP/1.1\r\nHost: localhost\r\nExpect: 100-continue\r\n"
        f"X-Chuan-Path: {'interrupted.bin'.encode().hex()}\r\nX-Chuan-Sender: {'Tester'.encode().hex()}\r\n"
        "X-Chuan-Size: 1000\r\nContent-Length: 1032\r\n\r\n")
    sock.sendall(req.encode()); check(header(sock).startswith(b"HTTP/1.1 100 "), "interruption test starts")
    sock.sendall(b"partial"); sock.close()
    check(eventual(lambda: not list(receive.glob("*.part"))) and not (receive / "interrupted.bin").exists(), "interrupted upload cleanup")
    # Browser requests use the same checksum trailer without Expect: 100-continue.
    check(all(get(port,path)[0]==200 for path in ['/web/style.css','/web/app.js','/web/sha256.js','/web/files.js']), 'embedded web assets')
    check('附近设备'.encode() in get(port,'/')[1] and '主机文件'.encode() not in get(port,'/')[1], 'web application has no public host library')
    check(web_post(port,'/web/api/refresh')[0]==200,'web refresh triggers device discovery')
    conn=http.client.HTTPConnection('127.0.0.1',port)
    conn.request('GET','/',headers={'Host':f'attacker.invalid:{port}'})
    check(conn.getresponse().status==403,'web rejects unrecognized host');conn.close()
    key=uuid.uuid4().hex;web_data=os.urandom(1048576+57)
    wh=web_headers('网页 中文.bin',web_data,key)
    body=web_data+hashlib.sha256(web_data).digest()
    check(web_post(port,'/web/api/upload',b'',wh,token='invalid')[0]==403, 'web requires request token')
    check(web_post(port,'/web/api/upload',b'',{**wh,'Origin':'http://attacker.invalid'})[0]==403, 'web rejects cross-origin upload')
    check(web_post(port,'/web/api/upload',body,wh)[0]==201 and (receive/'网页 中文.bin').read_bytes()==web_data,'browser upload and checksum validation')
    direct_folder='直接接收/子目录/说明.txt'
    direct_data='文件夹直接接收'.encode()
    direct_upload=web_post(port,'/web/api/upload',direct_data+hashlib.sha256(direct_data).digest(),web_headers(direct_folder,direct_data,uuid.uuid4().hex))
    check(direct_upload[0]==201 and (receive/direct_folder).read_bytes()==direct_data,'browser folder upload to host preserves complete hierarchy')
    view=state(port);shared=next(f for f in view['jobs'] if f['name']=='网页 中文.bin')
    check('files' not in view and get(port,'/web/api/state')[0]==401, 'no public file list or anonymous state')
    job=next(j for j in view['jobs'] if j['uploadKey']==key)
    check(job['state']==3 and job['done']==len(web_data),'web transfer state synchronized')
    check(not job['receiving'] and not view['inbox'],'browser upload is outgoing and does not enter its own inbox')
    browser_client=uuid.uuid4().hex
    browser_view=json.loads(get(port,'/web/api/state',{'X-Chuan-Client':browser_client,'X-Chuan-Name':'测试浏览器'.encode().hex()})[1])
    browser_id=next(p['id'] for p in state(port)['peers'] if p['name']=='测试浏览器')
    check(browser_client not in browser_id and browser_client not in json.dumps(state(port)), 'browser secret is not published as peer identity')
    check(any(p['id']==browser_id and p['name']=='测试浏览器' and p['browser'] for p in state(port)['peers']),'named browser appears in online device list')
    check(not any(p['id']==browser_id for p in browser_view['peers']),'browser excludes itself from destinations')
    get(port,'/web/api/state',{'X-Chuan-Client':browser_client,'X-Chuan-Name':'浏览器新名称'.encode().hex()})
    check(any(p['id']==browser_id and p['name']=='浏览器新名称' for p in state(port)['peers']),'browser name updates without duplicate device')
    check(all(not j['uploadKey'] for j in state(port,uuid.uuid4().hex)['jobs']),'web upload ownership isolated')
    check(get(port,f"/web/files/{shared['id']}")[0]==404,'public upload download is disabled')
    check(get(port,f"/web/files/{shared['id']}",{'X-Chuan-Client':client})[1]==web_data,'owner can access own uploaded file')
    check(get(port,'/web/files/9999999')[0]==404 and get(port,'/web/files/../settings.ini')[0]==404,'web download limited to received file identifiers')
    bad=web_headers('web-bad.bin',b'broken',uuid.uuid4().hex)
    check(web_post(port,'/web/api/upload',b'broken'+bytes(32),bad)[0]==422 and not (receive/'web-bad.bin').exists(),'web rejects corrupt checksum')
    target_port,_,_,target_receive=start('ForwardTarget')
    target_info=json.loads(get(target_port,'/api/info')[1])
    announcement=f"CHUAN1|{target_info['id']}|{target_port}|{target_info['nameHex']}".encode()
    with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as ad:ad.sendto(announcement,('127.0.0.1',udp))
    check(eventual(lambda:any(p['id']==target_info['id'] for p in state(port)['peers'])),'web nearby devices synchronized')
    forward={'X-Chuan-Peer':target_info['id'].encode().hex(),'X-Chuan-Upload':uuid.uuid4().hex}
    check(web_post(port,f"/web/api/forward/{shared['id']}",extra=forward)[0]==202,'web forwarding accepted')
    check(eventual(lambda:(target_receive/'网页 中文.bin').exists()) and (target_receive/'网页 中文.bin').read_bytes()==web_data,'web forwards to actual native receiver')
    check(not state(target_port)['jobs'] and 'files' not in state(target_port),'native receiver does not publish files to browsers')
    for subfolder in ['甲','乙']:
        folder_path=f'网页文件夹/{subfolder}/同名.txt'
        folder_data=('保留目录 '+subfolder).encode()
        folder_key=uuid.uuid4().hex
        folder_headers={**web_headers(folder_path,folder_data,folder_key),'X-Chuan-Target':target_info['id'].encode().hex()}
        folder_upload=web_post(port,'/web/api/upload',folder_data+hashlib.sha256(folder_data).digest(),folder_headers)
        check(folder_upload[0]==201,'folder upload preserves nested path '+subfolder)
        folder_id=json.loads(folder_upload[1])['id']
        check(web_post(port,f'/web/api/forward/{folder_id}',extra={'X-Chuan-Peer':target_info['id'].encode().hex(),'X-Chuan-Upload':folder_key})[0]==202,'folder relay accepted '+subfolder)
        folder_destination=target_receive/'网页文件夹'/subfolder/'同名.txt'
        check(eventual(folder_destination.exists) and folder_destination.read_bytes()==folder_data,'folder relay preserves hierarchy and duplicate basenames '+subfolder)
    browser_forward={'X-Chuan-Peer':browser_id.encode().hex(),'X-Chuan-Upload':uuid.uuid4().hex}
    check(web_post(port,f"/web/api/forward/{shared['id']}",extra=browser_forward)[0]==202,'web forwards to online browser')
    inbox=state(port,browser_client)['inbox']; delivery=next(f for f in inbox if f['name']=='网页 中文.bin')
    check(not delivery['received'] and not state(port)['inbox'],'only destination browser has pending receive entry')
    check(get(port,f"/web/files/{delivery['id']}")[0]==404,'browser offered file rejects missing recipient identifier')
    check(get(port,f"/web/files/{delivery['id']}",{'X-Chuan-Client':client})[0]==404,'another browser cannot fetch private delivery')
    check(get(port,f"/web/files/{delivery['id']}?client={browser_client}")[1]==web_data,'destination browser streams byte-identical file')
    browser_received=state(port,browser_client)
    check(any(f['id']==delivery['id'] and f['received'] for f in browser_received['inbox']) and any(j['id']==delivery['id'] and j['receiving'] and j['state']==3 for j in browser_received['jobs']),'browser delivery completes with receiver direction')
    check(web_post(port,f"/web/api/delete/{delivery['id']}",who=browser_client)[0]==200 and (receive/'网页 中文.bin').exists(),'recipient removes own record without deleting sender source')
    check(web_post(port,'/web/api/leave',who=browser_client)[0]==200 and not any(p['id']==browser_id for p in state(port)['peers']),'closed browser removes online entry')
    delete_data=b'deletion test';delete_key=uuid.uuid4().hex
    deletion=web_post(port,'/web/api/upload',delete_data+hashlib.sha256(delete_data).digest(),web_headers('delete-owned.txt',delete_data,delete_key))
    delete_id=json.loads(deletion[1])['id']
    check(web_post(port,f'/web/api/delete/{delete_id}',who=uuid.uuid4().hex)[0]==404,'other browser cannot delete owned upload')
    check(web_post(port,f'/web/api/delete/{delete_id}')[0]==200 and not (receive/'delete-owned.txt').exists() and not any(j['id']==delete_id for j in state(port)['jobs']),'owner deletes host file and transfer record')
    check(web_post(port,f"/web/api/forward/{shared['id']}",extra={**forward,'X-Chuan-Peer':'unknown'.encode().hex()})[0]==409,'web refuses offline forwarding target')
    (receive/'网页 中文.bin').write_bytes(b'changed')
    check(get(port,f"/web/files/{shared['id']}",{'X-Chuan-Client':client})[0]==409,'web detects received file change')
    check(web_post(port,f"/web/api/forward/{shared['id']}",extra={**forward,'X-Chuan-Upload':uuid.uuid4().hex})[0]==409,'web refuses forwarding a changed received file')
    off_port, _, _, _ = start("NoShare", "--no-share")
    check(get(off_port, "/")[0] == 403 and get(off_port, "/download/Chuan.exe")[0] == 403, "software distribution switch")
    check(get(off_port,'/web/api/state')[0]==403 and get(off_port,'/web/app.js')[0]==403,'web service switch blocks all web routes')
    manual_port, _, _, manual_receive = start("Manual", "--no-auto")
    sock = socket.create_connection(("127.0.0.1", manual_port), timeout=5)
    sock.sendall(req.encode()); sock.settimeout(0.6)
    waiting = False
    try:
        waiting = sock.recv(1) == b""
    except socket.timeout:
        waiting = True
    sock.close()
    check(waiting and not list(manual_receive.iterdir()), "manual mode waits before accepting file bytes")
    manual_key=uuid.uuid4().hex;manual_body=b'web pending';manual_token=state(manual_port)['token']
    pending=http.client.HTTPConnection('127.0.0.1',manual_port,timeout=10)
    mh=web_headers('web-manual.txt',manual_body,manual_key)
    mh.update({'X-Chuan-Token':manual_token,'X-Chuan-Client':client})
    pending.request('POST','/web/api/upload',body=manual_body+hashlib.sha256(manual_body).digest(),headers=mh)
    check(eventual(lambda:any(j['uploadKey']==manual_key and j['state']==1 for j in state(manual_port)['jobs'])) and not list(manual_receive.iterdir()),'web upload waits for desktop confirmation')
    check(web_post(manual_port,f'/web/api/cancel/{manual_key}',who=uuid.uuid4().hex)[0]==404,'other browser cannot cancel owned web task')
    check(web_post(manual_port,f'/web/api/cancel/{manual_key}')[0]==200,'browser cancels pending upload')
    try:
        response=pending.getresponse();response.read()
    except (http.client.RemoteDisconnected, ConnectionResetError):
        pass
    pending.close()
    check(eventual(lambda:any(j['uploadKey']==manual_key and j['state']==4 for j in state(manual_port)['jobs'])) and not list(manual_receive.iterdir()),'cancelled web upload has no saved file')
    print(f"{checks} integration checks passed", flush=True)
finally:
    for process in processes:
        if process.poll() is None:
            process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
