/* drivers/dma-buf/hm_sfence.c
 *
 * A minimal source of mainline sync_file fds, for a userspace that cannot read the
 * legacy /dev/sync fences this kernel's MDP and kgsl create.
 *
 * Why: HarmonyOS render_service merges the release fence the composer returns with
 * SYNC_IOC_MERGE, and mainline sync_file_get_fence() rejects any fd whose f_op is not
 * sync_file_fops. The mdp-fence and kgsl pt fences this kernel hands out are legacy
 * struct sync_fence objects, so that merge failed with EINVAL once per frame (561
 * times in 57.8 s in the 11-02 hilog spool) and the compositor fell back to waiting
 * for the GPU to retire.
 *
 * Why an id, not an fd, to signal by: the first version signalled through the fd it
 * had minted, so the composer had to keep a duplicate while handing the original to
 * HOS. HOS moves that fd over IPC and never closes the duplicate, which leaked one
 * descriptor per frame until creation started failing and the legacy fd came back.
 * Here CREATE returns an fd and an id; the fd crosses once, by value, and completion
 * is addressed by id.
 *
 * Modelled on drivers/staging/android/oneshot_sync.c (same node and create/signal
 * shape) but built on struct fence + sync_file_create() instead of
 * sync_fence_create(), which is the point of the driver.
 *
 * This program is free software; you can redistribute it and/or modify it under the
 * terms of the GNU General Public License version 2 as published by the Free Software
 * Foundation.
 */

#include <linux/fs.h>
#include <linux/idr.h>
#include <linux/ioctl.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/uaccess.h>

#include <linux/fence.h>
#include <linux/sync_file.h>

#define HM_SFENCE_IOC_MAGIC 'h'

/**
 * struct hm_sfence_create - argument to the create ioctl
 * @name:	in: debugging name, up to 31 characters
 * @fence_fd:	out: new sync_file descriptor, the caller's to close from here on
 * @id:		out: handle for a later SIGNAL or RELEASE
 */
struct hm_sfence_create {
	char name[32];
	int32_t fence_fd;
	uint32_t id;
};

#define HM_SFENCE_CREATE _IOWR(HM_SFENCE_IOC_MAGIC, 1, struct hm_sfence_create)
/* complete the fence named by an id; the id is consumed */
#define HM_SFENCE_SIGNAL _IOW(HM_SFENCE_IOC_MAGIC, 2, uint32_t)
/* abandon a fence that will never be handed out; the id is consumed */
#define HM_SFENCE_RELEASE _IOW(HM_SFENCE_IOC_MAGIC, 3, uint32_t)

struct hm_sfence {
	struct fence fence;
	char name[32];
	uint32_t id;
};

static DEFINE_SPINLOCK(hm_fence_lock);
static DEFINE_MUTEX(hm_id_lock);
static DEFINE_IDR(hm_idr);
static u64 hm_context;

static const char *hm_get_driver_name(struct fence *f)
{
	return "hm_sfence";
}

static const char *hm_get_timeline_name(struct fence *f)
{
	struct hm_sfence *hf = container_of(f, struct hm_sfence, fence);

	return hf->name[0] ? hf->name : "sfence";
}

/* nothing to arm: completion is driven from userspace, by id */
static bool hm_enable_signaling(struct fence *f)
{
	return true;
}

static void hm_release_obj(struct fence *f)
{
	kfree(container_of(f, struct hm_sfence, fence));
}

static const struct fence_ops hm_fence_ops = {
	.get_driver_name = hm_get_driver_name,
	.get_timeline_name = hm_get_timeline_name,
	.enable_signaling = hm_enable_signaling,
	.wait = fence_default_wait,
	.release = hm_release_obj,
};

/* unlink an id and hand its object to the caller, who then owns the table's
 * reference and must fence_put() it */
static struct hm_sfence *hm_take(uint32_t id)
{
	struct hm_sfence *hf;

	mutex_lock(&hm_id_lock);
	/* idr_remove() returns void in this tree, so look the entry up first and drop it
	 * under the same lock */
	hf = idr_find(&hm_idr, id);
	if (hf)
		idr_remove(&hm_idr, id);
	mutex_unlock(&hm_id_lock);
	return hf;
}

/* the lowest live id at or after *id, removed from the table and returned to the
 * caller, which owns its reference; *id is set to the id that was taken */
static struct hm_sfence *hm_take_first(int *id)
{
	struct hm_sfence *hf;

	mutex_lock(&hm_id_lock);
	hf = idr_get_next(&hm_idr, id);
	if (hf)
		idr_remove(&hm_idr, *id);
	mutex_unlock(&hm_id_lock);
	return hf;
}

/* undo a create that never reached fd_install(): drop the table reference and the
 * initial one, which together are all that is left once any sync_file fput() ran */
