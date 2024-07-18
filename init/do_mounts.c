// SPDX-License-Identifier: GPL-2.0-only
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/ctype.h>
#include <linux/fd.h>
#include <linux/tty.h>
#include <linux/suspend.h>
#include <linux/root_dev.h>
#include <linux/security.h>
#include <linux/delay.h>
#include <linux/genhd.h>
#include <linux/mount.h>
#include <linux/device.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/initrd.h>
#include <linux/async.h>
#include <linux/fs_struct.h>
#include <linux/slab.h>
#include <linux/ramfs.h>
#include <linux/shmem_fs.h>

#include <linux/nfs_fs.h>
#include <linux/nfs_fs_sb.h>
#include <linux/nfs_mount.h>
#include <linux/raid/detect.h>
#include <uapi/linux/mount.h>

#include "do_mounts.h"

int root_mountflags = MS_RDONLY | MS_SILENT;
static char * __initdata root_device_name;
static char __initdata saved_root_name[64];
static int root_wait;

dev_t ROOT_DEV;

static int __init load_ramdisk(char *str)
{
	pr_warn("ignoring the deprecated load_ramdisk= option\n");
	return 1;
}
__setup("load_ramdisk=", load_ramdisk);

static int __init readonly(char *str)
{
	if (*str)
		return 0;
	root_mountflags |= MS_RDONLY;
	return 1;
}

static int __init readwrite(char *str)
{
	if (*str)
		return 0;
	root_mountflags &= ~MS_RDONLY;
	return 1;
}

__setup("ro", readonly);
__setup("rw", readwrite);

#ifdef CONFIG_BLOCK
struct uuidcmp {
	const char *uuid;
	int len;
};

/**
 * match_dev_by_uuid - callback for finding a partition using its uuid
 * @dev:	device passed in by the caller
 * @data:	opaque pointer to the desired struct uuidcmp to match
 *
 * Returns 1 if the device matches, and 0 otherwise.
 */
static int match_dev_by_uuid(struct device *dev, const void *data)
{
	const struct uuidcmp *cmp = data;
	struct hd_struct *part = dev_to_part(dev);

	if (!part->info)
		goto no_match;

	if (strncasecmp(cmp->uuid, part->info->uuid, cmp->len))
		goto no_match;

	return 1;
no_match:
	return 0;
}


/**
 * devt_from_partuuid - looks up the dev_t of a partition by its UUID
 * @uuid_str:	char array containing ascii UUID
 *
 * The function will return the first partition which contains a matching
 * UUID value in its partition_meta_info struct.  This does not search
 * by filesystem UUIDs.
 *
 * If @uuid_str is followed by a "/PARTNROFF=%d", then the number will be
 * extracted and used as an offset from the partition identified by the UUID.
 *
 * Returns the matching dev_t on success or 0 on failure.
 */
static dev_t devt_from_partuuid(const char *uuid_str)
{
	dev_t res = 0;
	struct uuidcmp cmp;
	struct device *dev = NULL;
	struct gendisk *disk;
	struct hd_struct *part;
	int offset = 0;
	bool clear_root_wait = false;
	char *slash;

	cmp.uuid = uuid_str;

	slash = strchr(uuid_str, '/');
	/* Check for optional partition number offset attributes. */
	if (slash) {
		char c = 0;
		/* Explicitly fail on poor PARTUUID syntax. */
		if (sscanf(slash + 1,
			   "PARTNROFF=%d%c", &offset, &c) != 1) {
			clear_root_wait = true;
			goto done;
		}
		cmp.len = slash - uuid_str;
	} else {
		cmp.len = strlen(uuid_str);
	}

	if (!cmp.len) {
		clear_root_wait = true;
		goto done;
	}

	dev = class_find_device(&block_class, NULL, &cmp,
				&match_dev_by_uuid);
	if (!dev)
		goto done;

	res = dev->devt;

	/* Attempt to find the partition by offset. */
	if (!offset)
		goto no_offset;

	res = 0;
	disk = part_to_disk(dev_to_part(dev));
	part = disk_get_part(disk, dev_to_part(dev)->partno + offset);
	if (part) {
		res = part_devt(part);
		put_device(part_to_dev(part));
	}

no_offset:
	put_device(dev);
done:
	if (clear_root_wait) {
		pr_err("VFS: PARTUUID= is invalid.\n"
		       "Expected PARTUUID=<valid-uuid-id>[/PARTNROFF=%%d]\n");
		if (root_wait)
			pr_err("Disabling rootwait; root= is invalid.\n");
		root_wait = 0;
	}
	return res;
}

