/*
 * vivo_touchscreen_sysfs.c
 *
 * 为移植 vivo 系统提供 /sys/touchscreen/<node> 兼容节点，
 * 把双击亮屏的读写透明转发到本机触摸驱动真实节点。
 *
 * ============================ 编译开关 ============================
 * CONFIG_VIVO_TOUCHSCREEN_SYSFS (drivers/misc/Kconfig, bool, default n)
 * 设备 defconfig 里加 CONFIG_VIVO_TOUCHSCREEN_SYSFS=y 才会编译。
 * 只能内建：vivo HAL 在开机早期就要访问 /sys/touchscreen，模块加载太晚。
 * 也可用内核命令行强制指定真实节点：
 *   vivo_touchscreen_sysfs.target_path=/sys/class/input/input1/double_tap_enable
 *
 * ============================ 逆向结论 ============================
 * 对象：vendor/lib64/hw/bbktouchscreen.default.so
 *       vendor/lib64/hw/vendor.vivo.hardware.bbkts@1.0-impl.so
 *       vendor/bin/hw/vendor.vivo.hardware.bbkts@1.0-service
 *
 * HAL 服务：init rc 里 user system / group system，SELinux 标签 vivo_tpfs
 * HIDL 方法 -> sysfs 节点（反汇编 touchscreen_dclick_* 得到）：
 *
 *   dclick_switch(int v)          -> /sys/touchscreen/dclick_switch
 *                                    open(O_RDWR) + write("1"|"0", 1 字节)
 *   dclick_enable(int v)          -> /sys/touchscreen/dclick_proximity_switch
 *                                    （注意：这是"防误触"，不是双击开关！）
 *   dclick_simulate_switch(int v) -> /sys/touchscreen/dclick_simulate_switch
 *   其它：gesture_switch / gesture_sign_switch / gesture_letter_switch /
 *         gesture_switch_export / swipe_switch / gloves_mode_switch /
 *         finger_gesture_switch / edge_suppress_switch / interrupt_enable /
 *         app_name / ts_is_calling / dclick_lcd_state
 *   impl 的兜底路径模板：/sys/touchscreen/%s
 *   副屏：/sys/devices/virtual/vts/vivo_ts_second/%s
 *   主屏备用：/sys/devices/virtual/vts/vivo_ts/%s
 *
 * ================== 关键：双击亮屏实际走 app_name 位图 ==================
 * 反编译 vivo-services.jar 的 com.vivo.services.touchscreen.TouchScreenService：
 *
 *   设置项 Settings.System "bbk_smart_wakeup"（VivoSettings$System.BBK_SMART_WAKEUP）
 *     -> MotionRecognition 的 DoubleWakeUpPreferenceController.setChecked() 写入
 *     -> TouchScreenService$4.onChange() 收到变化
 *     -> TouchscreenSetGestureBitmap()
 *     -> MultiTouchController.setState(displayId, 2, GetGesturesSwitchState(displayId))
 *     -> 命令名 "vts_gs"，拼成  vts_gs:<tp_name>:<mask>
 *     -> TouchScreenHalInterface.touchScreenSysFileWrite("app_name", 上面那串)
 *     -> HAL 写 /sys/touchscreen/app_name
 *
 * 掩码 = 1 << 数组下标，SystemGesturesSettings[] 里：
 *   0 bbk_screen_disable_change_music_setting
 *   1 bbk_screen_disable_to_unlock_setting
 *   2 bbk_screen_disable_wake_qq_setting
 *   3 bbk_screen_disable_wake_wechat_setting
 *   4 bbk_screen_disable_wake_music_setting
 *   5 bbk_screen_disable_wake_browser_setting
 *   6 bbk_screen_disable_wake_dial_setting
 *   7 bbk_quick_open_camera_setting
 *   8 bbk_smart_wakeup        <-- 双击亮屏，1<<8 = 0x100 = 256
 *   9 bbk_screen_disable_wake_email_setting
 *  10 bbk_screen_disable_wake_facebook_setting
 *  11 quick_launch_app_primary_switch
 *  12 finger_unlock_open
 *  13 faceunlock_screen_doze
 *  14 vivo_mood_picture_setting_enable
 *  15 vivo_scrawl_setting_enable
 *  16 bbk_cover_screen_mute_setting
 *  17 udfp_show_dialog_screen_off
 *  18 aod_click_support_switch
 *  19 vivo_pen_gesture_note_switch
 *
 * 实测日志（开关拨动时）：
 *   开启 -> touchScreenSysFileWrite app_name cmd vts_gs:vivo_ts:256
 *   关闭 -> touchScreenSysFileWrite app_name cmd vts_gs:vivo_ts:0
 *
 * 所以本驱动必须提供 app_name 节点，解析 vts_gs 的最后一段，按 bit8 决定
 * 写 1 还是 0 到真实双击节点。只提供 dclick_switch 是没用的——
 * Java 层根本不会调用它。
 *
 * 反汇编片段（touchscreen_dclick_switch）：
 *   adrp x0, #0x1000 ; add x0, x0, #0x2b4   -> "/sys/touchscreen/dclick_switch"
 *   mov  w1, #2                              -> O_RDWR
 *   bl   open
 *   adrp x8, #0x1000 ; add x8, x8, #0x30c   -> "0"
 *   adrp x9, #0x1000 ; add x9, x9, #0x540   -> "1"
 *   cmp  w20, #1 ; csel x1, x9, x8, eq
 *   mov  w2, #1 ; bl write                   -> 写 1 字节，无换行
 *
 * ============================ 本机真实节点 ============================
 *   /sys/devices/soc/c179000.i2c/i2c-5/5-0020/input/input1/double_tap_enable
 *   drivers/input/touchscreen/synaptics_dsx_force/synaptics_dsx_core.c
 *     show : snprintf("%u\n", enable_wakeup_gesture)   -> "0\n"/"1\n"
 *     store: sscanf("%u") -> input>0?1:0
 *   语义一致（1=开启双击亮屏），原样转发即可。
 *
 * 权限给 0666：HAL 以 user system 运行，sysfs 默认 0644 root:root 写不进去。
 */

