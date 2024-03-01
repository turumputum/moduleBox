/* 
 * The MIT License (MIT)
 *
 * Copyright (c) 2019 Ha Thach (tinyusb.org)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 */


#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>

#include "board_api.h"
#include "uf2.h"
#include <esp_crc.h>
#include "aip.h"
#include "esp_partition.h"
#include <esp_system.h>
#include <esp_ota_ops.h>
#include "update.h"

#define UPDATE_FILE_WITH_PATH   MOUNT_POINT "/" UPDATE_FILENAME

//--------------------------------------------------------------------+
// MACRO CONSTANT TYPEDEF PROTYPES
//--------------------------------------------------------------------+
//#define USE_DFU_BUTTON    1

static volatile uint32_t _timer_count = 0;

//--------------------------------------------------------------------+
//
//--------------------------------------------------------------------+
static bool check_update_available(void);
       
#define FLASH_BLOCK_SIZE    (SPI_FLASH_SEC_SIZE)


static void update_remove()
{
  remove(MOUNT_POINT "/" UPDATE_FILENAME);
}

int checkFileCrc(FILE * fp, uint32_t checksum)
{
  uint8_t *   buff;
  int         rd;
  uint32_t    cs        = 0;
        
  if ((buff = malloc(FLASH_BLOCK_SIZE)) != 0)
  {
      while ((rd = fread(buff, 1, FLASH_BLOCK_SIZE, fp)) > 0)
      {
          cs = esp_crc32_le(cs, buff, rd);
      }

      free(buff);
  }
    
  return cs == checksum;
}
static const char * PROGRAM_LEBEL = "program";

void update_appy()
{
    FILE *            fp;
    uint8_t *         buff;
    int               rd;
    uint32_t          addr = 0;
    esp_partition_t * part = NULL;

    printf("Applying update ...\n");

    if ((fp = fopen(MOUNT_POINT "/" UPDATE_FILENAME, "rb")) != 0)
    {
      if ((buff = malloc(FLASH_BLOCK_SIZE)) != 0)
      {
        //if ((part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "storage")) != NULL)
        if ((part = (esp_partition_t*)esp_partition_find_first(0, ESP_PARTITION_SUBTYPE_APP_OTA_0, PROGRAM_LEBEL)) != NULL)
        {
          //part->no_protect
        
          fseek(fp, sizeof(UPDATEHEAD), SEEK_SET);
        
          while ((rd = fread(buff, 1, FLASH_BLOCK_SIZE, fp)) > 0)
          {
            if (rd < FLASH_BLOCK_SIZE)
            {
              //memset(buff + rd, 0xff, FLASH_BLOCK_SIZE - rd);
            }

            esp_partition_erase_range(part, addr, FLASH_BLOCK_SIZE);
            esp_partition_write(part, addr, buff, FLASH_BLOCK_SIZE);

            addr += rd;
          }
        }

        printf("Done.\n");

        free(buff);
      }
      else
        printf("UPDATE: cannot open update file\n");

      fclose(fp);
    }
    else
      printf("UPDATE: cannot open update file\n");
}
int update_check()
{
    int         result = 0;
    FILE *      fp;
    UPDATEHEAD  head;

    if ((fp = fopen(UPDATE_FILE_WITH_PATH, "rb")) != 0)
    {
      fseek(fp, 0, SEEK_END);
      int fullSize = ftell(fp);
      fseek(fp, 0, SEEK_SET);

      printf("update_check: %s found, size if %d\n", UPDATE_FILE_WITH_PATH, fullSize);

      if (fread(&head, sizeof(UPDATEHEAD), 1, fp))
      {
          if ((fullSize - sizeof(UPDATEHEAD)) == head.size)
          {
            printf("Checking update file ...\n");
            if (checkFileCrc(fp, head.checksum))
            {
              result = 1;
            }
            else
              printf("update_check: CRC failed\n");
          }
          else
            printf("update_check: size and header info mismatch\n");
      }
          
      fseek(fp, sizeof(UPDATEHEAD), SEEK_SET);
          
      fclose(fp);
    }
    else
        printf("Update %s not found\n", UPDATE_FILE_WITH_PATH);

    return result;
}


int main(void)
{
  esp_partition_t const * part;

  board_init();
  
  printf("bootld...\n");

  aip_init();

// #if BOOTLDSD_PROTECT_BOOTLOADER
//   board_flash_protect_bootloader(true);
// #endif

  check_update_available();

  esp_restart();
}


// return true if start DFU mode, else App mode
static bool check_update_available(void)
{
  if (update_check())
  {
    update_appy();
    update_remove();
    esp_restart();
  }

  return false;
}

//--------------------------------------------------------------------+
// Indicator
//--------------------------------------------------------------------+

void board_timer_handler(void)
{
  _timer_count++;

}
