/******************************************************************************
*******************************************************************************
**
**  Copyright (C) Sistina Software, Inc.  2003  All rights reserved.
**  Copyright (C) 2004 Red Hat, Inc.  All rights reserved.
**
**  This copyrighted material is made available to anyone wishing to use,
**  modify, copy, or redistribute it subject to the terms and conditions
**  of the GNU General Public License v.2.
**
*******************************************************************************
******************************************************************************/

#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/smp_lock.h>
#include <linux/spinlock.h>
#include <asm/semaphore.h>
#include <linux/completion.h>
#include <linux/buffer_head.h>
#include <linux/posix_acl.h>
#include <linux/posix_acl_xattr.h>

#include "gfs.h"
#include "acl.h"
#include "eattr.h"
#include "inode.h"

/**
 * gfs_acl_validate_set -
 * @ip:
 * @access:
 * @er:
 * @mode:
 * @remove:
 *
 * Returns: errno
 */

int
gfs_acl_validate_set(struct gfs_inode *ip, int access,
		     struct gfs_ea_request *er,
		     int *remove, mode_t *mode)
{
	ENTER(GFN_ACL_VALIDATE_SET)
	struct posix_acl *acl;
	int error;

	error = gfs_acl_validate_remove(ip, access);
	if (error)
		RETURN(GFN_ACL_VALIDATE_SET, error);

	if (!er->er_data)
		RETURN(GFN_ACL_VALIDATE_SET, -EINVAL);

	acl = posix_acl_from_xattr(er->er_data, er->er_data_len);
	if (IS_ERR(acl))
		RETURN(GFN_ACL_VALIDATE_SET, PTR_ERR(acl));
	if (!acl) {
		*remove = TRUE;
		RETURN(GFN_ACL_VALIDATE_SET, 0);
	}

	error = posix_acl_valid(acl);
	if (error)
		goto out;

	if (access) {
		error = posix_acl_equiv_mode(acl, mode);
		if (!error)
			*remove = TRUE;
		else if (error > 0)
			error = 0;
	}

 out:
	posix_acl_release(acl);

	RETURN(GFN_ACL_VALIDATE_SET, error);
}

/**
 * gfs_acl_validate_remove -
 * @ip:
 * @access:
 *
 * Returns: errno
 */

int
gfs_acl_validate_remove(struct gfs_inode *ip, int access)
{
	ENTER(GFN_ACL_VALIDATE_REMOVE)

	if (!ip->i_sbd->sd_args.ar_posix_acls)
		RETURN(GFN_ACL_VALIDATE_REMOVE, -EOPNOTSUPP);
	if (current->fsuid != ip->i_di.di_uid && !capable(CAP_FOWNER))
		RETURN(GFN_ACL_VALIDATE_REMOVE, -EPERM);
	if (ip->i_di.di_type == GFS_FILE_LNK)
		RETURN(GFN_ACL_VALIDATE_REMOVE, -EOPNOTSUPP);
	if (!access && ip->i_di.di_type != GFS_FILE_DIR)
		RETURN(GFN_ACL_VALIDATE_REMOVE, -EACCES);

	RETURN(GFN_ACL_VALIDATE_REMOVE, 0);
}

/**
 * acl_get -
 * @ip:
 * @access:
 * @acl:
 *
 * Returns: errno
 */

