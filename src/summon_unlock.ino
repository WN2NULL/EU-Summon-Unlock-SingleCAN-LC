#define CAN_TX_PIN       15
#define CAN_RX_PIN       16

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <Update.h>
#include "driver/twai.h"
#include "index_html.h"

#define FW_VERSION "V2.7-LC"
static volatile bool forceMode = false;

static portMUX_TYPE stateMux = portMUX_INITIALIZER_UNLOCKED;

static Preferences   prefs;
static volatile bool summonEnabled       = true;
static volatile bool tlsscEnabled        = false;   // "Enable TLSSC"
static volatile bool tlsscRestoreEnabled = false;   // "TLSSC Restore" (0x331)
static volatile bool laneChangeEnabled   = true;    // 独立 0x293 自动变道开关

// ── Lane Change / UI_ulcBlindSpotConfig (0x3F8) ──
// 0 = STANDARD, 1 = AGGRESSIVE, 2 = MAD_MAX
static volatile uint8_t blindspotConfig = 0;

static volatile bool gateAPActive  = false;
static volatile bool gateParked    = true;
static volatile bool gateSummoning = false;

static volatile bool sprSeen  = false;
static volatile bool lastAca  = false;

#define PARKED_TIMEOUT_MS  5000
static volatile uint32_t last280Millis = 0;

static volatile uint32_t rxMux1    = 0;
static volatile uint32_t txOk      = 0;
static volatile uint32_t txFail    = 0;
static volatile uint32_t rx280     = 0;
static volatile uint32_t rx390     = 0;
static volatile uint32_t rx921     = 0;
static volatile uint32_t rx1016    = 0;
static volatile uint32_t rx659     = 0;     // 0x293 接收计数
static volatile uint32_t tx659     = 0;     // 0x293 注入成功计数
static volatile uint8_t  last293Raw = 0xFF; // 原车 0x293 bit24..25 原始值 (0..3, 255=未收到)
static unsigned long     bootTime  = 0;

static char gateBlockReason[48] = "boot";

// ── Force inject / force mode ─────────────────────────────────
static uint8_t       last1021Data[8] = {0};
static volatile bool last1021Valid   = false;

// ═══════════════════════════════════════════════════════════════
// HELPERS CAN
// ═══════════════════════════════════════════════════════════════

static inline uint8_t readMuxID(const uint8_t *data) {
    return data[0] & 0x07;
}

static inline bool getBit(const uint8_t *data, int bit) {
    return (data[bit / 8] >> (bit % 8)) & 0x01;
}

static inline void setBit(uint8_t *data, int bit, bool val) {
    uint8_t mask = (uint8_t)(1U << (bit % 8));
    if (val) data[bit / 8] |=  mask;
    else     data[bit / 8] &= ~mask;
}

static inline uint8_t readDIGear(const uint8_t *data) {
    return (data[2] >> 5) & 0x07;
}

static inline uint8_t readVehicleGear(const uint8_t *data) {
    return (data[2] >> 5) & 0x07;
}

static inline int gearState(uint8_t gear) {
    if (gear == 1)                           return  1;
    if (gear == 2 || gear == 3 || gear == 4) return  0;
    return -1;
}

static inline uint8_t readDASStatus(const uint8_t *data) {
    return data[0] & 0x07;
}

// AP active for status 3,4,5,6
static inline bool isDASActive(uint8_t status) {
    bool fm = false;

    switch (status) {
        case 3:
        case 4:
        case 5:
        case 6:
            fm = true;
            break;
        case 0:
        case 1:
        case 8:
        case 9:
        case 14:
        default:
            fm = false;
            break;
    }

    portENTER_CRITICAL(&stateMux);
    forceMode = fm;
    portEXIT_CRITICAL(&stateMux);

    return status == 3 || status == 4 || status == 5 || status == 6;
}

// ═══════════════════════════════════════════════════════════════
// LOGIQUE GATE
// ═══════════════════════════════════════════════════════════════

static inline bool injectionGateOpen() {
    return gateParked || gateSummoning;
}

static void recomputeSummoning() {
    gateSummoning = lastAca && sprSeen;
}

static void clearSummonOnPark() {
    gateSummoning = false;
    sprSeen       = false;
}

