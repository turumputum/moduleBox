#ifndef __UPDATE_H__
#define __UPDATE_H__

#include <stdint.h>

#define UPDATE_FILENAME     "UPDATE.FW"
#define UPDATE_MAGIC        0x4E464E4D

typedef struct __tag_UPDATEHEAD
{
  uint32_t      magic;
  uint32_t      size;
  uint32_t      checksum;
  uint32_t      version;
} UPDATEHEAD, * PUPDATEHEAD;



#endif