/**
 * match_dev_by_label - callback for finding a partition using its label
 * @dev:	device passed in by the caller
 * @data:	opaque pointer to the label to match
 *
 * Returns 1 if the device matches, and 0 otherwise.
 */
static int match_dev_by_label(struct device *dev, const void *data)
{
	const char *label = data;
	struct hd_struct *part = dev_to_part(dev);

	if (part->info && !strcmp(label, part->info->volname))
		return 1;

	return 0;
}
#endif

/*
 *	Convert a name into device number.  We accept the following variants:
 *
 *	1) <hex_major><hex_minor> device number in hexadecimal represents itself
 *         no leading 0x, for example b302.
 *	2) /dev/nfs represents Root_NFS (0xff)
 *	3) /dev/<disk_name> represents the device number of disk
 *	4) /dev/<disk_name><decimal> represents the device number
 *         of partition - device number of disk plus the partition number
 *	5) /dev/<disk_name>p<decimal> - same as the above, that form is
 *	   used when disk name of partitioned disk ends on a digit.
 *	6) PARTUUID=00112233-4455-6677-8899-AABBCCDDEEFF representing the
 *	   unique id of a partition if the partition table provides it.
 *	   The UUID may be either an EFI/GPT UUID, or refer to an MSDOS
 *	   partition using the format SSSSSSSS-PP, where SSSSSSSS is a zero-
 *	   filled hex representation of the 32-bit "NT disk signature", and PP
 *	   is a zero-filled hex representation of the 1-based partition number.
 *	7) PARTUUID=<UUID>/PARTNROFF=<int> to select a partition in relation to
 *	   a partition with a known unique id.
 *	8) <major>:<minor> major and minor number of the device separated by
 *	   a colon.
 *	9) PARTLABEL=<name> with name being the GPT partition label.
 *	   MSDOS partitions do not support labels!
 *	10) /dev/cifs represents Root_CIFS (0xfe)
 *
 *	If name doesn't have fall into the categories above, we return (0,0).
 *	block_class is used to check if something is a disk name. If the disk
 *	name contains slashes, the device name has them replaced with
 *	bangs.
 */