static void clearSummonOnParkIfAcaInactive(uint8_t gear) {
    if (gear == 1 && !lastAca)
        clearSummonOnPark();
}

static void handle280(const uint8_t *data) {
    rx280++;
    last280Millis = (uint32_t)millis();
    uint8_t gear = readDIGear(data);
    int     gs   = gearState(gear);

    portENTER_CRITICAL(&stateMux);
    if (gs == 1) gateParked = true;
    if (gs == 0) gateParked = false;

    bool aca = (data[6] & 0x04) != 0;
    if (lastAca && !aca)
        sprSeen = false;
    lastAca = aca;
    recomputeSummoning();

    clearSummonOnParkIfAcaInactive(gear);
    portEXIT_CRITICAL(&stateMux);
}

static void handle390(const uint8_t *data) {
    rx390++;
    uint8_t gear = readVehicleGear(data);
    int     gs   = gearState(gear);
    if (gs < 0) return;

    portENTER_CRITICAL(&stateMux);
    uint32_t age = (uint32_t)millis() - last280Millis;
    if (last280Millis == 0 || age > PARKED_TIMEOUT_MS) {
        gateParked = (gs == 1);
        clearSummonOnParkIfAcaInactive(gear);
    }
    portEXIT_CRITICAL(&stateMux);
}

static void handle921(const uint8_t *data) {
    rx921++;
    bool ap = isDASActive(readDASStatus(data));

    portENTER_CRITICAL(&stateMux);
    gateAPActive = ap;
    portEXIT_CRITICAL(&stateMux);
}

static void handle1016(const uint8_t *data, uint8_t dlc) {
    if (dlc < 4) return;
    rx1016++;
    uint8_t spr = (data[3] >> 4) & 0x0F;

    portENTER_CRITICAL(&stateMux);
    if (spr != 0)
        sprSeen = true;
    recomputeSummoning();
    portEXIT_CRITICAL(&stateMux);
}

// ── Lane Change : 0x3F8 / UI_ulcBlindSpotConfig ─────────────
static void injectBlindspotConfig(const twai_message_t &src) {
    if (src.data_length_code < 7) return;

    bool apOnly;
    uint8_t mode;

    portENTER_CRITICAL(&stateMux);
    apOnly = gateAPActive;
    mode   = blindspotConfig;
    portEXIT_CRITICAL(&stateMux);

    if (!apOnly) return;
    mode = (mode > 2) ? 0 : mode;

    uint8_t oldValue = (src.data[6] >> 4) & 0x03;
    if (oldValue == mode) return;

    twai_message_t out;
    out.identifier       = 1016;   // 0x3F8
    out.data_length_code = src.data_length_code;
    out.flags            = 0;
    memcpy(out.data, src.data, src.data_length_code);

    out.data[6] = (uint8_t)((out.data[6] & 0xCF) | ((mode & 0x03) << 4));

    esp_err_t err = twai_transmit(&out, pdMS_TO_TICKS(2));
    if (err == ESP_OK) txOk++;
    else               txFail++;
}

// ═══════════════════════════════════════════════════════════════
// INJECTION SUMMON & 0x293 AUTO LANE CHANGE
// ═══════════════════════════════════════════════════════════════

static void doInjectSummon(const uint8_t *srcData, uint8_t dlc) {
    if (dlc < 8) return;
    twai_message_t out;
    out.identifier       = 1021;
    out.data_length_code = 8;
    out.flags            = 0;
    memcpy(out.data, srcData, 8);

    setBit(out.data, 19, false);
    setBit(out.data, 47, true);

    rxMux1++;
    esp_err_t err = twai_transmit(&out, pdMS_TO_TICKS(2));
    if (err == ESP_OK) txOk++;
    else               txFail++;
}

#define SUMMON_PERIODIC_TX_MS 500
static volatile uint32_t lastSummonPeriodicTxMs = 0;

