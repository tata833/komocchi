// M5Stack AtomS3 Lite + WS2812B 24連リング — 近接マッチ版 v4
//
// ・スマホ(Web/アプリ)から「各問題の回答」「一致時に光らせるLED・色」「イベント設定」を登録
// ・同じイベントのAtom同士で「互いに最寄り」になった相手とだけ照合
// ・1問でも同じ回答があれば、その問題に対応するLEDをその回答の色で点灯
// ・通常時はリング消灯
//
// v4 で追加したこと（中間発表の講評を受けた改良）
// ・イベントID   : 同じイベントのバッジ同士だけ照合する（別イベントとは反応しない）
// ・話しかけOK/NG: ボタン長押し(1〜5秒)で切替。NG中は照合せず、回答も発信しない
// ・気配         : 遠めに一致する人がいると、リング全体が弱く呼吸するように光る
// ・合図と同期   : 照合中に二人ともボタンを短く押すと、両方のリングが回って全点灯する
// ・グループモード: 最寄り1台ではなく、範囲内の全員との一致をまとめて表示する
// ・距離の閾値・明るさ・グループモードをWeb(questions.json)から登録できる
//
// ボタン: 短く押す=合図 / 1〜5秒長押し=話しかけOK⇔NG / 5秒以上=登録を消去
//
// 仕組み: 各Atomが「イベントID」「状態フラグ」「自分の回答」「自分にとって最寄りの相手のID」を
//         アドバタイズに載せて発信。相手の発信に自分のIDがあれば「互いに最寄り」
//
// ボード: "M5AtomS3"（または ESP32S3 Dev Module） / Tools > USB CDC On Boot: Enabled
// ライブラリ: FastLED
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <Preferences.h>
#include <FastLED.h>

// ===== 台ごとに変える =====
#define DEVICE_NAME      "MyDevice-01"   // 2台目は "MyDevice-02"。11文字以内

// ===== 共通設定（全台同じ）=====
#define SERVICE_UUID     "b5f3a1c0-4d2e-4f8a-9c1b-2a7e3d5f6a01"
#define CONFIG_CHAR_UUID "b5f3a1c1-4d2e-4f8a-9c1b-2a7e3d5f6a01"
#define STATUS_CHAR_UUID "b5f3a1c2-4d2e-4f8a-9c1b-2a7e3d5f6a01"
#define DEBUG_CHAR_UUID  "b5f3a1c3-4d2e-4f8a-9c1b-2a7e3d5f6a01"   // デバッグ情報（読み出し）
#define SIM_MS           30000   // 仮想の相手を有効にしておく時間
#define NQ               8      // 問題数の上限（Web側の問題はこれ以下にする）

// ===== 近接判定の初期値（Webから t=近く,切れる,気配 で上書きできる）=====
#define DEF_NEAR      61    // -61dBm より強いと「範囲内」
#define DEF_EXIT      76    // -76dBm より弱くなると「範囲外」（NEARとの差でチラつき防止）
#define DEF_AURA      85    // -85dBm より強い一致者がいると「気配」
#define DEF_BRIGHT    60
#define HOLD_MS       1500  // 一致が途切れてもこの時間は点灯を保つ（瞬断対策）
#define FADE_OUT_MS   500   // 保持時間が過ぎたらこの時間で消える
#define SWITCH_MARGIN 6     // 最寄りを乗り換えるのに必要な差(dB)。チラつき防止
#define LOST_MS       3000  // この時間届かなければ消えた扱い
#define MAX_PEERS     12
#define WAVE_MS       10000 // 合図の有効時間（この間に相手も押せば同期）
#define SYNC_MS       2000  // 同期演出の長さ
#define OKFLASH_MS    900   // OK/NG切替の表示時間

// ===== ハード =====
#define LED_PIN     2    // GroveのG2（黄線）→ リングのDI。光らなければ 1 に変更
#define LED_NUM     24   // リングのLED数（40個まで。それ以上はデータ形式の拡張が必要）
#define STATUS_PIN  35
#define BTN_PIN     41
#define MAX_MA      400  // USB給電向けの電流制限（24個全灯で最大約1.4A流れるため）

const uint16_t COMPANY_ID = 0xFFFF;
const uint8_t  MAGIC0 = 'H', MAGIC1 = 'X', PROTO = 3;
const uint64_t ALL_LEDS = (1ULL << LED_NUM) - 1;
const uint8_t  FLAG_OK = 0x01, FLAG_WAVE = 0x02, FLAG_GROUP = 0x04;
#define ADV_LEN 15   // cid(2) magic(2) proto(1) event(2) flags(1) ans(4) nearest(3)

CRGB hexLeds[LED_NUM];
CRGB statusLed[1];

Preferences prefs;
BLECharacteristic *configChar;
BLECharacteristic *statusChar;
BLECharacteristic *debugChar;
BLEScan *scanner;

