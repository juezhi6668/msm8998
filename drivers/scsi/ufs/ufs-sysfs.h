/* Copyright (c) 2024, The Linux Foundation. All rights reserved. */

#ifndef _UFS_SYSFS_H_
#define _UFS_SYSFS_H_

struct ufs_hba;

int ufs_sysfs_init(void);
void ufs_sysfs_exit(void);
void ufs_sysfs_set_hba(struct ufs_hba *hba);
void ufs_sysfs_clear_hba(void);

#endif /* _UFS_SYSFS_H_ */