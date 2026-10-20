/*
 * vivo LCM sysfs compatibility layer
 *
 * OriginOS 5 的用户空间（SurfaceFlinger、framework、vivo.biometrics）会访问
 * vivo 私有的 /sys/lcm 与 /sys/lcm1 节点。这些节点在原厂内核里由 vivo 改过的
 * LCM/panel 驱动注册；移植到其它内核后它们整体缺失，logcat 里会出现大量：
 *
 *   _V_F/PowerUtils-no impl: can't read file:/sys/lcm/aod_disp_info:
 *       open failed: ENOENT (No such file or directory)
 *   _V_F/PowerUtils-no impl: write(fp_1hz_enable=0) failed: ...
 *
 * 其中 /sys/lcm/aod_disp_info 决定「全屏 AOD」是否可用：
 * SurfaceFlinger 读取该节点，内容里出现 "support_full_screen=1" 时才会把
 * mSupportFullScreenAod 置为 true（见 updateAodType 相关代码）。
 *
 * 本驱动只做「接口兼容」：
 *   - 建立 /sys/lcm 与 /sys/lcm1，提供面板信息、AOD 信息等节点；
 *   - aod_disp_info / panel_info 接受 "key=value" 形式的写入，解析后按状态回读；
 *   - 其余数值节点只保存并打日志，不改动真实面板。
 *
 * 它不会让 LCD 面板变成 OLED：全屏 AOD 的显示效果仍由 SurfaceFlinger 合成、
 * 由 HWC/显示驱动决定面板是否保持点亮。
 *
 * 可调参数（/sys/module/vivo_lcm_sysfs/parameters/）：
 *   support_full_screen  默认 1；置 0 则 SurfaceFlinger 认为不支持全屏 AOD
 *   panel_name           默认 panel_boe_ili7838e（原 ROM 对应的面板名）
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/string.h>
#include <linux/mutex.h>
#include <linux/errno.h>
#include <linux/mm.h>

#define VLCM_TXT_MAX	512
#define VLCM_NAME_MAX	64
#define VLCM_NR_INST	2

static const char * const vlcm_extra_names[] = {
	"hbm_max_brightness",
	"oled_acl",
	"lcm_cabc",
	"ess_level",
	"extic_enable",
	"vivo_esd_check_enable",
	"dc_dimming",
	"dc_dimming1",
	"ltpo_mode",
	"dimming_speed",
	"colour_gamut",
	"shutdown_flag",
	"mipi_error",
	"oled_hbm",
	"oled_ore",
	"hdr10p_bl_remap",
	"elec_sre",
	"lcm_sre",
};
enum { VLCM_NR_EXTRA = ARRAY_SIZE(vlcm_extra_names) };

struct vlcm_inst {
	struct kobject kobj;
	struct mutex lock;
	char tag[8];

	char aod_disp_info[VLCM_TXT_MAX];
	char panel_info[VLCM_TXT_MAX];
	char panel_name[VLCM_NAME_MAX];

	int support_full_screen;
	int fp_1hz_enable;
	int aod_enable;
	int aod_bl_level;
	int bl_level;
	int lcm_id;
	int extra_val[VLCM_NR_EXTRA];
};

static struct vlcm_inst vlcm_insts[VLCM_NR_INST];

/* 模块参数 */
static int support_full_screen = 1;
module_param(support_full_screen, int, 0644);
MODULE_PARM_DESC(support_full_screen,
		 "report support_full_screen=1 in /sys/lcm/aod_disp_info (fullscreen AOD)");

static char *panel_name = "panel_boe_ili7838e";
module_param(panel_name, charp, 0644);
MODULE_PARM_DESC(panel_name, "panel name reported through /sys/lcm/panel_name");

static inline struct vlcm_inst *vlcm_from_kobj(struct kobject *kobj)
{
	return container_of(kobj, struct vlcm_inst, kobj);
}

static int vlcm_extra_index(const char *name)
{
	int i;

	for (i = 0; i < VLCM_NR_EXTRA; i++)
		if (!strcmp(vlcm_extra_names[i], name))
			return i;
	return -1;
}

