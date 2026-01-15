#ifndef _WAV_HANDLE_H_
#define _WAV_HANDLE_H_

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <stdint.h>
#include <stdio.h>
#include "esp_audio.h"
#include "driver/i2s_std.h"

// ---------------------------------------------------------------------------
// ------------------------------- DEFINITIONS -------------------------------
// -----|-------------------|-------------------------------------------------

#define WAV_BUF_SIZE 4096

// I2S Configuration
#define I2S_BLK_PIN GPIO_NUM_4
#define I2S_WS_PIN GPIO_NUM_5
#define I2S_DATA_OUT_PIN GPIO_NUM_10
#define I2S_DATA_IN_PIN I2S_GPIO_UNUSED
#define I2S_SCLK_PIN I2S_GPIO_UNUSED

// ---------------------------------------------------------------------------
// ---------------------------------- TYPES ----------------------------------
// -|-----------------------|-------------------------------------------------

typedef enum
{
    WAVCMD_silence       = 0,
    WAVCMD_stop,
    WAVCMD_play
} WAVCMD;


typedef struct wav_handle* wav_handle_t;


/**
 * @brief Descriptor for a WAV resource.
 *
 * The `wav_obj_t` contains a `type` field indicating where the WAV data
 * is located and a union with storage-specific metadata. For embedded
 * data, provide a pointer to the raw WAV bytes. For SPIFFS/MMC, provide
 * the path to the file.
 */
typedef struct {
    union {
        struct {
            const uint8_t *addr; /*!< Pointer to embedded WAV data in flash/ROM. */
        } embed;
        struct {
            const char *path; /*!< Path to WAV file inside SPIFFS. */
        } spiffs;
        struct {
            const char *path; /*!< Path to WAV file on MMC/SD card. */
        } mmc;
    };
} wav_obj_t;



typedef struct wav_header {
    /* RIFF Header */
    char     riff_header[4]; /*!< Contains the ASCII tag "RIFF". */
    uint32_t wav_size;       /*!< Size of the WAV portion of the file (file size - 8). */
    char     wave_header[4]; /*!< Contains the ASCII tag "WAVE". */

    /* Format Header */
    char     fmt_header[4];    /*!< Contains the ASCII tag "fmt " (includes trailing space). */
    uint32_t fmt_chunk_size;   /*!< Size of the fmt chunk (typically 16 for PCM). */
    uint16_t audio_format;     /*!< Audio format (1 = PCM, 3 = IEEE float). */
    uint16_t num_channels;     /*!< Number of audio channels. */
    uint32_t sample_rate;      /*!< Sampling rate in Hz. */
    uint32_t byte_rate;        /*!< Bytes per second (sample_rate * num_channels * bytes_per_sample). */
    uint16_t sample_alignment; /*!< Block alignment (num_channels * bytes_per_sample). */
    uint16_t bit_depth;        /*!< Bits per sample (e.g. 16). */

    /* Data */
    char     data_header[4]; /*!< Contains the ASCII tag "data". */
    uint32_t data_bytes;     /*!< Number of bytes in the data chunk (samples * frame_size). */
} wav_header_t;

typedef struct _tag_player_stage_t
{
    WAVCMD          mode;
    FILE *          fin;
} player_stage_t;

struct wav_handle {
    const char *        tag;
    void *              buf;
    player_stage_t      stage;
    QueueHandle_t       queue;
    bool                enabled;
    i2s_chan_handle_t 	audio_ch_handle;

    void *ctx;                                              /*!< Backend-specific context pointer. */
    int (*open)(wav_handle_t h);                           /*!< Open/initialize the backend (returns 0 on success). */
    size_t (*read)(wav_handle_t h, void *buf, size_t len); /*!< Read up to `len` bytes into `buf`. */
    int (*seek)(wav_handle_t h, size_t offset);            /*!< Seek to `offset` within the WAV data. */
    void (*close)(wav_handle_t h);                         /*!< Close the backend and release any resources. */
    void (*clean_ctx)(wav_handle_t h);                     /*!< Optional cleanup function for `ctx`. */

    /* Filled by wav_parse_header() */
    uint16_t num_channels;     /*!< Number of audio channels. */
    uint32_t sample_rate;      /*!< Sampling rate in Hz. */
    uint32_t byte_rate;        /*!< Bytes per second (sample_rate * block_align). */
    uint32_t sample_alignment; /*!< Block align: number of bytes per sample frame. */
    uint16_t bit_depth;        /*!< Bits per sample (e.g. 16). */
    size_t   data_start;       /*!< Offset (in bytes) from start of file to audio data. */
    size_t   data_bytes;       /*!< Number of bytes in the audio data chunk. */
};

// ---------------------------------------------------------------------------
// -------------------------------- FUNCTIONS --------------------------------
// -----------------|---------------------------(|------------------|---------

wav_handle_t wav_handle_init(const char * tag);
void wav_handle_stop(wav_handle_t h);
void wav_handle_play(wav_handle_t h, char * fname);
wav_handle_t wav_handle_deinit(wav_handle_t h);
int wav_handle_turn(wav_handle_t h);

#endif // _WAV_HANDLE_H_