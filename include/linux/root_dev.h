/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ROOT_DEV_H_
#define _ROOT_DEV_H_

#include <linux/major.h>
#include <linux/types.h>
#include <linux/kdev_t.h>

enum {
	Root_NFS = MKDEV(UNNAMED_MAJOR, 255),    /* Root_NFS  ==> 0xff      */
	Root_CIFS = MKDEV(UNNAMED_MAJOR, 254),   /* Root_CIFS ==> 0xfe      */
	Root_RAM0 = MKDEV(RAMDISK_MAJOR, 0),     /* Root_RAM0 ==> 0x100000  */
	Root_RAM1 = MKDEV(RAMDISK_MAJOR, 1),     /* Root_RAM1 ==> 0x100001  */
	Root_FD0 = MKDEV(FLOPPY_MAJOR, 0),       /* Root_FD0  ==> 0x200000  */
	Root_HDA1 = MKDEV(IDE0_MAJOR, 1),        /* Root_HDA1 ==> 0x300001  */
	Root_HDA2 = MKDEV(IDE0_MAJOR, 2),        /* Root_HDA2 ==> 0x300002  */
	Root_SDA1 = MKDEV(SCSI_DISK0_MAJOR, 1),  /* Root_SDA1 ==> 0x800001  */
	Root_SDA2 = MKDEV(SCSI_DISK0_MAJOR, 2),  /* Root_SDA2 ==> 0x800002  */
	Root_HDC1 = MKDEV(IDE1_MAJOR, 1),        /* Root_HDC1 ==> 0x1600001 */
	Root_SR0 = MKDEV(SCSI_CDROM_MAJOR, 0),   /* Root_SR0  ==> 0xb00000  */
};

extern dev_t ROOT_DEV;

#endif
