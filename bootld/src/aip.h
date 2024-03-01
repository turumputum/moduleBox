#ifndef __AIPSD_H__
#define __AIPSD_H__

#include <stdint.h>

#define MOUNT_POINT "/sdcard"

int aip_get_sector_size();
int aip_get_sector_num();
int aip_init();
int aip_deinit();
int aip_sectors_read(void * dst, uint32_t start_sector, uint32_t num);
int aip_sectors_write(void * dst, uint32_t start_sector, uint32_t num);
int aip_mount_fs();
void aip_list_root();


#endif // __AIPSD_H__

