// SPDX-License-Identifier: GPL-2.0
/*
 * Routines that mimic syscalls, but don't use the user address space or file
 * descriptors.  Only for init/ and related early init code.
 */
#include <linux/init.h>
#include <linux/mount.h>
#include <linux/namei.h>
#include <linux/fs.h>
#include <linux/fs_struct.h>
#include <linux/file.h>
#include <linux/init_syscalls.h>
#include <linux/security.h>
#include "internal.h"

/*  init_mount("/dev/root", "/root", "ext4", 32768, data_page)  */
int __init init_mount(const char *dev_name, const char *dir_name,
		const char *type_page, unsigned long flags, void *data_page)
{
	struct path path;
	int ret;

	/* dir_name ==> /root */
	/* kern_path ("/root", 1, path=0xffffffc011aebcb8) */
	ret = kern_path(dir_name, LOOKUP_FOLLOW, &path);
	if (ret)
		return ret;

	/* dev_name ==> /dev/root */
	/*  如果 kern_path 成功，则调用 path_mount 函数（注意：这不是Linux内核标准API的一部分，可能是自定义的或特定于某个项目的）来执行挂载操作。
	    这个函数将使用 dev_name、&path（挂载点）、type_page（文件系统类型）、flags（挂载标志）和  data_page（文件系统特定数据）
		作为参数来尝试挂载文件系统
	*/
	/* path_mount ("/dev/root", path=0xffffffc011aebcb8, "ext4", flags=32768, data_page=0x0) */
	/* /dev/root 待挂载的设备节点，这个块设备节点 */
	/* path 这个是挂载点  */
	ret = path_mount(dev_name, &path, type_page, flags, data_page);
	path_put(&path);
	return ret;
}

int __init init_umount(const char *name, int flags)
{
	int lookup_flags = LOOKUP_MOUNTPOINT;
	struct path path;
	int ret;

	if (!(flags & UMOUNT_NOFOLLOW))
		lookup_flags |= LOOKUP_FOLLOW;
	ret = kern_path(name, lookup_flags, &path);
	if (ret)
		return ret;
	return path_umount(&path, flags);
}

int __init init_chdir(const char *filename)
{
	struct path path;
	int error;

	error = kern_path(filename, LOOKUP_FOLLOW | LOOKUP_DIRECTORY, &path);
	if (error)
		return error;
	error = inode_permission(path.dentry->d_inode, MAY_EXEC | MAY_CHDIR);
	if (!error)
		set_fs_pwd(current->fs, &path);
	path_put(&path);
	return error;
}

int __init init_chroot(const char *filename)
{
	struct path path;
	int error;

	error = kern_path(filename, LOOKUP_FOLLOW | LOOKUP_DIRECTORY, &path);
	if (error)
		return error;
	error = inode_permission(path.dentry->d_inode, MAY_EXEC | MAY_CHDIR);
	if (error)
		goto dput_and_out;
	error = -EPERM;
	if (!ns_capable(current_user_ns(), CAP_SYS_CHROOT))
		goto dput_and_out;
	error = security_path_chroot(&path);
	if (error)
		goto dput_and_out;
	set_fs_root(current->fs, &path);
dput_and_out:
	path_put(&path);
	return error;
}

int __init init_chown(const char *filename, uid_t user, gid_t group, int flags)
{
	int lookup_flags = (flags & AT_SYMLINK_NOFOLLOW) ? 0 : LOOKUP_FOLLOW;
	struct path path;
	int error;

	error = kern_path(filename, lookup_flags, &path);
	if (error)
		return error;
	error = mnt_want_write(path.mnt);
	if (!error) {
		error = chown_common(&path, user, group);
		mnt_drop_write(path.mnt);
	}
	path_put(&path);
	return error;
}

int __init init_chmod(const char *filename, umode_t mode)
{
	struct path path;
	int error;

	error = kern_path(filename, LOOKUP_FOLLOW, &path);
	if (error)
		return error;
	error = chmod_common(&path, mode);
	path_put(&path);
	return error;
}

int __init init_eaccess(const char *filename)
{
	struct path path;
	int error;

	error = kern_path(filename, LOOKUP_FOLLOW, &path);
	if (error)
		return error;
	error = inode_permission(d_inode(path.dentry), MAY_ACCESS);
	path_put(&path);
	return error;
}

int __init init_stat(const char *filename, struct kstat *stat, int flags)
{
	int lookup_flags = (flags & AT_SYMLINK_NOFOLLOW) ? 0 : LOOKUP_FOLLOW;
	struct path path;
	int error;

	error = kern_path(filename, lookup_flags, &path);
	if (error)
		return error;
	error = vfs_getattr(&path, stat, STATX_BASIC_STATS,
			    flags | AT_NO_AUTOMOUNT);
	path_put(&path);
	return error;
}