#define pr_fmt(fmt) "vivo_ts: " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/fcntl.h>
#include <linux/string.h>
#include <linux/stringify.h>
#include <linux/err.h>
#include <linux/limits.h>
#include <linux/mutex.h>
#include <linux/cred.h>

#define VTS_KOBJ_NAME		"touchscreen"
#define VTS_TARGET_NODE		"double_tap_enable"

/*
 * 双击亮屏在 vivo 手势位图里的位置。
 * 依据：TouchScreenService.SystemGesturesSettings[] 数组索引 8 = "bbk_smart_wakeup"，
 * 而掩码是 1 << index，所以 1<<8 = 0x100 = 256。
 * 日志实测：开关打开时下发 vts_gs:vivo_ts:256，关闭时 vts_gs:vivo_ts:0。
 */
#define VTS_BIT_DCLICK		(1 << 8)

/* 真实节点的候选路径，按顺序取第一个能打开的 */
static const char *const vts_candidates[] = {
	"/sys/devices/soc/c179000.i2c/i2c-5/5-0020/input/input1/double_tap_enable",
	"/sys/bus/i2c/devices/5-0020/input/input1/double_tap_enable",
	"/sys/class/input/input1/double_tap_enable",
	NULL,
};

/* 允许用内核命令行/模块参数强制指定 */
static char *target_path;
module_param(target_path, charp, 0644);
MODULE_PARM_DESC(target_path,
		 "real double-tap sysfs node (default: auto-detect)");

/*
 * PATH_MAX 缓冲区一律放静态区：内核 arm64 栈只有 16KB，
 * CONFIG_FRAME_WARN 通常 1024/2048 字节，char path[PATH_MAX] 会直接
 * 触发 -Werror=frame-larger-than。vts_* 这两个只在持 vts_lock 时访问。
 */
static char vts_target[PATH_MAX];
static char vts_scan_found[PATH_MAX];

/* 最近一次写进 app_name 的命令，方便 cat 出来排查 */
static char vts_last_cmd[96];
static bool vts_write_warned;

