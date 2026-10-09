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


def manifest(items):
    return ''.join(f"{name.encode().hex()}\t{'D' if folder else 'F'}\t{count}\t{size}\n" for name,folder,count,size in items).encode()

def offer(client, target, items, batch=None):
    batch=batch or uuid.uuid4().hex
    status, body=client.post('/web/api/offer',{'X-Chuan-Batch':batch,'X-Chuan-Sender':'QA sender'.encode().hex(),'X-Chuan-Target':target.encode().hex()},manifest(items))
    return batch,status,body

def upload(client,target,batch,index,name,payload=b'',folder=None):
    headers={'X-Chuan-Batch':batch,'X-Chuan-Item':str(index),'X-Chuan-Sender':'QA sender'.encode().hex(),'X-Chuan-Target':target.encode().hex(),
        'X-Chuan-Upload':uuid.uuid4().hex,'X-Chuan-Path':name.encode().hex(),'X-Chuan-Size':str(len(payload))}
    if folder:
        root,count,size,folderid=folder
        headers.update({'X-Chuan-Folder':folderid,'X-Chuan-Folder-Name':root.encode().hex(),'X-Chuan-Folder-Count':str(count),'X-Chuan-Folder-Size':str(size)})
    status,body=client.post('/web/api/upload',headers,payload+hashlib.sha256(payload).digest())
    return status,body,headers['X-Chuan-Upload']

thread=threading.Thread(target=heartbeats,daemon=True)
pool=concurrent.futures.ThreadPoolExecutor(4)
try:
    h1=start('BatchHost');h2=start('BatchRemote');link(h1,h2)
    a=Browser(h1);b=Browser(h2);outsider=Browser(h2);thread.start()
    check(eventual(lambda:any(p['browser'] and p['name']==b.state()['clientName'] for p in a.state()['peers'])),'remote browser visible')
    target=next(p['id'] for p in a.state()['peers'] if p['browser'] and p['name']==b.state()['clientName'])
    items=[(f'file-{i}.txt',False,1,len(str(i))) for i in range(50)]
    future=pool.submit(offer,a,target,items)
    check(eventual(lambda:len(b.state()['requests'])==1),'forwarded 50-file send creates exactly one request')
    pending=b.state()['requests'][0]
    check(len(pending['items'])==50 and not b.state()['inbox'],'complete selection is visible before any payload')
    check(pending['sender'].startswith('QA sender'),'relay preserves sender display name without replacing host identity')
    check(not outsider.state()['requests'],'other browser cannot see approval request')
    bits=''.join('1' if i in (1,17,49) else '0' for i in range(50))
    check(outsider.post('/web/api/decision/'+pending['id'],body=bits.encode())[0]==400,'other client cannot approve batch')
    check(b.post('/web/api/decision/'+pending['id'],body=bits.encode())[0]==200,'recipient submits partial selection once')
    batch,status,selection=future.result();check(status==200 and selection.decode()==bits,'selection returns through relay to sender')
    check(upload(a,target,batch,0,'file-0.txt',b'0')[0]==417,'unselected payload rejected at relay')
    check(upload(a,target,batch,1,'different-name.txt',b'1')[0]==417,'approved item cannot change path')
    check(upload(a,target,batch,1,'file-1.txt',b'11')[0]==417,'approved item cannot change size')
    for index in (1,17,49):
        payload=str(index).encode();status,body,key=upload(a,target,batch,index,f'file-{index}.txt',payload)
        check(status==201,'selected payload staged')
        fileid=json.loads(body)['id']
        check(a.post('/web/api/forward/'+fileid,{'X-Chuan-Upload':key,'X-Chuan-Peer':target.encode().hex()})[0]==202,'selected payload forwarded')
        check(eventual(lambda:any(f['name']==f'file-{index}.txt' for f in b.state()['inbox'])),'approved recipient receives selected item without further prompt')
        received=next(f for f in b.state()['inbox'] if f['name']==f'file-{index}.txt')
        check(request(h2['port'],'/web/files/'+received['id'],b.headers())[1]==payload,'remote browser receives exact payload')
    check(not b.state()['requests'],'batch does not produce per-file prompts')
    check(upload(a,target,batch,1,'file-1.txt',b'1')[0]==417,'replaying an accepted item rejected')
    check(offer(a,target,items,batch)[1]==409,'batch identifier cannot be reused')
    check(b.post('/web/api/preferences',{'X-Chuan-Max-Files':'2','X-Chuan-Max-Bytes':'0'})[0]==200,'browser count limit configured')
    check(offer(a,target,items)[1]!=200 and not b.state()['requests'],'receiver browser limit rejects aggregate before prompt')
    b.post('/web/api/preferences',{'X-Chuan-Max-Files':'0','X-Chuan-Max-Bytes':'1'})
    check(offer(a,target,[('a',False,1,1),('b',False,1,1)])[1]!=200,'browser byte limit applies across files')
    b.post('/web/api/preferences',{'X-Chuan-Max-Files':'0','X-Chuan-Max-Bytes':'0'})
    large=[('large',True,6001,9007199254740993)]
    future=pool.submit(offer,a,target,large)
    check(eventual(lambda:len(b.state()['requests'])==1),'unlimited receiver accepts more than 5000 and exact large aggregate')
    pending=b.state()['requests'][0]
    check(pending['items']==[dict(name='large',folder=True,count='6001',total='9007199254740993')],'folder is one atomic consent item with exact byte total')
    b.post('/web/api/decision/'+pending['id'],body=b'0');check(future.result()[2]==b'0','folder rejected atomically')
    native=h1['info']['id']; root='Nested'; fid=uuid.uuid4().hex
    batch,status,selection=offer(a,native,[(root,True,2,6)])
    check(status==200 and selection==b'1','native auto receive approves folder once')
    check(upload(a,native,batch,0,'Nested/one.txt',b'one',(root,2,6,fid))[0]==201,'first nested folder file saved')
    check(upload(a,native,batch,0,'Nested/sub/two.txt',b'two',(root,2,6,fid))[0]==201,'remaining nested file requires no further approval')
    check((h1['root']/'received/Nested/sub/two.txt').read_bytes()==b'two','directory hierarchy preserved')
    check(upload(a,native,batch,0,'Nested/extra.txt',b'',(root,2,6,fid))[0]==417,'folder approval cannot add extra files')
    bigitems=[(f'item-{i}.txt',False,1,0) for i in range(6001)]
    check(offer(a,native,bigitems)[1:]==(200,b'1'*6001),'unlimited manifest body supports 6001 individually selectable files')
    check(offer(a,native,[('same',False,1,0),('same',False,1,0)])[1]==400,'duplicate top-level path rejected')
    check(offer(a,native,[('../outside',False,1,0)])[1]==400,'unsafe manifest path rejected')
    print(f'{checks} batch checks passed',flush=True)
finally:
    stop.set()
    for process in processes:
        if process.poll() is None:process.terminate()
    for process in processes:
        try:process.wait(timeout=5)
        except subprocess.TimeoutExpired:process.kill()
    pool.shutdown(wait=True,cancel_futures=True)