// ---- 自分の登録内容 ----
uint8_t  answers[NQ];   // 0=未回答, 1〜15=選んだ選択肢
uint8_t  qcolor[NQ];    // 一致時の色（パレット番号）
uint64_t qmask[NQ];     // 一致時に光らせるLED
uint8_t  myId[3];       // 自分のBLEアドレス下位3バイト
uint16_t eventHash = 0; // イベントID（Webでハッシュ化した16bit）
int      nearRssi = -DEF_NEAR, exitRssi = -DEF_EXIT, auraRssi = -DEF_AURA;
uint8_t  bright = DEF_BRIGHT;
bool     groupMode = false;

// ---- 状態 ----
bool talkOk = true;               // 話しかけOK（本体に保存）
unsigned long waveUntil = 0;      // 合図中
unsigned long syncUntil = 0;      // 同期演出中
unsigned long okFlashUntil = 0;   // OK/NG切替の表示中
bool syncFired = false;           // 今回の合図で同期済み
uint8_t heldAns[4];               // 一致表示に使う相手の回答（途切れても保持）

volatile bool phoneConnected = false;
volatile bool scanDone = true;

// スマホからの受信（分割送信を組み立てる。固定長バッファで受ける）
#define RX_MAX 400
char rxBuf[RX_MAX + 1];
int rxLen = 0;
char pendingBuf[RX_MAX + 1];
volatile bool hasPending = false;
volatile bool rxOverflow = false;
portMUX_TYPE rxMux = portMUX_INITIALIZER_UNLOCKED;

unsigned long flashUntil = 0, previewUntil = 0, matchSince = 0;
unsigned long testUntil = 0;              // LEDテスト（番号順に点灯）

// ---- デバッグ: 仮想の相手（合図済み・同じイベント・OKとして扱う）----
bool simActive = false;
unsigned long simUntil = 0;
uint8_t simAns[4] = {0, 0, 0, 0};
bool dbgMutual = false, dbgMatched = false;
bool lastMatched = false;

// ---- 周囲のAtom（同じイベント・話しかけOKのものだけ記録）----
struct Peer {
  bool used;
  uint8_t addr[6];
  uint8_t flags;
  uint8_t ans[4];        // 回答(4bit×8問)
  uint8_t nearestId[3];  // 相手にとっての最寄り
  float rssi;
  bool inRange;
  uint32_t lastSeen;
};
Peer peers[MAX_PEERS];
portMUX_TYPE peerMux = portMUX_INITIALIZER_UNLOCKED;

bool hasNearest = false;
uint8_t nearestAddr[6];
bool advDirty = true;

// ================= 共通 =================

// Web側の PALETTE と同じ番号
CRGB colorOf(int c) {
  switch (c) {
    case 1: return CRGB(255, 0, 0);     // 赤
    case 2: return CRGB(0, 255, 0);     // 緑
    case 3: return CRGB(0, 0, 255);     // 青
    case 4: return CRGB(255, 180, 0);   // 黄
    case 5: return CRGB(160, 0, 255);   // 紫
    case 6: return CRGB(0, 200, 255);   // 水色
    case 7: return CRGB(255, 255, 255); // 白
    case 8: return CRGB(255, 80, 0);    // 橙
    default: return CRGB(255, 255, 255);
  }
}
const CRGB WARM = CRGB(255, 110, 40);   // 気配・演出の色（ロゴの暖色）

int hexNibble(char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
  if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
  return -1;
}

bool allHex(const String &s) {
  for (unsigned int i = 0; i < s.length(); i++) if (hexNibble(s[i]) < 0) return false;
  return true;
}

String getVal(const String &s, const char *key) {
  String src = ";" + s, k = String(";") + key + "=";
  int i = src.indexOf(k);
  if (i < 0) return "";
  int st = i + k.length(), en = src.indexOf(';', st);
  return src.substring(st, en < 0 ? src.length() : en);
}

uint8_t nibOf(const uint8_t a[4], int q) { return (a[q / 2] >> ((q % 2) ? 0 : 4)) & 0x0F; }
void setNib(uint8_t a[4], int q, uint8_t v) {
  if (q % 2) a[q / 2] = (a[q / 2] & 0xF0) | (v & 0x0F);
  else       a[q / 2] = (a[q / 2] & 0x0F) | (v << 4);
}
bool waving() { return (long)(waveUntil - millis()) > 0; }