static void summonPeriodicTick() {
    uint32_t now = (uint32_t)millis();
    bool en, summoning, haveTemplate;
    uint8_t data[8];

    portENTER_CRITICAL(&stateMux);
    en           = summonEnabled;
    summoning    = gateSummoning;
    haveTemplate = last1021Valid;
    if (haveTemplate)
        memcpy(data, last1021Data, 8);
    portEXIT_CRITICAL(&stateMux);

    if (!en || !summoning || !haveTemplate)
        return;

    if (lastSummonPeriodicTxMs != 0 &&
        (uint32_t)(now - lastSummonPeriodicTxMs) < SUMMON_PERIODIC_TX_MS)
        return;

    twai_message_t out;
    out.identifier       = 1021;
    out.data_length_code = 8;
    out.flags            = 0;
    memcpy(out.data, data, 8);

    setBit(out.data, 19, false);
    setBit(out.data, 47, true);

    esp_err_t err = twai_transmit(&out, pdMS_TO_TICKS(2));
    if (err == ESP_OK) {
        txOk++;
        lastSummonPeriodicTxMs = now;
    } else {
        txFail++;
    }
}

// Tesla 0x293 校验和计算：(低ID + 高ID + 前7字节数据) % 256
static inline uint8_t ui293ChecksumPure(const uint8_t *data) {
    if (!data) return 0;
    uint16_t sum = (0x293 & 0xFF) + ((0x293 >> 8) & 0xFF);
    for (int i = 0; i < 7; i++) {
        sum += data[i];
    }
    return (uint8_t)(sum & 0xFF);
}

// 将 UI_autoLaneChangeEnable (bits 24..25) 置为 1，推进 4-bit 计数器，重算 Checksum
static inline bool uiAutoLaneChangeFinalizePure(uint8_t *data, uint8_t dlc) {
    if (!data || dlc < 8) return false;

    data[3] = (uint8_t)((data[3] & 0xFCu) | 0x01u);

    const uint8_t counter = (uint8_t)(((data[6] >> 4) + 1u) & 0x0Fu);
    data[6] = (uint8_t)((data[6] & 0x0Fu) | (uint8_t)(counter << 4));

    data[7] = ui293ChecksumPure(data);
    return true;
}

static void injectAutoLaneChange0x293(const twai_message_t &src) {
    if (src.data_length_code < 8) return;

    const uint8_t rawVal = (uint8_t)(src.data[3] & 0x03u);

    bool en, apActive;
    portENTER_CRITICAL(&stateMux);
    last293Raw = rawVal;
    en         = laneChangeEnabled;
    apActive   = gateAPActive;
    portEXIT_CRITICAL(&stateMux);

    // 仅依赖独立开关与 AP 激活状态（不再绑定转向灯）
    if (!en || !apActive)
        return;

    // 如果原车本身已经是 ON (1)，无需重复覆盖
    if (rawVal == 0x01u)
        return;

    twai_message_t out;
    out.identifier       = 0x293;   // 659
    out.data_length_code = 8;
    out.flags            = 0;
    memcpy(out.data, src.data, 8);

    if (uiAutoLaneChangeFinalizePure(out.data, out.data_length_code)) {
        esp_err_t err = twai_transmit(&out, pdMS_TO_TICKS(2));
        if (err == ESP_OK) {
            txOk++;
            tx659++;
        } else {
            txFail++;
        }
    }
}

// ── TLSSC : 0x3FD mux0 bit38,39 ───────────────────────────────
static void doInjectTLSSC(const uint8_t *srcData, uint8_t dlc) {
    if (dlc < 8) return;
    twai_message_t out;
    out.identifier       = 1021;
    out.data_length_code = 8;
    out.flags            = 0;
    memcpy(out.data, srcData, 8);

    setBit(out.data, 38, true);
    setBit(out.data, 39, true);

    esp_err_t err = twai_transmit(&out, pdMS_TO_TICKS(2));
    if (err == ESP_OK) txOk++;
    else               txFail++;
}

static void injectTLSSC(const twai_message_t &src) {
    bool en, apActive, fmode;
    portENTER_CRITICAL(&stateMux);
    en       = tlsscEnabled;
    apActive = gateAPActive;
    fmode    = forceMode;
    portEXIT_CRITICAL(&stateMux);

    // 保持你已验证可用的 TLSSC 门控
    if (!en || (!apActive && !fmode))
        return;

    doInjectTLSSC(src.data, src.data_length_code);
}

