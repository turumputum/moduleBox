#ifndef __UPDATE_H__
#define __UPDATE_H__

#include <stdint.h>

#define UPDATE_FILENAME     "UPDATE.FW"
#define UPDATE_MAGIC        0x4E464E4D

#define UPDATE_PLATFORM_ESP32S3   0
#define UPDATE_PLATFORM_ESP32P4   1


//#pragma pack(1)
typedef struct  __attribute__((aligned(1),packed)) __tag_UPDATEHEAD 
{
  uint32_t      magic;
  uint32_t      size;
  uint32_t      checksum;
  uint16_t      platform;
  uint16_t      version;
} UPDATEHEAD, * PUPDATEHEAD;
//#pragma pack()


#endif
