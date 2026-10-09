"""Exercise browser sessions, mesh routing, aliases, heartbeat expiry and Ping acknowledgements."""
import argparse
import concurrent.futures
import hashlib
import http.client
import json
from pathlib import Path
import socket
import subprocess
import threading
import time
import uuid

parser = argparse.ArgumentParser()
parser.add_argument('exe', type=Path)
parser.add_argument('work', type=Path)
args = parser.parse_args()
work = args.work.resolve() / uuid.uuid4().hex[:10]
work.mkdir(parents=True)
processes = []
stop = threading.Event()
clients = []
checks = 0

def check(condition, message):
    global checks
    assert condition, message
    checks += 1
    print('PASS', message, flush=True)

def request(port, path, headers=None, body=None):
    conn = http.client.HTTPConnection('127.0.0.1', port, timeout=8)
    conn.request('POST' if body is not None else 'GET', path, body=body, headers=headers or {})
    response = conn.getresponse()
    result = response.status, response.read()
    conn.close()
    return result

def free(kind=socket.SOCK_STREAM):
    with socket.socket(socket.AF_INET, kind) as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]

def eventual(predicate, seconds=9):
    until = time.monotonic() + seconds
    while time.monotonic() < until:
        if predicate():
            return True
        time.sleep(.1)
    return False

def start(name, manual=False):
    port, udp = free(), free(socket.SOCK_DGRAM)
    root = work / name
    proc = subprocess.Popen([str(args.exe.resolve()), '--headless', '--port', str(port),
        '--discovery-port', str(udp), '--data-dir', str(root / 'config'), '--receive-dir', str(root / 'received'),
        '--name', name, '--run-seconds', '180'] + (['--no-auto'] if manual else []))
    processes.append(proc)
    def ready():
        try: return request(port, '/api/info')[0] == 200
        except OSError: return False
    assert eventual(ready)
    info = json.loads(request(port, '/api/info')[1])
    return dict(port=port, udp=udp, root=root, proc=proc, info=info)

def link(a, b):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.sendto(f"CHUAN1|{b['info']['id']}|{b['port']}|{b['info']['nameHex']}".encode(), ('127.0.0.1', a['udp']))

class Browser:
    def __init__(self, host, name='', tab='first'):
        self.host, self.name, self.tab = host, name, tab
        self.key = uuid.uuid4().hex
        self.token = ''
        self.ack = 0
        self.enabled = True
        self.state()
        clients.append(self)

    def headers(self, tab=None):
        return {'X-Chuan-Client': self.key, 'X-Chuan-Tab': self.tab if tab is None else tab,
            'X-Chuan-Name': self.name.encode().hex(), 'X-Chuan-Ping-Ack': str(self.ack),
            'X-Chuan-Token': self.token}

    def state(self, tab=None):
        status, body = request(self.host['port'], '/web/api/state', self.headers(tab))
        assert status == 200, body
        value = json.loads(body)
        self.token = value['token']
        return value

    def post(self, path, extra=None, body=b'', tab=None):
        return request(self.host['port'], path, {**self.headers(tab), **(extra or {})}, body)

def heartbeats():
    while not stop.wait(1):
        for client in list(clients):
            if client.enabled:
                try: client.state()
                except (OSError, AssertionError): pass