static void hm_create_rollback(struct hm_sfence *hf, uint32_t id)
{
	struct hm_sfence *tbl = hm_take(id);

	if (tbl)
		fence_put(&tbl->fence);
	fence_put(&hf->fence);
}

static long hm_create(struct hm_sfence_create __user *uarg)
{
	struct hm_sfence_create arg;
	struct sync_file *sync_file;
	struct hm_sfence *hf;
	int fd;
	int id;

	if (copy_from_user(&arg, uarg, sizeof(arg)))
		return -EFAULT;
	arg.name[ARRAY_SIZE(arg.name) - 1] = '\0';

	hf = kzalloc(sizeof(*hf), GFP_KERNEL);
	if (!hf)
		return -ENOMEM;
	strlcpy(hf->name, arg.name, sizeof(hf->name));

	mutex_lock(&hm_id_lock);
	/* idr_alloc (not idr_alloc_cyclic, which is 4.11+): this is 4.4 */
	id = idr_alloc(&hm_idr, hf, 1, 0, GFP_KERNEL);
	if (id > 0) {
		hf->id = id;
		/* one context for the driver, the id as the sequence number: no ordering
		 * is implied between two of our fences, each is a one-shot event */
		fence_init(&hf->fence, &hm_fence_ops, &hm_fence_lock, hm_context,
			   (unsigned int)id);
		fence_get(&hf->fence);	/* the reference the id table holds */
	}
	mutex_unlock(&hm_id_lock);
	if (id < 0) {
		kfree(hf);
		return id;
	}

	/* refs now: 1 from fence_init, 1 held by the table. sync_file_create adds its
	 * own, so a success leaves 3 and the fd, the table and the init reference each
	 * account for one */
	sync_file = sync_file_create(&hf->fence);
	if (!sync_file) {
		hm_create_rollback(hf, id);
		return -ENOMEM;
	}

	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) {
		fput(sync_file->file);
		hm_create_rollback(hf, id);
		return fd;
	}

	arg.fence_fd = fd;
	arg.id = id;
	if (copy_to_user(uarg, &arg, sizeof(arg))) {
		put_unused_fd(fd);
		fput(sync_file->file);
		hm_create_rollback(hf, id);
		return -EFAULT;
	}

	fd_install(fd, sync_file->file);
	fence_put(&hf->fence);	/* the init reference: the fd and the table own the rest */
	return 0;
}

static long hm_signal(uint32_t __user *uid)
{
	struct hm_sfence *hf;
	uint32_t id;

	if (get_user(id, uid))
		return -EFAULT;
	hf = hm_take(id);
	if (!hf)
		return -ENOENT;
	/* fence->status is left at the 0 it was allocated with: this tree's
	 * fence_get_status() reports a signalled fence with status 0 as success, and
	 * writing it here would be outside the fence lock. */
	fence_signal(&hf->fence);
	fence_put(&hf->fence);	/* the table reference, consumed with the id */
	return 0;
}

static long hm_release_id(uint32_t __user *uid)
{
	struct hm_sfence *hf;
	uint32_t id;

	if (get_user(id, uid))
		return -EFAULT;
	hf = hm_take(id);
	if (!hf)
		return -ENOENT;
	fence_put(&hf->fence);
	return 0;
}

static long hm_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	switch (cmd) {
	case HM_SFENCE_CREATE:
		return hm_create((struct hm_sfence_create __user *)arg);
	case HM_SFENCE_SIGNAL:
		return hm_signal((uint32_t __user *)arg);
	case HM_SFENCE_RELEASE:
		return hm_release_id((uint32_t __user *)arg);
	default:
		return -ENOTTY;
	}
}

static const struct file_operations hm_sfence_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = hm_ioctl,
	.compat_ioctl = hm_ioctl,
};

static struct miscdevice hm_sfence_dev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "hm_sfence",
	.fops = &hm_sfence_fops,
	.mode = 0666,
};

static int __init hm_sfence_init(void)
{
	hm_context = fence_context_alloc(1);
	return misc_register(&hm_sfence_dev);
}

static void __exit hm_sfence_exit(void)
{
	struct hm_sfence *hf;
	int id = 1;

	misc_deregister(&hm_sfence_dev);
	/* drain through hm_take_first() rather than idr_for_each_entry(), which does not
	 * survive removing the entry it is visiting */
	while ((hf = hm_take_first(&id)) != NULL) {
		fence_put(&hf->fence);
		++id;
	}
	idr_destroy(&hm_idr);
}

module_init(hm_sfence_init);
module_exit(hm_sfence_exit);
MODULE_DESCRIPTION("mainline sync_file source for the HarmonyOS display composer");
MODULE_LICENSE("GPL v2");
