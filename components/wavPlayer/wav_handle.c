// ***************************************************************************
// TITLE
//
// PROJECT
//     moduleBox
// ***************************************************************************

#include "esp_log.h"
#include <string.h>

#include <wav_handle.h>
#include <arsenal.h>

// ---------------------------------------------------------------------------
// ------------------------------- DEFINITIONS -------------------------------
// -----|-------------------|-------------------------------------------------

#define AUDIO_FORMAT_PCM    1            


// ---------------------------------------------------------------------------
// ---------------------------------- TYPES ----------------------------------
// -|-----------------------|-------------------------------------------------

typedef struct _tag_queue_message_t
{
    WAVCMD      command;
    char *      filenamame;
} queue_message_t;

// ---------------------------------------------------------------------------
// -------------------------------- FUNCTIONS --------------------------------
// -----------------|---------------------------(|------------------|---------

  static esp_err_t i2s_setup(wav_handle_t h)
{
  // setup a standard config and the channel
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
  ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &h->audio_ch_handle, NULL));

  // setup the i2s config
  i2s_std_config_t std_cfg = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(h->stage.desc.samplerate),                                  // the wav file sample rate
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(h->stage.desc.width, h->stage.desc.channels),     // the wav faile bit and channel config
      .gpio_cfg = {
          // refer to configuration.h for pin setup
          .mclk = I2S_SCLK_PIN,
          .bclk = I2S_BLK_PIN,
          .ws = I2S_WS_PIN,
          .dout = I2S_DATA_OUT_PIN,
          .din = I2S_DATA_IN_PIN,
          .invert_flags = {
              .mclk_inv = false,
              .bclk_inv = false,
              .ws_inv = false,
          },
      },
  };
  return i2s_channel_init_std_mode(h->audio_ch_handle, &std_cfg);
}
static FILE * wavfile_open(wav_handle_t    h, PWAVDESC        desc, const char *    fname){
    FILE *          result      = NULL;
    wav_header_t    header;
    fmt_header_t    fmt;
    data_header_t   data;

    ESP_LOGI(h->tag, "Attempting to open WAV file: %s", fname);

    if ((result = fopen(fname, "rb")) != nil)
    {
        ESP_LOGD(h->tag, "File opened successfully, reading header...");

        if (fread(&header, sizeof(header), 1, result) != 1)
        {
            ESP_LOGE(h->tag, "Failed to read WAV header from %s", fname);
            fclose(result);
            return NULL;
        }

        if (memcmp(header.riff_header, "RIFF", 4) != 0)
        {
            ESP_LOGE(h->tag, "Invalid RIFF header in %s (expected 'RIFF', got '%.4s')", fname, header.riff_header);
            fclose(result);
            return NULL;
        }

        if (memcmp(header.wave_header, "WAVE", 4) != 0)
        {
            ESP_LOGE(h->tag, "Invalid WAVE header in %s (expected 'WAVE', got '%.4s')", fname, header.wave_header);
            fclose(result);
            return NULL;
        }

        if (memcmp(header.fmt_header, "fmt ", 4) != 0)
        {
            ESP_LOGE(h->tag, "Invalid fmt header in %s (expected 'fmt ', got '%.4s')", fname, header.fmt_header);
            fclose(result);
            return NULL;
        }

        if (fread(&fmt, MAC_MIN(sizeof(fmt_header_t), header.fmt_chunk_size), 1, result) != 1)
        {
            ESP_LOGE(h->tag, "Failed to read fmt chunk from %s", fname);
            fclose(result);
            return NULL;
        }

        // Skip any extra bytes in fmt chunk if it's larger than expected
        if (header.fmt_chunk_size > sizeof(fmt_header_t)){
            long skip_bytes = header.fmt_chunk_size - sizeof(fmt_header_t);
            fseek(result, skip_bytes, SEEK_CUR);
            ESP_LOGD(h->tag, "Skipped %ld extra fmt bytes", skip_bytes);
        }

        // Search for 'data' chunk, skip other chunks like 'LIST', 'INFO', etc.
        int data_found = 0;
        int attempts = 0;
        const int max_attempts = 10;

        while (!data_found && attempts < max_attempts) {
            if (fread(&data, sizeof(data_header_t), 1, result) != 1) {
                ESP_LOGE(h->tag, "Failed to read chunk header from %s", fname);
                fclose(result);
                return NULL;
            }
        
            if (memcmp(data.data_header, "data", 4) == 0){
                // Found data chunk
                data_found = 1;
                ESP_LOGI(h->tag, "Found 'data' chunk, size=%lu bytes", data.data_bytes);
            }else{  // <-- ДОБАВИТЬ ФИГУРНЫЕ СКОБКИ
                // Skip this chunk (LIST, INFO, etc.)
                ESP_LOGI(h->tag, "Skipping '%.4s' chunk (%lu bytes)", data.data_header, data.data_bytes);
        
                if (fseek(result, data.data_bytes, SEEK_CUR) != 0)  // <-- ТЕПЕРЬ ТОЛЬКО ДЛЯ НЕ-DATA ЧАНКОВ
                {
                    ESP_LOGE(h->tag, "Failed to skip '%.4s' chunk in %s", data.data_header, fname);
                    fclose(result);
                    return NULL;
                }
            }  // <-- ЗАКРЫТЬ БЛОК else
        
            attempts++;
        }

    if (!data_found){
        ESP_LOGE(h->tag, "No 'data' chunk found in %s", fname);
        fclose(result);
        return NULL;
    }

    if (fmt.audio_format != AUDIO_FORMAT_PCM){
        ESP_LOGE(h->tag, "Unsupported audio format in %s (format=%d, expected PCM=%d)", fname, fmt.audio_format, AUDIO_FORMAT_PCM);
        fclose(result);
        return NULL;
    }

    if (fmt.sample_rate < 8000 || fmt.sample_rate > 44100){
        ESP_LOGE(h->tag, "Unsupported sample rate in %s (rate=%lu, must be 8000-44100)", fname, fmt.sample_rate);
        fclose(result);
        return NULL;
    }

    ESP_LOGI(h->tag, "WAV file %s validated successfully", fname);
    ESP_LOGD(h->tag, "num_channels=%" PRIu16, fmt.num_channels);
    ESP_LOGD(h->tag, "sample_rate=%" PRIu32, fmt.sample_rate);
    ESP_LOGD(h->tag, "byte_rate=%" PRIu32, fmt.byte_rate);
    ESP_LOGD(h->tag, "sample_alignment=%" PRIu16, fmt.sample_alignment);
    ESP_LOGD(h->tag, "bit_depth=%" PRIu16, fmt.bit_depth);
    ESP_LOGD(h->tag, "data_bytes=%" PRIu32, data.data_bytes);

    desc->channels      = fmt.num_channels;
    desc->samplerate    = fmt.sample_rate;
    desc->width         = fmt.bit_depth;
    desc->len           = data.data_bytes;
    }
    else
    {
    ESP_LOGE(h->tag, "Failed to fopen() file %s - file does not exist or cannot be accessed", fname);
    }

    return result;
}
static void _stopCurrent(wav_handle_t    h)
{
    if (h->stage.fin)
    {
        fclose(h->stage.fin);

        h->stage.fin = 0;
    }

    if (h->enabled)
    {
        i2s_channel_disable(h->audio_ch_handle);
        i2s_del_channel(h->audio_ch_handle); // delete the channel

        h->enabled = false;
    }

    h->stage.mode = WAVCMD_silence;

    ESP_LOGD(h->tag, "stopped");
}
static int _openNew(wav_handle_t    h, 
                    char *             fname)
{
    int         result      = 0;
    
    if ((h->stage.fin = wavfile_open(h, &h->stage.desc, fname)) != NULL)
    {
        i2s_setup(h);
        i2s_channel_enable(h->audio_ch_handle);
        h->enabled      = true;

        h->stage.left   = h->stage.desc.len;
        result          = 1;
    }
    else
        ESP_LOGE(h->tag, "Failed to open file %s", fname);

    return result;
}
static int firstTime = 0;
static void _setVolume(wav_handle_t    h, 
                       int size)
{
    if (h->stage.volume  != 100)
    {
        float coef = (float)h->stage.volume / 100;

        if (h->stage.desc.width == 8)
        {
            int8_t* on = (int8_t*)h->buf;

            for (size_t i = 0; i < size; i++, on++)
            {
                *on *= coef;
            }
        }
        else
        {
            int16_t* on = (int16_t*)h->buf;

            for (size_t i = 0; i < (size >> 1); i++, on++)
            {
                *on *= coef;
            }

            firstTime++;
        }
    }
}
static int _playCurrent(wav_handle_t    h)
{
    int         result          = -1;
    size_t      bytes_written;

    if (h->stage.fin != NULL)
    {
        if (h->stage.left)
        {
            if ((result = fread(h->buf, 1, MAC_MIN(WAV_BUF_SIZE, h->stage.left), h->stage.fin)) > 0)
            {
                _setVolume(h, result);

                i2s_channel_write(h->audio_ch_handle, h->buf, result, &bytes_written, portMAX_DELAY);

                h->stage.left -= result;
            }
        }
        else
            result = 0;

        //ESP_LOGE(h->tag, "offset: %d, result: %d\n", (int)ftell(h->stage.fin), result);
    }

    return result;
}
wav_handle_t wav_handle_init(const char * tag)
{
    wav_handle_t            result  = 1;

    if ((result = calloc(1, sizeof(struct wav_handle))) != nil)
    {
        if (   ((result->buf = malloc(WAV_BUF_SIZE)) != nil)                            &&
               ((result->queue = xQueueCreate(5, sizeof(queue_message_t))) != nil)      )
        {
            result->stage.volume = 100;
            result->tag = tag;
        }
        else
            result = wav_handle_deinit(result);
    }

    return result;
}
int wav_handle_turn(wav_handle_t h)
{
    int                     result  = result;
    queue_message_t         msg;

    if (xQueueReceive(h->queue, &msg, 0) == pdPASS)
    {
        switch (msg.command)
        {
            case WAVCMD_play:
                _stopCurrent(h);
                if (_openNew(h, msg.filenamame))
                {
                    h->stage.mode = WAVCMD_play;
                }
                break;

            case WAVCMD_stop:
                _stopCurrent(h);
                break;
            
            default:
                break;
        }
    }


    switch (h->stage.mode)
    {
        case WAVCMD_play:
            if (_playCurrent(h) <= 0)
            {
                _stopCurrent(h);
            }

            result = 1;
            break;
        
        default:
            break;
    }

    return result;
}
void wav_handle_stop(wav_handle_t h)
{
    queue_message_t msg = { WAVCMD_stop, nil };
    
    xQueueSend(h->queue, &msg, portMAX_DELAY);
}
wav_handle_t wav_handle_deinit(wav_handle_t h)
{
    if (h)
    {
        _stopCurrent(h);

        if (h->buf)
            free(h->buf);

        if (h->queue)
            vQueueUnregisterQueue(h->queue);

        free(h);
    }

    return nil;
}
void wav_handle_play(wav_handle_t h, char * fname)
{
    queue_message_t msg = { WAVCMD_play, fname };

    xQueueSend(h->queue, &msg, portMAX_DELAY);
}
int wav_handle_is_playing(wav_handle_t h)
{
    return h->stage.mode == WAVCMD_silence ? 0 : 1;    
}
void wav_handle_set_volume(wav_handle_t h, 
                           int volume)
{
    if (volume > 100)
        volume = 100;

    if (volume < 0)
        volume = 0;

    h->stage.volume = volume;
}