// 形式: v=3;e=<4桁>;a=<8桁>;c=<8桁>;m=<10桁×8>;t=<近く>,<切れる>,<気配>;b=<明るさ>;g=<0|1>
//   e: イベントID(16進4桁)  a: 各問の回答(16進1桁, 0=未回答)  c: 各問の色  m: 各問のLED(16進10桁)
//   t: 電波の閾値(dBmの絶対値)  b: 明るさ(5〜255)  g: グループモード
//   v=2（e, t, b, g なし）も受け付ける。足りない値は初期値になる
bool parseConfig(const String &s) {
  String a = getVal(s, "a"), c = getVal(s, "c"), m = getVal(s, "m");
  if (a.length() != NQ || c.length() != NQ || m.length() != NQ * 10) {
    Serial.printf("parse NG: 長さ a=%d c=%d m=%d（正しくは %d %d %d）\n",
                  a.length(), c.length(), m.length(), NQ, NQ, NQ * 10);
    return false;
  }
  if (!allHex(a + c + m)) { Serial.println("parse NG: 16進数でない文字"); return false; }

  uint16_t ev = 0;
  String e = getVal(s, "e");
  if (e.length()) {
    if (e.length() != 4 || !allHex(e)) { Serial.println("parse NG: e は16進4桁"); return false; }
    ev = (uint16_t)strtoul(e.c_str(), nullptr, 16);
  }
  int tn = DEF_NEAR, te = DEF_EXIT, ta = DEF_AURA;
  String t = getVal(s, "t");
  if (t.length() && sscanf(t.c_str(), "%d,%d,%d", &tn, &te, &ta) != 3) {
    Serial.println("parse NG: t は 近く,切れる,気配"); return false;
  }
  tn = constrain(tn, 30, 100);
  te = constrain(te, tn + 3, 105);   // 切れる閾値は近くより弱く
  ta = constrain(ta, te, 110);       // 気配は切れる閾値以上に遠く
  int b = DEF_BRIGHT;
  String bs = getVal(s, "b");
  if (bs.length()) b = constrain(bs.toInt(), 5, 255);
  String gs = getVal(s, "g");

  for (int i = 0; i < NQ; i++) {
    answers[i] = hexNibble(a[i]);
    qcolor[i]  = hexNibble(c[i]);
    qmask[i]   = strtoull(m.substring(i * 10, i * 10 + 10).c_str(), nullptr, 16) & ALL_LEDS;
  }
  eventHash = ev;
  nearRssi = -tn; exitRssi = -te; auraRssi = -ta;
  bright = b;
  groupMode = (gs == "1");
  FastLED.setBrightness(bright);
  return true;
}

String emptyConfig() {
  String s = "v=3;e=0000;a=";
  for (int i = 0; i < NQ; i++) s += "0";
  s += ";c=";
  for (int i = 0; i < NQ; i++) s += "0";
  s += ";m=";
  for (int i = 0; i < NQ * 10; i++) s += "0";
  return s;
}

// スマホが読み出す値（回答だけ。画面の復元に使う）
void updateReadValue() {
  String a = "a=";
  for (int i = 0; i < NQ; i++) a += String(answers[i], HEX);
  configChar->setValue(a.c_str());
}

// ================= アドバタイズ =================
uint8_t myFlags() {
  return (talkOk ? FLAG_OK : 0) | (talkOk && waving() ? FLAG_WAVE : 0) | (groupMode ? FLAG_GROUP : 0);
}

void updateAdvertising() {
  uint8_t d[ADV_LEN];
  d[0] = COMPANY_ID & 0xFF;
  d[1] = COMPANY_ID >> 8;
  d[2] = MAGIC0; d[3] = MAGIC1; d[4] = PROTO;
  d[5] = eventHash & 0xFF; d[6] = eventHash >> 8;
  d[7] = myFlags();
  // NG中は回答を発信しない（情報開示のリスク対策）
  for (int k = 0; k < 4; k++) d[8 + k] = talkOk ? ((answers[2 * k] << 4) | (answers[2 * k + 1] & 0x0F)) : 0;
  if (talkOk && hasNearest) memcpy(&d[12], &nearestAddr[3], 3);
  else memset(&d[12], 0, 3);

  BLEAdvertisementData adv;
  adv.setFlags(0x06);
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  String md; md.concat((const char *)d, sizeof(d));
  adv.setManufacturerData(md);
#else
  adv.setManufacturerData(std::string((const char *)d, sizeof(d)));
#endif
  // 名前はスキャン応答側へ（アドバタイズ本体31バイトに収めるため）
  BLEAdvertisementData resp;
  resp.setCompleteServices(BLEUUID(SERVICE_UUID));
  resp.setName(DEVICE_NAME);

  BLEAdvertising *a = BLEDevice::getAdvertising();
  a->stop();
  a->setAdvertisementData(adv);
  a->setScanResponseData(resp);
  a->start();
}

void saveAndApply(const String &cfg) {
  prefs.putString("config", cfg);
  updateReadValue();
  advDirty = true;
  flashUntil   = millis() + 600;
  previewUntil = millis() + 2400;
  Serial.println("saved: " + cfg);
}