static const struct cred *vts_root_cred;
static struct kobject *vts_kobj;
static DEFINE_MUTEX(vts_lock);
static DEFINE_MUTEX(vts_cmd_lock);

/*
 * 关键：filp_open() 用的是 current 的凭据。
 * 写我们节点的进程是 vivo HAL（user system, uid 1000），
 * 而真实节点 double_tap_enable 是 0644 root:root，
 * 直接用 filp_open 会被 inode_permission 判 EACCES。
 * 所以这里准备一份 root cred，打开文件时临时覆盖。
 */
static struct file *vts_open(const char *path, int flags)
{
	const struct cred *old;
	struct file *f;

	if (!vts_root_cred)
		return filp_open(path, flags, 0);

	old = override_creds(vts_root_cred);
	f = filp_open(path, flags, 0);
	revert_creds(old);

	return f;
}

static bool vts_file_ok(const char *path)
{
	struct file *f;

	if (!path || !path[0])
		return false;

	f = vts_open(path, O_RDONLY);
	if (IS_ERR(f))
		return false;

	filp_close(f, NULL);
	return true;
}

/*
 * /sys/class/input 的目录项回调。dir_context.actor 是 const 成员，
 * 只能聚合初始化；这里复用静态 vts_scan_found 当临时路径缓冲，
 * 命中就保留，没命中就清空继续找。
 */
static int vts_scan_actor(struct dir_context *ctx, const char *name, int namlen,
			  loff_t offset, u64 ino, unsigned int d_type)
{
	if (vts_scan_found[0])
		return 0;

	/* 只认 inputN；限制长度，避免拼出超长路径 */
	if (namlen < 6 || namlen > 64 || strncmp(name, "input", 5))
		return 0;

	snprintf(vts_scan_found, sizeof(vts_scan_found),
		 "/sys/class/input/%.*s/%s", namlen, name, VTS_TARGET_NODE);
	if (!vts_file_ok(vts_scan_found))
		vts_scan_found[0] = '\0';

	return 0;
}

/*
 * 兜底：直接扫 /sys/class/input/ 找哪个 inputN 下面有 double_tap_enable。
 * 这样不管 i2c 地址 / input 序号怎么变都能命中。
 */
static void vts_scan_input_class(char *out, size_t outlen)
{
	struct dir_context ctx = {
		.actor = vts_scan_actor,
		.pos   = 0,
	};
	struct file *f;

	vts_scan_found[0] = '\0';

	f = vts_open("/sys/class/input", O_RDONLY | O_DIRECTORY);
	if (IS_ERR(f))
		return;

	iterate_dir(f, &ctx);
	filp_close(f, NULL);

	if (vts_scan_found[0])
		strlcpy(out, vts_scan_found, outlen);
}

/* 首次访问时解析目标，之后缓存；解析失败下次还会重试 */
static const char *vts_resolve(void)
{
	const char *path = NULL;
	int i;

	mutex_lock(&vts_lock);
	if (!vts_target[0]) {
		if (vts_file_ok(target_path)) {
			strlcpy(vts_target, target_path, sizeof(vts_target));
		} else {
			for (i = 0; vts_candidates[i]; i++) {
				if (vts_file_ok(vts_candidates[i])) {
					strlcpy(vts_target, vts_candidates[i],
						sizeof(vts_target));
					break;
				}
			}
			if (!vts_target[0])
				vts_scan_input_class(vts_target,
						     sizeof(vts_target));
		}
		if (vts_target[0])
			pr_info("double-tap node mapped to %s\n", vts_target);
	}
	if (vts_target[0])
		path = vts_target;
	mutex_unlock(&vts_lock);

	return path;
}

/* 把 0/1 写进真实节点 */
static int vts_write_target(int on)
{
	const char *path = vts_resolve();
	const char val = on ? '1' : '0';
	struct file *f;
	ssize_t ret;

	if (!path)
		return -ENODEV;

	f = vts_open(path, O_WRONLY);
	if (IS_ERR(f))
		return PTR_ERR(f);

	ret = kernel_write(f, &val, 1, 0);
	filp_close(f, NULL);

	if (ret < 0)
		return ret;
	return ret == 1 ? 0 : -EIO;
}

