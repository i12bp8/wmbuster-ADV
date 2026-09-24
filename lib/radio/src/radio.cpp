// wM-Buster ADV — wM-Bus radio front end (SX1262 / CC1101).
// GPL-3.0
#include "radio/radio.h"

#include <Arduino.h>
#include <SPI.h>
#include <RadioLib.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "config.h"

namespace wmb {

// ---------------------------------------------------------------------------
// Shared state
// ---------------------------------------------------------------------------
static RadioChip s_chip = RadioChip::None;
static RadioOptions s_opt;
static volatile RadioBand s_band = RadioBand::CT;
static volatile RadioBand s_want_band = RadioBand::CT;
static volatile bool s_busy = false;
static QueueHandle_t s_queue = nullptr;
static TaskHandle_t s_task = nullptr;
static RadioStats s_stats;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static RadioCapture s_cap;          // work buffer of the radio task
static uint32_t s_last_sync_ms = 0;

static const uint8_t SYNC_CT[2] = { 0x54, 0x3D };  // T: n*(01) 0000111101, C: + 54CD/543D
static const uint8_t SYNC_S[2] = { 0x76, 0x96 };   // Manchester coded S-mode sync

#define STAT_INC(field) do { portENTER_CRITICAL(&s_mux); s_stats.field++; portEXIT_CRITICAL(&s_mux); } while (0)

static void IRAM_ATTR on_irq() {
    BaseType_t woken = pdFALSE;
    if (s_task) vTaskNotifyGiveFromISR(s_task, &woken);
    if (woken) portYIELD_FROM_ISR();
}

static inline int64_t now_us() { return esp_timer_get_time(); }

// Sleep most of the time, busy wait the last stretch for accuracy.
static void wait_until_us(int64_t t) {
    for (;;) {
        int64_t d = t - now_us();
        if (d <= 0) return;
        if (d > 2500) vTaskDelay(pdMS_TO_TICKS((uint32_t)((d - 1500) / 1000)));
        else delayMicroseconds((uint32_t)d);
    }
}

static uint32_t byte_us(RadioBand b) { return b == RadioBand::CT ? 80 : 245; }
// Bytes needed before capture_expected_len() can tell the frame length.
static size_t head_len(RadioBand b) { return b == RadioBand::CT ? 3 : 24; }

static void push_capture(RadioCapture* c) {
    portENTER_CRITICAL(&s_mux);
    s_stats.captures++;
    if (c->len > s_stats.max_len) s_stats.max_len = c->len;
    portEXIT_CRITICAL(&s_mux);
    if (xQueueSend(s_queue, c, 0) != pdTRUE) STAT_INC(dropped);
}

// ---------------------------------------------------------------------------
// SX1262 (RadioLib)
// ---------------------------------------------------------------------------
class WmbSX1262 : public SX1262 {
public:
    explicit WmbSX1262(Module* m) : SX1262(m) {}
    using SX126x::readBuffer;
};

static Module* s_mod = nullptr;
static WmbSX1262* s_sx = nullptr;
static float s_tcxo = 1.6f;

static bool sx_start_rx() {
    RadioLibIrqFlags_t f = (1UL << RADIOLIB_IRQ_SYNC_WORD_VALID) | (1UL << RADIOLIB_IRQ_RX_DONE);
    return s_sx->startReceive(RADIOLIB_SX126X_RX_TIMEOUT_INF, f, f, 0) == RADIOLIB_ERR_NONE;
}

static bool sx_config(RadioBand band) {
    bool ct = band == RadioBand::CT;
    float freq = ct ? 868.95f : 868.30f;
    float br = ct ? 100.0f : 32.768f;
    float bw = ct ? s_opt.sx_rx_bw_khz : 187.2f;
    // Preamble 8 -> 8 bit preamble detector: short C-mode preambles lock too.
    int16_t st = s_sx->beginFSK(freq, br, 50.0f, bw, 10, 8, s_tcxo, false);
    if (st != RADIOLIB_ERR_NONE) {
        Serial.printf("[RF] SX1262 beginFSK(%.2f) failed: %d\n", (double)freq, st);
        return false;
    }
    s_sx->setDataShaping(RADIOLIB_SHAPING_NONE);
    s_sx->setEncoding(RADIOLIB_ENCODING_NRZ);  // also switches whitening off
    s_sx->setCRC(0);                           // DLL CRCs are checked in software
    uint8_t sync[2];
    memcpy(sync, ct ? SYNC_CT : SYNC_S, 2);
    s_sx->setSyncWord(sync, 2);
    s_sx->fixedPacketLengthMode(255);          // upper bound; reads normally stop earlier
    if (s_opt.sx_boosted_gain) s_sx->setRxBoostedGainMode(true);
    s_sx->setDio1Action(on_irq);
    return sx_start_rx();
}

static bool sx_probe() {
    pinMode(PIN_LORA_NSS, OUTPUT);
    digitalWrite(PIN_LORA_NSS, HIGH);
    if (s_opt.rf_switch && !s_opt.rf_switch()) Serial.println("[RF] RF switch expander not found");
    s_mod = new Module(PIN_LORA_NSS, PIN_LORA_IRQ, PIN_LORA_RESET, PIN_LORA_BUSY, SPI,
                       SPISettings(8000000, MSBFIRST, SPI_MODE0));
    s_sx = new WmbSX1262(s_mod);
    // The Cap LoRa-1262 runs its SX1262 from a TCXO on DIO3; fall back to a
    // plain crystal if that fails.
    static const float TCXO[] = { 1.6f, 1.8f, 3.0f, 0.0f };
    for (float v : TCXO) {
        s_tcxo = v;
        if (s_sx->beginFSK(868.95f, 100.0f, 50.0f, 234.3f, 10, 8, v, false) == RADIOLIB_ERR_NONE) return true;
    }
    delete s_sx;
    delete s_mod;
    s_sx = nullptr;
    s_mod = nullptr;
    return false;
}

static void sx_restart() {
    if (!sx_start_rx()) {
        // Something is badly off (brown out, SPI glitch): configure again.
        sx_config(s_band);
    }
    s_busy = false;
}

static void sx_handle() {
    uint32_t irq = s_sx->getIrqFlags();
    if (!(irq & (RADIOLIB_SX126X_IRQ_SYNC_WORD_VALID | RADIOLIB_SX126X_IRQ_RX_DONE))) {
        if (irq) s_sx->clearIrqFlags(RADIOLIB_SX126X_IRQ_ALL);  // unexpected flags keep DIO1 high
        return;
    }
    int64_t t0 = now_us();
    s_busy = true;
    STAT_INC(syncs);
    s_last_sync_ms = millis();
    RadioCapture* c = &s_cap;
    c->band = s_band;
    c->t_ms = millis();
    c->rssi_dbm = (int16_t)s_sx->getRSSI(false);
    uint32_t bu = byte_us(c->band);
    int need = 255;
    if (!(irq & RADIOLIB_SX126X_IRQ_RX_DONE)) {
        size_t head = head_len(c->band);
        wait_until_us(t0 + (int64_t)(head + 1) * bu + 150);
        s_sx->readBuffer(c->data, (uint8_t)head, 0);
        need = capture_expected_len(c->data, head, c->band);
        if (need <= 0) {
            STAT_INC(noise);
            sx_restart();
            return;
        }
        if (need > 255) need = 255;
        wait_until_us(t0 + (int64_t)(need + 1) * bu + 200);
    }
    s_sx->readBuffer(c->data, (uint8_t)need, 0);
    c->len = (uint16_t)need;
    sx_restart();
    push_capture(c);
}

static int16_t sx_rssi_now() { return (int16_t)s_sx->getRSSI(false); }

// ---------------------------------------------------------------------------
// CC1101 (raw SPI, infinite packet mode)
// ---------------------------------------------------------------------------
enum : uint8_t {
    CC_IOCFG2 = 0x00, CC_IOCFG0 = 0x02, CC_SYNC1 = 0x04, CC_SYNC0 = 0x05, CC_FSCTRL1 = 0x0B,
    CC_FREQ2 = 0x0D, CC_FREQ1 = 0x0E, CC_FREQ0 = 0x0F, CC_MDMCFG4 = 0x10, CC_MDMCFG3 = 0x11,
    CC_DEVIATN = 0x15,
    CC_SRES = 0x30, CC_SCAL = 0x33, CC_SRX = 0x34, CC_SIDLE = 0x36, CC_SFRX = 0x3A,
    CC_PARTNUM = 0x30, CC_VERSION = 0x31, CC_RSSI = 0x34, CC_MARCSTATE = 0x35, CC_RXBYTES = 0x3B,
    CC_FIFO = 0x3F,
};

// Register values for wM-Bus C/T-mode reception (TI DN022 based, as used by
// the esphome wmbus components): 2-FSK, ~103 kBaud, 325 kHz channel filter,
// 16/16 sync word 0x543D, infinite packet length, GDO0 = sync detected.
static const uint8_t CC_REGS_CT[0x2F] = {
    0x2E, 0x2E, 0x06, 0x07, 0x54, 0x3D, 0xFF, 0x00, 0x02, 0x00, 0x00, 0x08, 0x00, 0x21, 0x6B, 0xD0,
    0x5C, 0x04, 0x06, 0x22, 0xF8, 0x44, 0x07, 0x00, 0x18, 0x2E, 0xBF, 0x43, 0x09, 0xB5, 0x87, 0x6B,
    0xFB, 0xB6, 0x10, 0xEA, 0x2A, 0x00, 0x1F, 0x41, 0x00, 0x59, 0x7F, 0x3F, 0x81, 0x35, 0x09,
};

static const SPISettings CC_SPI(4000000, MSBFIRST, SPI_MODE0);

static uint8_t cc_xfer(uint8_t addr, uint8_t v) {
    SPI.beginTransaction(CC_SPI);
    digitalWrite(PIN_CC1101_CS, LOW);
    SPI.transfer(addr);
    uint8_t r = SPI.transfer(v);
    digitalWrite(PIN_CC1101_CS, HIGH);
    SPI.endTransaction();
    return r;
}

static uint8_t cc_strobe(uint8_t cmd) {
    SPI.beginTransaction(CC_SPI);
    digitalWrite(PIN_CC1101_CS, LOW);
    uint8_t r = SPI.transfer(cmd);
    digitalWrite(PIN_CC1101_CS, HIGH);
    SPI.endTransaction();
    return r;
}

static void cc_write(uint8_t addr, uint8_t v) { cc_xfer(addr, v); }
static uint8_t cc_status(uint8_t addr) { return cc_xfer(addr | 0xC0, 0); }

// Status registers can be read while they change: read until two agree.
static uint8_t cc_status_stable(uint8_t addr) {
    uint8_t a = cc_status(addr), b;
    for (int i = 0; i < 4; ++i) {
        b = cc_status(addr);
        if (a == b) return a;
        a = b;
    }
    return a;
}

static void cc_read_fifo(uint8_t* buf, size_t n) {
    SPI.beginTransaction(CC_SPI);
    digitalWrite(PIN_CC1101_CS, LOW);
    SPI.transfer(CC_FIFO | 0xC0);
    for (size_t i = 0; i < n; ++i) buf[i] = SPI.transfer(0);
    digitalWrite(PIN_CC1101_CS, HIGH);
    SPI.endTransaction();
}

static int16_t cc_rssi_dbm() {
    uint8_t r = cc_status(CC_RSSI);
    int v = r >= 128 ? (int)r - 256 : r;
    return (int16_t)(v / 2 - 74);
}

static void cc_rx_on() {
    cc_strobe(CC_SIDLE);
    cc_strobe(CC_SFRX);
    cc_strobe(CC_SRX);
}

static bool cc_config(RadioBand band) {
    cc_strobe(CC_SIDLE);
    for (uint8_t a = 0; a < sizeof(CC_REGS_CT); ++a) cc_write(a, CC_REGS_CT[a]);
    uint32_t hz = band == RadioBand::CT ? 868950000u : 868300000u;
    uint32_t f = (uint32_t)(((uint64_t)hz << 16) / 26000000u);
    cc_write(CC_FREQ2, (uint8_t)(f >> 16));
    cc_write(CC_FREQ1, (uint8_t)(f >> 8));
    cc_write(CC_FREQ0, (uint8_t)f);
    if (band == RadioBand::S) {
        // 32.768 kchip/s, 203 kHz filter, +-50 kHz, Manchester chips kept raw.
        cc_write(CC_MDMCFG4, 0x8A);
        cc_write(CC_MDMCFG3, 0x4A);
        cc_write(CC_DEVIATN, 0x50);
        cc_write(CC_FSCTRL1, 0x06);
        cc_write(CC_SYNC1, SYNC_S[0]);
        cc_write(CC_SYNC0, SYNC_S[1]);
    }
    cc_strobe(CC_SCAL);
    delay(2);
    cc_rx_on();
    return true;
}

static bool cc_probe() {
    pinMode(PIN_CC1101_CS, OUTPUT);
    digitalWrite(PIN_CC1101_CS, HIGH);
    delay(1);
    cc_strobe(CC_SRES);
    delay(5);
    uint8_t part = cc_status(CC_PARTNUM);
    uint8_t ver = cc_status(CC_VERSION);
    Serial.printf("[RF] CC1101 probe: part=%02X version=%02X\n", part, ver);
    return part == 0x00 && (ver == 0x04 || ver == 0x14 || ver == 0x17);
}

static void cc_handle() {
    int64_t t0 = now_us();
    s_busy = true;
    STAT_INC(syncs);
    s_last_sync_ms = millis();
    RadioCapture* c = &s_cap;
    c->band = s_band;
    c->t_ms = millis();
    c->rssi_dbm = cc_rssi_dbm();
    uint32_t bu = byte_us(c->band);
    size_t head = head_len(c->band);
    size_t total = 0, need = 0;
    int64_t deadline = t0 + (int64_t)(RADIO_CAPTURE_MAX + 16) * bu;
    bool done = false;
    for (;;) {
        uint8_t rb = cc_status_stable(CC_RXBYTES);
        if (rb & 0x80) {
            STAT_INC(overflows);
            break;
        }
        size_t avail = rb & 0x7F;
        size_t want = (need ? need : RADIO_CAPTURE_MAX) - total;
        size_t take = avail < want ? avail : want;
        // Errata: never empty the RX FIFO while more bytes are coming.
        if (take == avail && take < want) take = avail > 0 ? avail - 1 : 0;
        if (take) {
            cc_read_fifo(c->data + total, take);
            total += take;
        }
        if (!need && total >= head) {
            int e = capture_expected_len(c->data, total, c->band);
            if (e < 0) {
                STAT_INC(noise);
                break;
            }
            if (e > 0) need = (size_t)e < RADIO_CAPTURE_MAX ? (size_t)e : RADIO_CAPTURE_MAX;
        }
        if (need && total >= need) {
            done = true;
            break;
        }
        if (now_us() > deadline) {
            STAT_INC(timeouts);
            break;
        }
        size_t target = need ? need : head;
        size_t remaining = target > total ? target - total : 1;
        // The 64 byte FIFO lasts 5 ms at 100 kchip/s: poll often enough.
        if (remaining * bu > 3000) vTaskDelay(1);
        else delayMicroseconds(remaining * bu / 2 + 50);
    }
    cc_rx_on();
    s_busy = false;
    if (done) {
        c->len = (uint16_t)total;
        push_capture(c);
    }
}

static int16_t cc_rssi_now() { return cc_rssi_dbm(); }

// ---------------------------------------------------------------------------
// Task
// ---------------------------------------------------------------------------
static void apply_band(RadioBand b) {
    bool ok = s_chip == RadioChip::SX1262 ? sx_config(b) : cc_config(b);
    if (ok) {
        s_band = b;
        Serial.printf("[RF] %s band active\n", radio_band_name(b));
    }
}

static void radio_task(void*) {
    int32_t floor_acc = 0;
    int floor_n = 0;
    uint32_t last_floor = 0;
    for (;;) {
        uint32_t n = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(250));
        if (s_want_band != s_band) apply_band(s_want_band);
        if (n) {
            if (s_chip == RadioChip::SX1262) sx_handle();
            else if (digitalRead(PIN_CC1101_IRQ)) cc_handle();
            continue;
        }
        // A missed edge leaves the IRQ line high: handle it now.
        if (s_chip == RadioChip::SX1262 && digitalRead(PIN_LORA_IRQ)) {
            sx_handle();
            continue;
        }
        if (s_chip == RadioChip::CC1101 && digitalRead(PIN_CC1101_IRQ)) {
            cc_handle();
            continue;
        }
        // Idle: sample the noise floor, re-arm a receiver that went quiet.
        int16_t r = s_chip == RadioChip::SX1262 ? sx_rssi_now() : cc_rssi_now();
        if (r > -160 && r < 0) {
            floor_acc += r;
            floor_n++;
        }
        uint32_t now = millis();
        if (now - last_floor > 2000 && floor_n > 0) {
            portENTER_CRITICAL(&s_mux);
            s_stats.noise_floor = (int16_t)(floor_acc / floor_n);
            portEXIT_CRITICAL(&s_mux);
            floor_acc = 0;
            floor_n = 0;
            last_floor = now;
        }
        if (now - s_last_sync_ms > 60000) {
            s_last_sync_ms = now;
            STAT_INC(restarts);
            if (s_chip == RadioChip::SX1262) sx_restart();
            else cc_rx_on();
        }
    }
}

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------
bool radio_begin(const RadioOptions& opt) {
    if (s_task) return s_chip != RadioChip::None;
    s_opt = opt;
    memset(&s_stats, 0, sizeof(s_stats));
    SPI.begin(PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI, -1);  // no-op when already running
    s_stats.noise_floor = -120;
    // Keep the SD card deselected while probing the shared bus.
    pinMode(PIN_SD_CS, OUTPUT);
    digitalWrite(PIN_SD_CS, HIGH);

    // The Hydra RF cap wires CC1101 GDO0 to G5, the SX1262 chip select of
    // the LoRa cap, so the CC1101 is probed first (G13 is harmless for both).
    if (opt.hw != RadioHw::SX1262 && cc_probe()) {
        s_chip = RadioChip::CC1101;
    } else if (opt.hw != RadioHw::CC1101 && sx_probe()) {
        s_chip = RadioChip::SX1262;
    } else {
        Serial.println("[RF] no supported radio found");
        return false;
    }
    Serial.printf("[RF] %s detected\n", radio_chip_name());

    s_queue = xQueueCreate(4, sizeof(RadioCapture));
    if (!s_queue) return false;
    s_band = opt.band;
    s_want_band = opt.band;
    if (s_chip == RadioChip::CC1101) {
        cc_config(opt.band);
        pinMode(PIN_CC1101_IRQ, INPUT);
        attachInterrupt(digitalPinToInterrupt(PIN_CC1101_IRQ), on_irq, RISING);
    } else if (!sx_config(opt.band)) {
        s_chip = RadioChip::None;
        return false;
    }
    s_last_sync_ms = millis();
    // Above the Arduino loop and the network stack, pinned next to the loop
    // task so WiFi interrupts on core 0 cannot delay the timed reads.
    xTaskCreatePinnedToCore(radio_task, "radio", 4096, nullptr, configMAX_PRIORITIES - 3, &s_task, 1);
    // An interrupt that fired before the task existed was dropped: let the
    // task look at the IRQ state once.
    if (s_task) xTaskNotifyGive(s_task);
    return s_task != nullptr;
}

RadioChip radio_chip() { return s_chip; }
bool radio_ready() { return s_chip != RadioChip::None && s_task; }

const char* radio_chip_name() {
    switch (s_chip) {
    case RadioChip::SX1262: return "SX1262";
    case RadioChip::CC1101: return "CC1101";
    default: return "none";
    }
}

const char* radio_band_name(RadioBand band) { return band == RadioBand::CT ? "C1/T1" : "S1"; }

void radio_set_band(RadioBand band) {
    s_want_band = band;
    if (s_task) xTaskNotifyGive(s_task);
}

RadioBand radio_band() { return s_band; }

bool radio_receive(RadioCapture* out, uint32_t wait_ms) {
    if (!s_queue) return false;
    return xQueueReceive(s_queue, out, pdMS_TO_TICKS(wait_ms)) == pdTRUE;
}

void radio_get_stats(RadioStats* out) {
    portENTER_CRITICAL(&s_mux);
    *out = s_stats;
    portEXIT_CRITICAL(&s_mux);
}

bool radio_busy() { return s_busy; }

} // namespace wmb