// ── TLSSC Restore : 0x331 ─────────────────────────────────────
static void doInjectTlsscRestore(const uint8_t *srcData, uint8_t dlc) {
    if (dlc < 1) return;
    twai_message_t out;
    out.identifier       = 817;   // 0x331
    out.data_length_code = dlc;
    out.flags            = 0;
    memcpy(out.data, srcData, dlc);

    out.data[0] = (uint8_t)((out.data[0] & 0xC0) | 0x1B);

    esp_err_t err = twai_transmit(&out, pdMS_TO_TICKS(2));
    if (err == ESP_OK) txOk++;
    else               txFail++;
}

static void injectTlsscRestore(const twai_message_t &src) {
    bool en, gate;
    portENTER_CRITICAL(&stateMux);
    en   = tlsscRestoreEnabled;
    gate = injectionGateOpen();
    portEXIT_CRITICAL(&stateMux);

    if (!en || !gate)
        return;

    doInjectTlsscRestore(src.data, src.data_length_code);
}

static void injectSummon(const twai_message_t &src) {
    bool en, gate, fmode;
    portENTER_CRITICAL(&stateMux);
    en    = summonEnabled;
    gate  = injectionGateOpen();
    fmode = forceMode;
    if (!gate && !fmode) {
        if (!gateAPActive && !gateParked && !gateSummoning)
            strncpy(gateBlockReason, "AP-,Park-,Summon-", sizeof(gateBlockReason));
    }
    portEXIT_CRITICAL(&stateMux);

    if ((!en || !gate) && !fmode)
        return;

    doInjectSummon(src.data, src.data_length_code);
}

// ═══════════════════════════════════════════════════════════════
// CAN (Core 1, HIGH PRIORITY)
// ═══════════════════════════════════════════════════════════════

static void canTask(void *arg) {
    for (;;) {
        twai_message_t f;
        while (twai_receive(&f, pdMS_TO_TICKS(2)) == ESP_OK) {
            switch (f.identifier) {
                case 280:
                    if (f.data_length_code >= 7) handle280(f.data);
                    break;
                case 390:
                    if (f.data_length_code >= 8) handle390(f.data);
                    break;
                case 659: // 0x293 UI_ChassisControl
                    if (f.data_length_code >= 8) {
                        rx659++;
                        injectAutoLaneChange0x293(f);
                    }
                    break;
                case 817: // 0x331 DAS_autopilotConfig
                    injectTlsscRestore(f);
                    break;
                case 921:
                    if (f.data_length_code >= 1) handle921(f.data);
                    break;
                case 1016:
                    handle1016(f.data, f.data_length_code);
                    injectBlindspotConfig(f);
                    break;
                case 1021:
                    if (f.data_length_code >= 8) {
                        uint8_t mux = readMuxID(f.data);
                        if (mux == 1) {
                            portENTER_CRITICAL(&stateMux);
                            memcpy(last1021Data, f.data, 8);
                            last1021Valid = true;
                            portEXIT_CRITICAL(&stateMux);
                            injectSummon(f);
                        } else if (mux == 0) {
                            injectTLSSC(f);
                        }
                    }
                    break;
                default:
                    break;
            }
        }

        twai_status_info_t st;
        twai_get_status_info(&st);
        if (st.state == TWAI_STATE_BUS_OFF) {
            twai_initiate_recovery();
            vTaskDelay(pdMS_TO_TICKS(300));
        }

        summonPeriodicTick();

        uint32_t now = (uint32_t)millis();
        portENTER_CRITICAL(&stateMux);
        bool can280Stale = (last280Millis > 0) &&
                           (now - last280Millis > PARKED_TIMEOUT_MS);
        if (can280Stale)
            gateParked = true;
        portEXIT_CRITICAL(&stateMux);

        vTaskDelay(1);
    }
}

// ═══════════════════════════════════════════════════════════════
// DASHBOARD WI-FI & CONFIG
// ═══════════════════════════════════════════════════════════════

extern const char INDEX_HTML[] PROGMEM;
static WebServer server(80);

static volatile bool     otaInProgress = false;
static volatile bool     otaSuccess    = false;
static volatile bool     otaError      = false;
static volatile uint32_t otaBytes      = 0;
static volatile uint32_t otaTotal      = 0;
static char              otaErrMsg[64] = "";

