// こもっち 共通処理（本サイト index.html と 編集ページ の両方が読み込む）
// ファーム ble_match_atom.ino v3 と合わせてある
(function () {
  const SERVICE_UUID = 'b5f3a1c0-4d2e-4f8a-9c1b-2a7e3d5f6a01';
  const CONFIG_UUID  = 'b5f3a1c1-4d2e-4f8a-9c1b-2a7e3d5f6a01';
  const STATUS_UUID  = 'b5f3a1c2-4d2e-4f8a-9c1b-2a7e3d5f6a01';
  const DEBUG_UUID   = 'b5f3a1c3-4d2e-4f8a-9c1b-2a7e3d5f6a01';
  const NQ = 8;        // バッジに送れる枠の数（ファームの NQ）
  const LED_NUM = 24;  // リングのLED数（ファームの LED_NUM）
  const CHUNK = 18;    // 1回の書き込みサイズ（テストプログラムで実績のある値）

  // 色番号（ファームの colorOf と同じ並び）
  const PALETTE = {
    1: { name: '赤', hex: '#ff3b3b' }, 2: { name: '緑', hex: '#34d058' },
    3: { name: '青', hex: '#3b82ff' }, 4: { name: '黄', hex: '#ffc400' },
    5: { name: '紫', hex: '#a64dff' }, 6: { name: '水色', hex: '#22c8ff' },
    7: { name: '白', hex: '#ffffff' }, 8: { name: '橙', hex: '#ff7a1a' },
  };

  // ---------- データ ----------
  // 選択肢は "文字" でも {label, color, leds} でもよい → すべてオブジェクトにそろえる
  function normalize(data) {
    data.questions = (data.questions || []).map((q, qi) => ({
      id: q.id || 'q' + (qi + 1),
      type: q.type === 'multi' ? 'multi' : 'single',
      max: q.max || 0,
      title: Array.isArray(q.title) ? q.title : String(q.title || '').split('\n'),
      hint: q.hint || '',
      summary: q.summary || null,
      color: q.color || 7,
      leds: q.leds || [],
      choices: (q.choices || []).map(c => typeof c === 'string' ? { label: c } : { ...c }),
    }));
    data.ring = Object.assign({ startAngle: 0, clockwise: true }, data.ring || {});
    data.device = Object.assign({ label: 'こもっち', prefixes: ['KOMOCCHI', 'MyDevice'] }, data.device || {});
    return data;
  }

  // 質問 → バッジの枠。single: 1問=1枠（回答=番号+1） / multi: 選択肢1つ=1枠（選んだら1）
  function buildSlots(data) {
    const slots = [];
    let error = '';
    data.questions.forEach((q, qi) => {
      if (q.type === 'multi') q.choices.forEach((_, ci) => slots.push({ qi, ci }));
      else slots.push({ qi, ci: null });
      if (q.type === 'single' && q.choices.length > 15) error = `Q${qi + 1} の選択肢が15個を超えています`;
      if (!q.choices.length) error = `Q${qi + 1} に選択肢がありません`;
    });
    if (!data.questions.length) error = '質問が1つもありません';
    if (slots.length > NQ) error = `バッジの枠が ${slots.length} 個必要です（上限 ${NQ}）`;
    return { slots, error };
  }

  // 枠ごとの「光らせるLED」と「色」
  //   single: 選んだ選択肢の leds（なければ質問の leds）、選んだ選択肢の color（なければ質問の color）
  //   multi : その選択肢の leds / color（なければ質問の値）
  function slotLook(data, slot, chosen) {
    const q = data.questions[slot.qi];
    const ci = slot.ci != null ? slot.ci : chosen;
    const c = ci != null ? q.choices[ci] || {} : {};
    return {
      leds: (c.leds && c.leds.length ? c.leds : q.leds) || [],
      color: c.color || q.color || 7,
    };
  }

  // answers: { [質問id]: 番号 | [番号...] } → 各枠の値(0〜15)
  function nibbles(data, slots, answers) {
    return slots.map(s => {
      const q = data.questions[s.qi];
      const a = answers[q.id];
      if (q.type === 'multi') return Array.isArray(a) && a.includes(s.ci) ? 1 : 0;
      return typeof a === 'number' ? a + 1 : 0;
    });
  }

  const maskHex = leds => {
    let m = 0n;
    for (const n of leds) if (n >= 0 && n < LED_NUM) m |= 1n << BigInt(n);
    return m.toString(16).padStart(10, '0');
  };

  // ファームの parseConfig 形式: v=2;a=<8桁>;c=<8桁>;m=<10桁×8>
  function encodeConfig(data, slots, answers) {
    const nib = nibbles(data, slots, answers);
    let a = '', c = '', m = '';
    for (let s = 0; s < NQ; s++) {
      if (!slots[s] || !nib[s]) { a += '0'; c += '0'; m += '0'.repeat(10); continue; }
      const q = data.questions[slots[s].qi];
      const look = slotLook(data, slots[s], q.type === 'single' ? answers[q.id] : null);
      a += nib[s].toString(16);
      c += look.color.toString(16);
      m += maskHex(look.leds);
    }
    return `v=2;a=${a};c=${c};m=${m}`;
  }

  // 仮想の相手（sim=<8桁>）
  function encodeSim(data, slots, answers) {
    const nib = nibbles(data, slots, answers);
    return 'sim=' + Array.from({ length: NQ }, (_, s) => (nib[s] || 0).toString(16)).join('');
  }

  // "a=XXXXXXXX" → answers
  function decodeAnswers(data, slots, text) {
    const mt = /a=([0-9a-fA-F]{8})/.exec(text || '');
    if (!mt) return null;
    const nib = [...mt[1]].map(ch => parseInt(ch, 16));
    const answers = {};
    slots.forEach((s, i) => {
      const q = data.questions[s.qi];
      if (q.type === 'multi') {
        answers[q.id] = answers[q.id] || [];
        if (nib[i] === 1) answers[q.id].push(s.ci);
      } else if (nib[i] > 0 && nib[i] <= q.choices.length) answers[q.id] = nib[i] - 1;
    });
    return answers;
  }

  // 全問一致したときの光り方（LED番号 → 色）
  function previewFills(data, slots, answers) {
    const fills = new Array(LED_NUM).fill(null);
    const nib = nibbles(data, slots, answers);
    slots.forEach((s, i) => {
      if (!nib[i]) return;
      const q = data.questions[s.qi];
      const look = slotLook(data, s, q.type === 'single' ? answers[q.id] : null);
      look.leds.forEach(n => { if (n >= 0 && n < LED_NUM) fills[n] = PALETTE[look.color].hex; });
    });
    return fills;
  }

  // 24個を枠の数で均等に分けて、連続した区間を割り当てる
  function autoAssign(data) {
    const { slots } = buildSlots(data);
    if (!slots.length) return;
    slots.forEach((s, i) => {
      const from = Math.round(i * LED_NUM / slots.length);
      const to = Math.round((i + 1) * LED_NUM / slots.length);
      const leds = Array.from({ length: to - from }, (_, k) => from + k);
      const q = data.questions[s.qi];
      if (q.type === 'multi') q.choices[s.ci].leds = leds;
      else { q.leds = leds; q.choices.forEach(c => delete c.leds); }
    });
  }

  // ---------- 絵 ----------
  function ledXY(ring, i, r, cx, cy) {
    const deg = ring.startAngle + (ring.clockwise ? 1 : -1) * i * 360 / LED_NUM;
    const rad = deg * Math.PI / 180;
    return [cx + Math.sin(rad) * r, cy - Math.cos(rad) * r];
  }

  // ロゴ: 2つの弧と2つの円
  let uid = 0;
  function logoSVG(cls = '', title = '') {
    const id = 'lg' + (++uid);
    return `<svg class="${cls}" viewBox="0 0 200 160" ${title ? `role="img" aria-label="${title}"` : 'aria-hidden="true"'}>
      <defs>
        <linearGradient id="${id}a" x1="0" y1="0" x2="1" y2="0"><stop offset="0" stop-color="#ff7448"/><stop offset="1" stop-color="#fb9a4c"/></linearGradient>
        <linearGradient id="${id}b" x1="0" y1="0" x2="1" y2="0"><stop offset="0" stop-color="#fbaa4a"/><stop offset="1" stop-color="#fbc145"/></linearGradient>
      </defs>
      <path d="M20 56 A68 68 0 0 1 130 56" fill="none" stroke="url(#${id}a)" stroke-width="20" stroke-linecap="round"/>
      <path d="M76 88 A54 54 0 0 0 182 88" fill="none" stroke="url(#${id}b)" stroke-width="20" stroke-linecap="round"/>
      <circle cx="33" cy="90" r="17" fill="#f2866d"/>
      <circle cx="166" cy="50" r="17" fill="#fbc145"/>
    </svg>`;
  }

  // バッジ本体（24個のLEDリング）。fills: LED番号ごとの色（null=消灯）
  function badgeSVG(ring, fills, cls = '') {
    const id = 'bd' + (++uid);
    const dots = Array.from({ length: LED_NUM }, (_, i) => {
      const [x, y] = ledXY(ring, i, 66, 100, 100);
      const c = fills && fills[i];
      return (c ? `<circle cx="${x.toFixed(1)}" cy="${y.toFixed(1)}" r="8" fill="${c}" opacity=".55" filter="url(#${id}g)"/>` : '') +
        `<circle cx="${x.toFixed(1)}" cy="${y.toFixed(1)}" r="5.5" fill="${c || '#ddd6ca'}"/>`;
    }).join('');
    return `<svg class="${cls}" viewBox="0 0 200 200" aria-hidden="true">
      <defs>
        <radialGradient id="${id}b" cx="40%" cy="35%" r="70%"><stop offset="0" stop-color="#fdfbf8"/><stop offset="1" stop-color="#eee8de"/></radialGradient>
        <filter id="${id}s" x="-30%" y="-30%" width="160%" height="170%"><feDropShadow dx="0" dy="8" stdDeviation="8" flood-color="#6f5d3f" flood-opacity=".18"/></filter>
        <filter id="${id}g" x="-100%" y="-100%" width="300%" height="300%"><feGaussianBlur stdDeviation="3"/></filter>
      </defs>
      <circle cx="100" cy="100" r="88" fill="url(#${id}b)" stroke="#e6e0d5" filter="url(#${id}s)"/>
      ${dots}
      <g transform="translate(64 72) scale(.36)" opacity=".9">${logoSVG().replace(/^<svg[^>]*>|<\/svg>\s*$/g, '')}</g>
    </svg>`;
  }

  // ---------- BLE ----------
  const hasBluetooth = () => !!(navigator.bluetooth && navigator.bluetooth.requestDevice);
  const enc = new TextEncoder(), dec = new TextDecoder();

  async function connectBadge(data, onDisconnect) {
    const filters = (data.device.prefixes || []).map(p => ({ namePrefix: p }));
    filters.push({ services: [SERVICE_UUID] });
    const device = await navigator.bluetooth.requestDevice({ filters, optionalServices: [SERVICE_UUID] });
    if (onDisconnect) device.addEventListener('gattserverdisconnected', onDisconnect);
    const server = await device.gatt.connect();
    const svc = await server.getPrimaryService(SERVICE_UUID);
    const cfg = await svc.getCharacteristic(CONFIG_UUID);
    const sts = await svc.getCharacteristic(STATUS_UUID);
    let dbg = null;
    try { dbg = await svc.getCharacteristic(DEBUG_UUID); } catch (e) {}
    await sts.startNotifications();
    const badge = {
      device, cfg, sts, dbg,
      name: device.name || '',
      // "!"〜"#" で分割送信し、バッジの返事（OK/ERR/SIM…）を待つ
      async send(text, timeout = 6000) {
        const reply = new Promise((resolve, reject) => {
          const t = setTimeout(() => reject(new Error('timeout')), timeout);
          sts.addEventListener('characteristicvaluechanged', e => {
            clearTimeout(t); resolve(dec.decode(e.target.value));
          }, { once: true });
        });
        const bytes = enc.encode('!' + text + '#');
        for (let i = 0; i < bytes.length; i += CHUNK) {
          const part = bytes.slice(i, i + CHUNK);
          if (cfg.writeValueWithResponse) await cfg.writeValueWithResponse(part);
          else await cfg.writeValue(part);
        }
        return reply;
      },
      async readAnswers() { return dec.decode(await cfg.readValue()); },
      async readDebug() { return dbg ? dec.decode(await dbg.readValue()) : ''; },
      disconnect() { try { device.gatt.connected && device.gatt.disconnect(); } catch (e) {} },
    };
    return badge;
  }

  function bleError(e) {
    if (e && e.name === 'NotFoundError') return 'バッジが選ばれませんでした。もう一度ボタンを押してください。';
    if (e && e.message === 'timeout') return 'バッジから返事がありません。近づけてもう一度お試しください。';
    if (e && String(e.message).startsWith('badge:')) return 'バッジが受け取れませんでした。もう一度お試しください。';
    return '接続できませんでした。バッジの電源とBluetoothを確認してください。';
  }

  const esc = s => String(s).replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));

  window.K = {
    NQ, LED_NUM, PALETTE, normalize, buildSlots, slotLook, encodeConfig, encodeSim, decodeAnswers,
    previewFills, autoAssign, ledXY, logoSVG, badgeSVG, hasBluetooth, connectBadge, bleError, esc,
  };
})();