// ================= スキャン =================
class ScanCB : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice dev) override {
    if (!dev.haveManufacturerData()) return;
    auto md = dev.getManufacturerData();
    if (md.length() < ADV_LEN) return;
    const uint8_t *d = (const uint8_t *)md.c_str();
    if (d[0] != (COMPANY_ID & 0xFF) || d[1] != (COMPANY_ID >> 8) ||
        d[2] != MAGIC0 || d[3] != MAGIC1 || d[4] != PROTO) return;
    uint16_t ev = d[5] | (d[6] << 8);
    if (ev != eventHash) return;           // 別イベントのバッジは無視
    if (!(d[7] & FLAG_OK)) return;         // 話しかけNGのバッジは無視

    int rssi = dev.getRSSI();
    BLEAddress address = dev.getAddress();
    const uint8_t *a = (const uint8_t *)address.getNative();
    uint32_t now = millis();

    portENTER_CRITICAL(&peerMux);
    int idx = -1, freeIdx = -1, oldest = 0;
    for (int i = 0; i < MAX_PEERS; i++) {
      if (peers[i].used && memcmp(peers[i].addr, a, 6) == 0) { idx = i; break; }
      if (!peers[i].used && freeIdx < 0) freeIdx = i;
      if (peers[i].lastSeen < peers[oldest].lastSeen) oldest = i;
    }
    if (idx < 0) {
      idx = (freeIdx >= 0) ? freeIdx : oldest;
      peers[idx].used = true;
      memcpy(peers[idx].addr, a, 6);
      peers[idx].rssi = rssi;
      peers[idx].inRange = false;
    }
    Peer &p = peers[idx];
    p.flags = d[7];
    memcpy(p.ans, &d[8], 4);
    memcpy(p.nearestId, &d[12], 3);
    p.rssi = p.rssi * 0.8f + rssi * 0.2f;   // ゆらぎを平滑化（大きいほどなめらか・反応は遅め）
    p.lastSeen = now;
    if (!p.inRange && p.rssi > nearRssi) p.inRange = true;
    else if (p.inRange && p.rssi < exitRssi) p.inRange = false;
    portEXIT_CRITICAL(&peerMux);
  }
};

void onScanComplete(BLEScanResults results) { scanDone = true; }

void keepScanning() {
  if (!scanDone) return;
  scanDone = false;
  scanner->clearResults();
  scanner->start(2, onScanComplete, false);
}

bool anyMatch(const uint8_t peerAns[4]) {
  for (int q = 0; q < NQ; q++) {
    if (answers[q] == 0) continue;
    if (nibOf(peerAns, q) == answers[q]) return true;
  }
  return false;
}

// 最寄りの相手を決め、「互いに最寄り」かどうかを判定
// 戻り値: 互いに最寄りの相手が見つかったら true（その回答を peerAns、状態を peerFlags に入れる）
bool updatePairing(uint8_t peerAns[4], bool &oneSided, uint8_t &peerFlags) {
  uint32_t now = millis();
  bool mutual = false;
  oneSided = false;
  peerFlags = 0;

  portENTER_CRITICAL(&peerMux);
  int cur = -1, best = -1;
  for (int i = 0; i < MAX_PEERS; i++) {
    Peer &p = peers[i];
    if (!p.used) continue;
    if (now - p.lastSeen > LOST_MS) { p.used = false; continue; }
    if (!p.inRange || !talkOk) continue;
    if (hasNearest && memcmp(p.addr, nearestAddr, 6) == 0) cur = i;
    if (best < 0 || p.rssi > peers[best].rssi) best = i;
  }
  // 今の相手が範囲内なら、十分に強い相手が現れたときだけ乗り換える
  int chosen = best;
  if (cur >= 0 && best >= 0 && best != cur && peers[best].rssi < peers[cur].rssi + SWITCH_MARGIN) chosen = cur;

  bool changed;
  if (chosen < 0) {
    changed = hasNearest;
    hasNearest = false;
  } else {
    changed = !hasNearest || memcmp(nearestAddr, peers[chosen].addr, 6) != 0;
    hasNearest = true;
    memcpy(nearestAddr, peers[chosen].addr, 6);
    oneSided = true;
    if (memcmp(peers[chosen].nearestId, myId, 3) == 0) {   // 相手の最寄りも自分
      mutual = true;
      oneSided = false;
      memcpy(peerAns, peers[chosen].ans, 4);
      peerFlags = peers[chosen].flags;
    }
  }
  portEXIT_CRITICAL(&peerMux);

  if (changed) advDirty = true;
  return mutual;
}