static void cfgLoad() {
    prefs.begin("summon", true);
    summonEnabled       = prefs.getBool("en", true);
    tlsscEnabled        = prefs.getBool("tlssc", true);
    tlsscRestoreEnabled = prefs.getBool("tlrst", false);
    laneChangeEnabled   = prefs.getBool("lc", true);
    blindspotConfig     = prefs.getUChar("bsCfg", 0);
    if (blindspotConfig > 2) blindspotConfig = 0;
    prefs.end();
}

static void cfgSave() {
    prefs.begin("summon", false);
    prefs.putBool("en", summonEnabled);
    prefs.putBool("tlssc", tlsscEnabled);
    prefs.putBool("tlrst", tlsscRestoreEnabled);
    prefs.putBool("lc", laneChangeEnabled);
    prefs.putUChar("bsCfg", blindspotConfig);
    prefs.end();
}

static String statsToJson() {
    bool en, tlssc, tlrst, lc, ap, parked, summon, aca, spr, fmode, l1021;
    uint8_t bsCfg, r293;
    uint32_t rmx, tok, tfail, r280, r390, r921, r1016, r659, t659;

    portENTER_CRITICAL(&stateMux);
    en     = summonEnabled;
    tlssc  = tlsscEnabled;
    tlrst  = tlsscRestoreEnabled;
    lc     = laneChangeEnabled;
    ap     = gateAPActive;
    parked = gateParked;
    summon = gateSummoning;
    aca    = lastAca;
    spr    = sprSeen;
    rmx    = rxMux1;
    tok    = txOk;
    tfail  = txFail;
    r280   = rx280;
    r390   = rx390;
    r921   = rx921;
    r1016  = rx1016;
    r659   = rx659;
    t659   = tx659;
    r293   = last293Raw;
    fmode  = forceMode;
    l1021  = last1021Valid;
    bsCfg  = blindspotConfig;
    portEXIT_CRITICAL(&stateMux);

    bool gate = parked || summon;

    twai_status_info_t st;
    twai_get_status_info(&st);

    String s = "{";
    s += "\"enabled\":"         + String(en     ? "true" : "false");
    s += ",\"tlssc\":"          + String(tlssc  ? "true" : "false");
    s += ",\"tlrst\":"          + String(tlrst  ? "true" : "false");
    s += ",\"laneChange\":"     + String(lc     ? "true" : "false");
    s += ",\"blindspotConfig\":" + String((int)bsCfg);
    s += ",\"gate\":"           + String(gate   ? "true" : "false");
    s += ",\"ap\":"             + String(ap     ? "true" : "false");
    s += ",\"parked\":"         + String(parked ? "true" : "false");
    s += ",\"summon\":"         + String(summon ? "true" : "false");
    s += ",\"aca\":"            + String(aca    ? "true" : "false");
    s += ",\"spr\":"            + String(spr    ? "true" : "false");
    s += ",\"forceMode\":"      + String(fmode  ? "true" : "false");
    s += ",\"last1021\":"       + String(l1021  ? "true" : "false");
    s += ",\"rxMux1\":"         + String(rmx);
    s += ",\"txOk\":"           + String(tok);
    s += ",\"txFail\":"         + String(tfail);
    s += ",\"rx280\":"          + String(r280);
    s += ",\"rx390\":"          + String(r390);
    s += ",\"rx921\":"          + String(r921);
    s += ",\"rx1016\":"         + String(r1016);
    s += ",\"rx659\":"          + String(r659);
    s += ",\"tx659\":"          + String(t659);
    s += ",\"raw293\":"         + String((int)r293);
    s += ",\"canState\":"       + String((int)st.state);
    s += ",\"uptimeS\":"        + String((millis() - bootTime) / 1000);
    s += ",\"fwVersion\":\""    + String(FW_VERSION) + "\"";
    s += ",\"otaInProgress\":"  + String(otaInProgress ? "true" : "false");
    s += ",\"otaSuccess\":"     + String(otaSuccess    ? "true" : "false");
    s += ",\"otaError\":"       + String(otaError      ? "true" : "false");
    s += ",\"otaErrMsg\":\""    + String(otaErrMsg) + "\"";
    s += ",\"otaBytes\":"       + String(otaBytes);
    s += ",\"otaTotal\":"       + String(otaTotal);
    s += ",\"freeHeap\":"       + String(ESP.getFreeHeap());
    s += "}";
    return s;
}

static void httpRoot()   { server.send_P(200, "text/html", INDEX_HTML); }
static void httpStats()  { server.send(200, "application/json", statsToJson()); }

