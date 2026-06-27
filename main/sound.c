#include "sound.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/ledc.h"
#include "esp_log.h"

static const char* TAG = "sound";

#define BUZZER_GPIO 14
#define LEDC_TIMER LEDC_TIMER_0
#define LEDC_MODE LEDC_LOW_SPEED_MODE
#define LEDC_CHANNEL LEDC_CHANNEL_0
#define LEDC_DUTY_RES LEDC_TIMER_13_BIT
#define LEDC_MAX_DUTY ((1 << 13) - 1)

#define SOUND_QUEUE_LENGTH 8

// Sound request types
typedef enum {
    SOUND_REQUEST_NOTE,
    SOUND_REQUEST_STOP,
} SoundRequestType;

// Sound queue item
typedef struct {
    SoundRequestType type;
    uint32_t frequency;
    uint32_t duration_ms;
    uint8_t volume;
} SoundRequest;

// Sound state
static bool s_initialized = false;
static bool s_effects_enabled = true;
static bool s_music_playing = false;
static uint8_t s_music_volume = 50;  // 0-100
static TaskHandle_t s_music_task = NULL;
static TaskHandle_t s_effects_task = NULL;
static QueueHandle_t s_sound_queue = NULL;

// Sound effect definitions
typedef struct {
    uint32_t frequency;
    uint32_t duration_ms;
} Note;

// Simple melody for background music (a cheerful tune)
static const Note bg_melody[] = {
    {NOTE_E4, 200}, {NOTE_G4, 200}, {NOTE_A4, 300},
    {NOTE_G4, 100}, {NOTE_E4, 200}, {NOTE_D4, 200},
    {NOTE_E4, 400},
    {NOTE_D4, 200}, {NOTE_F4, 200}, {NOTE_A4, 300},
    {NOTE_F4, 100}, {NOTE_D4, 200}, {NOTE_C4, 200},
    {NOTE_D4, 400},
    {0, 200},  // Pause
};

static const int bg_melody_len = sizeof(bg_melody) / sizeof(bg_melody[0]);

// Sound effect patterns
static const Note effect_key_click[] = {{NOTE_A5, 50}};
static const Note effect_menu_select[] = {{NOTE_C5, 80}, {NOTE_E5, 80}};
static const Note effect_move[] = {{NOTE_C4, 100}};
static const Note effect_capture[] = {{NOTE_G4, 100}, {NOTE_C5, 150}};
static const Note effect_check[] = {{NOTE_E5, 100}, {NOTE_G5, 100}, {NOTE_E5, 100}};
static const Note effect_checkmate[] = {{NOTE_G4, 200}, {NOTE_E4, 200}, {NOTE_C4, 400}};
static const Note effect_game_start[] = {{NOTE_C4, 100}, {NOTE_E4, 100}, {NOTE_G4, 100}, {NOTE_C5, 300}};

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

    // Calculate duty cycle based on volume (50% max for buzzer)
    uint32_t max_duty = (LEDC_MAX_DUTY * volume) / 100 / 2;

    ledc_set_freq(LEDC_MODE, LEDC_TIMER, frequency);
    set_buzzer_duty(max_duty);
    vTaskDelay(pdMS_TO_TICKS(duration_ms));
    set_buzzer_duty(0);
}

static void effects_task(void* arg)
{
    ESP_LOGI(TAG, "Effects task started");
    SoundRequest req;

    while (1) {
        if (xQueueReceive(s_sound_queue, &req, portMAX_DELAY) == pdPASS) {
            if (req.type == SOUND_REQUEST_STOP) {
                set_buzzer_duty(0);
                continue;
            }
            play_note_internal(req.frequency, req.duration_ms, req.volume);
        }
    }
}

static void queue_note(uint32_t frequency, uint32_t duration_ms, uint8_t volume)
{
    if (!s_sound_queue) return;
    SoundRequest req = {
        .type = SOUND_REQUEST_NOTE,
        .frequency = frequency,
        .duration_ms = duration_ms,
        .volume = volume
    };
    xQueueSend(s_sound_queue, &req, 0);  // Non-blocking send
}

