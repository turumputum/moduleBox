#ifndef __SPISD_H__
#define __SPISD_H__

#include <stdint.h>

int spisd_get_sector_size();
int spisd_get_sector_num();
int spisd_init();
int spisd_deinit();
int spisd_sectors_read(void * dst, uint32_t start_sector, uint32_t num);
int spisd_sectors_write(void * dst, uint32_t start_sector, uint32_t num);
int spisd_mount_fs();
void spisd_umount_fs();
void spisd_list_root();

/** Захватить мьютекс SD карты (перед прямым sdmmc_read_sectors или VFS операциями,
 *  которые могут конкурировать с USB MSC) */
void sdcard_lock(void);
void sdcard_unlock(void);

/** Пометить том как захваченный USB-хостом: PC записал сектора, кэш FATFS
 *  прошивки протух. С этого момента прошивка на карту не пишет до перезагрузки. */
void sdcard_mark_host_dirty(void);
int sdcard_is_host_dirty(void);

/** Счётчик сбоев обмена с картой (диагностика помех-питания). */
uint32_t sdcard_io_errors(void);

#endif // __SPISD_H__