thread = threading.Thread(target=heartbeats, daemon=True)
try:
    h1 = start('Relay', True)
    h2 = start('Other')
    link(h1, h2)
    a = Browser(h1)
    thread.start()
    check(eventual(lambda: any(p['id'] == h2['info']['id'] for p in a.state()['peers'])), 'TCP verified mesh discovery')
    b = Browser(h2)
    check(eventual(lambda: a.state()['clientName'] == 'A1' and b.state()['clientName'] == 'A2'), 'LAN-wide aliases follow sequential browser registrations')
    check(eventual(lambda: any(p['name']=='A2' and p['browser'] for p in a.state()['peers'])), 'browser presence propagates across native hosts')
    browser_id = next(p['id'] for p in a.state()['peers'] if p['name']=='A2')
    check(a.key not in json.dumps(b.state()) and b.key not in json.dumps(a.state()), 'public mesh does not disclose browser secrets')
    c = Browser(h1)
    check(c.state()['clientName'] == 'A3', 'distinct browsers on one IP are separate clients')
    a.state('second')
    check(a.post('/web/api/leave', tab='second')[0] == 200, 'closing one browser tab accepted')
    check(any(p['name']=='A1' for p in c.state()['peers']), 'another live tab preserves browser online status')
    a.state('second')
    check(a.state()['clientName']=='A1', 'refresh preserves client alias')
    check(a.post('/web/api/note', {'X-Chuan-Address':'127.0.0.1','X-Chuan-Note':'测试设备'.encode().hex()})[0]==200, 'temporary IP note accepted')
    check(any(p.get('note')=='测试设备' for p in a.state()['peers']) and all(not p.get('note') for p in c.state()['peers']), 'IP notes are private to their client')
    with concurrent.futures.ThreadPoolExecutor() as pool:
        ping = pool.submit(a.post, '/web/api/ping', {'X-Chuan-Peer':browser_id.encode().hex()})
        observed = {}
        def observe_ping():
            observed.update(b.state())
            return observed['ping'] > 0
        check(eventual(observe_ping, 3), 'remote browser receives a Ping event')
        b.ack = observed['ping']
        b.state()
        check(ping.result()[0] == 200, 'Ping completes only after browser acknowledgement')
    time.sleep(.9)
    check(a.post('/web/api/ping', {'X-Chuan-Peer':h2['info']['id'].encode().hex()})[0]==200, 'native Ping endpoint reachable')
    content = b'cross-host browser relay\x00' * 3000
    key = uuid.uuid4().hex
    upload_headers = {'X-Chuan-Upload':key,'X-Chuan-Target':browser_id.encode().hex(),
        'X-Chuan-Path':'浏览器互传.bin'.encode().hex(),'X-Chuan-Sender':'A1'.encode().hex(),
        'X-Chuan-Size':str(len(content)), 'Content-Type':'application/octet-stream'}
    uploaded = a.post('/web/api/upload', upload_headers, content+hashlib.sha256(content).digest())
    check(uploaded[0]==201, 'relay upload bypasses unrelated host receive confirmation')
    file_id=json.loads(uploaded[1])['id']
    check(not (h1['root']/'received'/'浏览器互传.bin').exists(), 'intermediate payload is kept outside receive directory')
    check(c.post(f'/web/api/forward/{file_id}', {'X-Chuan-Upload':key,'X-Chuan-Peer':browser_id.encode().hex()})[0]==404, 'another browser cannot forward private staging file')
    forward={'X-Chuan-Upload':key,'X-Chuan-Peer':browser_id.encode().hex()}
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        results=list(pool.map(lambda _:a.post(f'/web/api/forward/{file_id}',forward)[0],range(4)))
    check(all(result==202 for result in results), 'concurrent cross-host forwarding retries are accepted')
    check(a.post(f'/web/api/forward/{file_id}',forward)[0]==202, 'duplicate forward submission is idempotent')
    check(eventual(lambda: bool(b.state()['inbox'])), 'remote host offers the file to its browser')
    inbox=b.state()['inbox']
    check(len(inbox)==1, 'duplicate request creates only one receive offer')
    delivery=inbox[0]
    check(request(h2['port'],f"/web/files/{delivery['id']}",a.headers())[0]==404, 'cross-host private delivery rejects another client')
    check(request(h2['port'],f"/web/files/{delivery['id']}",b.headers())[1]==content, 'cross-host browser delivery preserves exact bytes')
    check(eventual(lambda: any(j['uploadKey']==key and j['state']==3 for j in a.state()['jobs'])), 'sender completion follows actual browser transfer')
    check(len([j for j in a.state()['jobs'] if j['uploadKey']==key])==1, 'relay phases appear as one logical transfer')
    check(b.post('/web/api/clear')[0]==200 and not b.state()['jobs'] and not b.state()['inbox'], 'clear completed affects only the requesting browser')
    check(bool(a.state()['jobs']), 'receiver clearing history preserves sender history')
    c.enabled=False
    check(eventual(lambda: all(p['name']!='A3' for p in a.state()['peers']),8), 'missing heartbeats expire a client within three intervals')
    c.enabled=True
    check(c.state()['clientName']=='A3', 'short reconnect retains allocated alias')
    leader=min([h1,h2], key=lambda h:h['info']['id'])
    survivor=h2 if leader is h1 else h1
    observer=b if survivor is h2 else a
    for client in clients:
        if client.host is leader: client.enabled=False
    leader['proc'].terminate();leader['proc'].wait(5)
    check(eventual(lambda: all(p['id']!=leader['info']['id'] for p in observer.state()['peers']),9), 'failed native host is removed after connection expiry')
    d=Browser(survivor)
    check(d.state()['clientName']=='A4', 'coordinator handoff preserves the allocation counter')
    for client in clients: client.enabled=False
    survivor['proc'].terminate();survivor['proc'].wait(5)
    fresh=start('Fresh')
    fresh_client=Browser(fresh)
    check(fresh_client.state()['clientName']=='A1', 'new LAN session restarts numbering at A1')
    print(f'{checks} presence checks passed',flush=True)
finally:
    stop.set()
    if thread.is_alive(): thread.join(3)
    for proc in processes:
        if proc.poll() is None: proc.terminate()
        try: proc.wait(5)
        except subprocess.TimeoutExpired: proc.kill()
