/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/kernel.h>
#include <linux/blkdev.h>
#include <linux/init.h>
#include <linux/syscalls.h>
#include <linux/unistd.h>
#include <linux/slab.h>
#include <linux/mount.h>
#include <linux/major.h>
#include <linux/root_dev.h>
#include <linux/init_syscalls.h>

void  mount_block_root(char *name, int flags);
void  mount_root(void);
extern int root_mountflags;

/* create_dev (name="/dev/root", dev=0x100000) */
/* ROOT_DEV = 0x100000 */
/* Root_NFS  ==> 0xff      */
/* Root_CIFS ==> 0xfe      */
/* Root_RAM0 ==> 0x100000  */
/* Root_RAM1 ==> 0x100001  */
/* Root_FD0  ==> 0x200000  */
/* Root_HDA1 ==> 0x300001  */
/* Root_HDA2 ==> 0x300002  */
/* Root_SDA1 ==> 0x800001  */
/* Root_SDA2 ==> 0x800002  */
/* Root_HDC1 ==> 0x1600001 */
/* Root_SR0  ==> 0xb00000  */
static inline __init int create_dev(char *name, dev_t dev)
{
	init_unlink(name);
	return init_mknod(name, S_IFBLK | 0600, new_encode_dev(dev));
}

#ifdef CONFIG_BLK_DEV_RAM

int __init rd_load_disk(int n);
int __init rd_load_image(char *from);

#else

static inline int rd_load_disk(int n) { return 0; }
static inline int rd_load_image(char *from) { return 0; }

#endif

#ifdef CONFIG_BLK_DEV_INITRD

bool __init initrd_load(void);

#else

static inline bool initrd_load(void) { return false; }

#endif
