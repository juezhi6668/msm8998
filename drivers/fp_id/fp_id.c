// SPDX-License-Identifier: GPL-2.0
/*
 * fp_id - expose the legacy vivo fingerprint-id sysfs node
 *
 * vivo's fingerprint service (com.android.server.biometrics.sensors.
 * fingerprint.PropertiesCreator, shipped in vivo-services.jar) reads a single
 * text file to decide which fingerprint hardware is present:
 *
 *     /sys/fp_id/fp_id
 *
 * The content is one line of the form
 *
 *     <type>_<manufacturer>_<module>
 *
 * where <type> is one of:
 *
 *     common      -> sensorType 1 (rear / classic)
 *     ultrasonic  -> sensorType 2 (under-display, ultrasonic)
 *     optical     -> sensorType 3 (under-display, optical)
 *     udfp        -> sensorType 3 (alias of optical)
 *     sidefp      -> sensorType 4 (side / power button)
 *
 * PropertiesCreator.parseConfig() reads at most 64 bytes, trims the line and
 * splits it on '_'.  If the file is missing or unreadable it falls back to the
 * HAL-reported sensor type, so this driver only needs to provide the value for
 * devices whose HAL does not advertise the right one.
 *
 * This driver creates a plain sysfs kobject named "fp_id" at the sysfs root, so
 * the attribute appears exactly where vivo expects it:
 *
 *     /sys/fp_id/fp_id
 *
 * The value is configurable from device tree via the "vivo,fp-id" property and
 * settable at runtime through the sysfs attribute itself.
 */

#define pr_fmt(fmt) "fp_id: " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/kobject.h>
#include <linux/init.h>
#include <linux/sysfs.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/err.h>
#include <linux/of.h>

/*
 * The device is a rear (classic) fingerprint, so the correct content is
 * "common_<manufacturer>_<module>".  The first field MUST be the keyword
 * "common" (which PropertiesCreator$Info.setType maps to sensorType 1) -- it is
 * NOT a number, so writing "1_..." would be parsed as UNKNOWN (type 0).
 *
 * The other two fields only need to be non-empty and != "xxxx" to pass
 * Info.isValid(); the real module name is not important for a rear sensor.
 *
 * The value is intentionally hardcoded (rather than pulled from a Kconfig
 * string) so the node is always correct regardless of how the defconfig was
 * assembled or whether an incremental build reused a stale .config.  It can
 * still be changed at runtime by writing to the sysfs node, or at boot from
 * device tree via the "vivo,fp-id" property.
 */
#define FP_ID_DEFAULT	"common_fpc_fpc1020"
#define FP_ID_MAX_LEN	64

/*
 * Runtime value.  Sized to FP_ID_MAX_LEN so callers that copy up to 64 bytes
 * (like PropertiesCreator.parseConfig) always get a sane, bounded string.
 */
static char fp_id_value[FP_ID_MAX_LEN + 1] = FP_ID_DEFAULT;

static struct kobject *fp_id_kobj;

static ssize_t fp_id_show(struct kobject *kobj, struct kobj_attribute *attr,
			  char *buf)
{
	/*
	 * vivo's parseConfig() calls FileUtils.readTextFile(file, 64) and then
	 * trim()s the result, so a trailing newline is harmless.
	 */
	return scnprintf(buf, PAGE_SIZE, "%s\n", fp_id_value);
}

static ssize_t fp_id_store(struct kobject *kobj, struct kobj_attribute *attr,
			   const char *buf, size_t count)
{
	size_t len;

	/* Trim the trailing newline that echo(1) adds. */
	len = strcspn(buf, "\r\n");
	if (len == 0)
		return -EINVAL;
	if (len > FP_ID_MAX_LEN)
		return -EINVAL;

	memcpy(fp_id_value, buf, len);
	fp_id_value[len] = '\0';

	pr_info("fp_id updated to \"%s\"\n", fp_id_value);

	return count;
}

static struct kobj_attribute fp_id_attr =
	__ATTR(fp_id, 0644, fp_id_show, fp_id_store);

static int __init fp_id_read_dt(void)
{
	struct device_node *np;
	const char *val;

	np = of_find_node_by_path("/chosen");
	if (!np)
		np = of_find_node_by_path("/");
	if (!np)
		return 0;

	if (!of_property_read_string(np, "vivo,fp-id", &val) && val &&
	    strlen(val) <= FP_ID_MAX_LEN) {
		strlcpy(fp_id_value, val, sizeof(fp_id_value));
		pr_info("fp_id from DT: \"%s\"\n", fp_id_value);
	}

	of_node_put(np);
	return 0;
}

static int __init fp_id_init(void)
{
	int ret;

	fp_id_read_dt();

	/*
	 * kobject_create_and_add("fp_id", NULL) puts the kobject directly at
	 * the sysfs root, i.e. /sys/fp_id/, which is what vivo hardcodes.
	 */
	fp_id_kobj = kobject_create_and_add("fp_id", NULL);
	if (!fp_id_kobj)
		return -ENOMEM;

	ret = sysfs_create_file(fp_id_kobj, &fp_id_attr.attr);
	if (ret) {
		pr_err("failed to create /sys/fp_id/fp_id (%d)\n", ret);
		kobject_put(fp_id_kobj);
		fp_id_kobj = NULL;
		return ret;
	}

	pr_info("/sys/fp_id/fp_id registered, value=\"%s\"\n", fp_id_value);

	return 0;
}

static void __maybe_unused fp_id_exit(void)
{
	if (fp_id_kobj) {
		sysfs_remove_file(fp_id_kobj, &fp_id_attr.attr);
		kobject_put(fp_id_kobj);
		fp_id_kobj = NULL;
	}
}

module_init(fp_id_init);
module_exit(fp_id_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("LineageOS / wayne");
MODULE_DESCRIPTION("Expose the legacy vivo /sys/fp_id/fp_id fingerprint node");