// グループモード: 範囲内の全員との一致をまとめる
// outAns には「誰かと一致した問題だけ自分の回答、それ以外は0」を入れる（drawMasks にそのまま渡せる形）
bool groupMatch(uint8_t outAns[4], bool &peerWave) {
  memset(outAns, 0, 4);
  peerWave = false;
  bool any = false;
  uint32_t now = millis();
  portENTER_CRITICAL(&peerMux);
  for (int i = 0; i < MAX_PEERS; i++) {
    Peer &p = peers[i];
    if (!p.used || !p.inRange || now - p.lastSeen > LOST_MS) continue;
    bool hit = false;
    for (int q = 0; q < NQ; q++) {
      if (answers[q] && nibOf(p.ans, q) == answers[q]) { setNib(outAns, q, answers[q]); hit = true; }
    }
    if (hit) { any = true; if (p.flags & FLAG_WAVE) peerWave = true; }
  }
  portEXIT_CRITICAL(&peerMux);
  return any;
}

// 気配: 「範囲内」ほど近くないが、気配の閾値より近くに一致する人がいる
bool auraNearby() {
  if (!talkOk) return false;
  bool found = false;
  uint32_t now = millis();
  portENTER_CRITICAL(&peerMux);
  for (int i = 0; i < MAX_PEERS && !found; i++) {
    Peer &p = peers[i];
    if (!p.used || now - p.lastSeen > LOST_MS) continue;
    if (p.rssi > auraRssi && anyMatch(p.ans)) found = true;
  }
  portEXIT_CRITICAL(&peerMux);
  return found;
}

// ================= スマホとの接続（登録用）=================
class ServerCB : public BLEServerCallbacks {
  void onConnect(BLEServer *s) override {
    phoneConnected = true;
    BLEDevice::startAdvertising();
  }
  void onDisconnect(BLEServer *s) override {
    phoneConnected = false;
    BLEDevice::startAdvertising();
  }
};

// 分割送信の受信: "!" で開始、"#" で終了
class ConfigCB : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *c) override {
    auto val = c->getValue();
    const char *p = val.c_str();
    int n = val.length();
    portENTER_CRITICAL(&rxMux);
    for (int i = 0; i < n; i++) {
      char ch = p[i];
      if (ch == '!') { rxLen = 0; continue; }            // 送信開始
      if (ch == '#') {                                    // 送信終了
        memcpy(pendingBuf, rxBuf, rxLen);
        pendingBuf[rxLen] = 0;
        rxLen = 0;
        hasPending = true;
        continue;
      }
      if (rxLen < RX_MAX) rxBuf[rxLen++] = ch;
      else rxOverflow = true;
    }
    portEXIT_CRITICAL(&rxMux);
  }
};

// LEDテスト: 0番を赤、以降を白で番号順に点灯 → 登録内容を表示
void startLedTest() {
  testUntil = millis() + LED_NUM * 80 + 300;
  previewUntil = testUntil + 2400;
}

// 合図（ボタン短押し）。NG中は合図できないことを赤の点滅で知らせる
void startWave() {
  if (!talkOk) { okFlashUntil = millis() + OKFLASH_MS; return; }
  waveUntil = millis() + WAVE_MS;
  advDirty = true;
}

// 話しかけOK⇔NG（ボタン長押し）
void toggleTalkOk() {
  talkOk = !talkOk;
  prefs.putBool("ok", talkOk);
  waveUntil = 0;
  okFlashUntil = millis() + OKFLASH_MS;
  advDirty = true;
  if (phoneConnected) { statusChar->setValue(talkOk ? "OKON" : "OKOFF"); statusChar->notify(); }
  Serial.println(talkOk ? "話しかけOK" : "話しかけNG");
}

void handlePending() {
  if (rxOverflow) { rxOverflow = false; Serial.println("recv NG: データが長すぎます"); }
  if (!hasPending) return;
  static char local[RX_MAX + 1];
  portENTER_CRITICAL(&rxMux);
  hasPending = false;
  memcpy(local, pendingBuf, sizeof(local));
  portEXIT_CRITICAL(&rxMux);
  String cfg = String(local);

  Serial.println("recv: " + cfg);

  // ---- デバッグ用コマンド ----
  if (cfg.startsWith("sim=")) {          // 仮想の相手: sim=<回答8桁>
    String a = cfg.substring(4);
    if (a.length() == NQ && allHex(a)) {
      for (int k = 0; k < 4; k++) simAns[k] = (hexNibble(a[2 * k]) << 4) | hexNibble(a[2 * k + 1]);
      simActive = true;
      simUntil = millis() + SIM_MS;
      statusChar->setValue("SIM");
    } else {
      statusChar->setValue("ERR");
    }
    statusChar->notify();
    return;
  }
  if (cfg == "simoff")   { simActive = false; statusChar->setValue("SIMOFF"); statusChar->notify(); return; }
  if (cfg == "test")     { startLedTest(); statusChar->setValue("TEST"); statusChar->notify(); return; }
  if (cfg == "wave")     { startWave(); statusChar->setValue(talkOk ? "WAVE" : "NG"); statusChar->notify(); return; }
  if (cfg == "toggleok") { toggleTalkOk(); return; }   // 返事は toggleTalkOk が送る

  if (parseConfig(cfg)) {
    saveAndApply(cfg);
    statusChar->setValue("OK");
  } else {
    parseConfig(prefs.getString("config", emptyConfig()));
    statusChar->setValue("ERR");
  }
  statusChar->notify();
}