/* name_to_dev_t("/dev/ram0") */
dev_t name_to_dev_t(const char *name)
{
	char s[32];
	char *p;
	dev_t res = 0;
	int part;

#ifdef CONFIG_BLOCK
	if (strncmp(name, "PARTUUID=", 9) == 0) {
		name += 9;
		res = devt_from_partuuid(name);
		if (!res)
			goto fail;
		goto done;
	} else if (strncmp(name, "PARTLABEL=", 10) == 0) {
		struct device *dev;

		dev = class_find_device(&block_class, NULL, name + 10,
					&match_dev_by_label);
		if (!dev)
			goto fail;

		res = dev->devt;
		put_device(dev);
		goto done;
	}
#endif
	/* name ==> /dev/ram0 */
	if (strncmp(name, "/dev/", 5) != 0) {
		unsigned maj, min, offset;
		char dummy;

		if ((sscanf(name, "%u:%u%c", &maj, &min, &dummy) == 2) ||
		    (sscanf(name, "%u:%u:%u:%c", &maj, &min, &offset, &dummy) == 3)) {
			res = MKDEV(maj, min);
			if (maj != MAJOR(res) || min != MINOR(res))
				goto fail;
		} else {
			res = new_decode_dev(simple_strtoul(name, &p, 16));
			if (*p)
				goto fail;
		}
		goto done;
	}
	/* name += 5 ==> ram0 */
	name += 5;
	res = Root_NFS;
	if (strcmp(name, "nfs") == 0)
		goto done;
	res = Root_CIFS;
	if (strcmp(name, "cifs") == 0)
		goto done;
	
	/* 走到这里来 ==> 但不会走这里的 goto done */
	res = Root_RAM0;
	if (strcmp(name, "ram") == 0)
		goto done;

	if (strlen(name) > 31)
		goto fail;
	strcpy(s, name);
	for (p = s; *p; p++)
		if (*p == '/')
			*p = '!';
	/* s ==> ram0 */
	res = blk_lookup_devt(s, 0);
	/* res ==> 0x100000 */
	if (res)
		/* 走这里返回 */
		goto done;

	/*
	 * try non-existent, but valid partition, which may only exist
	 * after revalidating the disk, like partitioned md devices
	 */
	while (p > s && isdigit(p[-1]))
		p--;
	if (p == s || !*p || *p == '0')
		goto fail;

	/* try disk name without <part number> */
	part = simple_strtoul(p, NULL, 10);
	*p = '\0';
	res = blk_lookup_devt(s, part);
	if (res)
		goto done;

	/* try disk name without p<part number> */
	if (p < s + 2 || !isdigit(p[-2]) || p[-1] != 'p')
		goto fail;
	p[-1] = '\0';
	res = blk_lookup_devt(s, part);
	if (res)
		goto done;

fail:
	return 0;
done:
	return res;
}
EXPORT_SYMBOL_GPL(name_to_dev_t);

static int __init root_dev_setup(char *line)
{
	strlcpy(saved_root_name, line, sizeof(saved_root_name));
	return 1;
}

__setup("root=", root_dev_setup);

static int __init rootwait_setup(char *str)
{
	if (*str)
		return 0;
	root_wait = 1;
	return 1;
}

__setup("rootwait", rootwait_setup);

static char * __initdata root_mount_data;
static int __init root_data_setup(char *str)
{
	root_mount_data = str;
	return 1;
}

static char * __initdata root_fs_names;
static int __init fs_names_setup(char *str)
{
	root_fs_names = str;
	return 1;
}

static unsigned int __initdata root_delay;
static int __init root_delay_setup(char *str)
{
	root_delay = simple_strtoul(str, NULL, 0);
	return 1;
}

__setup("rootflags=", root_data_setup);
__setup("rootfstype=", fs_names_setup);
__setup("rootdelay=", root_delay_setup);

static void __init get_fs_names(char *page)
{
	char *s = page;

	if (root_fs_names) {
		strcpy(page, root_fs_names);
		while (*s++) {
			if (s[-1] == ',')
				s[-1] = '\0';
		}
	} else {
		int len = get_filesystem_list(page);
		char *p, *next;

		page[len] = '\0';
		for (p = page-1; p; p = next) {
			next = strchr(++p, '\n');
			if (*p++ != '\t')
				continue;
			while ((*s++ = *p++) != '\n')
				;
			s[-1] = '\0';
		}
	}
	*s = '\0';
}