static void queue_sequence(const Note* notes, int count, uint8_t volume)
{
    for (int i = 0; i < count; i++) {
        queue_note(notes[i].frequency, notes[i].duration_ms, volume);
    }
}

static void background_music_task(void* arg)
{
    ESP_LOGI(TAG, "Background music task started");

    while (s_music_playing) {
        for (int i = 0; i < bg_melody_len && s_music_playing; i++) {
            play_note_internal(bg_melody[i].frequency, bg_melody[i].duration_ms, s_music_volume);
            if (s_music_playing && i < bg_melody_len - 1) {
                vTaskDelay(pdMS_TO_TICKS(30));
            }
        }
        if (s_music_playing) {
            vTaskDelay(pdMS_TO_TICKS(1000));  // Pause between repeats
        }
    }

    ESP_LOGI(TAG, "Background music task stopped");
    s_music_task = NULL;
    vTaskDelete(NULL);
}

void sound_init(void)
{
    if (s_initialized) return;

    ESP_LOGI(TAG, "Initializing sound system");

    ledc_timer_config_t ledc_timer = {
        .speed_mode = LEDC_MODE,
        .timer_num = LEDC_TIMER,
        .duty_resolution = LEDC_DUTY_RES,
        .freq_hz = 4000,
        .clk_cfg = LEDC_AUTO_CLK
    };
    ledc_timer_config(&ledc_timer);

    ledc_channel_config_t ledc_channel = {
        .speed_mode = LEDC_MODE,
        .channel = LEDC_CHANNEL,
        .timer_sel = LEDC_TIMER,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = BUZZER_GPIO,
        .duty = 0,
        .hpoint = 0
    };
    ledc_channel_config(&ledc_channel);

    // Create sound queue
    s_sound_queue = xQueueCreate(SOUND_QUEUE_LENGTH, sizeof(SoundRequest));

    // Create effects task
    xTaskCreate(effects_task, "SoundEffects", 2048, NULL, 4, &s_effects_task);

    s_initialized = true;
    s_effects_enabled = true;
    ESP_LOGI(TAG, "Sound system initialized");
}

void sound_play(SoundType sound)
{
    if (!s_initialized || !s_effects_enabled) return;

    switch (sound) {
        case SOUND_KEY_CLICK:
            queue_sequence(effect_key_click, 1, 40);
            break;
        case SOUND_MENU_SELECT:
            queue_sequence(effect_menu_select, 2, 50);
            break;
        case SOUND_MOVE:
            queue_sequence(effect_move, 1, 45);
            break;
        case SOUND_CAPTURE:
            queue_sequence(effect_capture, 2, 60);
            break;
        case SOUND_CHECK:
            queue_sequence(effect_check, 3, 55);
            break;
        case SOUND_CHECKMATE:
            queue_sequence(effect_checkmate, 3, 70);
            break;
        case SOUND_GAME_START:
            queue_sequence(effect_game_start, 4, 60);
            break;
        default:
            break;
    }
}

void sound_play_tone(uint32_t frequency, uint32_t duration_ms)
{
    if (!s_initialized || !s_effects_enabled) return;
    queue_note(frequency, duration_ms, 50);
}

void sound_start_background_music(void)
{
    if (!s_initialized) return;
    if (s_music_playing) return;

    // Stop any ongoing effects
    if (s_sound_queue) {
        xQueueReset(s_sound_queue);
    }
    set_buzzer_duty(0);

    s_music_playing = true;
    xTaskCreate(background_music_task, "BgMusic", 2048, NULL, 3, &s_music_task);
    ESP_LOGI(TAG, "Background music started");
}

void sound_stop_background_music(void)
{
    if (!s_music_playing) return;

    s_music_playing = false;
    set_buzzer_duty(0);
    ESP_LOGI(TAG, "Background music stopped");
}

void sound_set_music_volume(uint8_t volume)
{
    if (volume > 100) volume = 100;
    s_music_volume = volume;
    ESP_LOGI(TAG, "Music volume set to %d", volume);
}

void sound_set_effects_enabled(bool enabled)
{
    s_effects_enabled = enabled;
    ESP_LOGI(TAG, "Sound effects %s", enabled ? "enabled" : "disabled");
}