int
acl_get(struct gfs_inode *ip, int access, struct posix_acl **acl)
{
	ENTER(GFN_ACL_GET)
	struct gfs_ea_request er;
	struct gfs_ea_location el;
	int error;

	if (!ip->i_di.di_eattr)
		RETURN(GFN_ACL_GET, 0);

	memset(&er, 0, sizeof(struct gfs_ea_request));
	if (access) {
		er.er_name = GFS_POSIX_ACL_ACCESS;
		er.er_name_len = GFS_POSIX_ACL_ACCESS_LEN;
	} else {
		er.er_name = GFS_POSIX_ACL_DEFAULT;
		er.er_name_len = GFS_POSIX_ACL_DEFAULT_LEN;
	}
	er.er_type = GFS_EATYPE_SYS;

	error = gfs_ea_find(ip, &er, &el);
	if (error)
		RETURN(GFN_ACL_GET, error);
	if (!el.el_ea)
		RETURN(GFN_ACL_GET, 0);
	if (!GFS_EA_DATA_LEN(el.el_ea))
		goto out;

	er.er_data = kmalloc(GFS_EA_DATA_LEN(el.el_ea), GFP_KERNEL);
	error = -ENOMEM;
	if (!er.er_data)
		goto out;

	error = gfs_ea_get_copy(ip, &el, er.er_data);
	if (error)
		goto out_kfree;

	*acl = posix_acl_from_xattr(er.er_data, GFS_EA_DATA_LEN(el.el_ea));
	if (IS_ERR(*acl))
		error = PTR_ERR(*acl);

 out_kfree:
	kfree(er.er_data);

 out:
	brelse(el.el_bh);

	RETURN(GFN_ACL_GET, error);
}

/**
 * gfs_check_acl - Check an ACL for to see if we're allowed to do something
 * @inode: the file we want to do something to
 * @mask: what we want to do
 *
 * Returns: errno
 */

int
gfs_check_acl(struct inode *inode, int mask)
{
	ENTER(GFN_CHECK_ACL)
	struct posix_acl *acl = NULL;
	int error;

	error = acl_get(get_v2ip(inode), TRUE, &acl);
	if (error)
		RETURN(GFN_CHECK_ACL, error);

	if (acl) {
		error = posix_acl_permission(inode, acl, mask);
		posix_acl_release(acl);
		RETURN(GFN_CHECK_ACL, error);
	}
      
	RETURN(GFN_CHECK_ACL, -EAGAIN);
}      

/**
 * gfs_acl_new_prep - 
 * @dip:
 * @type:
 * @mode:
 * @a_acl:
 * @d_acl:
 * @blocks:
 * @data:
 *
 * Returns: errno
 */

int
gfs_acl_new_prep(struct gfs_inode *dip,
		 unsigned int type, mode_t *mode,
		 void **a_data, void **d_data,
		 unsigned int *size,
		 unsigned int *blocks)
{
	ENTER(GFN_ACL_NEW_PREP)
	struct posix_acl *acl = NULL;
	int set_a = FALSE, set_d = FALSE;
	int error;

	if (!dip->i_sbd->sd_args.ar_posix_acls)
		RETURN(GFN_ACL_NEW_PREP, 0);
	if (type == GFS_FILE_LNK)
		RETURN(GFN_ACL_NEW_PREP, 0);

	error = acl_get(dip, FALSE, &acl);
	if (error)
		RETURN(GFN_ACL_NEW_PREP, error);
	if (!acl) {
		(*mode) &= ~current->fs->umask;
		RETURN(GFN_ACL_NEW_PREP, 0);
	}

	{
		struct posix_acl *clone = posix_acl_clone(acl, GFP_KERNEL);
		error = -ENOMEM;
		if (!clone)
			goto out;
		posix_acl_release(acl);
		acl = clone;
	}

	error = posix_acl_create_masq(acl, mode);
	if (error < 0)
		goto out;
	if (error > 0) {
		set_a = TRUE;
		error = 0;
	}
	if (type == GFS_FILE_DIR)
		set_d = TRUE;

	if (set_a || set_d) {
		struct gfs_ea_request er;
		void *d;
		unsigned int s = posix_acl_xattr_size(acl->a_count);
		unsigned int b;

		memset(&er, 0, sizeof(struct gfs_ea_request));
		er.er_name_len = GFS_POSIX_ACL_DEFAULT_LEN;
		er.er_data_len = s;
		error = gfs_ea_check_size(dip->i_sbd, &er);
		if (error)
			goto out;

		b = DIV_RU(er.er_data_len, dip->i_sbd->sd_jbsize);
		if (set_a && set_d)
			b *= 2;
		b++;

		d = kmalloc(s, GFP_KERNEL);
		error = -ENOMEM;
		if (!d)
			goto out;
		posix_acl_to_xattr(acl, d, s);

		if (set_a)
			*a_data = d;
		if (set_d)
			*d_data = d;
		*size = s;
		*blocks = b;

		error = 0;
	}

 out:
	posix_acl_release(acl);

	RETURN(GFN_ACL_NEW_PREP, error);
}

