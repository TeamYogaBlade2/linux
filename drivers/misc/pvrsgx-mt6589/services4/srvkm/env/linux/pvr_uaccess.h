// SPDX-License-Identifier: MIT OR GPL-2.0-only

#ifndef __PVR_UACCESS_H__
#define __PVR_UACCESS_H__

#include <linux/version.h>
#include <linux/uaccess.h>

static inline unsigned long pvr_copy_to_user(void __user *pvTo, const void *pvFrom, unsigned long ulBytes)
{
    if (access_ok(pvTo, ulBytes))
    {
	return __copy_to_user(pvTo, pvFrom, ulBytes);
    }
    return ulBytes;
}

static inline unsigned long pvr_copy_from_user(void *pvTo, const void __user *pvFrom, unsigned long ulBytes)
{
    /*
     * The compile time correctness checking introduced for copy_from_user in
     * Linux 2.6.33 isn't fully comaptible with our usage of the function.
     */
    if (access_ok(pvFrom, ulBytes))
    {
	return __copy_from_user(pvTo, pvFrom, ulBytes);
    }
    return ulBytes;
}

#define	pvr_put_user	put_user
#define	pvr_get_user	get_user

#endif /* __PVR_UACCESS_H__ */