/* init_mknod (filename="/dev/root", mode=24960, dev=256) */
/* create_dev("/dev/ram", Root_RAM0); */
/* create_dev("/dev/root", ROOT_DEV); ROOT_DEV = Root_RAM0 */
int __init init_mknod(const char *filename, umode_t mode, unsigned int dev)
{
	struct dentry *dentry;
	struct path path;
	int error;

	if (S_ISFIFO(mode) || S_ISSOCK(mode))
		dev = 0;
	else if (!(S_ISBLK(mode) || S_ISCHR(mode)))
		return -EINVAL;

	dentry = kern_path_create(AT_FDCWD, filename, &path, 0);
	if (IS_ERR(dentry))
		return PTR_ERR(dentry);

	if (!IS_POSIXACL(path.dentry->d_inode))
		mode &= ~current_umask();
	error = security_path_mknod(&path, dentry, mode, dev);
	if (!error)
	    /* vfs_mknod 会调用 ramfs_mknod 分配一个inode 【struct inode * inode = new_inode(sb);】 赋值给这个dentry的d_inode    */
		/*  (gdb) p /x dentry->d_inode.i_rdev ==> 说明 这个dentry 就是 主次设备节点 Root_RAM0 对应的设备节点 
                                             ==> 所以我们打开这个设备节点的时候，我们就拿到了这个i_rdev，就知道了这个设备节点对应的主次设备号，通过这个主次设备号 我们就能所以到对应的设备了，且我们也知道这个设备的fops
            $61 = 0x100000 
            (gdb) p    dentry->d_inode.i_fop 
            $65 = (const struct file_operations *) 0xffffffc010d94a98 <def_blk_fops> 
		*/
		/* dir->i_op->mknod ==> ramfs_mknod */
		/* ramfs_mknod 里面会分配一个inode 【struct inode * inode = new_inode(sb);】 赋值给这个d_inode   */
		/* 
			ramfs_mknod--> ramfs_get_inode --> init_special_inode ==> 会将 dev赋值给
				inode->i_fop = &def_blk_fops;
				inode->i_rdev = rdev; 
		*/
		error = vfs_mknod(path.dentry->d_inode, dentry, mode,
				  new_decode_dev(dev)); /* new_decode_dev(dev) ==> 0x100000 代表 Root_RAM0 ==> 0x100000 */

	/*
		vfs_mknod 过后，打印下面的信息 
		(gdb) p /x dentry->d_inode.i_rdev ==> 说明 这个dentry 就是 主次设备节点 Root_RAM0 对应的设备节点 
		                                  ==> 所以我们打开这个设备节点的时候，我们就拿到了这个i_rdev，就知道了这个设备节点对应的主次设备号，通过这个主次设备号 我们就能所以到对应的设备了，且我们也知道这个设备的fops
		$61 = 0x100000 
		(gdb) p    dentry->d_inode.i_fop 
		$65 = (const struct file_operations *) 0xffffffc010d94a98 <def_blk_fops>
	*/

	done_path_create(&path, dentry);
	return error;
}

int __init init_link(const char *oldname, const char *newname)
{
	struct dentry *new_dentry;
	struct path old_path, new_path;
	int error;

	error = kern_path(oldname, 0, &old_path);
	if (error)
		return error;

	new_dentry = kern_path_create(AT_FDCWD, newname, &new_path, 0);
	error = PTR_ERR(new_dentry);
	if (IS_ERR(new_dentry))
		goto out;

	error = -EXDEV;
	if (old_path.mnt != new_path.mnt)
		goto out_dput;
	error = may_linkat(&old_path);
	if (unlikely(error))
		goto out_dput;
	error = security_path_link(old_path.dentry, &new_path, new_dentry);
	if (error)
		goto out_dput;
	error = vfs_link(old_path.dentry, new_path.dentry->d_inode, new_dentry,
			 NULL);
out_dput:
	done_path_create(&new_path, new_dentry);
out:
	path_put(&old_path);
	return error;
}

int __init init_symlink(const char *oldname, const char *newname)
{
	struct dentry *dentry;
	struct path path;
	int error;

	dentry = kern_path_create(AT_FDCWD, newname, &path, 0);
	if (IS_ERR(dentry))
		return PTR_ERR(dentry);
	error = security_path_symlink(&path, dentry, oldname);
	if (!error)
		error = vfs_symlink(path.dentry->d_inode, dentry, oldname);
	done_path_create(&path, dentry);
	return error;
}

int __init init_unlink(const char *pathname)
{
	return do_unlinkat(AT_FDCWD, getname_kernel(pathname));
}

int __init init_mkdir(const char *pathname, umode_t mode)
{
	struct dentry *dentry;
	struct path path;
	int error;

	dentry = kern_path_create(AT_FDCWD, pathname, &path, LOOKUP_DIRECTORY);
	if (IS_ERR(dentry))
		return PTR_ERR(dentry);
	if (!IS_POSIXACL(path.dentry->d_inode))
		mode &= ~current_umask();
	error = security_path_mkdir(&path, dentry, mode);
	if (!error)
		error = vfs_mkdir(path.dentry->d_inode, dentry, mode);
	done_path_create(&path, dentry);
	return error;
}

int __init init_rmdir(const char *pathname)
{
	return do_rmdir(AT_FDCWD, getname_kernel(pathname));
}

int __init init_utimes(char *filename, struct timespec64 *ts)
{
	struct path path;
	int error;

	error = kern_path(filename, 0, &path);
	if (error)
		return error;
	error = vfs_utimes(&path, ts);
	path_put(&path);
	return error;
}

int __init init_dup(struct file *file)
{
	int fd;

	fd = get_unused_fd_flags(0);
	if (fd < 0)
		return fd;
	fd_install(fd, get_file(file));
	return 0;
}
