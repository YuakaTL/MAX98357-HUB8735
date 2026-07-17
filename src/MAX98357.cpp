/*
 * MAX98357.cpp - implementation. See MAX98357.h for pin mapping.
 *
 * The sequence and interrupt model follow Realtek's production
 * module_i2s.c (ameba-rtos-pro2):
 *  - The low layer has no real TX-only mode: run I2S_DIR_TXRX, register
 *    BOTH irq handlers and arm all RX pages, then ignore RX data.
 *  - Pre-fill and send ALL TX pages BEFORE i2s_enable(); enabling with
 *    unowned pages causes an immediate "page own control error" storm.
 *  - The TX-done ISR must always refill and resend a page (silence when
 *    no data is ready) so the DMA never starves.
 *
 * Data path: SD card -> ring buffer (filled in caller context) -> TX-done
 * ISR copies ring slots into DMA pages. SD reads never happen in the ISR.
 */

#include "MAX98357.h"

#include "objects.h"     // struct i2s_s (hal_i2s_adapter_t)
#include "i2s_api.h"     // mbed I2S HAL API, implemented in liboutsrc.a
#include "PinNames.h"

#define I2S_DMA_PAGE_NUM  4
#define I2S_DMA_PAGE_SIZE 1024  // bytes per page, multiple of 64, max 4095

#define RING_SLOTS 4             // page-sized slots between producer and ISR

static i2s_t s_i2s;
static uint8_t s_tx_buf[I2S_DMA_PAGE_NUM * I2S_DMA_PAGE_SIZE] __attribute__((aligned(32)));
static uint8_t s_rx_buf[I2S_DMA_PAGE_NUM * I2S_DMA_PAGE_SIZE] __attribute__((aligned(32)));
static uint8_t s_readbuf[I2S_DMA_PAGE_SIZE];

// SPSC ring: producer = playWav caller context, consumer = TX-done ISR.
static uint8_t s_ring[RING_SLOTS][I2S_DMA_PAGE_SIZE];
static volatile uint32_t s_ring_head = 0;  // total slots produced
static volatile uint32_t s_ring_tail = 0;  // total slots consumed

// Copy the next ring slot (or silence) into a DMA page and hand it back
// to the hardware. Shared by the pre-fill loop and the TX-done ISR.
static void fill_and_send_page(int *page)
{
    if (s_ring_head - s_ring_tail > 0) {
        memcpy(page, s_ring[s_ring_tail % RING_SLOTS], I2S_DMA_PAGE_SIZE);
        s_ring_tail = s_ring_tail + 1;
    } else {
        memset(page, 0, I2S_DMA_PAGE_SIZE);
    }
    i2s_send_page(&s_i2s, (uint32_t *)page);
}

static void tx_done_cb(uint32_t id, char *pbuf)
{
    (void)id;
    (void)pbuf;
    int *page = i2s_get_tx_page(&s_i2s);
    if (page) {
        fill_and_send_page(page);
    }
}

static void rx_done_cb(uint32_t id, char *pbuf)
{
    (void)id;
    (void)pbuf;
    // RX data is unused; just re-arm the page so the RX engine never
    // runs out (the low layer has no TX-only mode).
    i2s_recv_page(&s_i2s);
}

MAX98357::MAX98357() : _vol_q8(256), _inited(false), _sdPin(-1), _err("")
{
}

void MAX98357::setShutdownPin(int pin)
{
    _sdPin = pin;
    if (_sdPin >= 0) {
        pinMode(_sdPin, OUTPUT);
        digitalWrite(_sdPin, LOW);  // hold the amp in shutdown until playback
    }
}

bool MAX98357::begin(void)
{
    if (_inited) {
        return true;
    }

    // Fixed I2S0 pin group: sck, ws, sd_tx, sd_rx, mck.
    // i2s_init() only accepts exactly this set and registers all 5 pins.
    i2s_init(&s_i2s, PF_13, PF_15, PF_14, PF_12, PF_11);
    i2s_set_format(&s_i2s, FORMAT_I2S);
    i2s_set_master(&s_i2s, I2S_MASTER);
    // Standard I2S timing (module_i2s.c "WS negative edge" setting):
    // the SDK default enables SCK inversion, undo it here.
    i2s_set_sck_inv(&s_i2s, 0);
    i2s_set_data_start_edge(&s_i2s, NEGATIVE_EDGE);
    i2s_tx_irq_handler(&s_i2s, tx_done_cb, (uint32_t)&s_i2s);
    i2s_rx_irq_handler(&s_i2s, rx_done_cb, (uint32_t)&s_i2s);

    _inited = true;
    return true;
}

