#include "sound.h"

#include "driver/ledc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char* TAG = "sound";

#define BUZZER_GPIO     14
#define LEDC_TIMER      LEDC_TIMER_0
#define LEDC_MODE       LEDC_LOW_SPEED_MODE
#define LEDC_CHANNEL    LEDC_CHANNEL_0
#define LEDC_DUTY_RES   LEDC_TIMER_13_BIT
#define LEDC_MAX_DUTY   ((1 << 13) - 1)

#define SOUND_QUEUE_LENGTH 8

/* Note frequencies (Hz). */
#define NOTE_C4  262
#define NOTE_G4  392
#define NOTE_A4  440
#define NOTE_C5  523
#define NOTE_E5  659
#define NOTE_G5  784
#define NOTE_A5  880

typedef struct {
    uint32_t frequency;
    uint32_t duration_ms;
} Note;

typedef struct {
    uint32_t frequency;
    uint32_t duration_ms;
    uint8_t volume;
} SoundRequest;

static bool s_initialized = false;
static QueueHandle_t s_queue = NULL;

static const Note effect_key_click[] = {
    {NOTE_A5, 30},
};
static const Note effect_menu_select[] = {
    {NOTE_C5, 60}, {NOTE_E5, 60},
};
static const Note effect_connect_ok[] = {
    {NOTE_C5, 90}, {NOTE_E5, 90}, {NOTE_G5, 150},
};
static const Note effect_connect_fail[] = {
    {NOTE_G4, 120}, {NOTE_C4, 200},
};
static const Note effect_chord[] = {
    {NOTE_E5, 50}, {NOTE_A5, 70},
};

static void set_buzzer_duty(uint32_t duty)
{
    ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, duty);
    ledc_update_duty(LEDC_MODE, LEDC_CHANNEL);
}

static void play_note_internal(uint32_t frequency, uint32_t duration_ms, uint8_t volume)
{
    if (volume == 0 || frequency == 0) {
        vTaskDelay(pdMS_TO_TICKS(duration_ms));
        return;
    }

    uint32_t duty = (LEDC_MAX_DUTY * volume) / 100 / 2;
    ledc_set_freq(LEDC_MODE, LEDC_TIMER, frequency);
    set_buzzer_duty(duty);
    vTaskDelay(pdMS_TO_TICKS(duration_ms));
    set_buzzer_duty(0);
}

static void sound_task(void* arg)
{
    (void)arg;
    SoundRequest request;

    while (1) {
        if (xQueueReceive(s_queue, &request, portMAX_DELAY) == pdPASS) {
            play_note_internal(request.frequency, request.duration_ms, request.volume);
        }
    }
}

static void queue_note(uint32_t frequency, uint32_t duration_ms, uint8_t volume)
{
    if (s_queue == NULL) {
        return;
    }

    SoundRequest request = {
        .frequency = frequency,
        .duration_ms = duration_ms,
        .volume = volume,
    };
    xQueueSend(s_queue, &request, 0);
}

static void queue_sequence(const Note* notes, int count, uint8_t volume)
{
    for (int i = 0; i < count; i++) {
        queue_note(notes[i].frequency, notes[i].duration_ms, volume);
    }
}

void sound_init(void)
{
    if (s_initialized) {
        return;
    }

    ledc_timer_config_t timer = {
        .speed_mode = LEDC_MODE,
        .timer_num = LEDC_TIMER,
        .duty_resolution = LEDC_DUTY_RES,
        .freq_hz = 4000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&timer);

    ledc_channel_config_t channel = {
        .speed_mode = LEDC_MODE,
        .channel = LEDC_CHANNEL,
        .timer_sel = LEDC_TIMER,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = BUZZER_GPIO,
        .duty = 0,
        .hpoint = 0,
    };
    ledc_channel_config(&channel);

    s_queue = xQueueCreate(SOUND_QUEUE_LENGTH, sizeof(SoundRequest));
    if (s_queue == NULL) {
        ESP_LOGE(TAG, "Cannot create sound queue");
        return;
    }

    xTaskCreate(sound_task, "sound", 2048, NULL, 4, NULL);
    s_initialized = true;
    ESP_LOGI(TAG, "Sound initialized");
}

void sound_play(SoundType sound)
{
    if (!s_initialized) {
        return;
    }

    switch (sound) {
    case SOUND_KEY_CLICK:
        queue_sequence(effect_key_click, 1, 40);
        break;
    case SOUND_MENU_SELECT:
        queue_sequence(effect_menu_select, 2, 45);
        break;
    case SOUND_CONNECT_OK:
        queue_sequence(effect_connect_ok, 3, 55);
        break;
    case SOUND_CONNECT_FAIL:
        queue_sequence(effect_connect_fail, 2, 50);
        break;
    case SOUND_CHORD:
        queue_sequence(effect_chord, 2, 50);
        break;
    default:
        break;
    }
}