// ================= 表示 =================
void drawMasks(const uint8_t peerAns[4], bool onlyMatched, uint8_t level) {
  fill_solid(hexLeds, LED_NUM, CRGB::Black);
  for (int q = 0; q < NQ; q++) {
    if (answers[q] == 0) continue;
    if (onlyMatched && nibOf(peerAns, q) != answers[q]) continue;   // この問題は不一致
    CRGB col = colorOf(qcolor[q]);
    col.nscale8(level);
    for (int i = 0; i < LED_NUM; i++) if ((qmask[q] >> i) & 1) hexLeds[i] = col;
  }
}

// 同期演出: 一致した色の光が2周回って、全体がふわっと点いて消える
void drawSync(unsigned long t) {
  CRGB base[LED_NUM];
  drawMasks(heldAns, true, 255);
  for (int i = 0; i < LED_NUM; i++) base[i] = hexLeds[i] ? hexLeds[i] : WARM;
  const unsigned long SPIN = 1400;
  if (t < SPIN) {
    int head = (int)(t * 2 * LED_NUM / SPIN);
    for (int i = 0; i < LED_NUM; i++) {
      int d = ((head - i) % LED_NUM + LED_NUM) % LED_NUM;
      CRGB c = base[i];
      c.nscale8(d < 8 ? 255 - d * 30 : 0);
      hexLeds[i] = c;
    }
  } else {
    uint8_t lv = 255 - (uint8_t)min<unsigned long>(255, (t - SPIN) * 255 / (SYNC_MS - SPIN));
    for (int i = 0; i < LED_NUM; i++) { CRGB c = base[i]; c.nscale8(lv); hexLeds[i] = c; }
  }
}

void drawHex(bool matched, const uint8_t peerAns[4], bool aura, bool meWave) {
  unsigned long now = millis();
  if (now < testUntil) {    // LEDテスト: 0番=赤、1番から白で順に点灯（向きと配線の確認用）
    int k = (int)((now - (testUntil - LED_NUM * 80 - 300)) / 80);
    for (int i = 0; i < LED_NUM; i++)
      hexLeds[i] = (i > k) ? CRGB::Black : (i == 0 ? CRGB::Red : CRGB(80, 80, 80));
    return;
  }
  if (now < flashUntil) {   // 登録直後: 白い光がリングを一周する
    int lit = (int)((600 - (flashUntil - now)) * LED_NUM / 600);
    for (int i = 0; i < LED_NUM; i++) hexLeds[i] = (i <= lit) ? CRGB::White : CRGB::Black;
    return;
  }
  if (now < okFlashUntil) { // OK/NG切替: OK=緑が一周 / NG=赤が2回点滅
    unsigned long t = OKFLASH_MS - (okFlashUntil - now);
    if (talkOk) {
      int lit = (int)(t * LED_NUM / OKFLASH_MS);
      for (int i = 0; i < LED_NUM; i++) hexLeds[i] = (i <= lit) ? CRGB(0, 255, 60) : CRGB::Black;
    } else {
      bool on = (t < 250) || (t >= 450 && t < 700);
      fill_solid(hexLeds, LED_NUM, on ? CRGB(255, 0, 0) : CRGB::Black);
    }
    return;
  }
  if (now < previewUntil) { // 登録内容の確認（全問一致したときの光り方）
    drawMasks(peerAns, false, 255);
    return;
  }
  if (!talkOk) {            // NG中: 0番だけ暗い赤（本人と周りへのサイン）
    fill_solid(hexLeds, LED_NUM, CRGB::Black);
    hexLeds[0] = CRGB(80, 0, 0);
    lastMatched = false;
    return;
  }
  // 一致している間は相手の回答を覚えておき、途切れても HOLD_MS は点灯を保つ
  static unsigned long lastMatchAt = 0;
  if (matched) {
    if (!lastMatched) matchSince = now;      // 消えていた状態から点くときだけフェードイン
    memcpy(heldAns, peerAns, 4);
    lastMatchAt = now;
    lastMatched = true;
  }
  if ((long)(syncUntil - now) > 0) {         // 同期演出
    drawSync(SYNC_MS - (syncUntil - now));
    return;
  }

  unsigned long since = now - lastMatchAt;
  if (lastMatched && since < HOLD_MS + FADE_OUT_MS) {
    uint8_t in  = (uint8_t)min<unsigned long>(255, (now - matchSince) / 2);   // 約0.5秒でフェードイン
    uint8_t out = (since < HOLD_MS) ? 255 : (uint8_t)(255 - (since - HOLD_MS) * 255 / FADE_OUT_MS);
    uint8_t lv = min(in, out);
    if (meWave) lv = scale8(lv, beatsin8(90, 120, 255));   // 合図中: 相手の合図を待って脈打つ
    drawMasks(heldAns, true, lv);
  } else {
    lastMatched = false;
    fill_solid(hexLeds, LED_NUM, CRGB::Black);
    if (aura) {                              // 気配: 全体が弱く呼吸する（約3秒周期）
      CRGB c = WARM;
      c.nscale8(beatsin8(20, 6, 70));
      fill_solid(hexLeds, LED_NUM, c);
    } else if (meWave) {                     // 一致なしで合図: 0番が暖色で点滅（押せたことの確認）
      if (millis() % 500 < 250) hexLeds[0] = WARM;
    }
  }
}