static void httpEnable() {
    portENTER_CRITICAL(&stateMux); summonEnabled = true; portEXIT_CRITICAL(&stateMux);
    cfgSave();
    server.send(200, "application/json", statsToJson());
}
static void httpDisable() {
    portENTER_CRITICAL(&stateMux); summonEnabled = false; portEXIT_CRITICAL(&stateMux);
    cfgSave();
    server.send(200, "application/json", statsToJson());
}

static void httpTlsscEnable() {
    portENTER_CRITICAL(&stateMux); tlsscEnabled = true; portEXIT_CRITICAL(&stateMux);
    cfgSave();
    server.send(200, "application/json", statsToJson());
}
static void httpTlsscDisable() {
    portENTER_CRITICAL(&stateMux); tlsscEnabled = false; portEXIT_CRITICAL(&stateMux);
    cfgSave();
    server.send(200, "application/json", statsToJson());
}

static void httpTlrstEnable() {
    portENTER_CRITICAL(&stateMux); tlsscRestoreEnabled = true; portEXIT_CRITICAL(&stateMux);
    cfgSave();
    server.send(200, "application/json", statsToJson());
}
static void httpTlrstDisable() {
    portENTER_CRITICAL(&stateMux); tlsscRestoreEnabled = false; portEXIT_CRITICAL(&stateMux);
    cfgSave();
    server.send(200, "application/json", statsToJson());
}

static void httpLcEnable() {
    portENTER_CRITICAL(&stateMux); laneChangeEnabled = true; portEXIT_CRITICAL(&stateMux);
    cfgSave();
    server.send(200, "application/json", statsToJson());
}
static void httpLcDisable() {
    portENTER_CRITICAL(&stateMux); laneChangeEnabled = false; portEXIT_CRITICAL(&stateMux);
    cfgSave();
    server.send(200, "application/json", statsToJson());
}

static void httpBlindspotConfig() {
    if (!server.hasArg("mode")) {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing mode\"}");
        return;
    }

    int mode = server.arg("mode").toInt();
    if (mode < 0 || mode > 2) {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"mode must be 0, 1 or 2\"}");
        return;
    }

    portENTER_CRITICAL(&stateMux);
    blindspotConfig = (uint8_t)mode;
    portEXIT_CRITICAL(&stateMux);

    cfgSave();
    server.send(200, "application/json", statsToJson());
}

static void httpForce() {
    uint8_t data[8];
    bool valid;

    portENTER_CRITICAL(&stateMux);
    valid = last1021Valid;
    if (valid) {
        memcpy(data, last1021Data, 8);
    } else {
        uint8_t fallback[8] = {0x01, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00};
        memcpy(data, fallback, 8);
    }
    portEXIT_CRITICAL(&stateMux);

    doInjectSummon(data, 8);
    server.send(200, "application/json", statsToJson());
}

static void httpForceMode() {
    portENTER_CRITICAL(&stateMux);
    forceMode = !forceMode;
    portEXIT_CRITICAL(&stateMux);
    server.send(200, "application/json", statsToJson());
}

static void httpOtaUpload() {
    HTTPUpload &up = server.upload();

    if (up.status == UPLOAD_FILE_START) {
        otaInProgress = true;
        otaSuccess    = false;
        otaError      = false;
        otaBytes      = 0;
        otaErrMsg[0]  = '\0';
        Serial.printf("[OTA] Start: %s\n", up.filename.c_str());

        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
            otaError = true;
            strncpy(otaErrMsg, Update.errorString(), sizeof(otaErrMsg) - 1);
        }
    } else if (up.status == UPLOAD_FILE_WRITE) {
        if (!otaError && Update.write(up.buf, up.currentSize) != up.currentSize) {
            otaError = true;
            strncpy(otaErrMsg, Update.errorString(), sizeof(otaErrMsg) - 1);
        }
        otaBytes += up.currentSize;
    } else if (up.status == UPLOAD_FILE_END) {
        if (!otaError && Update.end(true)) {
            otaSuccess = true;
            otaTotal   = otaBytes;
        } else if (!otaError) {
            otaError = true;
            strncpy(otaErrMsg, Update.errorString(), sizeof(otaErrMsg) - 1);
        }
        otaInProgress = false;
    } else if (up.status == UPLOAD_FILE_ABORTED) {
        Update.end();
        otaInProgress = false;
        otaError      = true;
        strncpy(otaErrMsg, "aborted", sizeof(otaErrMsg) - 1);
    }
}