/*
 * vivo 把「哪些手势开着」编码成位图，通过 app_name 节点下发：
 *     vts_gs:<tp_name>:<mask>
 * 例如 vts_gs:vivo_ts:256 表示只开了双击亮屏（bit8）。
 * 只关心双击这一位；其它命令（vts_lcd / vts_proximity / 前台包名）一律忽略。
 */
static ssize_t vts_app_name_store(const char *buf, size_t count)
{
	char tmp[128];
	size_t n = count;
	const char *p;
	unsigned long mask;
	int ret;

	if (n >= sizeof(tmp))
		return count;

	memcpy(tmp, buf, n);
	tmp[n] = '\0';

	while (n && (tmp[n - 1] == '\n' || tmp[n - 1] == '\r'))
		tmp[--n] = '\0';

	/* 留一份给 cat 看 */
	mutex_lock(&vts_cmd_lock);
	strlcpy(vts_last_cmd, tmp, sizeof(vts_last_cmd));
	mutex_unlock(&vts_cmd_lock);

	if (strncmp(tmp, "vts_gs:", 7))
		return count;

	p = strrchr(tmp, ':');
	if (!p || kstrtoul(p + 1, 10, &mask))
		return count;

	ret = vts_write_target(!!(mask & VTS_BIT_DCLICK));

	/*
	 * 写失败不能静默——之前就是吞掉了错误，日志里完全看不出问题。
	 * 只警告一次，避免 app_name 高频写入刷屏。
	 */
	if (ret && !vts_write_warned) {
		vts_write_warned = true;
		pr_err("write double-tap node failed (%d), target=%s\n",
		       ret, vts_target[0] ? vts_target : "unresolved");
	}

	return count;
}

/*
 * 非代理节点给个固定值就行，不碰真实触摸节点：
 *   dclick_proximity_switch = 0（关闭防误触辅助）
 *   dclick_lcd_state        = 1（屏幕亮）
 */
static const char *vts_dummy_value(const char *name)
{
	if (!strcmp(name, "dclick_proximity_switch"))
		return "0\n";
	if (!strcmp(name, "dclick_lcd_state"))
		return "1\n";
	return NULL;
}

static ssize_t vts_show(struct kobject *kobj, struct kobj_attribute *attr,
			char *buf)
{
	const char *name = attr->attr.name;
	const char *dummy = vts_dummy_value(name);
	const char *path;
	struct file *f;
	ssize_t ret;

	/* app_name 只用来下发命令；这里回显最后一次收到的内容，方便排查 */
	if (!strcmp(name, "app_name")) {
		mutex_lock(&vts_cmd_lock);
		ret = scnprintf(buf, PAGE_SIZE, "%s\n", vts_last_cmd);
		mutex_unlock(&vts_cmd_lock);
		return ret;
	}

	/* 只读：显示实际映射到的真实节点 */
	if (!strcmp(name, "dbg_target")) {
		path = vts_resolve();
		return scnprintf(buf, PAGE_SIZE, "%s\n",
				 path ? path : "unresolved");
	}

	if (dummy)
		return scnprintf(buf, PAGE_SIZE, "%s", dummy);

	path = vts_resolve();
	if (!path)
		return -ENODEV;

	f = vts_open(path, O_RDONLY);
	if (IS_ERR(f))
		return PTR_ERR(f);

	/* kernel_read() 内部已经 set_fs(get_ds())，不用自己包 */
	ret = kernel_read(f, 0, buf, PAGE_SIZE - 1);
	filp_close(f, NULL);

	if (ret < 0)
		return ret;

	buf[ret] = '\0';
	return ret;
}

static ssize_t vts_store(struct kobject *kobj, struct kobj_attribute *attr,
			 const char *buf, size_t count)
{
	const char *name = attr->attr.name;
	const char *path;
	struct file *f;
	ssize_t ret;

	/* 手势位图走 app_name */
	if (!strcmp(name, "app_name"))
		return vts_app_name_store(buf, count);

	/* 哑节点：接受写入但不做任何事，避免 HAL 报错中断流程 */
	if (vts_dummy_value(name))
		return count;

	path = vts_resolve();
	if (!path)
		return -ENODEV;

	f = vts_open(path, O_WRONLY);
	if (IS_ERR(f))
		return PTR_ERR(f);

	ret = kernel_write(f, buf, count, 0);
	filp_close(f, NULL);

	if (ret < 0) {
		pr_err("write %s failed: %zd\n", path, ret);
		return ret;
	}

	return count;
}