/*  do_mount_root(name="/dev/root", fs="ext4", flags=32768, data=0x0)  */
static int __init do_mount_root(const char *name, const char *fs,
				 const int flags, const void *data)
{
	struct super_block *s;
	struct page *p = NULL;
	char *data_page = NULL;
	int ret;

	if (data) {
		/* init_mount() requires a full page as fifth argument */
		p = alloc_page(GFP_KERNEL);
		if (!p)
			return -ENOMEM;
		data_page = page_address(p);
		/* zero-pad. init_mount() will make sure it's terminated */
		strncpy(data_page, data, PAGE_SIZE);
	}
	/* init_mount("/dev/root", "/root", "ext4", 32768, data_page) */
	/* 
		尝试遍历文件系统类型对 /dev/root进行挂载，
		挂载点为/root, /root 为 root用户的home目录，
		并且调用init_chdir( "/root" )将工作目录切换到/root目录下 
	*/
	/* init_mount (name="/dev/root", fs="ext4", flags=32768, data=0x0) */
	ret = init_mount(name, "/root", fs, flags, data_page);
	if (ret)
		goto out;
	
	/* 调用init_chdir( "/root" )将工作目录切换到/root目录下  */
	init_chdir("/root");
	
	/* 上面将 /dev/root 挂载到了 /root上，且把当前路径也切到到了/root */
	/*  current->fs->pwd.dentry->d_sb ==> 所以这里就是 /dev/root 文件系统的超级块了 */
	s = current->fs->pwd.dentry->d_sb;
	ROOT_DEV = s->s_dev;

	/* [   79.810099] VFS: Mounted root (ext4 filesystem) on device 1:0. */
	printk(KERN_INFO
	       "VFS: Mounted root (%s filesystem)%s on device %u:%u.\n",
	       s->s_type->name,
	       sb_rdonly(s) ? " readonly" : "",
	       MAJOR(ROOT_DEV), MINOR(ROOT_DEV));

out:
	if (p)
		put_page(p);
	return ret;
}

/* mount_block_root (name="/dev/root", flags=0x8000) */
void __init mount_block_root(char *name, int flags)
{
	struct page *page = alloc_page(GFP_KERNEL); /* 分配一个page，也就是申请一块物理内存，用来给下面使用 */
	char *fs_names = page_address(page);        /* fs_names 指向这块内存的起始地址 */
	char *p;
	char b[BDEVNAME_SIZE];

	scnprintf(b, BDEVNAME_SIZE, "unknown-block(%u,%u)",
		  MAJOR(ROOT_DEV), MINOR(ROOT_DEV));
	get_fs_names(fs_names); /* fs_names 里面存放所有的文件系统的类型名：ext3 ext2 ext4 ... */
retry:
		/* 
		   尝试遍历文件系统类型对 /dev/root进行挂载，
		*/
	for (p = fs_names; *p; p += strlen(p)+1) { /* 逐个文件系统进行遍历使用，我这里会最终用到ext4 */
	    /*  do_mount_root(name="/dev/root", fs="ext4", flags=32768, data=0x0) */
		int err = do_mount_root(name, p, flags, root_mount_data); 
		switch (err) {
			case 0:
				goto out;
			case -EACCES:
			case -EINVAL:
				continue;
		}
	        /*
		 * Allow the user to distinguish between failed sys_open
		 * and bad superblock on root device.
		 * and give them a list of the available devices
		 */
		printk("VFS: Cannot open root device \"%s\" or %s: error %d\n",
				root_device_name, b, err);
		printk("Please append a correct \"root=\" boot option; here are the available partitions:\n");

		printk_all_partitions();
#ifdef CONFIG_DEBUG_BLOCK_EXT_DEVT
		printk("DEBUG_BLOCK_EXT_DEVT is enabled, you need to specify "
		       "explicit textual name for \"root=\" boot option.\n");
#endif
		panic("VFS: Unable to mount root fs on %s", b);
	}
	if (!(flags & SB_RDONLY)) {
		flags |= SB_RDONLY;
		goto retry;
	}

	printk("List of all partitions:\n");
	printk_all_partitions();
	printk("No filesystem could mount root, tried: ");
	for (p = fs_names; *p; p += strlen(p)+1)
		printk(" %s", p);
	printk("\n");
	panic("VFS: Unable to mount root fs on %s", b);
out:
	put_page(page);
}
 
#ifdef CONFIG_ROOT_NFS

#define NFSROOT_TIMEOUT_MIN	5
#define NFSROOT_TIMEOUT_MAX	30
#define NFSROOT_RETRY_MAX	5

