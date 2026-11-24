// main/sfx.c -- bell-like chimes. PCM writes block, so they run on their own task.
//
// Each note is a sine with two quiet overtones, a few milliseconds of attack and
// an exponential decay: a struck-bar sound rather than a buzzer. The same maths
// lives in tools/sfx_preview.py, which renders the chimes to WAV files.
#include "sfx.h"

#include "bsp_audio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <math.h>
#include <stdint.h>

#define SFX_SAMPLE_RATE 16000
#define SFX_PEAK        9000    // of 32767: leaves headroom on the small speaker
#define SFX_VOLUME      55      // codec output volume, percent
#define SFX_CHUNK       256     // samples per write: 16 ms
#define SFX_MAX_NOTES   5
#define SFX_ATTACK      64      // samples of linear fade-in (4 ms), avoids a click
#define SINE_BITS       8
#define SINE_SIZE       (1 << SINE_BITS)

typedef struct {
    uint16_t hz;       // 0 = rest
    uint16_t ms;       // how long the note is given before the next one starts
    uint16_t decay_ms; // time for the note to fall to about a third of its level
    uint8_t level;     // 0..255 relative loudness
} sfx_note_t;

static const char *TAG = "sfx";

// Pitches are taken from a C major pentatonic so any two chimes sound related.
static const sfx_note_t CHIMES[SFX_COUNT][SFX_MAX_NOTES] = {
    [SFX_BLIP]     = { {1319, 70, 28, 110} },
    [SFX_RING]     = { {784, 170, 90, 220}, {1047, 170, 90, 220}, {1319, 460, 170, 255} },
    [SFX_COIN]     = { {988, 95, 60, 220}, {1319, 560, 210, 255} },
    [SFX_LEVEL_UP] = { {523, 120, 80, 220}, {659, 120, 80, 220}, {784, 120, 80, 220},
                       {1047, 620, 240, 255} },
    [SFX_LOSE]     = { {659, 190, 110, 190}, {523, 460, 190, 190} },
    [SFX_PAID_OUT] = { {1319, 120, 70, 230}, {1568, 600, 230, 255} },
};

static QueueHandle_t s_queue;
static volatile bool s_enabled;
static int16_t s_sine[SINE_SIZE];
static int16_t s_chunk[SFX_CHUNK];

static inline int32_t sine_at(uint32_t phase)
{
    return s_sine[phase >> (32 - SINE_BITS)];
}

static void play_note(const sfx_note_t *note)
{
    const int total = SFX_SAMPLE_RATE * note->ms / 1000;
    const uint32_t step = (uint32_t)(((uint64_t)note->hz << 32) / SFX_SAMPLE_RATE);
    // Per-sample decay factor in Q16: exp(-1 / (decay_ms / 1000 * rate)).
    const uint32_t decay =
        (uint32_t)(65536.0f * expf(-1000.0f / ((float)note->decay_ms * SFX_SAMPLE_RATE)));
    uint32_t envelope = 65536u * note->level / 255;   // Q16 gain, at most 1.0
    uint32_t phase = 0;

    for (int done = 0; done < total; done += SFX_CHUNK) {
        const int n = (total - done) < SFX_CHUNK ? (total - done) : SFX_CHUNK;
        for (int i = 0; i < n; i++) {
            if (note->hz == 0) {
                s_chunk[i] = 0;
                continue;
            }
            // Fundamental plus the octave and the twelfth, weights 180/56/20 of 256.
            int32_t wave = sine_at(phase) * 180 + sine_at(phase * 2) * 56 + sine_at(phase * 3) * 20;
            wave >>= 8;                                            // back to +-32767
            int32_t sample = (int32_t)(((int64_t)wave * envelope) >> 16);
            sample = sample * SFX_PEAK / 32767;
            const int at = done + i;
            if (at < SFX_ATTACK) sample = sample * at / SFX_ATTACK;
            s_chunk[i] = (int16_t)sample;
            phase += step;
            envelope = (uint32_t)(((uint64_t)envelope * decay) >> 16);
        }
        if (bsp_audio_write(s_chunk, (size_t)n * sizeof(int16_t)) != ESP_OK) return;
    }
}

static void sfx_task(void *arg)
{
    (void)arg;
    sfx_id_t id;
    for (;;) {
        if (xQueueReceive(s_queue, &id, portMAX_DELAY) != pdTRUE) continue;
        for (int i = 0; i < SFX_MAX_NOTES && CHIMES[id][i].ms && s_enabled; i++) {
            play_note(&CHIMES[id][i]);
        }
    }
}

esp_err_t sfx_init(void)
{
    for (int i = 0; i < SINE_SIZE; i++) {
        s_sine[i] = (int16_t)(32767.0f * sinf(2.0f * (float)M_PI * (float)i / SINE_SIZE));
    }

    esp_err_t err = bsp_audio_init();
    if (err == ESP_OK) err = bsp_audio_set_format(SFX_SAMPLE_RATE, 16, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "audio unavailable: %s", esp_err_to_name(err));
        return err;
    }
    bsp_audio_set_volume(SFX_VOLUME);

    s_queue = xQueueCreate(4, sizeof(sfx_id_t));
    if (!s_queue) return ESP_ERR_NO_MEM;
    if (xTaskCreate(sfx_task, "sfx", 3072, NULL, 5, NULL) != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void sfx_set_enabled(bool enabled)
{
    s_enabled = enabled;
    if (!enabled && s_queue) xQueueReset(s_queue);   // drop anything still waiting to play
}

bool sfx_enabled(void)
{
    return s_enabled;
}

void sfx_play(sfx_id_t id)
{
    if (!s_enabled || !s_queue || id >= SFX_COUNT) return;
    (void)xQueueSend(s_queue, &id, 0);
}