/* 按当前状态重新生成两个文本节点的内容 */
static void vlcm_refresh(struct vlcm_inst *in)
{
	snprintf(in->aod_disp_info, sizeof(in->aod_disp_info),
		 "support_full_screen=%d\n"
		 "aod_enable=%d\n"
		 "aod_bl=%d\n"
		 "fp_1hz_enable=%d\n",
		 in->support_full_screen, in->aod_enable,
		 in->aod_bl_level, in->fp_1hz_enable);

	snprintf(in->panel_info, sizeof(in->panel_info),
		 "panel_name=%s\n"
		 "lcm_id=%d\n"
		 "support_full_screen=%d\n"
		 "hbm_max_brightness=%d\n",
		 in->panel_name, in->lcm_id, in->support_full_screen,
		 in->extra_val[vlcm_extra_index("hbm_max_brightness")]);
}

static void vlcm_set_key(struct vlcm_inst *in, const char *key, int val)
{
	int idx;

	if (!strcmp(key, "support_full_screen")) {
		in->support_full_screen = !!val;
	} else if (!strcmp(key, "fp_1hz_enable")) {
		in->fp_1hz_enable = !!val;
	} else if (!strcmp(key, "aod_enable")) {
		in->aod_enable = !!val;
	} else if (!strcmp(key, "aod_bl") || !strcmp(key, "aod_bl_level")) {
		in->aod_bl_level = val;
	} else if (!strcmp(key, "bl_level")) {
		in->bl_level = val;
	} else if (!strcmp(key, "lcm_id")) {
		in->lcm_id = val;
	} else if (!strcmp(key, "panel_name")) {
		/* 只记录，不覆盖 module_param 给的默认值以外的东西 */
		return;
	} else {
		idx = vlcm_extra_index(key);
		if (idx < 0) {
			pr_info("vlcm: %s: ignore unknown key '%s=%d'\n",
				in->tag, key, val);
			return;
		}
		in->extra_val[idx] = val;
	}

	pr_info("vlcm: %s: %s=%d\n", in->tag, key, val);
}

/* 解析 "key=value key=value ..."（空格/逗号/分号/换行分隔） */
static void vlcm_parse_kv(struct vlcm_inst *in, const char *buf, size_t len)
{
	char tmp[VLCM_TXT_MAX];
	char *p = tmp, *tok, *eq;
	size_t n = min(len, (size_t)(VLCM_TXT_MAX - 1));
	int val;

	memcpy(tmp, buf, n);
	tmp[n] = '\0';

	while ((tok = strsep(&p, " \t\r\n,;")) != NULL) {
		if (!*tok)
			continue;
		eq = strchr(tok, '=');
		if (!eq) {
			pr_info("vlcm: %s: ignore '%s'\n", in->tag, tok);
			continue;
		}
		*eq = '\0';
		if (kstrtoint(eq + 1, 0, &val))
			val = 0;
		vlcm_set_key(in, tok, val);
	}
}

static ssize_t vlcm_text_show(struct kobject *kobj,
			      struct kobj_attribute *attr, char *buf)
{
	struct vlcm_inst *in = vlcm_from_kobj(kobj);
	ssize_t ret;

	mutex_lock(&in->lock);
	vlcm_refresh(in);
	if (!strcmp(attr->attr.name, "aod_disp_info"))
		ret = scnprintf(buf, PAGE_SIZE, "%s", in->aod_disp_info);
	else
		ret = scnprintf(buf, PAGE_SIZE, "%s", in->panel_info);
	mutex_unlock(&in->lock);

	return ret;
}

static ssize_t vlcm_text_store(struct kobject *kobj,
			       struct kobj_attribute *attr,
			       const char *buf, size_t len)
{
	struct vlcm_inst *in = vlcm_from_kobj(kobj);

	mutex_lock(&in->lock);
	vlcm_parse_kv(in, buf, len);
	vlcm_refresh(in);
	mutex_unlock(&in->lock);

	return len;
}

static ssize_t vlcm_extra_show(struct kobject *kobj,
			       struct kobj_attribute *attr, char *buf)
{
	struct vlcm_inst *in = vlcm_from_kobj(kobj);
	int idx = vlcm_extra_index(attr->attr.name);

	if (idx < 0)
		return -EINVAL;
	return scnprintf(buf, PAGE_SIZE, "%d\n", in->extra_val[idx]);
}