static void httpOtaFinish() {
    bool ok = otaSuccess && !otaError;
    String resp = String("{\"ok\":") + (ok ? "true" : "false") +
                  ",\"error\":\"" + String(otaErrMsg) + "\"}";
    server.sendHeader("Connection", "close");
    server.send(200, "application/json", resp);
    if (ok) {
        delay(700);
        ESP.restart();
    }
}

static void webTask(void *arg) {
    WiFi.mode(WIFI_AP);
    uint8_t mac[6]; WiFi.softAPmacAddress(mac);
    char ssid[28];
    snprintf(ssid, sizeof(ssid), "Summon-%02X%02X", mac[4], mac[5]);
    WiFi.softAP(ssid, "summon1234");

    server.on("/",              HTTP_GET,  httpRoot);
    server.on("/api/stats",     HTTP_GET,  httpStats);
    server.on("/api/enable",    HTTP_POST, httpEnable);
    server.on("/api/disable",   HTTP_POST, httpDisable);
    server.on("/api/tlssc-enable",  HTTP_POST, httpTlsscEnable);
    server.on("/api/tlssc-disable", HTTP_POST, httpTlsscDisable);
    server.on("/api/tlrst-enable",  HTTP_POST, httpTlrstEnable);
    server.on("/api/tlrst-disable", HTTP_POST, httpTlrstDisable);
    server.on("/api/lc-enable",     HTTP_POST, httpLcEnable);
    server.on("/api/lc-disable",    HTTP_POST, httpLcDisable);
    server.on("/api/blindspot", HTTP_POST, httpBlindspotConfig);
    server.on("/api/force",     HTTP_POST, httpForce);
    server.on("/api/forcemode", HTTP_POST, httpForceMode);
    server.on("/update",        HTTP_POST, httpOtaFinish, httpOtaUpload);
    server.begin();
    for (;;) { server.handleClient(); vTaskDelay(1); }
}

void setup() {
    bootTime = millis();
    Serial.begin(115200);
    delay(500);

    cfgLoad();

    twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT(
        (gpio_num_t)CAN_TX_PIN, (gpio_num_t)CAN_RX_PIN, TWAI_MODE_NORMAL);
    g.rx_queue_len = 64;
    g.tx_queue_len = 16;
    twai_timing_config_t t = TWAI_TIMING_CONFIG_500KBITS();
    twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    Serial.printf("twai_install=%d  twai_start=%d\n",
                  (int)twai_driver_install(&g, &t, &f),
                  (int)twai_start());
    Serial.printf("=== %s ready | LC=%s | TLSSC=%s ===\n",
                  FW_VERSION,
                  laneChangeEnabled ? "ON" : "OFF",
                  tlsscEnabled ? "ON" : "OFF");

    xTaskCreatePinnedToCore(canTask, "can", 4096, nullptr, 5, nullptr, 1);
    xTaskCreatePinnedToCore(webTask, "web", 8192, nullptr, 1, nullptr, 0);
}

void loop() {
    static uint32_t lastLogMs = 0;
    uint32_t now = (uint32_t)millis();

    if (now - lastLogMs >= 2000) {
        lastLogMs = now;
        bool ap, lc;
        uint8_t r293;
        uint32_t r659, t659, tok, tfail;

        portENTER_CRITICAL(&stateMux);
        ap    = gateAPActive;
        lc    = laneChangeEnabled;
        r293  = last293Raw;
        r659  = rx659;
        t659  = tx659;
        tok   = txOk;
        tfail = txFail;
        portEXIT_CRITICAL(&stateMux);

        Serial.printf("[STAT] AP=%d LC=%d rx659=%lu tx659=%lu raw293=%u txOk=%lu txFail=%lu\n",
                      ap ? 1 : 0, lc ? 1 : 0,
                      (unsigned long)r659, (unsigned long)t659,
                      (unsigned)r293,
                      (unsigned long)tok, (unsigned long)tfail);
    }

    vTaskDelay(pdMS_TO_TICKS(50));
}