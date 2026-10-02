#!/usr/bin/env python3
"""Upload a folder made by prep_photos.py to the sign, in 16 KB pieces (the
size the Tab5's Wi-Fi takes reliably), each photo followed by its thumbnail.

    tools/put_folder.py HOST LOCAL_DIR SIGN_FOLDER     e.g. flapboard.local scratch/big-canada photos/big-canada
"""
import sys, os, json, time, random, string, urllib.request, urllib.parse

host, local, dest = sys.argv[1], sys.argv[2], sys.argv[3].strip('/')

def post(path, data, ctype='application/octet-stream'):
    req = urllib.request.Request(f'http://{host}{path}', data=data, method='POST', headers={'Content-Type': ctype})
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.load(r)

def put(folder, name, data, thumb=False):
    """One file in pieces. Survives the sign restarting mid-file: pieces are
    retried for ~2 minutes, and a 409 says where the sign's copy ends."""
    uid = ''.join(random.choices(string.ascii_lowercase, k=10)); off = 0; last = None
    while off < len(data):
        body = data[off:off + 16384]
        q = {'path': folder, 'name': name, 'id': uid, 'offset': off, 'total': len(data)}
        if thumb: q['thumb'] = 1
        t0 = time.time()
        while True:
            try:
                last = post('/library/upload_part?' + urllib.parse.urlencode(q), body)
                off += len(body); break
            except urllib.error.HTTPError as e:
                info = {}
                try: info = json.loads(e.read())
                except Exception: pass
                if e.code == 409 and isinstance(info.get('have'), int) and info['have'] < off:
                    off = info['have']; break       # resume where the sign's copy ends
                if e.code < 500 and e.code != 409: raise
            except Exception:
                pass
            if time.time() - t0 > 150: raise RuntimeError('the sign stopped answering')
            time.sleep(3)
    return last

parent, leaf = os.path.split(dest)
acc = ''
for part in dest.split('/'):   # make the folder (an existing one is fine)
    try: post('/library/mkdir', json.dumps({'path': acc, 'name': part}).encode(), 'application/json')
    except Exception: pass
    acc = f'{acc}/{part}' if acc else part
names = sorted(f for f in os.listdir(local) if f.lower().endswith('.jpg') and not f.startswith('.'))
t0 = time.time(); sent = 0
for i, n in enumerate(names, 1):
    data = open(os.path.join(local, n), 'rb').read()
    r = put(dest, n, data)
    saved = (r.get('saved') or [f'{dest}/{n}'])[0].split('/')[-1]
    tp = os.path.join(local, '.thumbs', n)
    if os.path.exists(tp): put(dest, saved, open(tp, 'rb').read(), thumb=True)
    sent += len(data)
    print(f'{i}/{len(names)} {saved}  {sent / (time.time() - t0) / 1024:.0f} KB/s', flush=True)
print('done', flush=True)
