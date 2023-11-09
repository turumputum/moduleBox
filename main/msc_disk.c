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

#include "bsp/board.h"
#include "tusb.h"
#include "sd_card.h"
#include "stdbool.h"
#include "esp_log.h"
#include "wear_levelling.h"
#include "ff.h"
#include "diskio.h"
#include "diskio_wl.h"
#include "stateConfig.h"

#define true 1
#define false 0
//#define NULL 0

extern configuration me_config;
extern stateStruct me_state;
extern wl_handle_t s_wl_handle;
uint8_t s_pdrv;

static const char *TAG = "MSD";

uint8_t FLAG_PC_AVAILEBLE = 0;
uint8_t FLAG_PC_EJECT=0;

//#if CFG_TUD_MSC

// Invoked when received SCSI_CMD_INQUIRY
// Application fill vendor id, product id and revision with string up to 8, 16, 4 characters respectively
void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16], uint8_t product_rev[4])
{
  (void) lun;

  const char vid[] = "monofon";
  const char pid[] = "mb";
  const char rev[] = "1.0";

  memcpy(vendor_id  , vid, strlen(vid));
  memcpy(product_id , pid, strlen(pid));
  memcpy(product_rev, rev, strlen(rev));
}

// Invoked when received Test Unit Ready command.
// return true allowing host to read/write this LUN e.g SD card inserted
bool tud_msc_test_unit_ready_cb(uint8_t lun)  // @suppress("Type cannot be resolved")
{
  (void) lun;

  return true; // RAM disk is always ready
}

// Invoked when received SCSI_CMD_READ_CAPACITY_10 and SCSI_CMD_READ_FORMAT_CAPACITY to determine the disk size
// Application update block count and block size
void tud_msc_capacity_cb(uint8_t lun, uint32_t* block_count, uint16_t* block_size)
{
  (void) lun;

  FLAG_PC_AVAILEBLE = 1;
  FLAG_PC_EJECT=0;

  //*block_count = spisd_get_sector_num();
  //*block_size  = spisd_get_sector_size();
  // *block_size = wl_sector_size(s_wl_handle);
  // *block_count = (wl_size(s_wl_handle))/wl_sector_size(s_wl_handle);
  if(me_state.sd_init_res!=ESP_OK){
    disk_ioctl(s_pdrv, GET_SECTOR_COUNT, block_count);
    disk_ioctl(s_pdrv, GET_SECTOR_SIZE, block_size);
    ESP_LOGD(__func__, "GET_SECTOR_COUNT = %ld，GET_SECTOR_SIZE = %d", *block_count, *block_size);
  }else{
    *block_count = spisd_get_sector_num();
    *block_size  = spisd_get_sector_size();
  }
}

// Invoked when received Start Stop Unit command
// - Start = 0 : stopped power mode, if load_eject = 1 : unload disk storage
// - Start = 1 : active mode, if load_eject = 1 : load disk storage
bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject)
{
  (void) lun;
  (void) power_condition;

  if ( load_eject )
  {
    if (start)
    {
      if(me_state.sd_init_res!=ESP_OK){
        s_pdrv = ff_diskio_get_pdrv_wl(s_wl_handle);
      }
    }
    else
    {
    	printf("Eject MSD device \n");
    	FLAG_PC_AVAILEBLE = 0;
    	FLAG_PC_EJECT =1;
      //esp_restart();
    }
  }

  return true;
}

// Callback invoked when received READ10 command.
// Copy disk's data to buffer (up to bufsize) and return number of copied bytes.
int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void* buffer, uint32_t bufsize)
{
    int32_t     result  = -1;

    (void) lun;

    if (!offset)
    {
      if(me_state.sd_init_res!=ESP_OK){
        const uint32_t block_count = bufsize / wl_sector_size(s_wl_handle);
        if (disk_read(s_pdrv,buffer,lba,block_count)==ESP_OK){
          result = bufsize;
        }
      }else{
        if (spisd_sectors_read(buffer, lba, bufsize / spisd_get_sector_size()) > 0){
          result = bufsize;
        }
      }

    }
    
    return result;
}

// Callback invoked when received WRITE10 command.
// Process data in buffer to disk's storage and return number of written bytes
int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t* buffer, uint32_t bufsize)
{
  int32_t    result  = -1;
  
  (void) lun;

    if (!offset)
    {
      if(me_state.sd_init_res!=ESP_OK){
        const uint32_t block_count = bufsize / wl_sector_size(s_wl_handle);
        if (disk_write(s_pdrv,buffer,lba,block_count)==ESP_OK){
          result = bufsize;
        }
      }else{
        if (spisd_sectors_write(buffer, lba, bufsize / spisd_get_sector_size()) > 0)
        {
          result = bufsize;
        }
      }
    }

  return bufsize;
}

// Callback invoked when received an SCSI command not in built-in list below
// - READ_CAPACITY10, READ_FORMAT_CAPACITY, INQUIRY, MODE_SENSE6, REQUEST_SENSE
// - READ10 and WRITE10 has their own callbacks
int32_t tud_msc_scsi_cb (uint8_t lun, uint8_t const scsi_cmd[16], void* buffer, uint16_t bufsize)
{
  // read10 & write10 has their own callback and MUST not be handled here

  void const* response = NULL;
  int32_t resplen = 0;

  // most scsi handled is input
  bool in_xfer = true;

  switch (scsi_cmd[0])
  {
    default:
      // Set Sense = Invalid Command Operation
      tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);

      // negative means error -> tinyusb could stall and/or response with failed status
      resplen = -1;
    break;
  }

  // return resplen must not larger than bufsize
  if ( resplen > bufsize ) resplen = bufsize;

  if ( response && (resplen > 0) )
  {
    if(in_xfer)
    {
      memcpy(buffer, response, resplen);
    }else
    {
      // SCSI output
    }
  }

  return resplen;
}

//#endif