static int __init mount_nfs_root(void)
{
	char *root_dev, *root_data;
	unsigned int timeout;
	int try, err;

	err = nfs_root_data(&root_dev, &root_data);
	if (err != 0)
		return 0;

	/*
	 * The server or network may not be ready, so try several
	 * times.  Stop after a few tries in case the client wants
	 * to fall back to other boot methods.
	 */
	timeout = NFSROOT_TIMEOUT_MIN;
	for (try = 1; ; try++) {
		err = do_mount_root(root_dev, "nfs",
					root_mountflags, root_data);
		if (err == 0)
			return 1;
		if (try > NFSROOT_RETRY_MAX)
			break;

		/* Wait, in case the server refused us immediately */
		ssleep(timeout);
		timeout <<= 1;
		if (timeout > NFSROOT_TIMEOUT_MAX)
			timeout = NFSROOT_TIMEOUT_MAX;
	}
	return 0;
}
#endif

#ifdef CONFIG_CIFS_ROOT

extern int cifs_root_data(char **dev, char **opts);

#define CIFSROOT_TIMEOUT_MIN	5
#define CIFSROOT_TIMEOUT_MAX	30
#define CIFSROOT_RETRY_MAX	5

static int __init mount_cifs_root(void)
{
	char *root_dev, *root_data;
	unsigned int timeout;
	int try, err;

	err = cifs_root_data(&root_dev, &root_data);
	if (err != 0)
		return 0;

	timeout = CIFSROOT_TIMEOUT_MIN;
	for (try = 1; ; try++) {
		err = do_mount_root(root_dev, "cifs", root_mountflags,
				    root_data);
		if (err == 0)
			return 1;
		if (try > CIFSROOT_RETRY_MAX)
			break;

		ssleep(timeout);
		timeout <<= 1;
		if (timeout > CIFSROOT_TIMEOUT_MAX)
			timeout = CIFSROOT_TIMEOUT_MAX;
	}
	return 0;
}
#endif

void __init mount_root(void)
{
#ifdef CONFIG_ROOT_NFS
	if (ROOT_DEV == Root_NFS) {
		if (!mount_nfs_root())
			printk(KERN_ERR "VFS: Unable to mount root fs via NFS.\n");
		return;
	}
#endif
#ifdef CONFIG_CIFS_ROOT
	if (ROOT_DEV == Root_CIFS) {
		if (!mount_cifs_root())
			printk(KERN_ERR "VFS: Unable to mount root fs via SMB.\n");
		return;
	}
#endif
#ifdef CONFIG_BLOCK /* initrd.image ramdisk镜像走这里 */
	{
		/* 
		   创建ROOT_DEV对应的设备节点/dev/root,
		   如果没有指定rootfstype 命令行参数就尝试遍历文件系统类型对 /dev/root进行挂载，
		   挂载点为/root,并且调用init_chdir( "/root" )将工作目录切换到 /root目录下 
		*/
		/* ROOT_DEV ==> 0x100000 */
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
		/* 创建设备节点 /dev/root ==> 并使/dev/root目录指向ROOT_DEV代表的设备*/
		/* 
			https://blog.csdn.net/jinking01/article/details/104666762
			该方法中先调用create_dev方法，使/dev/root目录指向ROOT_DEV代表的设备
			【create_dev函数的作用原来如此，之前认为只是创建了一个设备节点，原来这里实现了指向的这个效果了】，
			访问/dev/root目录等价于访问ROOT_DEV代表的设备的内容。
			此时，/dev/root目录就等价于硬盘分区/dev/nvme0n1p2里的根目录。 
		*/
		int err = create_dev("/dev/root", ROOT_DEV);

		if (err < 0)
			pr_emerg("Failed to create /dev/root: %d\n", err);
		
		/* 将块设备节点 /dev/root 挂载到 /root ==> /root 是 root用户的home目录 */
		/* ~ 是用户的主目录,root用户的主目录是/root，普通用户的主目录是“/home/普通用户名” */
		/*  mount_block_root (name="/dev/root", flags=0x8000)  */
		mount_block_root("/dev/root", root_mountflags);
	}
#endif
}

/*
 * Prepare the namespace - decide what/where to mount, load ramdisks, etc.
 */
