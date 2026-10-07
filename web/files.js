// A file manager for one folder of the SD card (and the folders inside it),
// placed on whichever page needs it: Messages (messages/), Photos (photos/,
// shrinking photos and making thumbnails as they go up), Settings (fonts/,
// sounds/). Same /library API as the Pi photo frame's file manager.
//
//   new FileManager(element, {root: 'photos', label: 'Photos', photos: true, onChange})
//
// Uploads go in 16 KB pieces, one file at a time (the Tab5's Wi-Fi chip
// wedges on big inbound bursts); drops on the manager (files or whole
// folders) and picks made during an upload join the queue.
(function () {
  const esc = s => { const d = document.createElement('div'); d.textContent = s; return d.innerHTML; };
  const size = n => { if (n == null) return ''; const u = ['B', 'KB', 'MB', 'GB', 'TB']; let i = 0; while (n >= 1000 && i < u.length - 1) { n /= 1000; i++; } return (i ? n.toFixed(1) : n) + ' ' + u[i]; };
  const post = async (url, body) => { const r = await fetch(url, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) }); return { ok: r.ok, d: await r.json() }; };
  const isImage = f => /^image\//.test(f.type) || /\.(jpe?g|png|heic|heif|webp|gif)$/i.test(f.name);

  // Safari will not upload a File taken straight from a file input as a
  // body (it sends nothing), so files are read into an ArrayBuffer first.
  function readFile(f) { return new Promise((res, rej) => { const fr = new FileReader(); fr.onload = () => res(fr.result); fr.onerror = () => rej(new Error('unreadable')); fr.readAsArrayBuffer(f); }); }
  function loadImage(blob) {
    const url = URL.createObjectURL(blob);
    return new Promise((res, rej) => { const i = new Image(); i.onload = () => res(i); i.onerror = rej; i.src = url; }).finally(() => URL.revokeObjectURL(url));
  }
  function thumbOf(img) {   // 240x135, cropped to fill, like the list's tiles
    const t = document.createElement('canvas'); t.width = 240; t.height = 135; const tc = t.getContext('2d');
    const k = Math.max(240 / img.naturalWidth, 135 / img.naturalHeight); tc.imageSmoothingQuality = 'high';
    tc.drawImage(img, (240 - img.naturalWidth * k) / 2, (135 - img.naturalHeight * k) / 2, img.naturalWidth * k, img.naturalHeight * k);
    return new Promise(r => t.toBlob(r, 'image/jpeg', 0.8));
  }
  async function makeThumb(blob) { try { return await thumbOf(await loadImage(blob)); } catch (e) { return null; } }
  // Photos: decoded in the browser (Safari also reads HEIC), scaled so the
  // picture still covers 1280x720, sent as a baseline JPEG: the Tab5's
  // decoders take only baseline JPEG and PNG, and a 12 MP original would
  // take a minute to send over its Wi-Fi.
  async function shrinkPhoto(f) {
    try {
      const img = await loadImage(f);
      const k = Math.min(1, Math.max(1280 / img.naturalWidth, 720 / img.naturalHeight));
      const cv = document.createElement('canvas'); cv.width = Math.round(img.naturalWidth * k); cv.height = Math.round(img.naturalHeight * k);
      const ctx = cv.getContext('2d'); ctx.imageSmoothingQuality = 'high'; ctx.drawImage(img, 0, 0, cv.width, cv.height);
      const main = await new Promise(r => cv.toBlob(r, 'image/jpeg', 0.88));
      return { main, thumb: await thumbOf(img), name: f.name.replace(/\.[^.]+$/, '') + '.jpg' };
    } catch (e) { return null; }
  }
  // If the sign stops answering (its Wi-Fi watchdog restarts it, ~40 s),
  // wait for it and carry on from the same piece; a 409 says where the
  // sign's copy ends.
  async function signBack(maxMs) {
    const t0 = Date.now();
    while (Date.now() - t0 < maxMs) {
      try { const r = await fetch('/api/status', { cache: 'no-store', signal: AbortSignal.timeout(3000) }); if (r.ok) return true; } catch (e) {}
      await new Promise(r => setTimeout(r, 2000));
    }
    return false;
  }
  async function putInPieces(folder, name, blob, onProgress, thumb) {
    const id = Math.random().toString(36).slice(2, 12), total = blob.size, piece = 16384;
    let off = 0, last = null;
    do {
      let ok = false;
      for (let a = 0; a < 6 && !ok; a++) {
        if (a) await new Promise(r => setTimeout(r, 1000 * a));
        const body = blob.slice(off, off + piece);
        try {
          const q = new URLSearchParams({ path: folder, name, id, offset: off, total }); if (thumb) q.set('thumb', '1');
          const r = await fetch('/library/upload_part?' + q, { method: 'POST', body, signal: AbortSignal.timeout(30000) }); last = await r.json();
          if (r.ok) { ok = true; off += body.size; }
          else if (r.status === 409 && typeof last.have === 'number' && last.have < off) off = last.have;
          else if (r.status < 500 && r.status !== 409) return { ok: false, data: last, status: r.status };
        } catch (e) { last = { error: 'connection lost' }; if (a >= 1 && !await signBack(120000)) break; }
      }
      if (!ok) return { ok: false, data: last || { error: 'connection lost' }, status: 0 };
      if (onProgress) onProgress(total ? off / total : 1);
    } while (off < total);
    return { ok: true, data: last, status: 200 };
  }
  // Drag and drop: folders are walked (webkitGetAsEntry); hidden files are left out.
  function walk(entry, dir, out) {
    return new Promise(resolve => {
      if (entry.name.startsWith('.')) return resolve();
      if (entry.isFile) entry.file(file => { out.push({ file, dir }); resolve(); }, () => resolve());
      else if (entry.isDirectory) {
        const sub = dir ? dir + '/' + entry.name : entry.name, r = entry.createReader(), all = [];
        const more = () => r.readEntries(async es => { if (es.length) { all.push(...es); more(); } else { for (const e of all) await walk(e, sub, out); resolve(); } }, () => resolve());
        more();
      } else resolve();
    });
  }
  function icon(e) {
    if (e.type === 'dir') return '&#128193;';
    if (e.is_image) return '&#128247;';
    const x = e.name.toLowerCase().split('.').pop();
    return x === 'txt' ? '&#128221;' : x === 'wav' ? '&#127925;' : (x === 'ttf' || x === 'otf') ? '&#128288;' : '&#128196;';
  }

  class FileManager {
    constructor(el, opts) {
      this.el = el;
      this.root = opts.root;               // e.g. 'photos'
      this.label = opts.label || opts.root;
      this.photos = !!opts.photos;          // offer shrinking and make photo thumbnails
      this.onChange = opts.onChange || (() => {});
      this.cwd = this.root;
      this.pending = []; this.uploading = false;
      this.thumbQueue = []; this.thumbBusy = false;
      el.classList.add('fm');
      el.innerHTML =
        '<div class="crumbs"></div><div class="dropmsg">Drop files or folders to upload them here</div><div class="fmnote"></div>' +
        '<div class="prog"><i></i></div><div class="progtxt"></div>' +
        '<div class="row" style="margin:0 0 12px"><button data-act="up">Upload files</button><button class="sec" data-act="mkdir">New folder</button>' +
        '<span class="hint" style="margin:0">or drag files and folders here</span><span class="space"></span></div>' +
        (this.photos ? '<label class="shrinkrow"><input type="checkbox" class="shrink" checked> Shrink photos to the screen\'s size while uploading (recommended: much faster, and iPhone HEIC photos work)</label>' : '') +
        '<input type="file" multiple hidden><div class="list"></div>';
      this.q = s => el.querySelector(s);
      this.picker = this.q('input[type=file]');
      this.picker.onchange = () => { const files = [...this.picker.files]; this.picker.value = ''; this.upload(files.map(file => ({ file, dir: this.cwd }))); };
      el.addEventListener('click', e => this.click(e));
      let depth = 0;
      const can = e => e.dataTransfer && [...e.dataTransfer.types].includes('Files');
      el.addEventListener('dragenter', e => { if (!can(e)) return; e.preventDefault(); if (++depth === 1) el.classList.add('dragging'); });
      el.addEventListener('dragover', e => { if (!can(e)) return; e.preventDefault(); e.dataTransfer.dropEffect = 'copy'; });
      el.addEventListener('dragleave', () => { if (depth && --depth === 0) el.classList.remove('dragging'); });
      el.addEventListener('drop', async e => {
        if (!e.dataTransfer) return;
        e.preventDefault(); depth = 0; el.classList.remove('dragging');
        const entries = [...e.dataTransfer.items].filter(i => i.kind === 'file').map(i => i.webkitGetAsEntry && i.webkitGetAsEntry());
        const out = [];
        if (entries.length && entries.every(Boolean)) { for (const en of entries) await walk(en, this.cwd, out); }
        else for (const file of e.dataTransfer.files) out.push({ file, dir: this.cwd });
        this.upload(out);
      });
      // Inside a closed section, list nothing until it is opened: the sign
      // answers one request at a time, and a big folder would hold up the page.
      const det = el.closest('details');
      if (det && !det.open) det.addEventListener('toggle', () => { if (det.open && !this.listed) this.go(this.root); });
      else this.go(this.root);
    }
    note(kind, text) {
      const n = this.q('.fmnote'); n.innerHTML = '<div class="msg ' + kind + '">' + esc(text) + '</div>';
      if (kind === 'ok') setTimeout(() => n.innerHTML = '', 4000);
    }
    async go(path) {
      if (path !== this.root && !path.startsWith(this.root + '/')) path = this.root;   // never above its own folder
      this.cwd = path; this.listed = true;
      try {
        const r = await fetch('/library/list?path=' + encodeURIComponent(path)); const d = await r.json();
        if (r.status === 404 && path === this.root) { await post('/library/mkdir', { path: '', name: this.root }); return this.go(path); }
        if (!r.ok) { this.note('err', d.error || 'Could not open the folder'); return; }
        this.render(d);
      } catch (e) { this.note('err', 'Could not reach the sign'); }
    }
    render(d) {
      const rel = this.cwd.slice(this.root.length).split('/').filter(Boolean);
      let crumbs = '<a data-go="' + esc(this.root) + '">' + esc(this.label) + '</a>', acc = this.root;
      for (const p of rel) { acc += '/' + p; crumbs += ' / <a data-go="' + esc(acc) + '">' + esc(p) + '</a>'; }
      this.q('.crumbs').innerHTML = crumbs;
      const sp = this.q('.space'); sp.textContent = d.total_bytes ? size(d.free_bytes) + ' free' : ''; sp.className = 'space' + (d.low_space ? ' low' : '');
      const list = this.q('.list');
      if (!d.entries.length) { list.innerHTML = '<div class="empty">This folder is empty. Use <b>Upload files</b>, or drop files here.</div>'; return; }
      let h = '<div class="grid">';
      for (const e of d.entries) {
        const p = esc(e.path), dl = '/library/download?path=' + encodeURIComponent(e.path);
        h += '<div class="item">';
        if (e.type === 'dir') h += '<div class="thumb" data-go="' + p + '">' + icon(e) + '</div>';
        else if (e.is_image) h += '<a class="thumb" href="' + dl + '" target="_blank"><img loading="lazy" src="/library/thumb?path=' + encodeURIComponent(e.path) + '" data-path="' + encodeURIComponent(e.path) + '"></a>';
        else h += '<a class="thumb" href="' + dl + '" target="_blank">' + icon(e) + '</a>';
        h += '<div class="nm">' + esc(e.name) + (e.size != null ? '<br><span class="dim">' + size(e.size) + '</span>' : '') + '</div>';
        h += '<div class="acts"><button data-act="rn" data-path="' + p + '" data-name="' + esc(e.name) + '">Rename</button>' +
          '<button class="del" data-act="rm" data-path="' + p + '" data-kind="' + (e.type === 'dir' ? 'folder' : 'file') + '">Delete</button></div></div>';
      }
      list.innerHTML = h + '</div>';
      list.querySelectorAll('img[data-path]').forEach(img => img.addEventListener('error', () => this.noThumb(img)));
    }
    async click(e) {
      const g = e.target.closest('[data-go]');
      if (g) { e.preventDefault(); this.go(g.dataset.go); return; }
      const b = e.target.closest('[data-act]');
      if (!b) return;
      const act = b.dataset.act;
      if (act === 'up') this.picker.click();
      else if (act === 'mkdir') {
        const name = prompt('Folder name:'); if (!name) return;
        const r = await post('/library/mkdir', { path: this.cwd, name });
        if (!r.ok) { this.note('err', r.d.error || 'Could not make the folder'); return; }
        this.note('ok', 'Folder made.'); this.go(this.cwd); this.onChange();
      } else if (act === 'rn') {
        const name = prompt('New name:', b.dataset.name); if (!name || name === b.dataset.name) return;
        const r = await post('/library/rename', { path: b.dataset.path, name });
        if (!r.ok) { this.note('err', r.d.error || 'Rename failed'); return; }
        this.note('ok', 'Renamed.'); this.go(this.cwd); this.onChange();
      } else if (act === 'rm') {
        const kind = b.dataset.kind;
        if (!confirm('Delete this ' + kind + '?' + (kind === 'folder' ? '\n\nEverything inside it is deleted too.' : ''))) return;
        const r = await post('/library/delete', { path: b.dataset.path });
        if (!r.ok) { this.note('err', r.d.error || 'Delete failed'); return; }
        this.note('ok', 'Deleted.'); this.go(this.cwd); this.onChange();
      }
    }
    // An image without a thumbnail (copied onto the card elsewhere): fetch
    // it once, make one here and store it on the sign. One at a time.
    noThumb(img) {
      if (img.dataset.tried) { img.replaceWith(document.createTextNode('\u{1F4F7}')); return; }
      img.dataset.tried = '1'; this.thumbQueue.push(img); this.pumpThumbs();
    }
    async pumpThumbs() {
      if (this.thumbBusy) return;
      this.thumbBusy = true;
      while (this.thumbQueue.length) {
        const img = this.thumbQueue.shift(); if (!img.isConnected) continue;
        while (this.uploading) await new Promise(r => setTimeout(r, 1000));
        const path = decodeURIComponent(img.dataset.path), dir = path.slice(0, path.lastIndexOf('/')), name = path.split('/').pop();
        let ok = false;
        try {
          const r = await fetch('/library/download?path=' + encodeURIComponent(path));
          if (r.ok) { const t = await makeThumb(await r.blob()); if (t) ok = (await putInPieces(dir, name, t, null, true)).ok; }
        } catch (e) {}
        if (ok) img.src = '/library/thumb?path=' + encodeURIComponent(path) + '&v=' + Date.now();
        else img.replaceWith(document.createTextNode('\u{1F4F7}'));
      }
      this.thumbBusy = false;
    }
    async upload(items) {
      this.pending.push(...items);
      if (this.uploading) return;
      this.uploading = true;
      while (this.pending.length) await this.runUploads(this.pending.splice(0));
      this.uploading = false;
      this.go(this.cwd); this.onChange();
    }
    async runUploads(items) {
      if (!items.length) return;
      const prog = this.q('.prog'), bar = prog.firstChild, txt = this.q('.progtxt'), upBtn = this.q('[data-act=up]');
      prog.style.display = txt.style.display = 'block'; bar.style.width = '0'; upBtn.disabled = true;
      let saved = 0, skipped = [], failed = []; const made = new Set();
      for (let i = 0; i < items.length; i++) {
        const f = items[i].file, dir = items[i].dir, base = i / items.length;
        // folders first, one level at a time (an existing folder is fine)
        let acc = this.cwd;
        for (const part of dir.slice(this.cwd.length).split('/').filter(Boolean)) {
          const parent = acc; acc = acc + '/' + part;
          if (!made.has(acc)) { made.add(acc); await post('/library/mkdir', { path: parent, name: part }).catch(() => {}); }
        }
        txt.textContent = 'Uploading ' + (i + 1) + ' of ' + items.length + ': ' + f.name;
        let res = null;
        try {
          let name = f.name, type = f.type, buf = null, thumb = null;
          const shrink = this.photos && this.q('.shrink') && this.q('.shrink').checked;
          if (shrink && isImage(f)) {
            txt.textContent = 'Shrinking ' + f.name + '...';
            const sp = await shrinkPhoto(f);
            if (sp) { name = sp.name; type = 'image/jpeg'; buf = await sp.main.arrayBuffer(); thumb = sp.thumb; }
          }
          if (!buf) { buf = await readFile(f); if (isImage(f)) thumb = await makeThumb(f); }
          txt.textContent = 'Uploading ' + (i + 1) + ' of ' + items.length + ': ' + name;
          res = await putInPieces(dir, name, new Blob([buf], { type: type || 'application/octet-stream' }), fr => bar.style.width = ((base + fr / items.length) * 100) + '%');
          if (res.ok && thumb && res.data.saved && res.data.saved[0])   // its thumbnail, under the name it was saved as
            await putInPieces(dir, res.data.saved[0].split('/').pop(), thumb, null, true);
        } catch (e) { res = { ok: false, data: { error: 'unreadable' }, status: 0 }; }
        if (res.ok) { saved += (res.data.saved || []).length; skipped = skipped.concat(res.data.skipped || []); }
        else failed.push(f.name + ' (' + (res.data.error || res.status) + ')');
        bar.style.width = (((i + 1) / items.length) * 100) + '%';
      }
      prog.style.display = txt.style.display = 'none'; upBtn.disabled = false;
      const parts = [];
      if (saved) parts.push(saved + ' file' + (saved === 1 ? '' : 's') + ' uploaded.');
      if (skipped.length) parts.push('Skipped ' + skipped.length + ' (names starting with a dot are not allowed).');
      if (failed.length) parts.push('Failed: ' + failed.join(', ') + '.');
      this.note(failed.length ? 'err' : 'ok', parts.join(' ') || 'Nothing to upload.');
    }
  }
  window.FileManager = FileManager;
  window.fbUpload = { putInPieces, signBack, loadImage };
})();
