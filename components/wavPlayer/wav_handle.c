// ***************************************************************************
// TITLE
//
// PROJECT
//     moduleBox
// ***************************************************************************

#include "esp_log.h"

#include <wav_handle.h>
#include <arsenal.h>

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

//static uint8_t buf[WAV_BUF_SIZE];

//i2s_del_channel(c->audio_ch_handle); // delete the channel

static esp_err_t i2s_setup(wav_handle_t h)
{
  // setup a standard config and the channel
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
  ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &h->audio_ch_handle, NULL));

  // setup the i2s config
  i2s_std_config_t std_cfg = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100),                                                    // the wav file sample rate
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO), // the wav faile bit and channel config
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
/*
esp_err_t play_wav(wav_handle_t h, char *fp)
{
  FILE *fh = fopen(fp, "rb");
  if (fh == NULL)
  {
    ESP_LOGE(h->tag, "Failed to open file");
    return ESP_ERR_INVALID_ARG;
  }
  
  // skip the header...
  fseek(fh, 44, SEEK_SET);

  // create a writer buffer
  size_t bytes_read = 0;
  size_t bytes_written = 0;

  bytes_read = fread(buf, sizeof(int16_t), AUDIO_BUFFER, fh);

  i2s_channel_enable(c->audio_ch_handle);

  while (bytes_read > 0)
  {
    // write the buffer to the i2s
    i2s_channel_write(c->audio_ch_handle, buf, bytes_read * sizeof(int16_t), &bytes_written, portMAX_DELAY);
    bytes_read = fread(buf, sizeof(int16_t), AUDIO_BUFFER, fh);
    ESP_LOGV(TAG, "Bytes read: %d", bytes_read);
  }
  
  i2s_channel_disable(c->audio_ch_handle);
  free(buf);

  return ESP_OK;
}
*/

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
        h->enabled = false;
    }

    h->stage.mode = WAVCMD_silence;
}

static int _openNew(wav_handle_t    h, 
                    char *             fname)
{
    int         result      = 0;

    if ((h->stage.fin = fopen(fname, "rb")) != NULL)
    {
        fseek(h->stage.fin, 44, SEEK_SET);

        i2s_channel_enable(h->audio_ch_handle);
        h->enabled   = true;
        result              = 1;
    }
    else
        ESP_LOGE(h->tag, "Failed to open file %s", fname);

    return result;
}
static int _playCurrent(wav_handle_t    h)
{
    int         result          = -1;
    size_t      bytes_written;

    if (h->stage.fin != NULL)
    {

        if ((result = fread(h->buf, 1, WAV_BUF_SIZE, h->stage.fin)) > 0)
        {
            i2s_channel_write(h->audio_ch_handle, h->buf, result, &bytes_written, portMAX_DELAY);
        }

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
            result->tag = tag;
            i2s_setup(result);
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
void wav_handle_play(wav_handle_t h, char * fname)
{
    queue_message_t msg = { WAVCMD_play, fname };
    
    xQueueSend(h->queue, &msg, portMAX_DELAY);
}
wav_handle_t wav_handle_deinit(wav_handle_t h)
{
    if (h)
    {
        if (h->buf)
            free(h->buf);

        if (h->queue)
            vQueueUnregisterQueue(h->queue);

        free(h);
    }

    return nil;
}