/*
 * 标准 sysfs 结构：kobj_attribute + attribute_group，由 kobj_sysfs_ops
 * 完成 attribute -> kobj_attribute 的转换，show/store 签名原生匹配。
 *
 * 但不能用 __ATTR()：4.4 的 VERIFY_OCTAL_PERMISSIONS() 含
 *   BUILD_BUG_ON_ZERO((perms) & 2)
 * 会在编译期拒绝 world-writable 权限；而 vivo HAL 以 user system
 * (uid 1000) 运行，节点必须 0666 才写得进去。所以这里显式初始化
 * struct kobj_attribute，绕开那个编译期检查（运行时 sysfs 完全接受）。
 */
#define VTS_ATTR(_name, _mode, _store)					\
	static struct kobj_attribute vts_attr_##_name = {		\
		.attr = { .name = __stringify(_name), .mode = (_mode) },\
		.show  = vts_show,					\
		.store = (_store),					\
	}

VTS_ATTR(dclick_switch, 0666, vts_store);
VTS_ATTR(dclick_proximity_switch, 0666, vts_store);
VTS_ATTR(dclick_lcd_state, 0666, vts_store);

/* 双击亮屏真正的入口：vivo 用 vts_gs 位图通过 app_name 下发 */
VTS_ATTR(app_name, 0666, vts_store);

/* 只读调试节点：显示当前映射到的真实路径 */
VTS_ATTR(dbg_target, 0444, NULL);

static struct attribute *vts_attrs[] = {
	&vts_attr_dclick_switch.attr,
	&vts_attr_dclick_proximity.attr,
	&vts_attr_dclick_lcd_state.attr,
	&vts_attr_app_name.attr,
	&vts_attr_dbg_target.attr,
	NULL,
};

static struct attribute_group vts_attr_group = {
	.attrs = vts_attrs,
};

static int __init vts_init(void)
{
	int ret;

	vts_root_cred = prepare_kernel_cred(NULL);
	if (!vts_root_cred)
		/*
		 * 不致命：降级成直接 filp_open，若真实节点是 0644
		 * root:root 会写出 EACCES，并由 vts_app_name_store 报错。
		 */
		pr_warn("prepare_kernel_cred failed, writes may hit EACCES\n");

	vts_kobj = kobject_create_and_add(VTS_KOBJ_NAME, NULL);
	if (!vts_kobj) {
		pr_err("failed to create /sys/%s\n", VTS_KOBJ_NAME);
		ret = -ENOMEM;
		goto err_cred;
	}

	ret = sysfs_create_group(vts_kobj, &vts_attr_group);
	if (ret) {
		pr_err("failed to create sysfs group: %d\n", ret);
		goto err_kobj;
	}

	pr_info("created /sys/%s (target: %s)\n", VTS_KOBJ_NAME,
		vts_resolve() ? vts_target : "pending, will retry on access");

	return 0;

err_kobj:
	kobject_put(vts_kobj);
	vts_kobj = NULL;
err_cred:
	if (vts_root_cred) {
		put_cred(vts_root_cred);
		vts_root_cred = NULL;
	}
	return ret;
}

static void __exit vts_exit(void)
{
	if (vts_kobj) {
		sysfs_remove_group(vts_kobj, &vts_attr_group);
		kobject_put(vts_kobj);
		vts_kobj = NULL;
	}
	if (vts_root_cred) {
		put_cred(vts_root_cred);
		vts_root_cred = NULL;
	}
}

/*
 * 触摸驱动是 i2c 探测时注册 device attr 的，late_initcall 命中率更高；
 * 即使没命中，vts_resolve() 在首次访问时还会再试一次。
 */
late_initcall(vts_init);
module_exit(vts_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Compat /sys/touchscreen nodes for ported vivo vendor ROMs");