// 本体LED: 通常は消灯 / スマホ接続中=緑 / 互いに最寄り=青 / 片思い=青の点滅
void drawStatus(bool mutual, bool oneSided) {
  if (phoneConnected)  statusLed[0] = CRGB(0, 30, 0);
  else if (mutual)     statusLed[0] = CRGB(0, 0, 30);
  else if (oneSided)   statusLed[0] = (millis() % 600 < 300) ? CRGB(0, 0, 30) : CRGB::Black;
  else                 statusLed[0] = CRGB::Black;
}

// 短く押す: 合図 / 1〜5秒長押し: 話しかけOK⇔NG / 5秒以上: 登録を消去
void checkButton() {
  static unsigned long pressedAt = 0;
  if (digitalRead(BTN_PIN) == LOW) {
    if (pressedAt == 0) pressedAt = millis();
    if (millis() - pressedAt > 5000) {
      String e = emptyConfig();
      parseConfig(e);
      saveAndApply(e);
      if (phoneConnected) { statusChar->setValue("RESET"); statusChar->notify(); }
      pressedAt = 0;
      while (digitalRead(BTN_PIN) == LOW) delay(10);
    }
  } else {
    if (pressedAt != 0) {
      unsigned long held = millis() - pressedAt;
      if (held > 50 && held < 1000) startWave();
      else if (held >= 1000) toggleTalkOk();
    }
    pressedAt = 0;
  }
}

// デバッグ情報をまとめる（Webのデバッグ画面が読み出す）
// 1行目: me=<自分ID>;a=<回答>;mu=<互いに最寄り>;mt=<一致>;sim=<仮想相手>;ph=<スマホ接続>;ok=;wv=;ev=;g=
// 2行目以降: p=<ID>,<RSSI>,<範囲内>,<自分の最寄り>,<相手の最寄りが自分>,<回答>,<フラグ>
void updateDebugValue() {
  static unsigned long last = 0;
  if (millis() - last < 300) return;
  last = millis();
  char buf[600];
  int n = snprintf(buf, sizeof(buf), "me=%02X%02X%02X;a=", myId[0], myId[1], myId[2]);
  for (int q = 0; q < NQ; q++) n += snprintf(buf + n, sizeof(buf) - n, "%X", answers[q]);
  n += snprintf(buf + n, sizeof(buf) - n, ";mu=%d;mt=%d;sim=%d;ph=%d;ok=%d;wv=%d;ev=%04X;g=%d",
                dbgMutual, dbgMatched, simActive ? (int)((simUntil - millis()) / 1000) + 1 : 0, phoneConnected,
                talkOk, waving(), eventHash, groupMode);
  Peer copy[MAX_PEERS];
  portENTER_CRITICAL(&peerMux);
  memcpy(copy, peers, sizeof(peers));
  portEXIT_CRITICAL(&peerMux);
  for (int i = 0; i < MAX_PEERS && n < (int)sizeof(buf) - 60; i++) {
    if (!copy[i].used) continue;
    bool isNearest = hasNearest && memcmp(copy[i].addr, nearestAddr, 6) == 0;
    bool back = memcmp(copy[i].nearestId, myId, 3) == 0;
    n += snprintf(buf + n, sizeof(buf) - n, "\np=%02X%02X%02X,%d,%d,%d,%d,%02X%02X%02X%02X,%X",
                  copy[i].addr[3], copy[i].addr[4], copy[i].addr[5], (int)copy[i].rssi,
                  copy[i].inRange, isNearest, back,
                  copy[i].ans[0], copy[i].ans[1], copy[i].ans[2], copy[i].ans[3], copy[i].flags);
  }
  debugChar->setValue((uint8_t *)buf, min(n, 510));
}

