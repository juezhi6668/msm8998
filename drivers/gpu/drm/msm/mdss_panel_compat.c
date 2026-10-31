// SPDX-License-Identifier: GPL-2.0-only
/*
 * Legacy MDSS panel hooks for drivers written against the fbdev display
 * driver (Xiaomi's Synaptics DSX fork, FPC1268 TEE fingerprint).
 *
 * msm_drm cannot be built together with CONFIG_FB_MSM_MDSS: both trees
 * export the same io_util/hdmi_util symbols. Once fbdev is off, the
 * consumers below lose these declarations from <linux/mdss_io_util.h>.
 */

#include <linux/errno.h>
#include <linux/fb.h>
#include <linux/jiffies.h>
#include <linux/mdss_io_util.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/types.h>

/* These belong to the real MDSS frame buffer driver when that is built. */
#if !IS_ENABLED(CONFIG_FB_MSM_MDSS)

/*
 * ULPS and reset-skip were hints for the fbdev DSI panel driver. The DRM DSI
 * manager picks power modes on its own, so there is nothing to remember here.
 */
void mdss_panel_reset_skip_enable(bool enable)
{
}
EXPORT_SYMBOL(mdss_panel_reset_skip_enable);

void mdss_dsi_ulps_enable(bool enable)
{
}
EXPORT_SYMBOL(mdss_dsi_ulps_enable);

void mdss_dsi_ulps_suspend_enable(bool enable)
{
}
EXPORT_SYMBOL(mdss_dsi_ulps_suspend_enable);

/*
 * The touch driver uses this to tell its own panel apart from secondary
 * displays. msm_fbdev registers a single framebuffer, so fb0 is the primary.
 */
bool mdss_panel_is_prim(void *fbinfo)
{
	struct fb_info *info = fbinfo;

	return !info || info->node == 0;
}
EXPORT_SYMBOL(mdss_panel_is_prim);

/*
 * Reported by fbdev ESD detection. Claiming a dead panel would silence the
 * blank notifier and leave touch suspended, so never report one.
 */
bool mdss_prim_panel_is_dead(void)
{
	return false;
}
EXPORT_SYMBOL(mdss_prim_panel_is_dead);

/*
 * mdss_prim_panel_fb_unblank() - power the panel back on
 * @timeout: ignored; the fbdev version waited on a resume queue that only
 *           existed inside the MDSS frame buffer driver.
 */
int mdss_prim_panel_fb_unblank(int timeout)
{
	struct fb_info *info = registered_fb[0];

	if (!info)
		return -ENODEV;

	return fb_blank(info, FB_BLANK_UNBLANK);
}
EXPORT_SYMBOL(mdss_prim_panel_fb_unblank);

#endif /* !CONFIG_FB_MSM_MDSS */
