#ifndef __UPDATE_H__
#define __UPDATE_H__

#include <stdint.h>

/* Формат файла UPDATE.FW - тот же, что делает bootldsd/bin2fw.c (bin2fw.c включает этот же файл:
   "loader/main/update.h"). Единственный источник формата. */

#define UPDATE_FILENAME     "UPDATE.FW"
#define UPDATE_MAGIC        0x4E464E4D

typedef struct __tag_UPDATEHEAD
{
  uint32_t      magic;      /* UPDATE_MAGIC */
  uint32_t      size;       /* длина полезной нагрузки (moduleBox.bin) в байтах */
  uint32_t      checksum;   /* CRC32 (poly 0xEDB88320, init/xorout 0xFFFFFFFF) полезной нагрузки */
  uint32_t      version;    /* не используется */
} UPDATEHEAD, * PUPDATEHEAD;

#endif