void debugPrint(bool mutual) {
  static unsigned long last = 0;
  if (millis() - last < 1000) return;
  last = millis();
  Peer copy[MAX_PEERS];
  portENTER_CRITICAL(&peerMux);
  memcpy(copy, peers, sizeof(peers));
  portEXIT_CRITICAL(&peerMux);
  for (int i = 0; i < MAX_PEERS; i++) {
    if (!copy[i].used) continue;
    bool isNearest = hasNearest && memcmp(copy[i].addr, nearestAddr, 6) == 0;
    Serial.printf("peer ..%02X%02X  rssi=%4.0f %s%s%s\n", copy[i].addr[4], copy[i].addr[5], copy[i].rssi,
                  copy[i].inRange ? "IN " : "out", (copy[i].flags & FLAG_WAVE) ? " 合図" : "",
                  isNearest ? (mutual ? "  <= 互いに最寄り" : "  <= 最寄り(片側)") : "");
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(BTN_PIN, INPUT_PULLUP);
  memset(peers, 0, sizeof(peers));
  memset(heldAns, 0, sizeof(heldAns));

  FastLED.addLeds<WS2812B, LED_PIN, GRB>(hexLeds, LED_NUM);
  FastLED.addLeds<WS2812, STATUS_PIN, GRB>(statusLed, 1);
  FastLED.setBrightness(DEF_BRIGHT);
  FastLED.setMaxPowerInVoltsAndMilliamps(5, MAX_MA);

  prefs.begin("devcfg2", false);
  String saved = prefs.getString("config", emptyConfig());
  if (!parseConfig(saved)) parseConfig(emptyConfig());
  talkOk = prefs.getBool("ok", true);

  BLEDevice::init(DEVICE_NAME);
  BLEAddress me = BLEDevice::getAddress();
  memcpy(myId, ((const uint8_t *)me.getNative()) + 3, 3);

  BLEServer *server = BLEDevice::createServer();
  server->setCallbacks(new ServerCB());
  BLEService *service = server->createService(SERVICE_UUID);

  configChar = service->createCharacteristic(
      CONFIG_CHAR_UUID,
      BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE);
  configChar->setCallbacks(new ConfigCB());
  updateReadValue();

  statusChar = service->createCharacteristic(
      STATUS_CHAR_UUID,
      BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  statusChar->addDescriptor(new BLE2902());

  debugChar = service->createCharacteristic(DEBUG_CHAR_UUID, BLECharacteristic::PROPERTY_READ);
  debugChar->setValue("me=");
  service->start();
  BLEDevice::setMTU(247);

  updateAdvertising();

  scanner = BLEDevice::getScan();
  scanner->setAdvertisedDeviceCallbacks(new ScanCB(), true);
  scanner->setActiveScan(false);
  scanner->setInterval(160);
  scanner->setWindow(80);

  Serial.printf("start: %s  id=%02X%02X%02X  event=%04X  %s\n", DEVICE_NAME, myId[0], myId[1], myId[2],
                eventHash, talkOk ? "OK" : "NG");
}

void loop() {
  handlePending();
  keepScanning();
  checkButton();

  uint8_t peerAns[4] = {0, 0, 0, 0};
  bool oneSided;
  uint8_t peerFlags;
  bool mutual = updatePairing(peerAns, oneSided, peerFlags);
  bool peerWave = mutual && (peerFlags & FLAG_WAVE);

  // グループモード: 範囲内の全員との一致をまとめて表示
  if (groupMode && talkOk) {
    mutual = groupMatch(peerAns, peerWave);
    oneSided = false;
  }

  // デバッグ: 仮想の相手がいるときは、その相手と「互いに最寄り」（合図済み）として扱う
  if (simActive && (long)(millis() - simUntil) > 0) simActive = false;
  if (simActive && talkOk) {
    mutual = true;
    oneSided = false;
    memcpy(peerAns, simAns, 4);
    peerWave = true;
  }
  bool matched = mutual && anyMatch(peerAns);
  dbgMutual = mutual;
  dbgMatched = matched;

  // 合図: 一致中に二人とも合図していたら、同期演出を1回だけ出す
  bool meWave = talkOk && waving();
  if (matched && meWave && peerWave && !syncFired) {
    syncFired = true;
    syncUntil = millis() + SYNC_MS;
    Serial.println("同期！");
  }
  if (!meWave) syncFired = false;

  bool aura = !matched && auraNearby();

  // 状態フラグ（合図の開始・終了、OK/NG）が変わったら発信し直す
  static uint8_t lastFlags = 0xFF;
  uint8_t f = myFlags();
  if (f != lastFlags) { lastFlags = f; advDirty = true; }

  // 発信内容を更新（頻繁になりすぎないよう間隔を空ける）
  static unsigned long lastAdv = 0;
  if (advDirty && millis() - lastAdv > 300) {
    advDirty = false;
    lastAdv = millis();
    updateAdvertising();
  }

  drawHex(matched, peerAns, aura, meWave);
  drawStatus(mutual, oneSided);
  FastLED.show();

  updateDebugValue();
  debugPrint(mutual);
  delay(16);
}
