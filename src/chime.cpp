#include "chime.h"

#include <Arduino.h>
#include <driver/i2s.h>
#include <math.h>

#include "config.h"
#include "settings.h"

static const i2s_port_t I2S_PORT = I2S_NUM_0;
static const int SAMPLE_RATE = 16000;
static const int FRAMES_PER_CHUNK = 256;
static const int SINE_TABLE_BITS = 10;

static TaskHandle_t chimeTask = nullptr;
static volatile bool playing = false;
static float sineTable[1 << SINE_TABLE_BITS];

// One decaying bell-like note made of a few partials
struct Partial {
    float ratio;     // frequency relative to the fundamental
    float amplitude;
};

static const Partial PARTIALS[] = {
    {1.0f, 1.0f},
    {2.0f, 0.35f},
    {3.0f, 0.12f},
    {4.2f, 0.06f}, // slightly inharmonic, gives the bell character
};
static const int PARTIAL_COUNT = sizeof(PARTIALS) / sizeof(PARTIALS[0]);

struct Note {
    float frequency;
    float startS;
    float decayS; // time constant of the fundamental, overtones decay faster
};

// "Ding" (E5), then "dong" (C5) while the first note still rings
static const Note NOTES[] = {
    {659.25f, 0.0f, 0.5f},
    {523.25f, 0.6f, 0.8f},
};
static const int NOTE_COUNT = sizeof(NOTES) / sizeof(NOTES[0]);
static const float DURATION_S = 2.8f;
static const float FADE_OUT_S = 0.3f;
static const float ATTACK_S = 0.003f;

struct Voice {
    uint32_t phase[PARTIAL_COUNT];
    uint32_t step[PARTIAL_COUNT];
    float envelope[PARTIAL_COUNT];
    float decay[PARTIAL_COUNT]; // per-sample envelope multiplier
    uint32_t startSample;
};

static void initVoices(Voice *voices)
{
    for (int n = 0; n < NOTE_COUNT; n++) {
        voices[n].startSample = (uint32_t)(NOTES[n].startS * SAMPLE_RATE);
        for (int p = 0; p < PARTIAL_COUNT; p++) {
            double freq = NOTES[n].frequency * PARTIALS[p].ratio;
            voices[n].phase[p] = 0;
            voices[n].step[p] = (uint32_t)(freq * 4294967296.0 / SAMPLE_RATE);
            voices[n].envelope[p] = PARTIALS[p].amplitude;
            float tau = NOTES[n].decayS / PARTIALS[p].ratio;
            voices[n].decay[p] = expf(-1.0f / (tau * SAMPLE_RATE));
        }
    }
}

static void playChime()
{
    // Perceived loudness is roughly logarithmic, square the linear setting
    float volume = settings.volume / 100.0f;
    float gain = volume * volume * 32767.0f * 0.5f; // two overlapping notes

    Voice voices[NOTE_COUNT];
    initVoices(voices);

    const uint32_t totalSamples = (uint32_t)(DURATION_S * SAMPLE_RATE);
    const uint32_t fadeStart = (uint32_t)((DURATION_S - FADE_OUT_S) * SAMPLE_RATE);
    const uint32_t attackSamples = (uint32_t)(ATTACK_S * SAMPLE_RATE);
    const int shift = 32 - SINE_TABLE_BITS;
    int16_t buffer[FRAMES_PER_CHUNK * 2]; // stereo frames, same sample on both channels

    i2s_start(I2S_PORT);
    for (uint32_t i = 0; i < totalSamples;) {
        int frames = 0;
        for (; frames < FRAMES_PER_CHUNK && i < totalSamples; frames++, i++) {
            float mix = 0.0f;
            for (int n = 0; n < NOTE_COUNT; n++) {
                Voice &v = voices[n];
                if (i < v.startSample)
                    continue;
                float note = 0.0f;
                for (int p = 0; p < PARTIAL_COUNT; p++) {
                    note += sineTable[v.phase[p] >> shift] * v.envelope[p];
                    v.phase[p] += v.step[p];
                    v.envelope[p] *= v.decay[p];
                }
                uint32_t age = i - v.startSample;
                if (age < attackSamples)
                    note *= (float)age / attackSamples; // avoid a click at the note start
                mix += note;
            }
            if (i >= fadeStart)
                mix *= (float)(totalSamples - i) / (totalSamples - fadeStart);
            float sample = mix * gain;
            sample = sample > 32767.0f ? 32767.0f : sample < -32768.0f ? -32768.0f : sample;
            buffer[frames * 2] = buffer[frames * 2 + 1] = (int16_t)sample;
        }
        size_t written;
        i2s_write(I2S_PORT, buffer, frames * 2 * sizeof(int16_t), &written, portMAX_DELAY);
    }

    // Flush silence through the DMA buffers, then stop the clocks: the
    // MAX98357A shuts down without BCLK, so it doesn't hiss while idle
    memset(buffer, 0, sizeof(buffer));
    size_t written;
    for (int k = 0; k < 4; k++)
        i2s_write(I2S_PORT, buffer, sizeof(buffer), &written, portMAX_DELAY);
    i2s_stop(I2S_PORT);
}

static void chimeTaskFn(void *)
{
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        uint32_t start = millis();
        playChime();
        Serial.printf("Chime played (%lu ms)\n", (unsigned long)(millis() - start));
        playing = false;
    }
}

bool chimeAvailable()
{
    return chimeTask != nullptr;
}

void chimePlay()
{
    if (!chimeTask || playing || settings.volume == 0)
        return;
    playing = true;
    xTaskNotifyGive(chimeTask);
}

void chimeBegin()
{
#if SPEAKER_BCLK_PIN >= 0 && SPEAKER_LRC_PIN >= 0 && SPEAKER_DIN_PIN >= 0
    for (int i = 0; i < (1 << SINE_TABLE_BITS); i++)
        sineTable[i] = sinf(2.0f * (float)M_PI * i / (1 << SINE_TABLE_BITS));

    i2s_config_t config = {};
    config.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
    config.sample_rate = SAMPLE_RATE;
    config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    config.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
    config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    config.dma_buf_count = 4;
    config.dma_buf_len = FRAMES_PER_CHUNK;
    config.tx_desc_auto_clear = true;
    // Never use the APLL: on the ESP32-PoE-ISO it generates the 50 MHz Ethernet
    // clock (GPIO17), retuning it for audio would break Ethernet.
    config.use_apll = false;

    i2s_pin_config_t pins = {};
    pins.mck_io_num = I2S_PIN_NO_CHANGE;
    pins.bck_io_num = SPEAKER_BCLK_PIN;
    pins.ws_io_num = SPEAKER_LRC_PIN;
    pins.data_out_num = SPEAKER_DIN_PIN;
    pins.data_in_num = I2S_PIN_NO_CHANGE;

    if (i2s_driver_install(I2S_PORT, &config, 0, nullptr) != ESP_OK || i2s_set_pin(I2S_PORT, &pins) != ESP_OK) {
        Serial.println("I2S init failed, chime disabled");
        return;
    }
    i2s_stop(I2S_PORT);
    xTaskCreate(chimeTaskFn, "chime", 4096, nullptr, 2, &chimeTask);
#endif
}