/**
 * gfs_acl_new_init - 
 * @dip:
 * @ip:
 * @a_data:
 * @d_data:
 * @size:
 *
 * Returns: errno
 */

int
gfs_acl_new_init(struct gfs_inode *dip, struct gfs_inode *ip,
		 void *a_data, void *d_data, unsigned int size)
{
	ENTER(GFN_ACL_NEW_INIT)
	void *data = (a_data) ? a_data : d_data;
	unsigned int x;
	int error = 0;

	ip->i_alloc = dip->i_alloc; /* Cheesy, but it works. */

	for (x = 0; x < 2; x++) {
		struct gfs_ea_request er;

		memset(&er, 0, sizeof(struct gfs_ea_request));
		if (x) {
			if (!a_data)
				continue;
			er.er_name = GFS_POSIX_ACL_ACCESS;
			er.er_name_len = GFS_POSIX_ACL_ACCESS_LEN;
		} else {
			if (!d_data)
				continue;
			er.er_name = GFS_POSIX_ACL_DEFAULT;
			er.er_name_len = GFS_POSIX_ACL_DEFAULT_LEN;
		}
		er.er_data = data;
		er.er_data_len = size;
		er.er_type = GFS_EATYPE_SYS;

		error = gfs_ea_acl_init(ip, &er);
		if (error)
			break;
	}	

	ip->i_alloc = NULL;

	kfree(data);

	RETURN(GFN_ACL_NEW_INIT, error);
}

/**
 * gfs_acl_chmod -
 * @ip:
 * @attr:
 *
 * Returns: errno
 */

int
gfs_acl_chmod(struct gfs_inode *ip, struct iattr *attr)
{
	ENTER(GFN_ACL_CHMOD)
	struct gfs_ea_request er;
	struct gfs_ea_location el;
	struct posix_acl *acl;
	int error;

	if (!ip->i_di.di_eattr)
		goto simple;

	memset(&er, 0, sizeof(struct gfs_ea_request));
	er.er_name = GFS_POSIX_ACL_ACCESS;
	er.er_name_len = GFS_POSIX_ACL_ACCESS_LEN;
	er.er_type = GFS_EATYPE_SYS;

	error = gfs_ea_find(ip, &er, &el);
	if (error)
		RETURN(GFN_ACL_CHMOD, error);
	if (!el.el_ea)
		goto simple;
	if (!GFS_EA_DATA_LEN(el.el_ea))
		goto simple;

	er.er_data = kmalloc(GFS_EA_DATA_LEN(el.el_ea), GFP_KERNEL);
	error = -ENOMEM;
	if (!er.er_data)
		goto out;

	error = gfs_ea_get_copy(ip, &el, er.er_data);
	if (error)
		goto out_kfree;

	acl = posix_acl_from_xattr(er.er_data, GFS_EA_DATA_LEN(el.el_ea));
	if (IS_ERR(acl)) {
		error = PTR_ERR(acl);
		goto out_kfree;
	} else if (!acl) {
		kfree(er.er_data);
		brelse(el.el_bh);
		goto simple;
	}

	error = posix_acl_chmod_masq(acl, attr->ia_mode);
	if (error)
		goto out_acl;

	posix_acl_to_xattr(acl, er.er_data, GFS_EA_DATA_LEN(el.el_ea));

	error = gfs_ea_acl_chmod(ip, &el, attr, er.er_data);

 out_acl:
	posix_acl_release(acl);

 out_kfree:
	kfree(er.er_data);

 out:
	brelse(el.el_bh);

	RETURN(GFN_ACL_CHMOD, error);

 simple:
	RETURN(GFN_ACL_CHMOD, gfs_setattr_simple(ip, attr));
}