void __init prepare_namespace(void)
{
	if (root_delay) {
		printk(KERN_INFO "Waiting %d sec before mounting root device...\n",
		       root_delay);
		ssleep(root_delay);
	}

	/*
	 * wait for the known devices to complete their probing
	 *
	 * Note: this is a potential source of long boot delays.
	 * For example, it is not atypical to wait 5 seconds here
	 * for the touchpad of a laptop to initialize.
	 */
	wait_for_device_probe();

	md_run_setup();

	/* saved_root_name ==> "/dev/ram0" */
	if (saved_root_name[0]) {
		/* root_device_name ==> "/dev/ram0" */
		root_device_name = saved_root_name;
		if (!strncmp(root_device_name, "mtd", 3) ||
		    !strncmp(root_device_name, "ubi", 3)) {
			mount_block_root(root_device_name, root_mountflags);
			goto out;
		}
		/* root_device_name ==> "/dev/ram0" */
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
		ROOT_DEV = name_to_dev_t(root_device_name);
		if (strncmp(root_device_name, "/dev/", 5) == 0)
		    /* root_device_name ==> "ram0" */
			root_device_name += 5;
	}

	/* 
		initrd_load 把 initrd (我做的ramdisk) 拷贝到了 /initrd.image，
		并把 /initrd.image填充到/dev/ram0 这个块设备节点来了  
	*/
	/* initrd_load 对应的打印是: 启动过程 [   47.034191] | ... [   73.289383] done. */
	if (initrd_load())
		goto out;

	/* wait for any asynchronous scanning to complete */
	if ((ROOT_DEV == 0) && root_wait) {
		printk(KERN_INFO "Waiting for root device %s...\n",
			saved_root_name);
		while (driver_probe_done() != 0 ||
			(ROOT_DEV = name_to_dev_t(saved_root_name)) == 0)
			msleep(5);
		async_synchronize_full();
	}

	/* 
		尝试遍历文件系统类型对/dev/root进行挂载，挂载点为/root, /root 为 root用户的home目录，
		mount_root里面挂载的过程中会调用init_chdir( "/root" )将工作目录切换到/root目录下 
	*/
	/*
	    prepare_namespace方法里调用mount_root方法，来挂载真正的根目录文件系统，也就是cmdline里面 root=xxx 的 xxx这个设备节点

		mount_root() 会打印下面的信息: 
		[   80.738952] EXT4-fs (ram0): recovery complete
		[   80.752144] EXT4-fs (ram0): mounted filesystem with ordered data mode. Opts: (null)
		[   80.766151] VFS: Mounted root (ext4 filesystem) on device 1:0.
	*/
	mount_root();
out:
	devtmpfs_mount();
	/* MS_MOVE通常用于将挂载点从一个位置移动到另一个位置 */
	/* mount_root函数里面会调用init_chdir( "/root" ) ==> 将工作目录切换到/root 目录下 ==> /root 变为当前的工作目录 */
	/* init_mount 这里将当前工作目录(/root) 移动挂载至/目录下 */
	/* 将当前目录挂载的文件系统移动到根目录 */
	init_mount(".", "/", NULL, MS_MOVE, NULL);
	/* 切换当前进程的根目录至当前目录 /root ==> /root 就变为了当前的根目录，在这之前 / 还是 rootfs */
	init_chroot(".");
}

static bool is_tmpfs;
static int rootfs_init_fs_context(struct fs_context *fc)
{
	if (IS_ENABLED(CONFIG_TMPFS) && is_tmpfs)
		return shmem_init_fs_context(fc);

	return ramfs_init_fs_context(fc);
}

struct file_system_type rootfs_fs_type = {
	.name		= "rootfs",
	.init_fs_context = rootfs_init_fs_context,
	.kill_sb	= kill_litter_super,
};

void __init init_rootfs(void)
{
	if (IS_ENABLED(CONFIG_TMPFS) && !saved_root_name[0] &&
		(!root_fs_names || strstr(root_fs_names, "tmpfs")))
		is_tmpfs = true;
}