void MAX98357::end(void)
{
    if (!_inited) {
        return;
    }
    i2s_disable(&s_i2s);
    i2s_deinit(&s_i2s);
    _inited = false;
}

void MAX98357::setVolume(float vol)
{
    if (vol < 0.0f) {
        vol = 0.0f;
    }
    if (vol > 1.0f) {
        vol = 1.0f;
    }
    _vol_q8 = (uint16_t)(vol * 256.0f);
}

const char *MAX98357::lastError(void) const
{
    return _err;
}

int MAX98357::mapSampleRate(uint32_t hz)
{
    switch (hz) {
        case 8000:  return SR_8KHZ;
        case 11025: return SR_11p025KHZ;
        case 12000: return SR_12KHZ;
        case 16000: return SR_16KHZ;
        case 22050: return SR_22p05KHZ;
        case 24000: return SR_24KHZ;
        case 32000: return SR_32KHZ;
        case 44100: return SR_44p1KHZ;
        case 48000: return SR_48KHZ;
        case 88200: return SR_88p2KHZ;
        case 96000: return SR_96KHZ;
        default:    return -1;
    }
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

bool MAX98357::parseWavHeader(File &f, uint32_t &sampleRate, uint16_t &channels,
                              uint16_t &bits, uint32_t &dataSize)
{
    uint8_t hdr[12];

    f.seek(0);
    if (f.read(hdr, 12) != 12 || memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) {
        _err = "not a RIFF/WAVE file";
        return false;
    }

    uint16_t audioFormat = 0;
    bool haveFmt = false;

    // Walk chunks until the "data" chunk; leave the file positioned at
    // the first audio byte.
    while (true) {
        uint8_t ch[8];
        if (f.read(ch, 8) != 8) {
            _err = "no data chunk found";
            return false;
        }
        uint32_t csize = le32(ch + 4);

        if (memcmp(ch, "fmt ", 4) == 0) {
            uint8_t fmt[16];
            if (csize < 16 || f.read(fmt, 16) != 16) {
                _err = "bad fmt chunk";
                return false;
            }
            audioFormat = le16(fmt + 0);
            channels    = le16(fmt + 2);
            sampleRate  = le32(fmt + 4);
            bits        = le16(fmt + 14);
            haveFmt = true;
            if (csize > 16) {
                f.seek(f.position() + (csize - 16) + (csize & 1));
            }
        } else if (memcmp(ch, "data", 4) == 0) {
            dataSize = csize;
            break;
        } else {
            f.seek(f.position() + csize + (csize & 1));
        }
    }

    if (!haveFmt) {
        _err = "fmt chunk missing";
        return false;
    }
    if (audioFormat != 1) {
        _err = "only PCM (uncompressed) WAV is supported";
        return false;
    }
    if (bits != 16) {
        _err = "only 16-bit WAV is supported";
        return false;
    }
    if (channels != 1 && channels != 2) {
        _err = "only mono or stereo WAV is supported";
        return false;
    }
    return true;
}

bool MAX98357::playWav(AmebaFatFS &fs, const char *filename)
{
    String path = String(fs.getRootPath()) + filename;
    File f = fs.open(path);
    if (!f) {
        _err = "cannot open file";
        return false;
    }
    bool ok = playWav(f);
    f.close();
    return ok;
}

bool MAX98357::playWav(File &f)
{
    if (!_inited) {
        _err = "begin() not called";
        return false;
    }

    uint32_t sampleRate = 0, dataSize = 0;
    uint16_t channels = 0, bits = 0;
    if (!parseWavHeader(f, sampleRate, channels, bits, dataSize)) {
        return false;
    }

    int sr = mapSampleRate(sampleRate);
    if (sr < 0) {
        _err = "unsupported sample rate";
        return false;
    }

    // Clip dataSize to what is actually left in the file (some encoders
    // write a bogus data chunk size).
    uint32_t remainInFile = f.size() - f.position();
    if (dataSize > remainInFile) {
        dataSize = remainInFile;
    }

    // Re-arm everything on every playback: i2s_disable() at the end of
    // the previous run resets the controller and page ownership.
    i2s_set_dma_buffer(&s_i2s, (char *)s_tx_buf, (char *)s_rx_buf,
                       I2S_DMA_PAGE_NUM, I2S_DMA_PAGE_SIZE);
    // Always output stereo: mono sources are duplicated to both channels.
    i2s_set_param(&s_i2s, CH_STEREO, sr, WL_16b);
    // No TX-only mode in the low layer - run TXRX and discard RX.
    i2s_set_direction(&s_i2s, I2S_DIR_TXRX);

    s_ring_head = 0;
    s_ring_tail = 0;

    // Pre-load the ring so the first DMA pages carry real audio.
    uint32_t remaining = dataSize;
    while ((s_ring_head - s_ring_tail) < RING_SLOTS && remaining > 0) {
        if (!produceSlot(f, remaining, channels)) {
            break;
        }
    }

    // Arm ALL RX pages and pre-send ALL TX pages, then enable - this
    // exact order comes from module_i2s.c / the official i2s example.
    for (int j = 0; j < I2S_DMA_PAGE_NUM; j++) {
        i2s_recv_page(&s_i2s);
        int *page = i2s_get_tx_page(&s_i2s);
        if (page) {
            fill_and_send_page(page);
        }
    }
    i2s_enable(&s_i2s);

    if (_sdPin >= 0) {
        digitalWrite(_sdPin, HIGH);
        // SD_MODE has ~1ms turn-on; clocks are already running by now.
        delay(2);
    }

    // Keep the ring full until the file is exhausted.
    bool ok = true;
    while (remaining > 0) {
        uint32_t t0 = millis();
        while ((s_ring_head - s_ring_tail) >= RING_SLOTS) {
            if ((millis() - t0) > 2000) {
                _err = "I2S TX stalled (no page consumed in 2s)";
                ok = false;
                goto stop;
            }
            delay(1);
        }
        if (!produceSlot(f, remaining, channels)) {
            break;  // early EOF - play what we have
        }
    }

    // Drain: wait for the ISR to consume the ring, then let the last
    // pages (page time = PAGE_SIZE / (rate * 4 bytes) ) flush out.
    {
        uint32_t t0 = millis();
        while ((s_ring_head - s_ring_tail) > 0 && (millis() - t0) < 2000) {
            delay(1);
        }
        uint32_t pageMs = (I2S_DMA_PAGE_SIZE * 1000UL) / (sampleRate * 4UL);
        delay(pageMs * (I2S_DMA_PAGE_NUM + 1) + 5);
    }

stop:
    if (_sdPin >= 0) {
        digitalWrite(_sdPin, LOW);  // hardware shutdown between playbacks
    }
    i2s_disable(&s_i2s);
    return ok;
}

// Read source data for exactly one DMA page, convert (mono->stereo,
// volume) into the next free ring slot and publish it.
// Returns false on EOF/read error. Caller must ensure a free slot exists.
bool MAX98357::produceSlot(File &f, uint32_t &remaining, uint16_t channels)
{
    const uint32_t srcPerPage = (channels == 2) ? I2S_DMA_PAGE_SIZE
                                                : I2S_DMA_PAGE_SIZE / 2;
    uint32_t want = (remaining > srcPerPage) ? srcPerPage : remaining;
    want &= ~(uint32_t)1;  // whole 16-bit samples only
    if (want == 0) {
        remaining = 0;
        return false;
    }

    int got = f.read(s_readbuf, want);
    if (got <= 0) {
        remaining = 0;
        return false;
    }
    remaining -= (uint32_t)got;

    uint8_t *slot = s_ring[s_ring_head % RING_SLOTS];
    int16_t *dst = (int16_t *)slot;
    const int16_t *src = (const int16_t *)s_readbuf;
    int nsamp = got / 2;
    uint32_t filled;

    if (channels == 2) {
        for (int i = 0; i < nsamp; i++) {
            dst[i] = (int16_t)(((int32_t)src[i] * _vol_q8) >> 8);
        }
        filled = (uint32_t)nsamp * 2;
    } else {
        for (int i = 0; i < nsamp; i++) {
            int16_t v = (int16_t)(((int32_t)src[i] * _vol_q8) >> 8);
            dst[2 * i]     = v;
            dst[2 * i + 1] = v;
        }
        filled = (uint32_t)nsamp * 4;
    }
    if (filled < I2S_DMA_PAGE_SIZE) {
        memset(slot + filled, 0, I2S_DMA_PAGE_SIZE - filled);
    }

    // Publish after the slot is fully written (SPSC ring, single writer).
    s_ring_head = s_ring_head + 1;
    return true;
}