static ssize_t vlcm_extra_store(struct kobject *kobj,
				struct kobj_attribute *attr,
				const char *buf, size_t len)
{
	struct vlcm_inst *in = vlcm_from_kobj(kobj);
	int idx = vlcm_extra_index(attr->attr.name);
	int val, ret;

	if (idx < 0)
		return -EINVAL;

	ret = kstrtoint(buf, 0, &val);
	if (ret)
		return ret;

	mutex_lock(&in->lock);
	in->extra_val[idx] = val;
	vlcm_refresh(in);
	mutex_unlock(&in->lock);

	pr_info("vlcm: %s: %s=%d\n", in->tag, attr->attr.name, val);
	return len;
}

#define VLCM_INT_ATTR(_fname, _field)					\
static ssize_t _fname##_show(struct kobject *kobj,			\
			     struct kobj_attribute *attr, char *buf)	\
{									\
	struct vlcm_inst *in = vlcm_from_kobj(kobj);			\
	return scnprintf(buf, PAGE_SIZE, "%d\n", in->_field);		\
}									\
static ssize_t _fname##_store(struct kobject *kobj,			\
			      struct kobj_attribute *attr,		\
			      const char *buf, size_t len)		\
{									\
	struct vlcm_inst *in = vlcm_from_kobj(kobj);			\
	int val, ret;							\
									\
	ret = kstrtoint(buf, 0, &val);					\
	if (ret)							\
		return ret;						\
	mutex_lock(&in->lock);						\
	in->_field = val;						\
	vlcm_refresh(in);						\
	mutex_unlock(&in->lock);					\
	pr_info("vlcm: %s: %s=%d\n", in->tag, attr->attr.name, val);	\
	return len;							\
}									\
static struct kobj_attribute _fname##_attr =				\
	__ATTR(_fname, 0644, _fname##_show, _fname##_store)

VLCM_INT_ATTR(vlcm_aod_bl_level, aod_bl_level);
VLCM_INT_ATTR(vlcm_bl_level, bl_level);
VLCM_INT_ATTR(vlcm_lcm_id, lcm_id);

#define VLCM_EXTRA_ATTR(_name)						\
static struct kobj_attribute vlcm_##_name##_attr =			\
	__ATTR(_name, 0644, vlcm_extra_show, vlcm_extra_store)

VLCM_EXTRA_ATTR(hbm_max_brightness);
VLCM_EXTRA_ATTR(oled_acl);
VLCM_EXTRA_ATTR(lcm_cabc);
VLCM_EXTRA_ATTR(ess_level);
VLCM_EXTRA_ATTR(extic_enable);
VLCM_EXTRA_ATTR(vivo_esd_check_enable);
VLCM_EXTRA_ATTR(dc_dimming);
VLCM_EXTRA_ATTR(dc_dimming1);
VLCM_EXTRA_ATTR(ltpo_mode);
VLCM_EXTRA_ATTR(dimming_speed);
VLCM_EXTRA_ATTR(colour_gamut);
VLCM_EXTRA_ATTR(shutdown_flag);
VLCM_EXTRA_ATTR(mipi_error);
VLCM_EXTRA_ATTR(oled_hbm);
VLCM_EXTRA_ATTR(oled_ore);
VLCM_EXTRA_ATTR(hdr10p_bl_remap);
VLCM_EXTRA_ATTR(elec_sre);
VLCM_EXTRA_ATTR(lcm_sre);

static struct kobj_attribute vlcm_aod_disp_info_attr =
	__ATTR(aod_disp_info, 0644, vlcm_text_show, vlcm_text_store);

static struct kobj_attribute vlcm_panel_info_attr =
	__ATTR(panel_info, 0644, vlcm_text_show, vlcm_text_store);

static ssize_t vlcm_panel_name_show(struct kobject *kobj,
				    struct kobj_attribute *attr, char *buf)
{
	struct vlcm_inst *in = vlcm_from_kobj(kobj);

	return scnprintf(buf, PAGE_SIZE, "%s\n", in->panel_name);
}

static struct kobj_attribute vlcm_panel_name_attr =
	__ATTR(panel_name, 0444, vlcm_panel_name_show, NULL);

static struct attribute *vlcm_attrs[] = {
	&vlcm_aod_disp_info_attr.attr,
	&vlcm_panel_info_attr.attr,
	&vlcm_panel_name_attr.attr,
	&vlcm_aod_bl_level_attr.attr,
	&vlcm_bl_level_attr.attr,
	&vlcm_lcm_id_attr.attr,
	&vlcm_hbm_max_brightness_attr.attr,
	&vlcm_oled_acl_attr.attr,
	&vlcm_lcm_cabc_attr.attr,
	&vlcm_ess_level_attr.attr,
	&vlcm_extic_enable_attr.attr,
	&vlcm_vivo_esd_check_enable_attr.attr,
	&vlcm_dc_dimming_attr.attr,
	&vlcm_dc_dimming1_attr.attr,
	&vlcm_ltpo_mode_attr.attr,
	&vlcm_dimming_speed_attr.attr,
	&vlcm_colour_gamut_attr.attr,
	&vlcm_shutdown_flag_attr.attr,
	&vlcm_mipi_error_attr.attr,
	&vlcm_oled_hbm_attr.attr,
	&vlcm_oled_ore_attr.attr,
	&vlcm_hdr10p_bl_remap_attr.attr,
	&vlcm_elec_sre_attr.attr,
	&vlcm_lcm_sre_attr.attr,
	NULL,
};

static ssize_t vlcm_attr_show(struct kobject *kobj,
			      struct attribute *attr, char *buf)
{
	struct kobj_attribute *ka =
		container_of(attr, struct kobj_attribute, attr);

	return ka->show(kobj, ka, buf);
}

static ssize_t vlcm_attr_store(struct kobject *kobj,
			       struct attribute *attr,
			       const char *buf, size_t len)
{
	struct kobj_attribute *ka =
		container_of(attr, struct kobj_attribute, attr);

	if (!ka->store)
		return -EPERM;
	return ka->store(kobj, ka, buf, len);
}

static const struct sysfs_ops vlcm_sysfs_ops = {
	.show	= vlcm_attr_show,
	.store	= vlcm_attr_store,
};

static void vlcm_release(struct kobject *kobj)
{
	/* 实例是静态分配的，无需释放 */
}

static struct kobj_type vlcm_ktype = {
	.sysfs_ops	= &vlcm_sysfs_ops,
	.release	= vlcm_release,
};

static int __init vlcm_init(void)
{
	int i, ret;

	for (i = 0; i < VLCM_NR_INST; i++) {
		struct vlcm_inst *in = &vlcm_insts[i];

		mutex_init(&in->lock);
		if (i == 0)
			strlcpy(in->tag, "lcm", sizeof(in->tag));
		else
			snprintf(in->tag, sizeof(in->tag), "lcm%d", i);
		strlcpy(in->panel_name, panel_name, sizeof(in->panel_name));
		in->support_full_screen = !!support_full_screen;
		in->fp_1hz_enable = 0;
		in->aod_enable = 0;
		in->aod_bl_level = 0;
		in->bl_level = 0;
		in->lcm_id = 0x7838;
		in->extra_val[vlcm_extra_index("hbm_max_brightness")] = 4095;
		vlcm_refresh(in);

		ret = kobject_init_and_add(&in->kobj, &vlcm_ktype, NULL,
					   "%s", in->tag);
		if (ret) {
			pr_err("vlcm: failed to create /sys/%s (%d)\n",
			       in->tag, ret);
			kobject_put(&in->kobj);
			return ret;
		}

		ret = sysfs_create_files(&in->kobj,
					 (const struct attribute **)vlcm_attrs);
		if (ret) {
			pr_err("vlcm: failed to create attrs under /sys/%s (%d)\n",
			       in->tag, ret);
			kobject_put(&in->kobj);
			return ret;
		}
	}

	pr_info("vlcm: vivo LCM sysfs ready: /sys/lcm, /sys/lcm1 "
		"(panel_name=%s, support_full_screen=%d)\n",
		panel_name, support_full_screen);
	return 0;
}

static void __exit vlcm_exit(void)
{
	int i;

	for (i = 0; i < VLCM_NR_INST; i++) {
		sysfs_remove_files(&vlcm_insts[i].kobj,
				   (const struct attribute **)vlcm_attrs);
		kobject_put(&vlcm_insts[i].kobj);
	}
}

/*
 * SurfaceFlinger 在开机早期就会读 /sys/lcm/aod_disp_info，
 * 必须赶在它之前注册好，所以放在 device_initcall 之前的 subsys 阶段。
 */
subsys_initcall(vlcm_init);
module_exit(vlcm_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("vivo LCM sysfs compatibility layer (/sys/lcm, /sys/lcm1)");
