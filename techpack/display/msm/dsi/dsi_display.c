// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2022-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 * Copyright (c) 2016-2021, The Linux Foundation. All rights reserved.
 */

#include <linux/list.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/err.h>

#include "msm_drv.h"
#include "sde_connector.h"
#include "msm_mmu.h"
#include "dsi_display.h"
#include "dsi_panel.h"
#include "dsi_ctrl.h"
#include "dsi_ctrl_hw.h"
#include "dsi_drm.h"
#include "dsi_clk.h"
#include "dsi_pwr.h"
#include "sde_dbg.h"
#include "dsi_parser.h"

#define to_dsi_display(x) container_of(x, struct dsi_display, host)
#define INT_BASE_10 10

#define MISR_BUFF_SIZE	256
#define ESD_MODE_STRING_MAX_LEN 256
#define ESD_TRIGGER_STRING_MAX_LEN 10

#define MAX_NAME_SIZE	64
#define MAX_TE_RECHECKS 5

#define DSI_CLOCK_BITRATE_RADIX 10
#define MAX_TE_SOURCE_ID  2

#define SEC_PANEL_NAME_MAX_LEN  256
#define MAX_ESD_RECOVERY_RETRY 5

u8 dbgfs_tx_cmd_buf[SZ_4K];
static char dsi_display_primary[MAX_CMDLINE_PARAM_LEN];
static char dsi_display_secondary[MAX_CMDLINE_PARAM_LEN];
static struct dsi_display_boot_param boot_displays[MAX_DSI_ACTIVE_DISPLAY] = {
	{.boot_param = dsi_display_primary},
	{.boot_param = dsi_display_secondary},
};

static const struct of_device_id dsi_display_dt_match[] = {
	{.compatible = "qcom,dsi-display"},
	{}
};

static int dsi_display_enable_status (struct dsi_display *display, bool enable);
static void dsi_display_is_probed(struct dsi_display *display,
					int probe_status);

bool is_skip_op_required(struct dsi_display *display)
{
	if (!display)
		return false;

	return (display->is_cont_splash_enabled || display->trusted_vm_env);
}

static void dsi_display_mask_ctrl_error_interrupts(struct dsi_display *display,
			u32 mask, bool enable)
{
	int i;
	struct dsi_display_ctrl *ctrl;

	if (!display)
		return;

	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		if (!ctrl)
			continue;
		dsi_ctrl_mask_error_status_interrupts(ctrl->ctrl, mask, enable);
	}
}

static int dsi_display_config_clk_gating(struct dsi_display *display,
					bool enable)
{
	int rc = 0, i = 0;
	struct dsi_display_ctrl *mctrl, *ctrl;
	enum dsi_clk_gate_type clk_selection;
	enum dsi_clk_gate_type const default_clk_select = PIXEL_CLK | DSI_PHY;

	if (!display) {
		DSI_ERR("Invalid params\n");
		return -EINVAL;
	}

	if (display->panel->host_config.force_hs_clk_lane) {
		DSI_DEBUG("no dsi clock gating for continuous clock mode\n");
		return 0;
	}

	mctrl = &display->ctrl[display->clk_master_idx];
	if (!mctrl) {
		DSI_ERR("Invalid controller\n");
		return -EINVAL;
	}

	clk_selection = display->clk_gating_config;

	if (!enable) {
		/* for disable path, make sure to disable all clk gating */
		clk_selection = DSI_CLK_ALL;
	} else if (!clk_selection || clk_selection > DSI_CLK_NONE) {
		/* Default selection, no overrides */
		clk_selection = default_clk_select;
	} else if (clk_selection == DSI_CLK_NONE) {
		clk_selection = 0;
	}

	DSI_DEBUG("%s clock gating Byte:%s Pixel:%s PHY:%s\n",
		enable ? "Enabling" : "Disabling",
		clk_selection & BYTE_CLK ? "yes" : "no",
		clk_selection & PIXEL_CLK ? "yes" : "no",
		clk_selection & DSI_PHY ? "yes" : "no");
	rc = dsi_ctrl_config_clk_gating(mctrl->ctrl, enable, clk_selection);
	if (rc) {
		DSI_ERR("[%s] failed to %s clk gating for clocks %d, rc=%d\n",
				display->name, enable ? "enable" : "disable",
				clk_selection, rc);
		return rc;
	}

	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		if (!ctrl->ctrl || (ctrl == mctrl))
			continue;
		/**
		 * In Split DSI usecase we should not enable clock gating on
		 * DSI PHY1 to ensure no display atrifacts are seen.
		 */
		clk_selection &= ~DSI_PHY;
		rc = dsi_ctrl_config_clk_gating(ctrl->ctrl, enable,
				clk_selection);
		if (rc) {
			DSI_ERR("[%s] failed to %s clk gating for clocks %d, rc=%d\n",
				display->name, enable ? "enable" : "disable",
				clk_selection, rc);
			return rc;
		}
	}

	return 0;
}

static void dsi_display_set_ctrl_esd_check_flag(struct dsi_display *display,
			bool enable)
{
	int i;
	struct dsi_display_ctrl *ctrl;

	if (!display)
		return;

	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		if (!ctrl)
			continue;
		ctrl->ctrl->esd_check_underway = enable;
	}

	if (enable)
		display->disp_esd_chk_underway = enable;
}

static void dsi_display_ctrl_irq_update(struct dsi_display *display, bool en)
{
	int i;
	struct dsi_display_ctrl *ctrl;

	if (!display)
		return;

	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		if (!ctrl)
			continue;
		dsi_ctrl_irq_update(ctrl->ctrl, en);
	}
}

void dsi_rect_intersect(const struct dsi_rect *r1,
		const struct dsi_rect *r2,
		struct dsi_rect *result)
{
	int l, t, r, b;

	if (!r1 || !r2 || !result)
		return;

	l = max(r1->x, r2->x);
	t = max(r1->y, r2->y);
	r = min((r1->x + r1->w), (r2->x + r2->w));
	b = min((r1->y + r1->h), (r2->y + r2->h));

	if (r <= l || b <= t) {
		memset(result, 0, sizeof(*result));
	} else {
		result->x = l;
		result->y = t;
		result->w = r - l;
		result->h = b - t;
	}
}

int dsi_display_set_backlight(struct drm_connector *connector,
		void *display, u32 bl_lvl)
{
	struct dsi_display *dsi_display = display;
	struct dsi_panel *panel;
	u32 bl_scale, bl_scale_sv;
	u64 bl_temp;
	int rc = 0;

	if (dsi_display == NULL || dsi_display->panel == NULL)
		return -EINVAL;

	panel = dsi_display->panel;

	mutex_lock(&panel->panel_lock);
	if (!dsi_panel_initialized(panel)) {
		DSI_INFO("Ignor bl_level %u as panel is not init.\n",(u32)bl_lvl);
		rc = -EINVAL;
		goto error;
	}

	if (!(panel->bl_config.bl_level && bl_lvl))
		DSI_INFO("bl_level changed from %u to %u\n",
		       (u32)(panel->bl_config.bl_level), (u32)bl_lvl);

	panel->bl_config.bl_level = bl_lvl;

	/* scale backlight */
	bl_scale = panel->bl_config.bl_scale;
	bl_temp = bl_lvl * bl_scale / MAX_BL_SCALE_LEVEL;

	bl_scale_sv = panel->bl_config.bl_scale_sv;
	bl_temp = (u32)bl_temp * bl_scale_sv / MAX_SV_BL_SCALE_LEVEL;

	DSI_DEBUG("bl_scale = %u, bl_scale_sv = %u, bl_lvl = %u\n",
		bl_scale, bl_scale_sv, (u32)bl_temp);
	rc = dsi_display_clk_ctrl(dsi_display->dsi_clk_handle,
			DSI_CORE_CLK, DSI_CLK_ON);
	if (rc) {
		DSI_ERR("[%s] failed to enable DSI core clocks, rc=%d\n",
		       dsi_display->name, rc);
		goto error;
	}

	rc = dsi_panel_set_backlight(panel, (u32)bl_temp);
	if (rc)
		DSI_ERR("unable to set backlight\n");

	rc = dsi_display_clk_ctrl(dsi_display->dsi_clk_handle,
			DSI_CORE_CLK, DSI_CLK_OFF);
	if (rc) {
		DSI_ERR("[%s] failed to disable DSI core clocks, rc=%d\n",
		       dsi_display->name, rc);
		goto error;
	}

error:
	mutex_unlock(&panel->panel_lock);
	return rc;
}

int dsi_display_set_param(void *display, struct msm_param_info *param_info)
{
	struct dsi_display *dsi_display = display;
	struct dsi_panel *panel;
	int rc = 0;

	if (dsi_display == NULL || dsi_display->panel == NULL)
		return -EINVAL;

	panel = dsi_display->panel;

	DSI_DEBUG("%s+\n", __func__);
	if (!dsi_panel_initialized(panel))
		return -EINVAL;

	rc = dsi_panel_set_param(panel, param_info);
	if (rc)
		DSI_ERR("[%s] failed to panel to set param. rc=%d\n",
				dsi_display->name, rc);
	return rc;
}

static int dsi_display_cmd_engine_enable(struct dsi_display *display)
{
	int rc = 0;
	int i;
	struct dsi_display_ctrl *m_ctrl, *ctrl;
	bool skip_op = is_skip_op_required(display);

	m_ctrl = &display->ctrl[display->cmd_master_idx];
	mutex_lock(&m_ctrl->ctrl->ctrl_lock);

	if (display->cmd_engine_refcount > 0) {
		display->cmd_engine_refcount++;
		goto done;
	}

	rc = dsi_ctrl_set_cmd_engine_state(m_ctrl->ctrl,
				DSI_CTRL_ENGINE_ON, skip_op);
	if (rc) {
		DSI_ERR("[%s] enable mcmd engine failed, skip_op:%d rc:%d\n",
		       display->name, skip_op, rc);
		goto done;
	}

	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		if (!ctrl->ctrl || (ctrl == m_ctrl))
			continue;

		rc = dsi_ctrl_set_cmd_engine_state(ctrl->ctrl,
					DSI_CTRL_ENGINE_ON, skip_op);
		if (rc) {
			DSI_ERR(
			    "[%s] enable cmd engine failed, skip_op:%d rc:%d\n",
			       display->name, skip_op, rc);
			goto error_disable_master;
		}
	}

	display->cmd_engine_refcount++;
	goto done;
error_disable_master:
	(void)dsi_ctrl_set_cmd_engine_state(m_ctrl->ctrl,
				DSI_CTRL_ENGINE_OFF, skip_op);
done:
	mutex_unlock(&m_ctrl->ctrl->ctrl_lock);
	return rc;
}

static int dsi_display_cmd_engine_disable(struct dsi_display *display)
{
	int rc = 0;
	int i;
	struct dsi_display_ctrl *m_ctrl, *ctrl;
	bool skip_op = is_skip_op_required(display);

	m_ctrl = &display->ctrl[display->cmd_master_idx];
	mutex_lock(&m_ctrl->ctrl->ctrl_lock);

	if (display->cmd_engine_refcount == 0) {
		DSI_ERR("[%s] Invalid refcount\n", display->name);
		goto done;
	} else if (display->cmd_engine_refcount > 1) {
		display->cmd_engine_refcount--;
		goto done;
	}

	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		if (!ctrl->ctrl || (ctrl == m_ctrl))
			continue;

		rc = dsi_ctrl_set_cmd_engine_state(ctrl->ctrl,
					DSI_CTRL_ENGINE_OFF, skip_op);
		if (rc)
			DSI_ERR(
			   "[%s] disable cmd engine failed, skip_op:%d rc:%d\n",
				display->name, skip_op, rc);
	}

	rc = dsi_ctrl_set_cmd_engine_state(m_ctrl->ctrl,
				DSI_CTRL_ENGINE_OFF, skip_op);
	if (rc) {
		DSI_ERR("[%s] disable mcmd engine failed, skip_op:%d rc:%d\n",
			display->name, skip_op, rc);
		goto error;
	}

error:
	display->cmd_engine_refcount = 0;
done:
	mutex_unlock(&m_ctrl->ctrl->ctrl_lock);
	return rc;
}

static void dsi_display_aspace_cb_locked(void *cb_data, bool is_detach)
{
	struct dsi_display *display;
	struct dsi_display_ctrl *display_ctrl;
	int rc, cnt;

	if (!cb_data) {
		DSI_ERR("aspace cb called with invalid cb_data\n");
		return;
	}
	display = (struct dsi_display *)cb_data;

	/*
	 * acquire panel_lock to make sure no commands are in-progress
	 * while detaching the non-secure context banks
	 */
	dsi_panel_acquire_panel_lock(display->panel);

	if (is_detach) {
		/* invalidate the stored iova */
		display->cmd_buffer_iova = 0;

		/* return the virtual address mapping */
		msm_gem_put_vaddr(display->tx_cmd_buf);
		msm_gem_vunmap(display->tx_cmd_buf, OBJ_LOCK_NORMAL);

	} else {
		rc = msm_gem_get_iova(display->tx_cmd_buf,
				display->aspace, &(display->cmd_buffer_iova));
		if (rc) {
			DSI_ERR("failed to get the iova rc %d\n", rc);
			goto end;
		}

		display->vaddr =
			(void *) msm_gem_get_vaddr(display->tx_cmd_buf);

		if (IS_ERR_OR_NULL(display->vaddr)) {
			DSI_ERR("failed to get va rc %d\n", rc);
			goto end;
		}
	}

	display_for_each_ctrl(cnt, display) {
		display_ctrl = &display->ctrl[cnt];
		display_ctrl->ctrl->cmd_buffer_size = display->cmd_buffer_size;
		display_ctrl->ctrl->cmd_buffer_iova = display->cmd_buffer_iova;
		display_ctrl->ctrl->vaddr = display->vaddr;
		display_ctrl->ctrl->secure_mode = is_detach;
	}

end:
	/* release panel_lock */
	dsi_panel_release_panel_lock(display->panel);
}

static irqreturn_t dsi_display_panel_te_irq_handler(int irq, void *data)
{
	struct dsi_display *display = (struct dsi_display *)data;

	/*
	 * This irq handler is used for sole purpose of identifying
	 * ESD attacks on panel and we can safely assume IRQ_HANDLED
	 * in case of display not being initialized yet
	 */
	if (!display)
		return IRQ_HANDLED;

	SDE_EVT32(SDE_EVTLOG_FUNC_CASE1);
	complete_all(&display->esd_te_gate);
	return IRQ_HANDLED;
}

static void dsi_display_change_te_irq_status(struct dsi_display *display,
					bool enable)
{
	if (!display) {
		DSI_ERR("Invalid params\n");
		return;
	}

	/* Handle unbalanced irq enable/disable calls */
	if (enable && !display->is_te_irq_enabled) {
		enable_irq(gpio_to_irq(display->disp_te_gpio));
		display->is_te_irq_enabled = true;
	} else if (!enable && display->is_te_irq_enabled) {
		disable_irq(gpio_to_irq(display->disp_te_gpio));
		display->is_te_irq_enabled = false;
	}
}

static void dsi_display_register_te_irq(struct dsi_display *display)
{
	int rc = 0;
	struct platform_device *pdev;
	struct device *dev;
	unsigned int te_irq;

	pdev = display->pdev;
	if (!pdev) {
		DSI_ERR("invalid platform device\n");
		return;
	}

	dev = &pdev->dev;
	if (!dev) {
		DSI_ERR("invalid device\n");
		return;
	}

	if (display->trusted_vm_env) {
		DSI_INFO("GPIO's are not enabled in trusted VM\n");
		return;
	}

	if (!gpio_is_valid(display->disp_te_gpio)) {
		rc = -EINVAL;
		goto error;
	}

	init_completion(&display->esd_te_gate);
	te_irq = gpio_to_irq(display->disp_te_gpio);

	/* Avoid deferred spurious irqs with disable_irq() */
	irq_set_status_flags(te_irq, IRQ_DISABLE_UNLAZY);

	rc = devm_request_irq(dev, te_irq, dsi_display_panel_te_irq_handler,
			      IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
			      "TE_GPIO", display);
	if (rc) {
		DSI_ERR("TE request_irq failed for ESD rc:%d\n", rc);
		irq_clear_status_flags(te_irq, IRQ_DISABLE_UNLAZY);
		goto error;
	}

	disable_irq(te_irq);
	display->is_te_irq_enabled = false;

	return;

error:
	/* disable the TE based ESD check */
	DSI_WARN("Unable to register for TE IRQ\n");
	if (display->panel->esd_config.status_mode == ESD_MODE_PANEL_TE ||
			display->panel->esd_config.status_mode == ESD_MODE_TE_CHK_REG_RD)
		display->panel->esd_config.esd_enabled = false;
}

/* Allocate memory for cmd dma tx buffer */
static int dsi_host_alloc_cmd_tx_buffer(struct dsi_display *display)
{
	int rc = 0, cnt = 0;
	struct dsi_display_ctrl *display_ctrl;

	display->tx_cmd_buf = msm_gem_new(display->drm_dev,
			SZ_4K,
			MSM_BO_UNCACHED);

	if ((display->tx_cmd_buf) == NULL) {
		DSI_ERR("Failed to allocate cmd tx buf memory\n");
		rc = -ENOMEM;
		goto error;
	}

	display->cmd_buffer_size = SZ_4K;

	display->aspace = msm_gem_smmu_address_space_get(
			display->drm_dev, MSM_SMMU_DOMAIN_UNSECURE);

	if (PTR_ERR(display->aspace) == -ENODEV) {
		display->aspace = NULL;
		DSI_DEBUG("IOMMU not present, relying on VRAM\n");
	} else if (IS_ERR_OR_NULL(display->aspace)) {
		rc = PTR_ERR(display->aspace);
		display->aspace = NULL;
		DSI_ERR("failed to get aspace %d\n", rc);
		goto free_gem;
	} else if (display->aspace) {
		/* register to aspace */
		rc = msm_gem_address_space_register_cb(display->aspace,
				dsi_display_aspace_cb_locked, (void *)display);
		if (rc) {
			DSI_ERR("failed to register callback %d\n", rc);
			goto free_gem;
		}
	}

	rc = msm_gem_get_iova(display->tx_cmd_buf, display->aspace,
				&(display->cmd_buffer_iova));
	if (rc) {
		DSI_ERR("failed to get the iova rc %d\n", rc);
		goto free_aspace_cb;
	}

	display->vaddr =
		(void *) msm_gem_get_vaddr(display->tx_cmd_buf);
	if (IS_ERR_OR_NULL(display->vaddr)) {
		DSI_ERR("failed to get va rc %d\n", rc);
		rc = -EINVAL;
		goto put_iova;
	}

	display_for_each_ctrl(cnt, display) {
		display_ctrl = &display->ctrl[cnt];
		display_ctrl->ctrl->cmd_buffer_size = SZ_4K;
		display_ctrl->ctrl->cmd_buffer_iova =
					display->cmd_buffer_iova;
		display_ctrl->ctrl->vaddr = display->vaddr;
		display_ctrl->ctrl->tx_cmd_buf = display->tx_cmd_buf;
	}

	return rc;

put_iova:
	msm_gem_put_iova(display->tx_cmd_buf, display->aspace);
free_aspace_cb:
	msm_gem_address_space_unregister_cb(display->aspace,
			dsi_display_aspace_cb_locked, display);
free_gem:
	mutex_lock(&display->drm_dev->struct_mutex);
	msm_gem_free_object(display->tx_cmd_buf);
	mutex_unlock(&display->drm_dev->struct_mutex);
error:
	return rc;
}

static bool dsi_display_validate_reg_read(struct dsi_panel *panel)
{
	int i, j = 0;
	int len = 0, *lenp;
	int group = 0, count = 0;
	struct drm_panel_esd_config *config;

	if (!panel)
		return false;

	config = &(panel->esd_config);

	lenp = config->status_valid_params ?: config->status_cmds_rlen;
	count = config->status_cmd.count;

	for (i = 0; i < count; i++)
		len += lenp[i];

	for (j = 0; j < config->groups; ++j) {
		for (i = 0; i < len; ++i) {
			if (config->return_buf[i] !=
				config->status_value[group + i]) {
				DRM_ERROR("mismatch: 0x%x\n",
						config->return_buf[i]);
				break;
			}
		}

		if (i == len)
			return true;
		group += len;
	}

	return false;
}

static void dsi_display_parse_te_data(struct dsi_display *display)
{
	struct platform_device *pdev;
	struct device *dev;
	int rc = 0;
	u32 val = 0;

	pdev = display->pdev;
	if (!pdev) {
		DSI_ERR("Invalid platform device\n");
		return;
	}

	dev = &pdev->dev;
	if (!dev) {
		DSI_ERR("Invalid platform device\n");
		return;
	}

	display->disp_te_gpio = of_get_named_gpio(dev->of_node,
					"qcom,platform-te-gpio", 0);

	if (display->fw)
		rc = dsi_parser_read_u32(display->parser_node,
			"qcom,panel-te-source", &val);
	else
		rc = of_property_read_u32(dev->of_node,
			"qcom,panel-te-source", &val);

	if (rc || (val  > MAX_TE_SOURCE_ID)) {
		DSI_ERR("invalid vsync source selection\n");
		val = 0;
	}

	display->te_source = val;
}

static int dsi_display_read_status(struct dsi_display_ctrl *ctrl,
		struct dsi_panel *panel)
{
	int i, rc = 0, count = 0, start = 0, *lenp;
	struct drm_panel_esd_config *config;
	struct dsi_cmd_desc *cmds;
	u32 flags = 0;

	if (!panel || !ctrl || !ctrl->ctrl)
		return -EINVAL;

	/*
	 * When DSI controller is not in initialized state, we do not want to
	 * report a false ESD failure and hence we defer until next read
	 * happen.
	 */
	if (!dsi_ctrl_validate_host_state(ctrl->ctrl))
		return 1;

	config = &(panel->esd_config);
	lenp = config->status_valid_params ?: config->status_cmds_rlen;
	count = config->status_cmd.count;
	cmds = config->status_cmd.cmds;
	flags |= (DSI_CTRL_CMD_FETCH_MEMORY | DSI_CTRL_CMD_READ);

	if (ctrl->ctrl->host_config.panel_mode == DSI_OP_VIDEO_MODE)
		flags |= DSI_CTRL_CMD_CUSTOM_DMA_SCHED;

	for (i = 0; i < count; ++i) {
		memset(config->status_buf, 0x0, SZ_4K);
		if (cmds[i].last_command) {
			cmds[i].msg.flags |= MIPI_DSI_MSG_LASTCOMMAND;
			flags |= DSI_CTRL_CMD_LAST_COMMAND;
		}
		if ((cmds[i].msg.flags & MIPI_DSI_MSG_CMD_DMA_SCHED) &&
			 (panel->panel_initialized))
			flags |= DSI_CTRL_CMD_CUSTOM_DMA_SCHED;

		if (config->status_cmd.state == DSI_CMD_SET_STATE_LP)
			cmds[i].msg.flags |= MIPI_DSI_MSG_USE_LPM;
		cmds[i].msg.rx_buf = config->status_buf;
		cmds[i].msg.rx_len = config->status_cmds_rlen[i];
		rc = dsi_ctrl_cmd_transfer(ctrl->ctrl, &cmds[i].msg, &flags);
		if (rc <= 0) {
			DSI_ERR("rx cmd transfer failed rc=%d\n", rc);
			return rc;
		}

		memcpy(config->return_buf + start,
			config->status_buf, lenp[i]);
		start += lenp[i];
	}

	return rc;
}

static int dsi_display_validate_status(struct dsi_display_ctrl *ctrl,
		struct dsi_panel *panel)
{
	int rc = 0;

	rc = dsi_display_read_status(ctrl, panel);
	if (rc <= 0) {
		goto exit;
	} else {
		/*
		 * panel status read successfully.
		 * check for validity of the data read back.
		 */
		rc = dsi_display_validate_reg_read(panel);
		if (!rc) {
			rc = -EINVAL;
			goto exit;
		}
	}

exit:
	return rc;
}

int dsi_display_cmd_mipi_transfer(struct dsi_display *display,
				struct mipi_dsi_msg *msg,
				u32 flags)
{
	int rc = 0;
	struct dsi_display_ctrl *m_ctrl;

	m_ctrl = &display->ctrl[display->cmd_master_idx];

	if (display->tx_cmd_buf == NULL) {
		rc = dsi_host_alloc_cmd_tx_buffer(display);
		if (rc) {
			DSI_ERR("failed to allocate cmd tx buffer memory\n");
			goto done;
		}
	}

	rc = dsi_display_cmd_engine_enable(display);
	if (rc) {
		DSI_ERR("cmd engine enable failed\n");
		return -EPERM;
	}

	flags |= DSI_CTRL_CMD_FETCH_MEMORY | DSI_CTRL_CMD_CUSTOM_DMA_SCHED;
	rc = dsi_ctrl_cmd_transfer(m_ctrl->ctrl, msg, &flags);
	if (((flags & DSI_CTRL_CMD_READ) && rc <= 0) ||
		(!(flags & DSI_CTRL_CMD_READ) && rc))
		DSI_ERR("failed to transfer cmd. rc = %d\n", rc);

	dsi_display_cmd_engine_disable(display);
done:
	return rc;
}

static int dsi_display_status_reg_read(struct dsi_display *display)
{
	int rc = 0, i;
	struct dsi_display_ctrl *m_ctrl, *ctrl;

	DSI_DEBUG(" ++\n");

	m_ctrl = &display->ctrl[display->cmd_master_idx];

	if (display->tx_cmd_buf == NULL) {
		rc = dsi_host_alloc_cmd_tx_buffer(display);
		if (rc) {
			DSI_ERR("failed to allocate cmd tx buffer memory\n");
			goto done;
		}
	}

	rc = dsi_display_cmd_engine_enable(display);
	if (rc) {
		DSI_ERR("cmd engine enable failed\n");
		return -EPERM;
	}

	rc = dsi_display_validate_status(m_ctrl, display->panel);
	if (rc <= 0) {
		DSI_ERR("[%s] read status failed on master,rc=%d\n",
		       display->name, rc);
		goto exit;
	}

	if (!display->panel->sync_broadcast_en)
		goto exit;

	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		if (ctrl == m_ctrl)
			continue;

		rc = dsi_display_validate_status(ctrl, display->panel);
		if (rc <= 0) {
			DSI_ERR("[%s] read status failed on slave,rc=%d\n",
			       display->name, rc);
			goto exit;
		}
	}
exit:
	dsi_display_cmd_engine_disable(display);
done:
	return rc;
}

static int dsi_display_status_bta_request(struct dsi_display *display)
{
	int rc = 0;

	DSI_DEBUG(" ++\n");
	/* TODO: trigger SW BTA and wait for acknowledgment */

	return rc;
}

static int dsi_display_status_check_te(struct dsi_display *display,
		int rechecks)
{
	int rc = 1, i = 0;
	int const esd_te_timeout = msecs_to_jiffies(3*20);

	if (!rechecks)
		return rc;

	dsi_display_change_te_irq_status(display, true);

	for (i = 0; i < rechecks; i++) {
		reinit_completion(&display->esd_te_gate);
		if (!wait_for_completion_timeout(&display->esd_te_gate,
					esd_te_timeout)) {
			DSI_ERR("TE check failed\n");
			dsi_display_change_te_irq_status(display, false);
			return -EINVAL;
		}
	}

	dsi_display_change_te_irq_status(display, false);

	return rc;
}

int dsi_display_check_status(struct drm_connector *connector, void *display,
					bool te_check_override)
{
	struct dsi_display *dsi_display = display;
	struct dsi_panel *panel;
	u32 status_mode;
	int rc = 0x1, ret;
	u32 mask;
	int te_rechecks = 1;

	if (!dsi_display || !dsi_display->panel)
		return -EINVAL;

	panel = dsi_display->panel;

	dsi_panel_acquire_panel_lock(panel);

	if (!panel->panel_initialized) {
		DSI_DEBUG("Panel not initialized\n");
		goto release_panel_lock;
	}

	/* Prevent another ESD check,when ESD recovery is underway */
	if (atomic_read(&panel->esd_recovery_pending))
		goto release_panel_lock;

	status_mode = panel->esd_config.status_mode;

	if ((status_mode == ESD_MODE_SW_SIM_SUCCESS) ||
			(dsi_display->sw_te_using_wd))
		goto release_panel_lock;

	if (status_mode == ESD_MODE_SW_SIM_FAILURE) {
		rc = -EINVAL;
		goto release_panel_lock;
	}
	SDE_EVT32(SDE_EVTLOG_FUNC_ENTRY, status_mode, te_check_override);

	if (te_check_override)
		te_rechecks = MAX_TE_RECHECKS;

	if ((dsi_display->trusted_vm_env) ||
			(panel->panel_mode == DSI_OP_VIDEO_MODE))
		te_rechecks = 0;

	ret = dsi_display_clk_ctrl(dsi_display->dsi_clk_handle,
		DSI_ALL_CLKS, DSI_CLK_ON);
	if (ret)
		goto release_panel_lock;

	/* Mask error interrupts before attempting ESD read */
	mask = BIT(DSI_FIFO_OVERFLOW) | BIT(DSI_FIFO_UNDERFLOW);
	dsi_display_set_ctrl_esd_check_flag(dsi_display, true);
	dsi_display_mask_ctrl_error_interrupts(dsi_display, mask, true);

	if (status_mode == ESD_MODE_REG_READ) {
		rc = dsi_display_status_reg_read(dsi_display);
	} else if (status_mode == ESD_MODE_SW_BTA) {
		rc = dsi_display_status_bta_request(dsi_display);
	} else if (status_mode == ESD_MODE_PANEL_TE) {
		rc = dsi_display_status_check_te(dsi_display, te_rechecks);
		te_check_override = false;
	} else if (status_mode == ESD_MODE_TE_CHK_REG_RD) {
		rc =  dsi_display_status_check_te(dsi_display, te_rechecks);
		if (rc > 0) {
			/* after checking for TE, then chk for reg_read status */
			rc = dsi_display_status_reg_read(dsi_display);
		}
	} else {
		DSI_WARN("Unsupported check status mode: %d\n", status_mode);
		panel->esd_config.esd_enabled = false;
	}

	/*
	 * TE check may fail even if status read is passing. In case of
	 * te_check_override, check the status both from reg read and TE.
	 */
	if (rc > 0 && te_check_override)
		rc = dsi_display_status_check_te(dsi_display, te_rechecks);
	/* Unmask error interrupts if check passed*/
	if (rc > 0) {
		dsi_display_set_ctrl_esd_check_flag(dsi_display, false);
		dsi_display_mask_ctrl_error_interrupts(dsi_display, mask,
							false);
	}

	dsi_display_clk_ctrl(dsi_display->dsi_clk_handle,
		DSI_ALL_CLKS, DSI_CLK_OFF);

	/* Handle Panel failures during display disable sequence */
	if (rc <=0)
		atomic_set(&panel->esd_recovery_pending, 1);

release_panel_lock:
	dsi_panel_release_panel_lock(panel);
	SDE_EVT32(SDE_EVTLOG_FUNC_EXIT, rc);

	return rc;
}

bool dsi_display_force_esd_disable(void *display)
{
	struct dsi_display *dsi_display = display;
	struct dsi_panel *panel;

	if (dsi_display == NULL)
		return false;

	panel = dsi_display->panel;

	return (panel->esd_utag_enable? false: true);
}

static int dsi_display_cmd_prepare(const char *cmd_buf, u32 cmd_buf_len,
		struct dsi_cmd_desc *cmd, u8 *payload, u32 payload_len)
{
	int i;

	memset(cmd, 0x00, sizeof(*cmd));
	cmd->msg.type = cmd_buf[0];
	cmd->last_command = (cmd_buf[1] == 1);
	cmd->msg.channel = cmd_buf[2];
	cmd->msg.flags = cmd_buf[3];
	cmd->msg.ctrl = 0;
	cmd->post_wait_ms = cmd->msg.wait_ms = cmd_buf[4];
	cmd->msg.tx_len = ((cmd_buf[5] << 8) | (cmd_buf[6]));

	if (cmd->msg.tx_len > payload_len) {
		DSI_ERR("Incorrect payload length tx_len %zu, payload_len %d\n",
		       cmd->msg.tx_len, payload_len);
		return -EINVAL;
	}

	if (cmd->last_command)
		cmd->msg.flags |= MIPI_DSI_MSG_LASTCOMMAND;

	for (i = 0; i < cmd->msg.tx_len; i++)
		payload[i] = cmd_buf[7 + i];

	cmd->msg.tx_buf = payload;
	return 0;
}


static int dsi_display_dispUtil_get_datatype (char dsi_cmd, u8 cmd_type,
				size_t tx_len, u8 *tx_data_type)
{
	int rc = 0;

	*tx_data_type = 0;

	/* cmd_type = 0 : DSI DCS cmd;
	 * cmd_type = 1 : DSI Generic cmd */
	if (cmd_type > DISPUTIL_DSI_GENERIC) {
		DSI_ERR("%s: Invalid DSI package type[%d] = 0x%x\n",
					__func__, DISPUTIL_MIPI_CMD_TYPE,
					cmd_type);
		return -EINVAL;
	}

	DSI_DEBUG("dsi_cmd =0x%x cmd_type=0x%x tx_len=0x%x\n",
				dsi_cmd, cmd_type, (u32)tx_len);

	if (dsi_cmd == DISPUTIL_DSI_WRITE) {
		/* This is a DSI write command */
		switch(tx_len) {
		case 1:
			if (cmd_type == DISPUTIL_DSI_GENERIC)
				/* write, generic, no param */
				*tx_data_type = 0x3;
			else if (cmd_type == DISPUTIL_DSI_DCS)
				/* write, DCS, no param */
				*tx_data_type = 0x5;
			break;
		case 2:
			if (cmd_type == DISPUTIL_DSI_GENERIC)
				/* Write, generic, 1 param */
				*tx_data_type = 0x13;
			else if (cmd_type == DISPUTIL_DSI_DCS)
				/* write, DCS, 1 param */
				*tx_data_type = 0x15;
			break;
		default:
			if (cmd_type == DISPUTIL_DSI_GENERIC)
				/* Write, generic, long*/
				*tx_data_type = 0x29;
			else if (cmd_type == DISPUTIL_DSI_DCS)
				/* write, DCS, long*/
				*tx_data_type = 0x39;
			break;
		}
	} else if (dsi_cmd == DISPUTIL_DSI_READ) {
		if ((cmd_type == DISPUTIL_DSI_DCS) && (tx_len > 1)) {
			/* Note: tx_len will include read command and payload */
			/* DCS read */
			DSI_ERR("DCS read supports no parameter. tx_len=%d\n",
					(u32)tx_len);
			rc = -EINVAL;
		} else {
			switch(tx_len) {
			case 1:
				if (cmd_type == DISPUTIL_DSI_GENERIC)
					/* Read, generic, no param */
					*tx_data_type = 0x4;
				else if (cmd_type == DISPUTIL_DSI_DCS)
					/* Read, DCS, no param */
					*tx_data_type = 0x6;
				break;
			case 2:
				if (cmd_type == DISPUTIL_DSI_GENERIC)
					/* Read, Generic, 1 param */
					*tx_data_type = 0x14;

				break;
			case 3:
				if (cmd_type == DISPUTIL_DSI_GENERIC)
					/* read, generic, 2 params */
					*tx_data_type = 0x24;
				break;
			default:
				DSI_ERR("Invalid for tx_len= %d\n", (u32)tx_len);
				break;
			}
		}
	} else
		rc = -EINVAL;

	if (rc)
		DSI_ERR("Invalid input data: dsi_cmd=0x%x tx_data_type =0x%x for tx_len=0x%x\n",
			dsi_cmd, *tx_data_type, (u32)tx_len);

	return rc;
}

static int dsi_display_dispUtil_prepare(const char *cmd_buf, u32 cmd_buf_len,
	struct dsi_cmd_desc *cmd, u8 *payload, u32 payload_len_max,
	struct motUtil *motUtil_data)
{
	int rc, i;
	u8 tx_data_type;

	memset(cmd, 0x00, sizeof(*cmd));

	/*
	 * DISPUTIL_PAYLOAD_LEN_U and SDE_MOT_UTIL_PAYLOAD_LEN_L to
	 * indicate number of payload in bytes
	 */
	cmd->msg.tx_len = ((cmd_buf[DISPUTIL_PAYLOAD_LEN_U] << 8) |
				(cmd_buf[DISPUTIL_PAYLOAD_LEN_L]));
	rc = dsi_display_dispUtil_get_datatype(cmd_buf[DISPUTIL_CMD_TYPE],
				cmd_buf[DISPUTIL_MIPI_CMD_TYPE],
				cmd->msg.tx_len, &tx_data_type);
	if (rc)
		return rc;

	cmd->msg.type = tx_data_type;
	cmd->msg.channel =  0;
	cmd->last_command = 1;
	cmd->msg.flags |= MIPI_DSI_MSG_LASTCOMMAND;

	/* DISPUTIL_CMD_TYPE = 1 for DSI write cmd
	 * DISPUTIL_CMD_TYPE = 0 for DSI read cmd */
	if (cmd_buf[DISPUTIL_CMD_TYPE] == DISPUTIL_DSI_READ) {
		//cmd->msg.flags |= MIPI_DSI_MSG_READ; /* This is a read DSI */
		cmd->msg.rx_buf = motUtil_data->rd_buf;
		/* cmd_buf[3] = number bytes host wants to read */
		cmd->msg.rx_len = cmd_buf[DISPUTIL_NUM_BYTE_RD];

		motUtil_data->read_cmd = true;
	} else
		motUtil_data->read_cmd = false;

	/*
	 * DISPUTIL_CMD_XFER_MODE = 0: DSI cmd transfers at HS mode
	 * DISPUTIL_CMD_XFER_MODE = 1: DSI cmd transfers at LP mode
	 */
	if (cmd_buf[DISPUTIL_CMD_XFER_MODE] == DISPUTIL_DSI_LP_MODE)
		cmd->msg.flags |= MIPI_DSI_MSG_USE_LPM;

	cmd->msg.ctrl = 0;
	cmd->post_wait_ms = 0;

	/*
	 * msg.tx_len is number of bytes of the payload.
	 * Checking msg.tx_len with the payload, DISPUTIL_PAYLOAD,
	 * to make sure they are matched
	 */
	if ((cmd->msg.tx_len > (cmd_buf_len - DISPUTIL_PAYLOAD)) ||
			(cmd->msg.tx_len > payload_len_max)) {
		DSI_ERR("Incorrect payload length tx_len:%ld, payload_len_max=%d cmd_buf_len=%d\n",
			cmd->msg.tx_len, payload_len_max, cmd_buf_len);
		return -EINVAL;
	}

	for (i = 0; i < cmd->msg.tx_len; i++)
		payload[i] = cmd_buf[DISPUTIL_PAYLOAD + i];

	cmd->msg.tx_buf = payload;

	return 0;
}


static int dsi_display_ctrl_get_host_init_state(struct dsi_display *dsi_display,
		bool *state)
{
	struct dsi_display_ctrl *ctrl;
	int i, rc = -EINVAL;

	display_for_each_ctrl(i, dsi_display) {
		ctrl = &dsi_display->ctrl[i];
		rc = dsi_ctrl_get_host_engine_init_state(ctrl->ctrl, state);
		if (rc)
			break;
	}
	return rc;
}

static int dsi_display_cmd_rx(struct dsi_display *display,
			      struct dsi_cmd_desc *cmd)
{
	struct dsi_display_ctrl *m_ctrl = NULL;
	u32 mask = 0, flags = 0;
	int rc = 0;

	if (!display || !display->panel)
		return -EINVAL;

	m_ctrl = &display->ctrl[display->cmd_master_idx];
	if (!m_ctrl || !m_ctrl->ctrl)
		return -EINVAL;

	/* acquire panel_lock to make sure no commands are in progress */
	dsi_panel_acquire_panel_lock(display->panel);
	if (!display->panel->panel_initialized) {
		DSI_DEBUG("panel not initialized\n");
		goto release_panel_lock;
	}

	rc = dsi_display_clk_ctrl(display->dsi_clk_handle,
		DSI_ALL_CLKS, DSI_CLK_ON);
	if (rc)
		goto release_panel_lock;

	mask = BIT(DSI_FIFO_OVERFLOW) | BIT(DSI_FIFO_UNDERFLOW);
	dsi_display_mask_ctrl_error_interrupts(display, mask, true);
	rc = dsi_display_cmd_engine_enable(display);
	if (rc) {
		DSI_ERR("cmd engine enable failed rc = %d\n", rc);
		goto error;
	}

	flags |= (DSI_CTRL_CMD_FETCH_MEMORY | DSI_CTRL_CMD_READ);
	if ((m_ctrl->ctrl->host_config.panel_mode == DSI_OP_VIDEO_MODE) ||
			((cmd->msg.flags & MIPI_DSI_MSG_CMD_DMA_SCHED) &&
			 (display->enabled)))
		flags |= DSI_CTRL_CMD_CUSTOM_DMA_SCHED;

	rc = dsi_ctrl_cmd_transfer(m_ctrl->ctrl, &cmd->msg, &flags);
	if (rc <= 0)
		DSI_ERR("rx cmd transfer failed rc = %d\n", rc);

	dsi_display_cmd_engine_disable(display);

error:
	dsi_display_mask_ctrl_error_interrupts(display, mask, false);
	dsi_display_clk_ctrl(display->dsi_clk_handle,
		DSI_ALL_CLKS, DSI_CLK_OFF);
release_panel_lock:
	dsi_panel_release_panel_lock(display->panel);
	return rc;
}


int dsi_display_cmd_transfer(struct drm_connector *connector,
		void *display, const char *cmd_buf,
		u32 cmd_buf_len)
{
	struct dsi_display *dsi_display = display;
	int rc = 0, cnt = 0, i = 0;
	bool state = false, transfer = false;
	struct dsi_panel_cmd_set *set;

	if (!dsi_display || !cmd_buf) {
		DSI_ERR("[DSI] invalid params\n");
		return -EINVAL;
	}

	SDE_EVT32(dsi_display->tx_cmd_buf_ndx, cmd_buf_len);
	DSI_DEBUG("[DSI] Display command transfer\n");

	if ((cmd_buf[1]) || (cmd_buf[3] & MIPI_DSI_MSG_LASTCOMMAND))
		transfer = true;

	mutex_lock(&dsi_display->display_lock);
	rc = dsi_display_ctrl_get_host_init_state(dsi_display, &state);

	/**
	 * Handle scenario where a command transfer is initiated through
	 * sysfs interface when device is in suepnd state.
	 */
	if (!rc && !state) {
		pr_warn_ratelimited("Command xfer attempted while device is in suspend state\n"
				);
		rc = -EPERM;
		goto end;
	}
	if (rc || !state) {
		DSI_ERR("[DSI] Invalid host state %d rc %d\n",
				state, rc);
		rc = -EPERM;
		goto end;
	}

	/*
	 * Reset the dbgfs buffer if the commands sent exceed the available
	 * buffer size. For video mode, limiting the buffer size to 2K to
	 * ensure no performance issues.
	 */
	if (dsi_display->panel->panel_mode == DSI_OP_CMD_MODE) {
		if ((dsi_display->tx_cmd_buf_ndx + cmd_buf_len) > SZ_4K) {
			memset(dbgfs_tx_cmd_buf, 0, SZ_4K);
			dsi_display->tx_cmd_buf_ndx = 0;
		}
	} else {
		if ((dsi_display->tx_cmd_buf_ndx + cmd_buf_len) > SZ_2K) {
			memset(dbgfs_tx_cmd_buf, 0, SZ_4K);
			dsi_display->tx_cmd_buf_ndx = 0;
		}
	}

	memcpy(&dbgfs_tx_cmd_buf[dsi_display->tx_cmd_buf_ndx], cmd_buf,
			cmd_buf_len);
	dsi_display->tx_cmd_buf_ndx += cmd_buf_len;
	if (transfer) {
		struct dsi_cmd_desc *cmds;

		set = &dsi_display->cmd_set;
		set->count = 0;
		dsi_panel_get_cmd_pkt_count(dbgfs_tx_cmd_buf,
				dsi_display->tx_cmd_buf_ndx, &cnt);
		dsi_panel_alloc_cmd_packets(set, cnt);
		dsi_panel_create_cmd_packets(dbgfs_tx_cmd_buf,
				dsi_display->tx_cmd_buf_ndx, cnt, set->cmds);
		cmds = set->cmds;
		dsi_display->tx_cmd_buf_ndx = 0;

		for (i = 0; i < cnt; i++) {
			if (cmds->last_command)
				cmds->msg.flags |= MIPI_DSI_MSG_LASTCOMMAND;
			rc = dsi_display->host.ops->transfer(&dsi_display->host,
					&cmds->msg);
			if (rc < 0) {
				DSI_ERR("failed to send command, rc=%d\n", rc);
				break;
			}
			if (cmds->post_wait_ms)
				usleep_range(cmds->post_wait_ms*1000,
						((cmds->post_wait_ms*1000)+10));
			cmds++;
		}

		memset(dbgfs_tx_cmd_buf, 0, SZ_4K);
		dsi_panel_destroy_cmd_packets(set);
		dsi_panel_dealloc_cmd_packets(set);
	}

end:
	mutex_unlock(&dsi_display->display_lock);
	return rc;
}

static int dsi_display_read_elvss_status(struct dsi_display_ctrl *ctrl,
		struct dsi_panel *panel)
{
	int rc = 0;
	struct dsi_cmd_desc cmds;
	u8 data[] = {06, 01, 00, 01, 00, 00, 01, 0xB7};
	u32 flags = 0;
	u8 *payload;
	int size, j;
	u8 elvss_val;
	pr_info("++\n");

	if (!panel || !ctrl || !ctrl->ctrl)
		return -EINVAL;

	DSI_DEBUG("++\n");
	cmds.msg.type = data[0];
	cmds.last_command = (data[1] == 1 ? true : false);
	cmds.msg.channel = data[2];
	cmds.msg.flags |= (data[3] == 1 ? MIPI_DSI_MSG_REQ_ACK : 0);
	cmds.msg.ctrl = 0;
	cmds.post_wait_ms = cmds.msg.wait_ms = data[4];
	cmds.msg.tx_len = ((data[5] << 8) | (data[6]));

	size = cmds.msg.tx_len * sizeof(u8);
	payload = kzalloc(size, GFP_KERNEL);
	if (!payload) {
		return -ENOMEM;
	}

	for (j = 0; j < cmds.msg.tx_len; j++)
		payload[j] = data[7+j];

	cmds.msg.tx_buf = payload;
	/*
	 * When DSI controller is not in initialized state, we do not want to
	 * read reg and hence we defer until next read
	 * happen.
	 */
	/*
	if (dsi_ctrl_validate_host_state(ctrl->ctrl))
		return 0;
	*/
	flags |= (DSI_CTRL_CMD_FETCH_MEMORY | DSI_CTRL_CMD_READ |
		  DSI_CTRL_CMD_CUSTOM_DMA_SCHED);

	if (cmds.last_command) {
		cmds.msg.flags |= MIPI_DSI_MSG_LASTCOMMAND;
		flags |= DSI_CTRL_CMD_LAST_COMMAND;
	}
	cmds.msg.rx_buf = &elvss_val;
	cmds.msg.rx_len = 1;
	rc = dsi_ctrl_cmd_transfer(ctrl->ctrl, &cmds.msg, &flags);
	if (rc <= 0) {
		DSI_ERR("rx cmd transfer failed rc=%d\n", rc);
		goto error;
	}

	DSI_INFO("elvss val = 0x%x\n", elvss_val);
error:
	if (elvss_val != 0)
		return elvss_val;
	return 0;
}

static int dsi_display_elvss_read(struct dsi_display *display)
{
	struct dsi_display_ctrl *m_ctrl;
	int rc;

	DSI_DEBUG("++\n");

	m_ctrl = &display->ctrl[display->cmd_master_idx];

	rc = dsi_display_cmd_engine_enable(display);
	if (rc) {
		DSI_ERR("cmd engine enable failed\n");
		return 0;
	}

	rc = dsi_display_read_elvss_status(m_ctrl, display->panel);
	if (rc <= 0) {
		DSI_ERR("[%s] read elvss failed on master,rc=%d\n",
		       display->name, rc);
		goto exit;
	}

exit:
	dsi_display_cmd_engine_disable(display);
	return rc;
}

int dsi_display_read_elvss_volt(void *display)
{
	struct dsi_display *dsi_display = display;
	struct dsi_panel *panel;
	int rc = 0x1;
	int elvss_vl = 0;
	static bool read_flag = false;

	if (read_flag)
		return rc;

	if (!dsi_display || !dsi_display->panel)
		return -EINVAL;

	panel = dsi_display->panel;

	dsi_panel_acquire_panel_lock(panel);

	if (!panel->panel_initialized) {
		DSI_WARN("Panel not initialized\n");
		goto release_panel_lock;
	}

	dsi_display_clk_ctrl(dsi_display->dsi_clk_handle,
		DSI_ALL_CLKS, DSI_CLK_ON);

	dsi_panel_get_elvss_data(panel);
	elvss_vl = dsi_display_elvss_read(dsi_display);
	dsi_panel_get_elvss_data_1(panel);
	if ( elvss_vl > 0) {
		dsi_panel_set_elvss_dim_off(panel, elvss_vl);
		dsi_panel_parse_elvss_config(panel, elvss_vl);
		read_flag = true;
	} else {
		elvss_vl = 0x92;
		dsi_panel_set_elvss_dim_off(panel, elvss_vl);
		DSI_WARN("read elvss volt failed\n");
	}
	dsi_display_clk_ctrl(dsi_display->dsi_clk_handle,
		DSI_ALL_CLKS, DSI_CLK_OFF);

release_panel_lock:
	dsi_panel_release_panel_lock(panel);
	return rc;
}

static void _dsi_display_continuous_clk_ctrl(struct dsi_display *display,
					     bool enable)
{
	int i;
	struct dsi_display_ctrl *ctrl;

	if (!display || !display->panel->host_config.force_hs_clk_lane)
		return;

	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];

		/*
		 * For phy ver 4.0 chipsets, configure DSI controller and
		 * DSI PHY to force clk lane to HS mode always whereas
		 * for other phy ver chipsets, configure DSI controller only.
		 */
		if (ctrl->phy->hw.ops.set_continuous_clk) {
			dsi_ctrl_hs_req_sel(ctrl->ctrl, true);
			dsi_ctrl_set_continuous_clk(ctrl->ctrl, enable);
			dsi_phy_set_continuous_clk(ctrl->phy, enable);
		} else {
			dsi_ctrl_set_continuous_clk(ctrl->ctrl, enable);
		}
	}
}

int dsi_display_cmd_receive(void *display, const char *cmd_buf,
		u32 cmd_buf_len,  u8 *recv_buf, u32 recv_buf_len)
{
	struct dsi_display *dsi_display = display;
	struct dsi_cmd_desc cmd = {};
	u8 cmd_payload[MAX_CMD_PAYLOAD_SIZE] = {0};
	bool state = false;
	int rc = -1;

	if (!dsi_display || !cmd_buf || !recv_buf) {
		DSI_ERR("[DSI] invalid params\n");
		return -EINVAL;
	}

	SDE_EVT32(cmd_buf_len, recv_buf_len);

	rc = dsi_display_cmd_prepare(cmd_buf, cmd_buf_len,
			&cmd, cmd_payload, MAX_CMD_PAYLOAD_SIZE);
	if (rc) {
		DSI_ERR("[DSI] command prepare failed, rc = %d\n", rc);
		return rc;
	}

	cmd.msg.rx_buf = recv_buf;
	cmd.msg.rx_len = recv_buf_len;

	mutex_lock(&dsi_display->display_lock);
	rc = dsi_display_ctrl_get_host_init_state(dsi_display, &state);
	if (rc || !state) {
		DSI_ERR("[DSI] Invalid host state = %d rc = %d\n",
			state, rc);
		rc = -EPERM;
		goto end;
	}

	rc = dsi_display_cmd_rx(dsi_display, &cmd);
	if (rc <= 0)
		DSI_ERR("[DSI] Display command receive failed, rc=%d\n", rc);

end:
	mutex_unlock(&dsi_display->display_lock);
	return rc;
}

int dsi_display_motUtil_transfer(void *display, const char *cmd_buf,
                u32 cmd_buf_len, struct motUtil *motUtil_data)
{
        struct dsi_display *dsi_display = display;
        struct dsi_cmd_desc cmd;
        u8 cmd_payload[MAX_CMD_PAYLOAD_SIZE];
        int rc = 0;
        bool state = false;

        if (!dsi_display || !cmd_buf) {
                DSI_ERR("[DSI] invalid params\n");
                return -EINVAL;
        }

        DSI_INFO("[DSI] Display dispUtil transfer\n");

        rc = dsi_display_dispUtil_prepare(cmd_buf, cmd_buf_len,
                                &cmd, cmd_payload, MAX_CMD_PAYLOAD_SIZE,
                                motUtil_data);
        if (rc) {
                DSI_ERR("[DSI] command prepare failed. rc %d\n", rc);
                return rc;
        }

        mutex_lock(&dsi_display->display_lock);
        rc = dsi_display_ctrl_get_host_init_state(dsi_display, &state);
        if (rc || !state) {
                DSI_ERR("[DSI] Invalid host state %d rc %d\n",
                                                state, rc);
                rc = -EPERM;
                goto end;
        }

        /*
         * rc will be returned from ops->transfer, which will be 0 or 1 for
         * DSI write command. rc will be returned for number of read bytes
         * for DSI read commad
         */
        rc = dsi_display->host.ops->transfer(&dsi_display->host,
                                                &cmd.msg);
end:
        mutex_unlock(&dsi_display->display_lock);
        return rc;
}

int dsi_display_soft_reset(void *display)
{
	struct dsi_display *dsi_display;
	struct dsi_display_ctrl *ctrl;
	int rc = 0;
	int i;

	if (!display)
		return -EINVAL;

	dsi_display = display;

	display_for_each_ctrl(i, dsi_display) {
		ctrl = &dsi_display->ctrl[i];
		rc = dsi_ctrl_soft_reset(ctrl->ctrl);
		if (rc) {
			DSI_ERR("[%s] failed to soft reset host_%d, rc=%d\n",
					dsi_display->name, i, rc);
			break;
		}
	}

	return rc;
}

enum dsi_pixel_format dsi_display_get_dst_format(
		struct drm_connector *connector,
		void *display)
{
	enum dsi_pixel_format format = DSI_PIXEL_FORMAT_MAX;
	struct dsi_display *dsi_display = (struct dsi_display *)display;

	if (!dsi_display || !dsi_display->panel) {
		DSI_ERR("Invalid params(s) dsi_display %pK, panel %pK\n",
			dsi_display,
			((dsi_display) ? dsi_display->panel : NULL));
		return format;
	}

	format = dsi_display->panel->host_config.dst_format;
	return format;
}

static void _dsi_display_setup_misr(struct dsi_display *display)
{
	int i;

	display_for_each_ctrl(i, display) {
		dsi_ctrl_setup_misr(display->ctrl[i].ctrl,
				display->misr_enable,
				display->misr_frame_count);
	}
}

int dsi_display_set_power(struct drm_connector *connector,
		int power_mode, void *disp)
{
	struct dsi_display *display = disp;
	int rc = 0;

	if (!display || !display->panel) {
		DSI_ERR("invalid display/panel\n");
		return -EINVAL;
	}

	switch (power_mode) {
	case SDE_MODE_DPMS_LP1:
		if (display->panel->power_mode == SDE_MODE_DPMS_LP2) {
			if (dsi_display_set_ulp_load(display, false) < 0)
				DSI_WARN("failed to set load for lp1 state\n");
		}
		rc = dsi_panel_set_lp1(display->panel);
		break;
	case SDE_MODE_DPMS_LP2:
		rc = dsi_panel_set_lp2(display->panel);
		if (dsi_display_set_ulp_load(display, true) < 0)
			DSI_WARN("failed to set load for lp2 state\n");
		break;
	case SDE_MODE_DPMS_ON:
		if (display->panel->power_mode == SDE_MODE_DPMS_LP2) {
			if (dsi_display_set_ulp_load(display, false) < 0)
				DSI_WARN("failed to set load for on state\n");
		}
		if ((display->panel->power_mode == SDE_MODE_DPMS_LP1) ||
			(display->panel->power_mode == SDE_MODE_DPMS_LP2))
			rc = dsi_panel_set_nolp(display->panel);
		break;
	case SDE_MODE_DPMS_OFF:
	default:
		return rc;
	}

	SDE_EVT32(display->panel->power_mode, power_mode, rc);
	DSI_DEBUG("Power mode transition from %d to %d %s",
			display->panel->power_mode, power_mode,
			rc ? "failed" : "successful");
	if (!rc)
		display->panel->power_mode = power_mode;

	return rc;
}

#ifdef CONFIG_DEBUG_FS
static bool dsi_display_is_te_based_esd(struct dsi_display *display)
{
	u32 status_mode = 0;

	if (!display->panel) {
		DSI_ERR("Invalid panel data\n");
		return false;
	}

	status_mode = display->panel->esd_config.status_mode;

	if ((status_mode == ESD_MODE_PANEL_TE ||
			status_mode == ESD_MODE_TE_CHK_REG_RD) &&
			gpio_is_valid(display->disp_te_gpio))
		return true;
	return false;
}

static ssize_t debugfs_dump_info_read(struct file *file,
				      char __user *user_buf,
				      size_t user_len,
				      loff_t *ppos)
{
	struct dsi_display *display = file->private_data;
	char *buf;
	u32 len = 0;
	int i;

	if (!display)
		return -ENODEV;

	if (*ppos)
		return 0;

	buf = kzalloc(SZ_4K, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	len += snprintf(buf + len, (SZ_4K - len), "name = %s\n", display->name);
	len += snprintf(buf + len, (SZ_4K - len),
			"\tResolution = %dx%d\n",
			display->config.video_timing.h_active,
			display->config.video_timing.v_active);

	display_for_each_ctrl(i, display) {
		len += snprintf(buf + len, (SZ_4K - len),
				"\tCTRL_%d:\n\t\tctrl = %s\n\t\tphy = %s\n",
				i, display->ctrl[i].ctrl->name,
				display->ctrl[i].phy->name);
	}

	len += snprintf(buf + len, (SZ_4K - len),
			"\tPanel = %s\n", display->panel->name);

	len += snprintf(buf + len, (SZ_4K - len),
			"\tClock master = %s\n",
			display->ctrl[display->clk_master_idx].ctrl->name);

	if (len > user_len)
		len = user_len;

	if (copy_to_user(user_buf, buf, len)) {
		kfree(buf);
		return -EFAULT;
	}

	*ppos += len;

	kfree(buf);
	return len;
}

static ssize_t debugfs_misr_setup(struct file *file,
				  const char __user *user_buf,
				  size_t user_len,
				  loff_t *ppos)
{
	struct dsi_display *display = file->private_data;
	char *buf;
	int rc = 0;
	size_t len;
	u32 enable, frame_count;

	if (!display)
		return -ENODEV;

	if (*ppos)
		return 0;

	buf = kzalloc(MISR_BUFF_SIZE, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	/* leave room for termination char */
	len = min_t(size_t, user_len, MISR_BUFF_SIZE - 1);
	if (copy_from_user(buf, user_buf, len)) {
		rc = -EINVAL;
		goto error;
	}

	buf[len] = '\0'; /* terminate the string */

	if (sscanf(buf, "%u %u", &enable, &frame_count) != 2) {
		rc = -EINVAL;
		goto error;
	}

	display->misr_enable = enable;
	display->misr_frame_count = frame_count;

	mutex_lock(&display->display_lock);

	if (!display->hw_ownership) {
		DSI_DEBUG("[%s] op not supported due to HW unavailability\n",
				display->name);
		rc = -EOPNOTSUPP;
		goto unlock;
	}

	rc = dsi_display_clk_ctrl(display->dsi_clk_handle,
			DSI_CORE_CLK, DSI_CLK_ON);
	if (rc) {
		DSI_ERR("[%s] failed to enable DSI core clocks, rc=%d\n",
		       display->name, rc);
		goto unlock;
	}

	_dsi_display_setup_misr(display);

	rc = dsi_display_clk_ctrl(display->dsi_clk_handle,
			DSI_CORE_CLK, DSI_CLK_OFF);
	if (rc) {
		DSI_ERR("[%s] failed to disable DSI core clocks, rc=%d\n",
		       display->name, rc);
		goto unlock;
	}

	rc = user_len;
unlock:
	mutex_unlock(&display->display_lock);
error:
	kfree(buf);
	return rc;
}

static ssize_t debugfs_misr_read(struct file *file,
				 char __user *user_buf,
				 size_t user_len,
				 loff_t *ppos)
{
	struct dsi_display *display = file->private_data;
	char *buf;
	u32 len = 0;
	int rc = 0;
	struct dsi_ctrl *dsi_ctrl;
	int i;
	u32 misr;
	size_t max_len = min_t(size_t, user_len, MISR_BUFF_SIZE);

	if (!display)
		return -ENODEV;

	if (*ppos)
		return 0;

	buf = kzalloc(max_len, GFP_KERNEL);
	if (ZERO_OR_NULL_PTR(buf))
		return -ENOMEM;

	mutex_lock(&display->display_lock);

	if (!display->hw_ownership) {
		DSI_DEBUG("[%s] op not supported due to HW unavailability\n",
				display->name);
		rc = -EOPNOTSUPP;
		goto error;
	}

	rc = dsi_display_clk_ctrl(display->dsi_clk_handle,
			DSI_CORE_CLK, DSI_CLK_ON);
	if (rc) {
		DSI_ERR("[%s] failed to enable DSI core clocks, rc=%d\n",
		       display->name, rc);
		goto error;
	}

	display_for_each_ctrl(i, display) {
		dsi_ctrl = display->ctrl[i].ctrl;
		misr = dsi_ctrl_collect_misr(display->ctrl[i].ctrl);

		len += snprintf((buf + len), max_len - len,
			"DSI_%d MISR: 0x%x\n", dsi_ctrl->cell_index, misr);

		if (len >= max_len)
			break;
	}

	rc = dsi_display_clk_ctrl(display->dsi_clk_handle,
			DSI_CORE_CLK, DSI_CLK_OFF);
	if (rc) {
		DSI_ERR("[%s] failed to disable DSI core clocks, rc=%d\n",
		       display->name, rc);
		goto error;
	}

	if (copy_to_user(user_buf, buf, max_len)) {
		rc = -EFAULT;
		goto error;
	}

	*ppos += len;

error:
	mutex_unlock(&display->display_lock);
	kfree(buf);
	return len;
}

static ssize_t debugfs_esd_trigger_check(struct file *file,
				  const char __user *user_buf,
				  size_t user_len,
				  loff_t *ppos)
{
	struct dsi_display *display = file->private_data;
	char *buf;
	int rc = 0;
	struct drm_panel_esd_config *esd_config = &display->panel->esd_config;
	u32 esd_trigger;
	size_t len;

	if (!display)
		return -ENODEV;

	if (*ppos)
		return 0;

	if (user_len > sizeof(u32))
		return -EINVAL;

	if (!user_len || !user_buf)
		return -EINVAL;

	if (!display->panel ||
		atomic_read(&display->panel->esd_recovery_pending))
		return user_len;

	if (!esd_config->esd_enabled) {
		DSI_ERR("ESD feature is not enabled\n");
		return -EINVAL;
	}

	buf = kzalloc(ESD_TRIGGER_STRING_MAX_LEN, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	len = min_t(size_t, user_len, ESD_TRIGGER_STRING_MAX_LEN - 1);
	if (copy_from_user(buf, user_buf, len)) {
		rc = -EINVAL;
		goto error;
	}

	buf[len] = '\0'; /* terminate the string */

	if (kstrtouint(buf, 10, &esd_trigger)) {
		rc = -EINVAL;
		goto error;
	}

	if (esd_trigger != 1) {
		rc = -EINVAL;
		goto error;
	}

	display->esd_trigger = esd_trigger;

	mutex_lock(&display->display_lock);

	if (!display->hw_ownership) {
		DSI_DEBUG("[%s] op not supported due to HW unavailability\n",
				display->name);
		rc = -EOPNOTSUPP;
		goto unlock;
	}

	if (display->esd_trigger) {
		DSI_INFO("ESD attack triggered by user\n");
		rc = dsi_panel_trigger_esd_attack(display->panel,
						display->trusted_vm_env);
		if (rc) {
			DSI_ERR("Failed to trigger ESD attack\n");
			goto unlock;
		}
	}

	rc = len;
unlock:
	mutex_unlock(&display->display_lock);
error:
	kfree(buf);
	return rc;
}

static ssize_t debugfs_alter_esd_check_mode(struct file *file,
				  const char __user *user_buf,
				  size_t user_len,
				  loff_t *ppos)
{
	struct dsi_display *display = file->private_data;
	struct drm_panel_esd_config *esd_config;
	char *buf;
	int rc = 0;
	size_t len;

	if (!display)
		return -ENODEV;

	if (*ppos)
		return 0;

	buf = kzalloc(ESD_MODE_STRING_MAX_LEN, GFP_KERNEL);
	if (ZERO_OR_NULL_PTR(buf))
		return -ENOMEM;

	len = min_t(size_t, user_len, ESD_MODE_STRING_MAX_LEN - 1);
	if (copy_from_user(buf, user_buf, len)) {
		rc = -EINVAL;
		goto error;
	}

	buf[len] = '\0'; /* terminate the string */
	if (!display->panel) {
		rc = -EINVAL;
		goto error;
	}

	esd_config = &display->panel->esd_config;
	if (!esd_config) {
		DSI_ERR("Invalid panel esd config\n");
		rc = -EINVAL;
		goto error;
	}

	if (!esd_config->esd_enabled) {
		rc = -EINVAL;
		goto error;
	}

	if (!strcmp(buf, "te_signal_check\n")) {
		if (display->panel->panel_mode == DSI_OP_VIDEO_MODE) {
			DSI_INFO("TE based ESD check for Video Mode panels is not allowed\n");
			rc = -EINVAL;
			goto error;
		}
		DSI_INFO("ESD check is switched to TE mode by user\n");
		esd_config->status_mode = ESD_MODE_PANEL_TE;
		dsi_display_change_te_irq_status(display, true);
	}

	if (!strcmp(buf, "reg_read\n")) {
		DSI_INFO("ESD check is switched to reg read by user\n");
		rc = dsi_panel_parse_esd_reg_read_configs(display->panel);
		if (rc) {
			DSI_ERR("failed to alter esd check mode,rc=%d\n",
						rc);
			rc = user_len;
			goto error;
		}
		esd_config->status_mode = ESD_MODE_REG_READ;
		if (dsi_display_is_te_based_esd(display))
			dsi_display_change_te_irq_status(display, false);
	}

	if (!strcmp(buf, "esd_sw_sim_success\n"))
		esd_config->status_mode = ESD_MODE_SW_SIM_SUCCESS;

	if (!strcmp(buf, "esd_sw_sim_failure\n"))
		esd_config->status_mode = ESD_MODE_SW_SIM_FAILURE;

	rc = len;
error:
	kfree(buf);
	return rc;
}

static ssize_t debugfs_read_esd_check_mode(struct file *file,
				 char __user *user_buf,
				 size_t user_len,
				 loff_t *ppos)
{
	struct dsi_display *display = file->private_data;
	struct drm_panel_esd_config *esd_config;
	char *buf;
	int rc = 0;
	size_t len = 0;

	if (!display)
		return -ENODEV;

	if (*ppos)
		return 0;

	if (!display->panel) {
		DSI_ERR("invalid panel data\n");
		return -EINVAL;
	}

	buf = kzalloc(ESD_MODE_STRING_MAX_LEN, GFP_KERNEL);
	if (ZERO_OR_NULL_PTR(buf))
		return -ENOMEM;

	esd_config = &display->panel->esd_config;
	if (!esd_config) {
		DSI_ERR("Invalid panel esd config\n");
		rc = -EINVAL;
		goto error;
	}

	len = min_t(size_t, user_len, ESD_MODE_STRING_MAX_LEN - 1);
	if (!esd_config->esd_enabled) {
		rc = snprintf(buf, len, "ESD feature not enabled");
		goto output_mode;
	}

	switch (esd_config->status_mode) {
	case ESD_MODE_REG_READ:
		rc = snprintf(buf, len, "reg_read");
		break;
	case ESD_MODE_PANEL_TE:
		rc = snprintf(buf, len, "te_signal_check");
		break;
	case ESD_MODE_SW_SIM_FAILURE:
		rc = snprintf(buf, len, "esd_sw_sim_failure");
		break;
	case ESD_MODE_SW_SIM_SUCCESS:
		rc = snprintf(buf, len, "esd_sw_sim_success");
		break;
	case ESD_MODE_TE_CHK_REG_RD:
		rc = snprintf(buf, len, "te_chk_reg_rd");
	default:
		rc = snprintf(buf, len, "invalid");
		break;
	}

output_mode:
	if (!rc) {
		rc = -EINVAL;
		goto error;
	}

	if (copy_to_user(user_buf, buf, len)) {
		rc = -EFAULT;
		goto error;
	}

	*ppos += len;

error:
	kfree(buf);
	return len;
}

static ssize_t debugfs_update_cmd_scheduling_params(struct file *file,
				  const char __user *user_buf,
				  size_t user_len,
				  loff_t *ppos)
{
	struct dsi_display *display = file->private_data;
	struct dsi_display_ctrl *display_ctrl;
	char *buf;
	int rc = 0;
	u32 line = 0, window = 0;
	size_t len;
	int i;

	if (!display)
		return -ENODEV;

	if (*ppos)
		return 0;

	buf = kzalloc(256, GFP_KERNEL);
	if (ZERO_OR_NULL_PTR(buf))
		return -ENOMEM;

	len = min_t(size_t, user_len, 255);
	if (copy_from_user(buf, user_buf, len)) {
		rc = -EINVAL;
		goto error;
	}

	buf[len] = '\0'; /* terminate the string */

	if (sscanf(buf, "%d %d", &line, &window) != 2)
		return -EFAULT;

	display_for_each_ctrl(i, display) {
		struct dsi_ctrl *ctrl;

		display_ctrl = &display->ctrl[i];
		if (!display_ctrl->ctrl)
			continue;

		ctrl = display_ctrl->ctrl;
		ctrl->host_config.common_config.dma_sched_line = line;
		ctrl->host_config.common_config.dma_sched_window = window;
	}

	rc = len;

error:
	kfree(buf);
	return rc;
}

static ssize_t debugfs_read_cmd_scheduling_params(struct file *file,
				 char __user *user_buf,
				 size_t user_len,
				 loff_t *ppos)
{
	struct dsi_display *display = file->private_data;
	struct dsi_display_ctrl *m_ctrl;
	struct dsi_ctrl *ctrl;
	char *buf;
	u32 len = 0;
	int rc = 0;
	size_t max_len = min_t(size_t, user_len, SZ_4K);

	if (!display)
		return -ENODEV;

	if (*ppos)
		return 0;

	m_ctrl = &display->ctrl[display->cmd_master_idx];
	ctrl = m_ctrl->ctrl;

	buf = kzalloc(max_len, GFP_KERNEL);
	if (ZERO_OR_NULL_PTR(buf))
		return -ENOMEM;

	len += scnprintf(buf, max_len, "Schedule command window start: %d\n",
			ctrl->host_config.common_config.dma_sched_line);
	len += scnprintf((buf + len), max_len - len,
			"Schedule command window width: %d\n",
			ctrl->host_config.common_config.dma_sched_window);

	if (len > max_len)
		len = max_len;

	if (copy_to_user(user_buf, buf, len)) {
		rc = -EFAULT;
		goto error;
	}

	*ppos += len;

error:
	kfree(buf);
	return len;

}

static const struct file_operations dump_info_fops = {
	.open = simple_open,
	.read = debugfs_dump_info_read,
};

static const struct file_operations misr_data_fops = {
	.open = simple_open,
	.read = debugfs_misr_read,
	.write = debugfs_misr_setup,
};

static const struct file_operations esd_trigger_fops = {
	.open = simple_open,
	.write = debugfs_esd_trigger_check,
};

static const struct file_operations esd_check_mode_fops = {
	.open = simple_open,
	.write = debugfs_alter_esd_check_mode,
	.read = debugfs_read_esd_check_mode,
};

static const struct file_operations dsi_command_scheduling_fops = {
	.open = simple_open,
	.write = debugfs_update_cmd_scheduling_params,
	.read = debugfs_read_cmd_scheduling_params,
};

static int dsi_display_debugfs_init(struct dsi_display *display)
{
	int rc = 0;
	struct dentry *dir, *dump_file, *misr_data;
	char name[MAX_NAME_SIZE];
	char panel_name[SEC_PANEL_NAME_MAX_LEN];
	char secondary_panel_str[] = "_secondary";
	int i;

	strlcpy(panel_name, display->name, SEC_PANEL_NAME_MAX_LEN);
	if (strcmp(display->display_type, "secondary") == 0)
		strlcat(panel_name, secondary_panel_str, SEC_PANEL_NAME_MAX_LEN);

	dir = debugfs_create_dir(panel_name, NULL);
	if (IS_ERR_OR_NULL(dir)) {
		rc = PTR_ERR(dir);
		DSI_ERR("[%s] debugfs create dir failed, rc = %d\n",
		       display->name, rc);
		goto error;
	}

	dump_file = debugfs_create_file("dump_info",
					0400,
					dir,
					display,
					&dump_info_fops);
	if (IS_ERR_OR_NULL(dump_file)) {
		rc = PTR_ERR(dump_file);
		DSI_ERR("[%s] debugfs create dump info file failed, rc=%d\n",
		       display->name, rc);
		goto error_remove_dir;
	}

	dump_file = debugfs_create_file("esd_trigger",
					0644,
					dir,
					display,
					&esd_trigger_fops);
	if (IS_ERR_OR_NULL(dump_file)) {
		rc = PTR_ERR(dump_file);
		DSI_ERR("[%s] debugfs for esd trigger file failed, rc=%d\n",
		       display->name, rc);
		goto error_remove_dir;
	}

	dump_file = debugfs_create_file("esd_check_mode",
					0644,
					dir,
					display,
					&esd_check_mode_fops);
	if (IS_ERR_OR_NULL(dump_file)) {
		rc = PTR_ERR(dump_file);
		DSI_ERR("[%s] debugfs for esd check mode failed, rc=%d\n",
		       display->name, rc);
		goto error_remove_dir;
	}

	dump_file = debugfs_create_file("cmd_sched_params",
					0644,
					dir,
					display,
					&dsi_command_scheduling_fops);
	if (IS_ERR_OR_NULL(dump_file)) {
		rc = PTR_ERR(dump_file);
		DSI_ERR("[%s] debugfs for cmd scheduling file failed, rc=%d\n",
		       display->name, rc);
		goto error_remove_dir;
	}

	misr_data = debugfs_create_file("misr_data",
					0600,
					dir,
					display,
					&misr_data_fops);
	if (IS_ERR_OR_NULL(misr_data)) {
		rc = PTR_ERR(misr_data);
		DSI_ERR("[%s] debugfs create misr datafile failed, rc=%d\n",
		       display->name, rc);
		goto error_remove_dir;
	}

	display_for_each_ctrl(i, display) {
		struct msm_dsi_phy *phy = display->ctrl[i].phy;

		if (!phy || !phy->name)
			continue;

		snprintf(name, ARRAY_SIZE(name),
				"%s_allow_phy_power_off", phy->name);
		dump_file = debugfs_create_bool(name, 0600, dir,
				&phy->allow_phy_power_off);
		if (IS_ERR_OR_NULL(dump_file)) {
			rc = PTR_ERR(dump_file);
			DSI_ERR("[%s] debugfs create %s failed, rc=%d\n",
			       display->name, name, rc);
			goto error_remove_dir;
		}

		snprintf(name, ARRAY_SIZE(name),
				"%s_regulator_min_datarate_bps", phy->name);
		dump_file = debugfs_create_u32(name, 0600, dir,
				&phy->regulator_min_datarate_bps);
		if (IS_ERR_OR_NULL(dump_file)) {
			rc = PTR_ERR(dump_file);
			DSI_ERR("[%s] debugfs create %s failed, rc=%d\n",
			       display->name, name, rc);
			goto error_remove_dir;
		}
	}

	if (!debugfs_create_bool("ulps_feature_enable", 0600, dir,
			&display->panel->ulps_feature_enabled)) {
		DSI_ERR("[%s] debugfs create ulps feature enable file failed\n",
		       display->name);
		goto error_remove_dir;
	}

	if (!debugfs_create_bool("ulps_suspend_feature_enable", 0600, dir,
			&display->panel->ulps_suspend_enabled)) {
		DSI_ERR("[%s] debugfs create ulps-suspend feature enable file failed\n",
		       display->name);
		goto error_remove_dir;
	}

	if (!debugfs_create_bool("ulps_status", 0400, dir,
			&display->ulps_enabled)) {
		DSI_ERR("[%s] debugfs create ulps status file failed\n",
		       display->name);
		goto error_remove_dir;
	}

	if (!debugfs_create_u32("clk_gating_config", 0600, dir,
			&display->clk_gating_config)) {
		DSI_ERR("[%s] debugfs create clk gating config failed\n",
		       display->name);
		goto error_remove_dir;
	}

	display->root = dir;
	dsi_parser_dbg_init(display->parser, dir);

	return rc;
error_remove_dir:
	debugfs_remove(dir);
error:
	return rc;
}

static int dsi_display_debugfs_deinit(struct dsi_display *display)
{
	debugfs_remove_recursive(display->root);

	return 0;
}
#else
static int dsi_display_debugfs_init(struct dsi_display *display)
{
	return 0;
}
static int dsi_display_debugfs_deinit(struct dsi_display *display)
{
	return 0;
}
#endif /* CONFIG_DEBUG_FS */

static void adjust_timing_by_ctrl_count(const struct dsi_display *display,
					struct dsi_display_mode *mode)
{
	struct dsi_host_common_cfg *host = &display->panel->host_config;
	bool is_split_link = host->split_link.split_link_enabled;
	u32 sublinks_count = host->split_link.num_sublinks;

	if (is_split_link && sublinks_count > 1) {
		mode->timing.h_active /= sublinks_count;
		mode->timing.h_front_porch /= sublinks_count;
		mode->timing.h_sync_width /= sublinks_count;
		mode->timing.h_back_porch /= sublinks_count;
		mode->timing.h_skew /= sublinks_count;
		mode->pixel_clk_khz /= sublinks_count;
	} else {
		if ((mode->priv_info) && (mode->priv_info->dsc_enabled))
			mode->priv_info->dsc.config.pic_width =
				mode->timing.h_active;
		mode->timing.h_active /= display->ctrl_count;
		mode->timing.h_front_porch /= display->ctrl_count;
		mode->timing.h_sync_width /= display->ctrl_count;
		mode->timing.h_back_porch /= display->ctrl_count;
		mode->timing.h_skew /= display->ctrl_count;
		mode->pixel_clk_khz /= display->ctrl_count;
	}
}

static int dsi_display_is_ulps_req_valid(struct dsi_display *display,
		bool enable)
{
	/* TODO: make checks based on cont. splash */

	DSI_DEBUG("checking ulps req validity\n");

	if (atomic_read(&display->panel->esd_recovery_pending)) {
		DSI_DEBUG("%s: ESD recovery sequence underway\n", __func__);
		return false;
	}

	if (!dsi_panel_ulps_feature_enabled(display->panel) &&
			!display->panel->ulps_suspend_enabled) {
		DSI_DEBUG("%s: ULPS feature is not enabled\n", __func__);
		return false;
	}

	if (!dsi_panel_initialized(display->panel) &&
			!display->panel->ulps_suspend_enabled) {
		DSI_DEBUG("%s: panel not yet initialized\n", __func__);
		return false;
	}

	if (enable && display->ulps_enabled) {
		DSI_DEBUG("ULPS already enabled\n");
		return false;
	} else if (!enable && !display->ulps_enabled) {
		DSI_DEBUG("ULPS already disabled\n");
		return false;
	}

	/*
	 * No need to enter ULPS when transitioning from splash screen to
	 * boot animation or trusted vm environments since it is expected
	 * that the clocks would be turned right back on.
	 */
	if (enable && is_skip_op_required(display))
		return false;

	return true;
}


/**
 * dsi_display_set_ulps() - set ULPS state for DSI lanes.
 * @dsi_display:         DSI display handle.
 * @enable:           enable/disable ULPS.
 *
 * ULPS can be enabled/disabled after DSI host engine is turned on.
 *
 * Return: error code.
 */
static int dsi_display_set_ulps(struct dsi_display *display, bool enable)
{
	int rc = 0;
	int i = 0;
	struct dsi_display_ctrl *m_ctrl, *ctrl;


	if (!display) {
		DSI_ERR("Invalid params\n");
		return -EINVAL;
	}

	if (!dsi_display_is_ulps_req_valid(display, enable)) {
		DSI_DEBUG("%s: skipping ULPS config, enable=%d\n",
			__func__, enable);
		return 0;
	}

	m_ctrl = &display->ctrl[display->cmd_master_idx];
	/*
	 * ULPS entry-exit can be either through the DSI controller or
	 * the DSI PHY depending on hardware variation. For some chipsets,
	 * both controller version and phy version ulps entry-exit ops can
	 * be present. To handle such cases, send ulps request through PHY,
	 * if ulps request is handled in PHY, then no need to send request
	 * through controller.
	 */

	rc = dsi_phy_set_ulps(m_ctrl->phy, &display->config, enable,
			display->clamp_enabled);

	if (rc == DSI_PHY_ULPS_ERROR) {
		DSI_ERR("Ulps PHY state change(%d) failed\n", enable);
		return -EINVAL;
	}

	else if (rc == DSI_PHY_ULPS_HANDLED) {
		display_for_each_ctrl(i, display) {
			ctrl = &display->ctrl[i];
			if (!ctrl->ctrl || (ctrl == m_ctrl))
				continue;

			rc = dsi_phy_set_ulps(ctrl->phy, &display->config,
					enable, display->clamp_enabled);
			if (rc == DSI_PHY_ULPS_ERROR) {
				DSI_ERR("Ulps PHY state change(%d) failed\n",
						enable);
				return -EINVAL;
			}
		}
	}

	else if (rc == DSI_PHY_ULPS_NOT_HANDLED) {
		rc = dsi_ctrl_set_ulps(m_ctrl->ctrl, enable);
		if (rc) {
			DSI_ERR("Ulps controller state change(%d) failed\n",
					enable);
			return rc;
		}
		display_for_each_ctrl(i, display) {
			ctrl = &display->ctrl[i];
			if (!ctrl->ctrl || (ctrl == m_ctrl))
				continue;

			rc = dsi_ctrl_set_ulps(ctrl->ctrl, enable);
			if (rc) {
				DSI_ERR("Ulps controller state change(%d) failed\n",
						enable);
				return rc;
			}
		}
	}

	display->ulps_enabled = enable;
	return 0;
}

/**
 * dsi_display_set_clamp() - set clamp state for DSI IO.
 * @dsi_display:         DSI display handle.
 * @enable:           enable/disable clamping.
 *
 * Return: error code.
 */
static int dsi_display_set_clamp(struct dsi_display *display, bool enable)
{
	int rc = 0;
	int i = 0;
	struct dsi_display_ctrl *m_ctrl, *ctrl;
	bool ulps_enabled = false;


	if (!display) {
		DSI_ERR("Invalid params\n");
		return -EINVAL;
	}

	m_ctrl = &display->ctrl[display->cmd_master_idx];
	ulps_enabled = display->ulps_enabled;

	/*
	 * Clamp control can be either through the DSI controller or
	 * the DSI PHY depending on hardware variation
	 */
	rc = dsi_ctrl_set_clamp_state(m_ctrl->ctrl, enable, ulps_enabled);
	if (rc) {
		DSI_ERR("DSI ctrl clamp state change(%d) failed\n", enable);
		return rc;
	}

	rc = dsi_phy_set_clamp_state(m_ctrl->phy, enable);
	if (rc) {
		DSI_ERR("DSI phy clamp state change(%d) failed\n", enable);
		return rc;
	}

	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		if (!ctrl->ctrl || (ctrl == m_ctrl))
			continue;

		rc = dsi_ctrl_set_clamp_state(ctrl->ctrl, enable, ulps_enabled);
		if (rc) {
			DSI_ERR("DSI Clamp state change(%d) failed\n", enable);
			return rc;
		}

		rc = dsi_phy_set_clamp_state(ctrl->phy, enable);
		if (rc) {
			DSI_ERR("DSI phy clamp state change(%d) failed\n",
				enable);
			return rc;
		}

		DSI_DEBUG("Clamps %s for ctrl%d\n",
			enable ? "enabled" : "disabled", i);
	}

	display->clamp_enabled = enable;
	return 0;
}

/**
 * dsi_display_setup_ctrl() - setup DSI controller.
 * @dsi_display:         DSI display handle.
 *
 * Return: error code.
 */
static int dsi_display_ctrl_setup(struct dsi_display *display)
{
	int rc = 0;
	int i = 0;
	struct dsi_display_ctrl *ctrl, *m_ctrl;


	if (!display) {
		DSI_ERR("Invalid params\n");
		return -EINVAL;
	}

	m_ctrl = &display->ctrl[display->cmd_master_idx];
	rc = dsi_ctrl_setup(m_ctrl->ctrl);
	if (rc) {
		DSI_ERR("DSI controller setup failed\n");
		return rc;
	}

	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		if (!ctrl->ctrl || (ctrl == m_ctrl))
			continue;

		rc = dsi_ctrl_setup(ctrl->ctrl);
		if (rc) {
			DSI_ERR("DSI controller setup failed\n");
			return rc;
		}
	}
	return 0;
}

static int dsi_display_phy_enable(struct dsi_display *display);

/**
 * dsi_display_phy_idle_on() - enable DSI PHY while coming out of idle screen.
 * @dsi_display:         DSI display handle.
 * @mmss_clamp:          True if clamp is enabled.
 *
 * Return: error code.
 */
static int dsi_display_phy_idle_on(struct dsi_display *display,
		bool mmss_clamp)
{
	int rc = 0;
	int i = 0;
	struct dsi_display_ctrl *m_ctrl, *ctrl;


	if (!display) {
		DSI_ERR("Invalid params\n");
		return -EINVAL;
	}

	if (mmss_clamp && !display->phy_idle_power_off) {
		dsi_display_phy_enable(display);
		return 0;
	}

	m_ctrl = &display->ctrl[display->cmd_master_idx];
	rc = dsi_phy_idle_ctrl(m_ctrl->phy, true);
	if (rc) {
		DSI_ERR("DSI controller setup failed\n");
		return rc;
	}

	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		if (!ctrl->ctrl || (ctrl == m_ctrl))
			continue;

		rc = dsi_phy_idle_ctrl(ctrl->phy, true);
		if (rc) {
			DSI_ERR("DSI controller setup failed\n");
			return rc;
		}
	}
	display->phy_idle_power_off = false;
	return 0;
}

/**
 * dsi_display_phy_idle_off() - disable DSI PHY while going to idle screen.
 * @dsi_display:         DSI display handle.
 *
 * Return: error code.
 */
static int dsi_display_phy_idle_off(struct dsi_display *display)
{
	int rc = 0;
	int i = 0;
	struct dsi_display_ctrl *m_ctrl, *ctrl;

	if (!display) {
		DSI_ERR("Invalid params\n");
		return -EINVAL;
	}

	display_for_each_ctrl(i, display) {
		struct msm_dsi_phy *phy = display->ctrl[i].phy;

		if (!phy)
			continue;

		if (!phy->allow_phy_power_off) {
			DSI_DEBUG("phy doesn't support this feature\n");
			return 0;
		}
	}

	m_ctrl = &display->ctrl[display->cmd_master_idx];

	rc = dsi_phy_idle_ctrl(m_ctrl->phy, false);
	if (rc) {
		DSI_ERR("[%s] failed to enable cmd engine, rc=%d\n",
		       display->name, rc);
		return rc;
	}

	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		if (!ctrl->ctrl || (ctrl == m_ctrl))
			continue;

		rc = dsi_phy_idle_ctrl(ctrl->phy, false);
		if (rc) {
			DSI_ERR("DSI controller setup failed\n");
			return rc;
		}
	}
	display->phy_idle_power_off = true;
	return 0;
}

void dsi_display_enable_event(struct drm_connector *connector,
		struct dsi_display *display,
		uint32_t event_idx, struct dsi_event_cb_info *event_info,
		bool enable)
{
	uint32_t irq_status_idx = DSI_STATUS_INTERRUPT_COUNT;
	int i;

	if (!display) {
		DSI_ERR("invalid display\n");
		return;
	}

	if (event_info)
		event_info->event_idx = event_idx;

	switch (event_idx) {
	case SDE_CONN_EVENT_VID_DONE:
		irq_status_idx = DSI_SINT_VIDEO_MODE_FRAME_DONE;
		break;
	case SDE_CONN_EVENT_CMD_DONE:
		irq_status_idx = DSI_SINT_CMD_FRAME_DONE;
		break;
	case SDE_CONN_EVENT_VID_FIFO_OVERFLOW:
	case SDE_CONN_EVENT_CMD_FIFO_UNDERFLOW:
		if (event_info) {
			display_for_each_ctrl(i, display)
				display->ctrl[i].ctrl->recovery_cb =
							*event_info;
		}
		break;
	case SDE_CONN_EVENT_PANEL_ID:
		if (event_info)
			display_for_each_ctrl(i, display)
				display->ctrl[i].ctrl->panel_id_cb
				    = *event_info;
		break;
	default:
		/* nothing to do */
		DSI_DEBUG("[%s] unhandled event %d\n", display->name, event_idx);
		return;
	}

	if (enable) {
		display_for_each_ctrl(i, display)
			dsi_ctrl_enable_status_interrupt(
					display->ctrl[i].ctrl, irq_status_idx,
					event_info);
	} else {
		display_for_each_ctrl(i, display)
			dsi_ctrl_disable_status_interrupt(
					display->ctrl[i].ctrl, irq_status_idx);
	}
}

int dsi_display_ctrl_vreg_on(struct dsi_display *display)
{
	int rc = 0;
	int i;
	struct dsi_display_ctrl *ctrl;
	struct dsi_ctrl *dsi_ctrl;

	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		if (!ctrl->ctrl)
			continue;

		dsi_ctrl = ctrl->ctrl;
		if (dsi_ctrl->current_state.host_initialized) {
			rc = dsi_pwr_enable_regulator(
					&dsi_ctrl->pwr_info.host_pwr, true);
			if (rc) {
				DSI_ERR("[%s] Failed to enable vreg, rc=%d\n",
				       dsi_ctrl->name, rc);
				goto error;
			}
			DSI_DEBUG("[%s] Enable ctrl vreg\n", dsi_ctrl->name);
		}
	}
error:
	return rc;
}

int dsi_display_ctrl_vreg_off(struct dsi_display *display)
{
	int rc = 0;
	int i;
	struct dsi_display_ctrl *ctrl;
	struct dsi_ctrl *dsi_ctrl;

	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		if (!ctrl->ctrl)
			continue;

		dsi_ctrl = ctrl->ctrl;
		if (dsi_ctrl->current_state.host_initialized) {
			rc = dsi_pwr_enable_regulator(
				&dsi_ctrl->pwr_info.host_pwr, false);
			if (rc) {
				DSI_ERR("[%s] Failed to disable vreg, rc=%d\n",
				       dsi_ctrl->name, rc);
				goto error;
			}
			DSI_DEBUG("[%s] Disable ctrl vreg\n", dsi_ctrl->name);
		}
	}
error:
	return rc;
}


static int dsi_display_ctrl_power_on(struct dsi_display *display)
{
	int rc = 0;
	int i;
	struct dsi_display_ctrl *ctrl;

	/* Sequence does not matter for split dsi usecases */
	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		if (!ctrl->ctrl)
			continue;

		rc = dsi_ctrl_set_power_state(ctrl->ctrl,
					      DSI_CTRL_POWER_VREG_ON);
		if (rc) {
			DSI_ERR("[%s] Failed to set power state, rc=%d\n",
			       ctrl->ctrl->name, rc);
			goto error;
		}
	}

	return rc;
error:
	for (i = i - 1; i >= 0; i--) {
		ctrl = &display->ctrl[i];
		if (!ctrl->ctrl)
			continue;
		(void)dsi_ctrl_set_power_state(ctrl->ctrl,
			DSI_CTRL_POWER_VREG_OFF);
	}
	return rc;
}

static int dsi_display_ctrl_power_off(struct dsi_display *display)
{
	int rc = 0;
	int i;
	struct dsi_display_ctrl *ctrl;

	/* Sequence does not matter for split dsi usecases */
	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		if (!ctrl->ctrl)
			continue;

		rc = dsi_ctrl_set_power_state(ctrl->ctrl,
			DSI_CTRL_POWER_VREG_OFF);
		if (rc) {
			DSI_ERR("[%s] Failed to power off, rc=%d\n",
			       ctrl->ctrl->name, rc);
			goto error;
		}
	}
error:
	return rc;
}

static void dsi_display_parse_cmdline_topology(struct dsi_display *display,
					unsigned int display_type)
{
	char *boot_str = NULL;
	char *str = NULL;
	char *sw_te = NULL;
	unsigned long cmdline_topology = NO_OVERRIDE;
	unsigned long cmdline_timing = NO_OVERRIDE;
	unsigned long panel_id = NO_OVERRIDE;

	if (display_type >= MAX_DSI_ACTIVE_DISPLAY) {
		DSI_ERR("display_type=%d not supported\n", display_type);
		goto end;
	}

	if (display_type == DSI_PRIMARY)
		boot_str = dsi_display_primary;
	else
		boot_str = dsi_display_secondary;

	sw_te = strnstr(boot_str, ":sim-swte", strlen(boot_str));
	if (sw_te)
		display->sw_te_using_wd = true;

	str = strnstr(boot_str, ":panelid", strlen(boot_str));
	if (str) {
		if (kstrtol(str + strlen(":panelid"), INT_BASE_10,
				(unsigned long *)&panel_id)) {
			DSI_INFO("panel id not found: %s\n", boot_str);
		} else {
			DSI_INFO("panel id found: %lx\n", panel_id);
			display->panel_id = panel_id;
		}
	}

	str = strnstr(boot_str, ":config", strlen(boot_str));
	if (str) {
		if (sscanf(str, ":config%lu", &cmdline_topology) != 1) {
			DSI_ERR("invalid config index override: %s\n",
				boot_str);
			goto end;
		}
	}

	str = strnstr(boot_str, ":timing", strlen(boot_str));
	if (str) {
		if (sscanf(str, ":timing%lu", &cmdline_timing) != 1) {
			DSI_ERR("invalid timing index override: %s\n",
				boot_str);
			cmdline_topology = NO_OVERRIDE;
			goto end;
		}
	}
	DSI_DEBUG("successfully parsed command line topology and timing\n");
end:
	display->cmdline_topology = cmdline_topology;
	display->cmdline_timing = cmdline_timing;
}

/**
 * dsi_display_parse_boot_display_selection()- Parse DSI boot display name
 *
 * Return:	returns error status
 */
static int dsi_display_parse_boot_display_selection(void)
{
	char *pos = NULL;
	char disp_buf[MAX_CMDLINE_PARAM_LEN] = {'\0'};
	int i, j;

	for (i = 0; i < MAX_DSI_ACTIVE_DISPLAY; i++) {
		strlcpy(disp_buf, boot_displays[i].boot_param,
			MAX_CMDLINE_PARAM_LEN);

		pos = strnstr(disp_buf, ":", strlen(disp_buf));

		/* Use ':' as a delimiter to retrieve the display name */
		if (!pos) {
			DSI_DEBUG("display name[%s]is not valid\n", disp_buf);
			continue;
		}

		for (j = 0; (disp_buf + j) < pos; j++)
			boot_displays[i].name[j] = *(disp_buf + j);

		boot_displays[i].name[j] = '\0';

		boot_displays[i].boot_disp_en = true;
	}

	return 0;
}

static int dsi_display_phy_power_on(struct dsi_display *display)
{
	int rc = 0;
	int i;
	struct dsi_display_ctrl *ctrl;

	/* Sequence does not matter for split dsi usecases */
	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		if (!ctrl->ctrl)
			continue;

		rc = dsi_phy_set_power_state(ctrl->phy, true);
		if (rc) {
			DSI_ERR("[%s] Failed to set power state, rc=%d\n",
			       ctrl->phy->name, rc);
			goto error;
		}
	}

	return rc;
error:
	for (i = i - 1; i >= 0; i--) {
		ctrl = &display->ctrl[i];
		if (!ctrl->phy)
			continue;
		(void)dsi_phy_set_power_state(ctrl->phy, false);
	}
	return rc;
}

static int dsi_display_phy_power_off(struct dsi_display *display)
{
	int rc = 0;
	int i;
	struct dsi_display_ctrl *ctrl;

	/* Sequence does not matter for split dsi usecases */
	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		if (!ctrl->phy)
			continue;

		rc = dsi_phy_set_power_state(ctrl->phy, false);
		if (rc) {
			DSI_ERR("[%s] Failed to power off, rc=%d\n",
			       ctrl->ctrl->name, rc);
			goto error;
		}
	}
error:
	return rc;
}

#if defined(CONFIG_DEEPSLEEP) || defined(CONFIG_HIBERNATION)
int dsi_display_unset_clk_src(struct dsi_display *display)
{
	int rc = 0;
	int i;
	struct dsi_display_ctrl *ctrl;

	DSI_DEBUG("[%s] unset source clocks\n", display->name);

	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		if (!ctrl->ctrl)
			continue;

		/* set ctrl clocks to xo source */
		rc = dsi_ctrl_set_clock_source(ctrl->ctrl,
			   &display->clock_info.xo_clks);
		if (rc) {
			DSI_ERR("[%s] failed to set source clocks, rc=%d\n",
				   display->name, rc);
			return rc;
		}
	}
	return 0;
}
#else
inline int dsi_display_unset_clk_src(struct dsi_display *display)
{
	return 0;
}
#endif

int dsi_display_set_clk_src(struct dsi_display *display)
{
	int rc = 0;
	int i;
	struct dsi_display_ctrl *m_ctrl, *ctrl;

	/*
	 * For CPHY mode, the parent of mux_clks need to be set
	 * to Cphy_clks to have correct dividers for byte and
	 * pixel clocks.
	 */
	if (display->panel->host_config.phy_type == DSI_PHY_TYPE_CPHY) {
		rc = dsi_clk_update_parent(&display->clock_info.cphy_clks,
			      &display->clock_info.mux_clks);
		if (rc) {
			DSI_ERR("failed update mux parent to shadow\n");
			return rc;
		}
	}

	/*
	 * In case of split DSI usecases, the clock for master controller should
	 * be enabled before the other controller. Master controller in the
	 * clock context refers to the controller that sources the clock.
	 */
	m_ctrl = &display->ctrl[display->clk_master_idx];

	rc = dsi_ctrl_set_clock_source(m_ctrl->ctrl,
		   &display->clock_info.mux_clks);
	if (rc) {
		DSI_ERR("[%s] failed to set source clocks for master, rc=%d\n",
			   display->name, rc);
		return rc;
	}

	/* Turn on rest of the controllers */
	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		if (!ctrl->ctrl || (ctrl == m_ctrl))
			continue;

		rc = dsi_ctrl_set_clock_source(ctrl->ctrl,
			   &display->clock_info.mux_clks);
		if (rc) {
			DSI_ERR("[%s] failed to set source clocks, rc=%d\n",
				   display->name, rc);
			return rc;
		}
	}
	return 0;
}

static int dsi_display_phy_reset_config(struct dsi_display *display,
		bool enable)
{
	int rc = 0;
	int i;
	struct dsi_display_ctrl *ctrl;

	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		rc = dsi_ctrl_phy_reset_config(ctrl->ctrl, enable);
		if (rc) {
			DSI_ERR("[%s] failed to %s phy reset, rc=%d\n",
			       display->name, enable ? "mask" : "unmask", rc);
			return rc;
		}
	}
	return 0;
}

static void dsi_display_toggle_resync_fifo(struct dsi_display *display)
{
	struct dsi_display_ctrl *ctrl;
	int i;

	if (!display)
		return;

	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		dsi_phy_toggle_resync_fifo(ctrl->phy);
	}

	/*
	 * After retime buffer synchronization we need to turn of clk_en_sel
	 * bit on each phy. Avoid this for Cphy.
	 */

	if (display->panel->host_config.phy_type == DSI_PHY_TYPE_CPHY)
		return;

	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		dsi_phy_reset_clk_en_sel(ctrl->phy);
	}

}

static int dsi_display_ctrl_update(struct dsi_display *display)
{
	int rc = 0;
	int i;
	struct dsi_display_ctrl *ctrl;

	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		rc = dsi_ctrl_host_timing_update(ctrl->ctrl);
		if (rc) {
			DSI_ERR("[%s] failed to update host_%d, rc=%d\n",
				   display->name, i, rc);
			goto error_host_deinit;
		}
	}

	return 0;
error_host_deinit:
	for (i = i - 1; i >= 0; i--) {
		ctrl = &display->ctrl[i];
		(void)dsi_ctrl_host_deinit(ctrl->ctrl);
	}

	return rc;
}

static int dsi_display_ctrl_init(struct dsi_display *display)
{
	int rc = 0;
	int i;
	struct dsi_display_ctrl *ctrl;
	bool skip_op = is_skip_op_required(display);

	/* when ULPS suspend feature is enabled, we will keep the lanes in
	 * ULPS during suspend state and clamp DSI phy. Hence while resuming
	 * we will programe DSI controller as part of core clock enable.
	 * After that we should not re-configure DSI controller again here for
	 * usecases where we are resuming from ulps suspend as it might put
	 * the HW in bad state.
	 */
	if (!display->panel->ulps_suspend_enabled || !display->ulps_enabled) {
		display_for_each_ctrl(i, display) {
			ctrl = &display->ctrl[i];
			rc = dsi_ctrl_host_init(ctrl->ctrl, skip_op);
			if (rc) {
				DSI_ERR(
				"[%s] failed to init host_%d, skip_op=%d, rc=%d\n",
				       display->name, i, skip_op, rc);
				goto error_host_deinit;
			}
		}
	} else {
		display_for_each_ctrl(i, display) {
			ctrl = &display->ctrl[i];
			rc = dsi_ctrl_update_host_state(ctrl->ctrl,
							DSI_CTRL_OP_HOST_INIT,
							true);
			if (rc)
				DSI_DEBUG("host init update failed rc=%d\n",
						rc);
		}
	}

	return rc;
error_host_deinit:
	for (i = i - 1; i >= 0; i--) {
		ctrl = &display->ctrl[i];
		(void)dsi_ctrl_host_deinit(ctrl->ctrl);
	}
	return rc;
}

static int dsi_display_ctrl_deinit(struct dsi_display *display)
{
	int rc = 0;
	int i;
	struct dsi_display_ctrl *ctrl;

	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		rc = dsi_ctrl_host_deinit(ctrl->ctrl);
		if (rc) {
			DSI_ERR("[%s] failed to deinit host_%d, rc=%d\n",
			       display->name, i, rc);
		}
	}

	return rc;
}

static int dsi_display_ctrl_host_enable(struct dsi_display *display)
{
	int rc = 0;
	int i;
	struct dsi_display_ctrl *m_ctrl, *ctrl;
	bool skip_op = is_skip_op_required(display);

	m_ctrl = &display->ctrl[display->cmd_master_idx];

	rc = dsi_ctrl_set_host_engine_state(m_ctrl->ctrl,
			DSI_CTRL_ENGINE_ON, skip_op);
	if (rc) {
		DSI_ERR("[%s]enable host engine failed, skip_op:%d rc:%d\n",
		       display->name, skip_op, rc);
		goto error;
	}

	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		if (!ctrl->ctrl || (ctrl == m_ctrl))
			continue;

		rc = dsi_ctrl_set_host_engine_state(ctrl->ctrl,
						DSI_CTRL_ENGINE_ON, skip_op);
		if (rc) {
			DSI_ERR(
			"[%s] enable host engine failed, skip_op:%d rc:%d\n",
			       display->name, skip_op, rc);
			goto error_disable_master;
		}
	}

	return rc;
error_disable_master:
	(void)dsi_ctrl_set_host_engine_state(m_ctrl->ctrl,
					DSI_CTRL_ENGINE_OFF, skip_op);
error:
	return rc;
}

static int dsi_display_ctrl_host_disable(struct dsi_display *display)
{
	int rc = 0;
	int i;
	struct dsi_display_ctrl *m_ctrl, *ctrl;
	bool skip_op = is_skip_op_required(display);

	m_ctrl = &display->ctrl[display->cmd_master_idx];
	/*
	 * For platforms where ULPS is controlled by DSI controller block,
	 * do not disable dsi controller block if lanes are to be
	 * kept in ULPS during suspend. So just update the SW state
	 * and return early.
	 */
	if (display->panel->ulps_suspend_enabled &&
			!m_ctrl->phy->hw.ops.ulps_ops.ulps_request) {
		display_for_each_ctrl(i, display) {
			ctrl = &display->ctrl[i];
			rc = dsi_ctrl_update_host_state(ctrl->ctrl,
					DSI_CTRL_OP_HOST_ENGINE,
					false);
			if (rc)
				DSI_DEBUG("host state update failed %d\n", rc);
		}
		return rc;
	}

	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		if (!ctrl->ctrl || (ctrl == m_ctrl))
			continue;

		rc = dsi_ctrl_set_host_engine_state(ctrl->ctrl,
					DSI_CTRL_ENGINE_OFF, skip_op);
		if (rc)
			DSI_ERR(
			"[%s] disable host engine failed, skip_op:%d rc:%d\n",
			       display->name, skip_op, rc);
	}

	rc = dsi_ctrl_set_host_engine_state(m_ctrl->ctrl,
				DSI_CTRL_ENGINE_OFF, skip_op);
	if (rc) {
		DSI_ERR("[%s] disable mhost engine failed, skip_op:%d rc:%d\n",
		       display->name, skip_op, rc);
		goto error;
	}

error:
	return rc;
}

static int dsi_display_vid_engine_enable(struct dsi_display *display)
{
	int rc = 0;
	int i;
	struct dsi_display_ctrl *m_ctrl, *ctrl;
	bool skip_op = is_skip_op_required(display);

	m_ctrl = &display->ctrl[display->video_master_idx];

	rc = dsi_ctrl_set_vid_engine_state(m_ctrl->ctrl,
			DSI_CTRL_ENGINE_ON, skip_op);
	if (rc) {
		DSI_ERR("[%s] enable mvid engine failed, skip_op:%d rc:%d\n",
				display->name, skip_op, rc);
		goto error;
	}

	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		if (!ctrl->ctrl || (ctrl == m_ctrl))
			continue;

		rc = dsi_ctrl_set_vid_engine_state(ctrl->ctrl,
					DSI_CTRL_ENGINE_ON, skip_op);
		if (rc) {
			DSI_ERR(
			    "[%s] enable vid engine failed, skip_op:%d rc:%d\n",
				display->name, skip_op, rc);
			goto error_disable_master;
		}
	}

	return rc;
error_disable_master:
	(void)dsi_ctrl_set_vid_engine_state(m_ctrl->ctrl,
				DSI_CTRL_ENGINE_OFF, skip_op);
error:
	return rc;
}

static int dsi_display_vid_engine_disable(struct dsi_display *display)
{
	int rc = 0;
	int i;
	struct dsi_display_ctrl *m_ctrl, *ctrl;
	bool skip_op = is_skip_op_required(display);

	m_ctrl = &display->ctrl[display->video_master_idx];

	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		if (!ctrl->ctrl || (ctrl == m_ctrl))
			continue;

		rc = dsi_ctrl_set_vid_engine_state(ctrl->ctrl,
					DSI_CTRL_ENGINE_OFF, skip_op);
		if (rc)
			DSI_ERR(
			   "[%s] disable vid engine failed, skip_op:%d rc:%d\n",
				display->name, skip_op, rc);
	}

	rc = dsi_ctrl_set_vid_engine_state(m_ctrl->ctrl,
				DSI_CTRL_ENGINE_OFF, skip_op);
	if (rc)
		DSI_ERR("[%s] disable mvid engine failed, skip_op:%d rc:%d\n",
				display->name, skip_op, rc);

	return rc;
}

static int dsi_display_phy_enable(struct dsi_display *display)
{
	int rc = 0;
	int i;
	struct dsi_display_ctrl *m_ctrl, *ctrl;
	enum dsi_phy_pll_source m_src = DSI_PLL_SOURCE_STANDALONE;
	bool skip_op = is_skip_op_required(display);

	m_ctrl = &display->ctrl[display->clk_master_idx];
	if (display->ctrl_count > 1)
		m_src = DSI_PLL_SOURCE_NATIVE;

	rc = dsi_phy_enable(m_ctrl->phy, &display->config,
			m_src, true, skip_op);
	if (rc) {
		DSI_ERR("[%s] failed to enable DSI PHY, skip_op=%d rc=%d\n",
		       display->name, skip_op, rc);
		goto error;
	}

	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		if (!ctrl->ctrl || (ctrl == m_ctrl))
			continue;

		rc = dsi_phy_enable(ctrl->phy, &display->config,
				DSI_PLL_SOURCE_NON_NATIVE, true, skip_op);
		if (rc) {
			DSI_ERR(
				"[%s] failed to enable DSI PHY, skip_op: %d rc=%d\n",
				display->name, skip_op, rc);
			goto error_disable_master;
		}
	}

	return rc;

error_disable_master:
	(void)dsi_phy_disable(m_ctrl->phy, skip_op);
error:
	return rc;
}

static int dsi_display_phy_disable(struct dsi_display *display)
{
	int rc = 0;
	int i;
	struct dsi_display_ctrl *m_ctrl, *ctrl;
	bool skip_op = is_skip_op_required(display);

	m_ctrl = &display->ctrl[display->clk_master_idx];

	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		if (!ctrl->ctrl || (ctrl == m_ctrl))
			continue;

		rc = dsi_phy_disable(ctrl->phy, skip_op);
		if (rc)
			DSI_ERR(
				"[%s] failed to disable DSI PHY, skip_op=%d rc=%d\n",
				display->name, skip_op,  rc);
	}

	rc = dsi_phy_disable(m_ctrl->phy, skip_op);
	if (rc)
		DSI_ERR("[%s] failed to disable DSI PHY, skip_op=%d rc=%d\n",
		       display->name, skip_op, rc);

	return rc;
}

static int dsi_display_wake_up(struct dsi_display *display)
{
	return 0;
}

static void dsi_display_mask_overflow(struct dsi_display *display, u32 flags,
						bool enable)
{
	struct dsi_display_ctrl *ctrl;
	int i;

	if (!(flags & DSI_CTRL_CMD_LAST_COMMAND))
		return;

	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		if (!ctrl)
			continue;
		dsi_ctrl_mask_overflow(ctrl->ctrl, enable);
	}
}

static int dsi_display_broadcast_cmd(struct dsi_display *display,
				     const struct mipi_dsi_msg *msg)
{
	int rc = 0;
	u32 flags, m_flags;
	struct dsi_display_ctrl *ctrl, *m_ctrl;
	int i;

	m_flags = (DSI_CTRL_CMD_BROADCAST | DSI_CTRL_CMD_BROADCAST_MASTER |
		   DSI_CTRL_CMD_DEFER_TRIGGER | DSI_CTRL_CMD_FETCH_MEMORY);
	flags = (DSI_CTRL_CMD_BROADCAST | DSI_CTRL_CMD_DEFER_TRIGGER |
		 DSI_CTRL_CMD_FETCH_MEMORY);

	if ((msg->flags & MIPI_DSI_MSG_LASTCOMMAND)) {
		flags |= DSI_CTRL_CMD_LAST_COMMAND;
		m_flags |= DSI_CTRL_CMD_LAST_COMMAND;
	}

	/*
	 * During broadcast command dma scheduling is always recommended.
	 * As long as the display is enabled and TE is running the
	 * DSI_CTRL_CMD_CUSTOM_DMA_SCHED flag should be set.
	 */
	if (display->enabled) {
		flags |= DSI_CTRL_CMD_CUSTOM_DMA_SCHED;
		m_flags |= DSI_CTRL_CMD_CUSTOM_DMA_SCHED;
	}

	if (display->queue_cmd_waits ||
			msg->flags & MIPI_DSI_MSG_ASYNC_OVERRIDE) {
		flags |= DSI_CTRL_CMD_ASYNC_WAIT;
		m_flags |= DSI_CTRL_CMD_ASYNC_WAIT;
	}

	/*
	 * 1. Setup commands in FIFO
	 * 2. Trigger commands
	 */
	m_ctrl = &display->ctrl[display->cmd_master_idx];
	dsi_display_mask_overflow(display, m_flags, true);
	rc = dsi_ctrl_cmd_transfer(m_ctrl->ctrl, msg, &m_flags);
	if (rc) {
		DSI_ERR("[%s] cmd transfer failed on master,rc=%d\n",
		       display->name, rc);
		goto error;
	}

	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		if (ctrl == m_ctrl)
			continue;

		rc = dsi_ctrl_cmd_transfer(ctrl->ctrl, msg, &flags);
		if (rc) {
			DSI_ERR("[%s] cmd transfer failed, rc=%d\n",
			       display->name, rc);
			goto error;
		}

		rc = dsi_ctrl_cmd_tx_trigger(ctrl->ctrl, flags);
		if (rc) {
			DSI_ERR("[%s] cmd trigger failed, rc=%d\n",
			       display->name, rc);
			goto error;
		}
	}

	rc = dsi_ctrl_cmd_tx_trigger(m_ctrl->ctrl, m_flags);
	if (rc) {
		DSI_ERR("[%s] cmd trigger failed for master, rc=%d\n",
		       display->name, rc);
		goto error;
	}

error:
	dsi_display_mask_overflow(display, m_flags, false);
	return rc;
}

static int dsi_display_phy_sw_reset(struct dsi_display *display)
{
	int rc = 0;
	int i;
	struct dsi_display_ctrl *m_ctrl, *ctrl;

	/*
	 * For continuous splash and trusted vm environment,
	 * ctrl states are updated separately and hence we do
	 * an early return
	 */
	if (is_skip_op_required(display)) {
		DSI_DEBUG(
			"cont splash/trusted vm use case, phy sw reset not required\n");
		return 0;
	}

	m_ctrl = &display->ctrl[display->cmd_master_idx];

	rc = dsi_ctrl_phy_sw_reset(m_ctrl->ctrl);
	if (rc) {
		DSI_ERR("[%s] failed to reset phy, rc=%d\n", display->name, rc);
		goto error;
	}

	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		if (!ctrl->ctrl || (ctrl == m_ctrl))
			continue;

		rc = dsi_ctrl_phy_sw_reset(ctrl->ctrl);
		if (rc) {
			DSI_ERR("[%s] failed to reset phy, rc=%d\n",
			       display->name, rc);
			goto error;
		}
	}

error:
	return rc;
}

static int dsi_host_attach(struct mipi_dsi_host *host,
			   struct mipi_dsi_device *dsi)
{
	return 0;
}

static int dsi_host_detach(struct mipi_dsi_host *host,
			   struct mipi_dsi_device *dsi)
{
	return 0;
}

static ssize_t dsi_host_transfer(struct mipi_dsi_host *host,
				 const struct mipi_dsi_msg *msg)
{
	struct dsi_display *display;
	int rc = 0, ret = 0;

	if (!host || !msg) {
		DSI_ERR("Invalid params\n");
		return 0;
	}

	display = to_dsi_display(host);

	/* Avoid sending DCS commands when ESD recovery is pending */
	if (atomic_read(&display->panel->esd_recovery_pending)) {
		DSI_DEBUG("ESD recovery pending\n");
		return 0;
	}

	rc = dsi_display_clk_ctrl(display->dsi_clk_handle,
			DSI_ALL_CLKS, DSI_CLK_ON);
	if (rc) {
		DSI_ERR("[%s] failed to enable all DSI clocks, rc=%d\n",
		       display->name, rc);
		goto error;
	}

	rc = dsi_display_wake_up(display);
	if (rc) {
		DSI_ERR("[%s] failed to wake up display, rc=%d\n",
		       display->name, rc);
		goto error_disable_clks;
	}

	rc = dsi_display_cmd_engine_enable(display);
	if (rc) {
		DSI_ERR("[%s] failed to enable cmd engine, rc=%d\n",
		       display->name, rc);
		goto error_disable_clks;
	}

	if (display->tx_cmd_buf == NULL) {
		rc = dsi_host_alloc_cmd_tx_buffer(display);
		if (rc) {
			DSI_ERR("failed to allocate cmd tx buffer memory\n");
			goto error_disable_cmd_engine;
		}
	}

	if (display->ctrl_count > 1 && !(msg->flags & MIPI_DSI_MSG_UNICAST)) {
		rc = dsi_display_broadcast_cmd(display, msg);
		if (rc) {
			DSI_ERR("[%s] cmd broadcast failed, rc=%d\n",
			       display->name, rc);
			goto error_disable_cmd_engine;
		}
	} else {
		int ctrl_idx = (msg->flags & MIPI_DSI_MSG_UNICAST) ?
				msg->ctrl : 0;
		u32 cmd_flags = DSI_CTRL_CMD_FETCH_MEMORY;

		if (display->queue_cmd_waits ||
				msg->flags & MIPI_DSI_MSG_ASYNC_OVERRIDE)
			cmd_flags |= DSI_CTRL_CMD_ASYNC_WAIT;

		if ((msg->flags & MIPI_DSI_MSG_CMD_DMA_SCHED) &&
				(display->enabled))
			cmd_flags |= DSI_CTRL_CMD_CUSTOM_DMA_SCHED;

		if (msg->rx_buf)
			cmd_flags |= DSI_CTRL_CMD_READ;
		rc = dsi_ctrl_cmd_transfer(display->ctrl[ctrl_idx].ctrl, msg,
				&cmd_flags);
		if (((cmd_flags & DSI_CTRL_CMD_READ) && rc <= 0) ||
				(!(cmd_flags & DSI_CTRL_CMD_READ) && rc < 0 )) {

			DSI_ERR("[%s] cmd transfer failed, rc=%d\n",
			       display->name, rc);
			goto error_disable_cmd_engine;
		}
	}

error_disable_cmd_engine:
	ret = dsi_display_cmd_engine_disable(display);
	if (ret) {
		DSI_ERR("[%s]failed to disable DSI cmd engine, rc=%d\n",
				display->name, ret);
	}
error_disable_clks:
	ret = dsi_display_clk_ctrl(display->dsi_clk_handle,
			DSI_ALL_CLKS, DSI_CLK_OFF);
	if (ret) {
		DSI_ERR("[%s] failed to disable all DSI clocks, rc=%d\n",
		       display->name, ret);
	}
error:
	return rc;
}


static struct mipi_dsi_host_ops dsi_host_ops = {
	.attach = dsi_host_attach,
	.detach = dsi_host_detach,
	.transfer = dsi_host_transfer,
};

static int dsi_display_mipi_host_init(struct dsi_display *display)
{
	int rc = 0;
	struct mipi_dsi_host *host = &display->host;

	host->dev = &display->pdev->dev;
	host->ops = &dsi_host_ops;

	rc = mipi_dsi_host_register(host);
	if (rc) {
		DSI_ERR("[%s] failed to register mipi dsi host, rc=%d\n",
		       display->name, rc);
		goto error;
	}

error:
	return rc;
}
static int dsi_display_mipi_host_deinit(struct dsi_display *display)
{
	int rc = 0;
	struct mipi_dsi_host *host = &display->host;

	mipi_dsi_host_unregister(host);

	host->dev = NULL;
	host->ops = NULL;

	return rc;
}

static int dsi_display_clocks_deinit(struct dsi_display *display)
{
	int rc = 0;
	struct dsi_clk_link_set *src = &display->clock_info.src_clks;
	struct dsi_clk_link_set *mux = &display->clock_info.mux_clks;
	struct dsi_clk_link_set *shadow = &display->clock_info.shadow_clks;

	if (src->byte_clk) {
		devm_clk_put(&display->pdev->dev, src->byte_clk);
		src->byte_clk = NULL;
	}

	if (src->pixel_clk) {
		devm_clk_put(&display->pdev->dev, src->pixel_clk);
		src->pixel_clk = NULL;
	}

	if (mux->byte_clk) {
		devm_clk_put(&display->pdev->dev, mux->byte_clk);
		mux->byte_clk = NULL;
	}

	if (mux->pixel_clk) {
		devm_clk_put(&display->pdev->dev, mux->pixel_clk);
		mux->pixel_clk = NULL;
	}

	if (shadow->byte_clk) {
		devm_clk_put(&display->pdev->dev, shadow->byte_clk);
		shadow->byte_clk = NULL;
	}

	if (shadow->pixel_clk) {
		devm_clk_put(&display->pdev->dev, shadow->pixel_clk);
		shadow->pixel_clk = NULL;
	}

	return rc;
}

static bool dsi_display_check_prefix(const char *clk_prefix,
					const char *clk_name)
{
	return !!strnstr(clk_name, clk_prefix, strlen(clk_name));
}

static int dsi_display_get_clocks_count(struct dsi_display *display,
						char *dsi_clk_name)
{
	if (display->fw)
		return dsi_parser_count_strings(display->parser_node,
			dsi_clk_name);
	else
		return of_property_count_strings(display->panel_node,
			dsi_clk_name);
}

static void dsi_display_get_clock_name(struct dsi_display *display,
					char *dsi_clk_name, int index,
					const char **clk_name)
{
	if (display->fw)
		dsi_parser_read_string_index(display->parser_node,
			dsi_clk_name, index, clk_name);
	else
		of_property_read_string_index(display->panel_node,
			dsi_clk_name, index, clk_name);
}

static int dsi_display_clocks_init(struct dsi_display *display)
{
	int i, rc = 0, num_clk = 0;
	const char *clk_name;
	const char *xo_byte = "xo_byte", *xo_pixel = "xo_pixel";
	const char *src_byte = "src_byte", *src_pixel = "src_pixel";
	const char *mux_byte = "mux_byte", *mux_pixel = "mux_pixel";
	const char *cphy_byte = "cphy_byte", *cphy_pixel = "cphy_pixel";
	const char *shadow_byte = "shadow_byte", *shadow_pixel = "shadow_pixel";
	const char *shadow_cphybyte = "shadow_cphybyte",
		   *shadow_cphypixel = "shadow_cphypixel";
	struct clk *dsi_clk;
	struct dsi_clk_link_set *xo = &display->clock_info.xo_clks;
	struct dsi_clk_link_set *src = &display->clock_info.src_clks;
	struct dsi_clk_link_set *mux = &display->clock_info.mux_clks;
	struct dsi_clk_link_set *cphy = &display->clock_info.cphy_clks;
	struct dsi_clk_link_set *shadow = &display->clock_info.shadow_clks;
	struct dsi_clk_link_set *shadow_cphy =
				&display->clock_info.shadow_cphy_clks;
	struct dsi_dyn_clk_caps *dyn_clk_caps = &(display->panel->dyn_clk_caps);
	char *dsi_clock_name;

	if (!strcmp(display->display_type, "primary"))
		dsi_clock_name = "qcom,dsi-select-clocks";
	else
		dsi_clock_name = "qcom,dsi-select-sec-clocks";

	num_clk = dsi_display_get_clocks_count(display, dsi_clock_name);

	if (num_clk <= 0) {
		rc = num_clk;
		DSI_WARN("failed to read %s, rc = %d\n", dsi_clock_name, num_clk);
		goto error;
	}

	DSI_DEBUG("clk count=%d\n", num_clk);

	for (i = 0; i < num_clk; i++) {
		dsi_display_get_clock_name(display, dsi_clock_name, i,
						&clk_name);

		DSI_DEBUG("clock name:%s\n", clk_name);

		dsi_clk = devm_clk_get(&display->pdev->dev, clk_name);
		if (IS_ERR_OR_NULL(dsi_clk)) {
			rc = PTR_ERR(dsi_clk);

			DSI_ERR("failed to get %s, rc=%d\n", clk_name, rc);

			if (dsi_display_check_prefix(xo_byte, clk_name)) {
				xo->byte_clk = NULL;
				goto error;
			}
			if (dsi_display_check_prefix(xo_pixel, clk_name)) {
				xo->pixel_clk = NULL;
				goto error;
			}

			if (dsi_display_check_prefix(mux_byte, clk_name)) {
				mux->byte_clk = NULL;
				goto error;
			}
			if (dsi_display_check_prefix(mux_pixel, clk_name)) {
				mux->pixel_clk = NULL;
				goto error;
			}

			if (dsi_display_check_prefix(cphy_byte, clk_name)) {
				cphy->byte_clk = NULL;
				goto error;
			}
			if (dsi_display_check_prefix(cphy_pixel, clk_name)) {
				cphy->pixel_clk = NULL;
				goto error;
			}

			if (dyn_clk_caps->dyn_clk_support &&
				(display->panel->panel_mode ==
					 DSI_OP_VIDEO_MODE)) {

				if (dsi_display_check_prefix(src_byte,
							clk_name))
					src->byte_clk = NULL;
				if (dsi_display_check_prefix(src_pixel,
							clk_name))
					src->pixel_clk = NULL;
				if (dsi_display_check_prefix(shadow_byte,
							clk_name))
					shadow->byte_clk = NULL;
				if (dsi_display_check_prefix(shadow_pixel,
							clk_name))
					shadow->pixel_clk = NULL;
				if (dsi_display_check_prefix(shadow_cphybyte,
							clk_name))
					shadow_cphy->byte_clk = NULL;
				if (dsi_display_check_prefix(shadow_cphypixel,
							clk_name))
					shadow_cphy->pixel_clk = NULL;

				dyn_clk_caps->dyn_clk_support = false;
			}
		}

		if (dsi_display_check_prefix(xo_byte, clk_name)) {
			xo->byte_clk = dsi_clk;
			continue;
		}

		if (dsi_display_check_prefix(xo_pixel, clk_name)) {
			xo->pixel_clk = dsi_clk;
			continue;
		}

		if (dsi_display_check_prefix(src_byte, clk_name)) {
			src->byte_clk = dsi_clk;
			continue;
		}

		if (dsi_display_check_prefix(src_pixel, clk_name)) {
			src->pixel_clk = dsi_clk;
			continue;
		}

		if (dsi_display_check_prefix(cphy_byte, clk_name)) {
			cphy->byte_clk = dsi_clk;
			continue;
		}

		if (dsi_display_check_prefix(cphy_pixel, clk_name)) {
			cphy->pixel_clk = dsi_clk;
			continue;
		}

		if (dsi_display_check_prefix(mux_byte, clk_name)) {
			mux->byte_clk = dsi_clk;
			continue;
		}

		if (dsi_display_check_prefix(mux_pixel, clk_name)) {
			mux->pixel_clk = dsi_clk;
			continue;
		}

		if (dsi_display_check_prefix(shadow_byte, clk_name)) {
			shadow->byte_clk = dsi_clk;
			continue;
		}

		if (dsi_display_check_prefix(shadow_pixel, clk_name)) {
			shadow->pixel_clk = dsi_clk;
			continue;
		}

		if (dsi_display_check_prefix(shadow_cphybyte, clk_name)) {
			shadow_cphy->byte_clk = dsi_clk;
			continue;
		}

		if (dsi_display_check_prefix(shadow_cphypixel, clk_name)) {
			shadow_cphy->pixel_clk = dsi_clk;
			continue;
		}
	}

	return 0;
error:
	(void)dsi_display_clocks_deinit(display);
	return rc;
}

static int dsi_display_clk_ctrl_cb(void *priv,
	struct dsi_clk_ctrl_info clk_state_info)
{
	int rc = 0;
	struct dsi_display *display = NULL;
	void *clk_handle = NULL;

	if (!priv) {
		DSI_ERR("Invalid params\n");
		return -EINVAL;
	}

	display = priv;

	if (clk_state_info.client == DSI_CLK_REQ_MDP_CLIENT) {
		clk_handle = display->mdp_clk_handle;
	} else if (clk_state_info.client == DSI_CLK_REQ_DSI_CLIENT) {
		clk_handle = display->dsi_clk_handle;
	} else {
		DSI_ERR("invalid clk handle, return error\n");
		return -EINVAL;
	}

	/*
	 * TODO: Wait for CMD_MDP_DONE interrupt if MDP client tries
	 * to turn off DSI clocks.
	 */
	rc = dsi_display_clk_ctrl(clk_handle,
		clk_state_info.clk_type, clk_state_info.clk_state);
	if (rc) {
		DSI_ERR("[%s] failed to %d DSI %d clocks, rc=%d\n",
		       display->name, clk_state_info.clk_state,
		       clk_state_info.clk_type, rc);
		return rc;
	}
	return 0;
}

static void dsi_display_ctrl_isr_configure(struct dsi_display *display, bool en)
{
	int i;
	struct dsi_display_ctrl *ctrl;

	if (!display)
		return;

	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		if (!ctrl)
			continue;
		dsi_ctrl_isr_configure(ctrl->ctrl, en);
	}
}

int dsi_pre_clkoff_cb(void *priv,
			   enum dsi_clk_type clk,
			   enum dsi_lclk_type l_type,
			   enum dsi_clk_state new_state)
{
	int rc = 0, i;
	struct dsi_display *display = priv;
	struct dsi_display_ctrl *ctrl;


	/*
	 * If Idle Power Collapse occurs immediately after a CMD
	 * transfer with an asynchronous wait for DMA done, ensure
	 * that the work queued is scheduled and completed before turning
	 * off the clocks and disabling interrupts to validate the command
	 * transfer.
	 */
	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		if (!ctrl->ctrl || !ctrl->ctrl->dma_wait_queued)
			continue;
		flush_workqueue(display->dma_cmd_workq);
		cancel_work_sync(&ctrl->ctrl->dma_cmd_wait);
		ctrl->ctrl->dma_wait_queued = false;
	}
	if ((clk & DSI_LINK_CLK) && (new_state == DSI_CLK_OFF) &&
		(l_type & DSI_LINK_LP_CLK)) {
		/*
		 * If continuous clock is enabled then disable it
		 * before entering into ULPS Mode.
		 */
		if (display->panel->host_config.force_hs_clk_lane)
			_dsi_display_continuous_clk_ctrl(display, false);
		/*
		 * If ULPS feature is enabled, enter ULPS first.
		 * However, when blanking the panel, we should enter ULPS
		 * only if ULPS during suspend feature is enabled.
		 */
		if (!dsi_panel_initialized(display->panel)) {
			if (display->panel->ulps_suspend_enabled)
				rc = dsi_display_set_ulps(display, true);
		} else if (dsi_panel_ulps_feature_enabled(display->panel)) {
			rc = dsi_display_set_ulps(display, true);
		}
		if (rc)
			DSI_ERR("%s: failed enable ulps, rc = %d\n",
			       __func__, rc);
	}

	if ((clk & DSI_LINK_CLK) && (new_state == DSI_CLK_OFF) &&
		(l_type & DSI_LINK_HS_CLK)) {
		/*
		 * PHY clock gating should be disabled before the PLL and the
		 * branch clocks are turned off. Otherwise, it is possible that
		 * the clock RCGs may not be turned off correctly resulting
		 * in clock warnings.
		 */
		rc = dsi_display_config_clk_gating(display, false);
		if (rc)
			DSI_ERR("[%s] failed to disable clk gating, rc=%d\n",
					display->name, rc);
	}

	if ((clk & DSI_CORE_CLK) && (new_state == DSI_CLK_OFF)) {
		/*
		 * Enable DSI clamps only if entering idle power collapse or
		 * when ULPS during suspend is enabled..
		 */
		if (dsi_panel_initialized(display->panel) ||
			display->panel->ulps_suspend_enabled) {
			dsi_display_phy_idle_off(display);
			rc = dsi_display_set_clamp(display, true);
			if (rc)
				DSI_ERR("%s: Failed to enable dsi clamps. rc=%d\n",
					__func__, rc);

			rc = dsi_display_phy_reset_config(display, false);
			if (rc)
				DSI_ERR("%s: Failed to reset phy, rc=%d\n",
						__func__, rc);
		} else {
			/* Make sure that controller is not in ULPS state when
			 * the DSI link is not active.
			 */
			rc = dsi_display_set_ulps(display, false);
			if (rc)
				DSI_ERR("%s: failed to disable ulps. rc=%d\n",
					__func__, rc);
		}
		/* dsi will not be able to serve irqs from here on */
		dsi_display_ctrl_irq_update(display, false);

		/* cache the MISR values */
		display_for_each_ctrl(i, display) {
			ctrl = &display->ctrl[i];
			if (!ctrl->ctrl)
				continue;
			dsi_ctrl_cache_misr(ctrl->ctrl);
		}

	}

	return rc;
}

int dsi_post_clkon_cb(void *priv,
			   enum dsi_clk_type clk,
			   enum dsi_lclk_type l_type,
			   enum dsi_clk_state curr_state)
{
	int rc = 0;
	struct dsi_display *display = priv;
	bool mmss_clamp = false;

	if ((clk & DSI_LINK_CLK) && (l_type & DSI_LINK_LP_CLK)) {
		mmss_clamp = display->clamp_enabled;
		/*
		 * controller setup is needed if coming out of idle
		 * power collapse with clamps enabled.
		 */
		if (mmss_clamp)
			dsi_display_ctrl_setup(display);

		/*
		 * Phy setup is needed if coming out of idle
		 * power collapse with clamps enabled.
		 */
		if (display->phy_idle_power_off || mmss_clamp)
			dsi_display_phy_idle_on(display, mmss_clamp);

		if (display->ulps_enabled && mmss_clamp) {
			/*
			 * ULPS Entry Request. This is needed if the lanes were
			 * in ULPS prior to power collapse, since after
			 * power collapse and reset, the DSI controller resets
			 * back to idle state and not ULPS. This ulps entry
			 * request will transition the state of the DSI
			 * controller to ULPS which will match the state of the
			 * DSI phy. This needs to be done prior to disabling
			 * the DSI clamps.
			 *
			 * Also, reset the ulps flag so that ulps_config
			 * function would reconfigure the controller state to
			 * ULPS.
			 */
			display->ulps_enabled = false;
			rc = dsi_display_set_ulps(display, true);
			if (rc) {
				DSI_ERR("%s: Failed to enter ULPS. rc=%d\n",
					__func__, rc);
				goto error;
			}
		}

		rc = dsi_display_phy_reset_config(display, true);
		if (rc) {
			DSI_ERR("%s: Failed to reset phy, rc=%d\n",
						__func__, rc);
			goto error;
		}

		rc = dsi_display_set_clamp(display, false);
		if (rc) {
			DSI_ERR("%s: Failed to disable dsi clamps. rc=%d\n",
				__func__, rc);
			goto error;
		}
	}

	if ((clk & DSI_LINK_CLK) && (l_type & DSI_LINK_HS_CLK)) {
		/*
		 * Toggle the resync FIFO everytime clock changes, except
		 * when cont-splash screen transition is going on.
		 * Toggling resync FIFO during cont splash transition
		 * can lead to blinks on the display.
		 */
		if (!display->is_cont_splash_enabled)
			dsi_display_toggle_resync_fifo(display);

		if (display->ulps_enabled) {
			rc = dsi_display_set_ulps(display, false);
			if (rc) {
				DSI_ERR("%s: failed to disable ulps, rc= %d\n",
				       __func__, rc);
				goto error;
			}
		}

		if (display->panel->host_config.force_hs_clk_lane)
			_dsi_display_continuous_clk_ctrl(display, true);

		rc = dsi_display_config_clk_gating(display, true);
		if (rc) {
			DSI_ERR("[%s] failed to enable clk gating %d\n",
					display->name, rc);
			goto error;
		}
	}

	/* enable dsi to serve irqs */
	if (clk & DSI_CORE_CLK)
		dsi_display_ctrl_irq_update(display, true);

error:
	return rc;
}

int dsi_post_clkoff_cb(void *priv,
			    enum dsi_clk_type clk_type,
			    enum dsi_lclk_type l_type,
			    enum dsi_clk_state curr_state)
{
	int rc = 0;
	struct dsi_display *display = priv;

	if (!display) {
		DSI_ERR("%s: Invalid arg\n", __func__);
		return -EINVAL;
	}

	if ((clk_type & DSI_CORE_CLK) &&
	    (curr_state == DSI_CLK_OFF)) {
		rc = dsi_display_phy_power_off(display);
		if (rc)
			DSI_ERR("[%s] failed to power off PHY, rc=%d\n",
				   display->name, rc);

		rc = dsi_display_ctrl_power_off(display);
		if (rc)
			DSI_ERR("[%s] failed to power DSI vregs, rc=%d\n",
				   display->name, rc);
	}
	return rc;
}

int dsi_pre_clkon_cb(void *priv,
			  enum dsi_clk_type clk_type,
			  enum dsi_lclk_type l_type,
			  enum dsi_clk_state new_state)
{
	int rc = 0;
	struct dsi_display *display = priv;

	if (!display) {
		DSI_ERR("%s: invalid input\n", __func__);
		return -EINVAL;
	}

	if ((clk_type & DSI_CORE_CLK) && (new_state == DSI_CLK_ON)) {
		/*
		 * Enable DSI core power
		 * 1.> PANEL_PM are controlled as part of
		 *     panel_power_ctrl. Needed not be handled here.
		 * 2.> CTRL_PM need to be enabled/disabled
		 *     only during unblank/blank. Their state should
		 *     not be changed during static screen.
		 */

		DSI_DEBUG("updating power states for ctrl and phy\n");
		rc = dsi_display_ctrl_power_on(display);
		if (rc) {
			DSI_ERR("[%s] failed to power on dsi controllers, rc=%d\n",
				   display->name, rc);
			return rc;
		}

		rc = dsi_display_phy_power_on(display);
		if (rc) {
			DSI_ERR("[%s] failed to power on dsi phy, rc = %d\n",
				   display->name, rc);
			return rc;
		}

		DSI_DEBUG("%s: Enable DSI core power\n", __func__);
	}

	return rc;
}

int dsi_display_set_ulp_load(struct dsi_display *display, bool enable)
{
	int i, rc = 0;
	struct dsi_display_ctrl *display_ctrl;
	struct dsi_ctrl *ctrl;
	struct dsi_panel *panel;

	display_for_each_ctrl(i, display) {
		display_ctrl = &display->ctrl[i];
		if (!display_ctrl->ctrl)
			continue;
		ctrl = display_ctrl->ctrl;

		rc = dsi_pwr_config_vreg_opt_mode(&ctrl->pwr_info.host_pwr, enable);
		if (rc) {
			DSI_ERR("failed to set ctrl load\n");
			return rc;
		}
	}

	panel = display->panel;
	rc = dsi_pwr_config_vreg_opt_mode(&panel->power_info, enable);
	if (rc) {
		DSI_ERR("failed to set panel load\n");
		return rc;
	}
	return rc;
}

static void __set_lane_map_v2(u8 *lane_map_v2,
	enum dsi_phy_data_lanes lane0,
	enum dsi_phy_data_lanes lane1,
	enum dsi_phy_data_lanes lane2,
	enum dsi_phy_data_lanes lane3)
{
	lane_map_v2[DSI_LOGICAL_LANE_0] = lane0;
	lane_map_v2[DSI_LOGICAL_LANE_1] = lane1;
	lane_map_v2[DSI_LOGICAL_LANE_2] = lane2;
	lane_map_v2[DSI_LOGICAL_LANE_3] = lane3;
}

static int dsi_display_parse_lane_map(struct dsi_display *display)
{
	int rc = 0, i = 0;
	const char *data;
	u8 temp[DSI_LANE_MAX - 1];

	if (!display) {
		DSI_ERR("invalid params\n");
		return -EINVAL;
	}

	/* lane-map-v2 supersedes lane-map-v1 setting */
	rc = of_property_read_u8_array(display->pdev->dev.of_node,
		"qcom,lane-map-v2", temp, (DSI_LANE_MAX - 1));
	if (!rc) {
		for (i = DSI_LOGICAL_LANE_0; i < (DSI_LANE_MAX - 1); i++)
			display->lane_map.lane_map_v2[i] = BIT(temp[i]);
		return 0;
	} else if (rc != EINVAL) {
		DSI_DEBUG("Incorrect mapping, configure default\n");
		goto set_default;
	}

	/* lane-map older version, for DSI controller version < 2.0 */
	data = of_get_property(display->pdev->dev.of_node,
		"qcom,lane-map", NULL);
	if (!data)
		goto set_default;

	if (!strcmp(data, "lane_map_3012")) {
		display->lane_map.lane_map_v1 = DSI_LANE_MAP_3012;
		__set_lane_map_v2(display->lane_map.lane_map_v2,
			DSI_PHYSICAL_LANE_1,
			DSI_PHYSICAL_LANE_2,
			DSI_PHYSICAL_LANE_3,
			DSI_PHYSICAL_LANE_0);
	} else if (!strcmp(data, "lane_map_2301")) {
		display->lane_map.lane_map_v1 = DSI_LANE_MAP_2301;
		__set_lane_map_v2(display->lane_map.lane_map_v2,
			DSI_PHYSICAL_LANE_2,
			DSI_PHYSICAL_LANE_3,
			DSI_PHYSICAL_LANE_0,
			DSI_PHYSICAL_LANE_1);
	} else if (!strcmp(data, "lane_map_1230")) {
		display->lane_map.lane_map_v1 = DSI_LANE_MAP_1230;
		__set_lane_map_v2(display->lane_map.lane_map_v2,
			DSI_PHYSICAL_LANE_3,
			DSI_PHYSICAL_LANE_0,
			DSI_PHYSICAL_LANE_1,
			DSI_PHYSICAL_LANE_2);
	} else if (!strcmp(data, "lane_map_0321")) {
		display->lane_map.lane_map_v1 = DSI_LANE_MAP_0321;
		__set_lane_map_v2(display->lane_map.lane_map_v2,
			DSI_PHYSICAL_LANE_0,
			DSI_PHYSICAL_LANE_3,
			DSI_PHYSICAL_LANE_2,
			DSI_PHYSICAL_LANE_1);
	} else if (!strcmp(data, "lane_map_1032")) {
		display->lane_map.lane_map_v1 = DSI_LANE_MAP_1032;
		__set_lane_map_v2(display->lane_map.lane_map_v2,
			DSI_PHYSICAL_LANE_1,
			DSI_PHYSICAL_LANE_0,
			DSI_PHYSICAL_LANE_3,
			DSI_PHYSICAL_LANE_2);
	} else if (!strcmp(data, "lane_map_2103")) {
		display->lane_map.lane_map_v1 = DSI_LANE_MAP_2103;
		__set_lane_map_v2(display->lane_map.lane_map_v2,
			DSI_PHYSICAL_LANE_2,
			DSI_PHYSICAL_LANE_1,
			DSI_PHYSICAL_LANE_0,
			DSI_PHYSICAL_LANE_3);
	} else if (!strcmp(data, "lane_map_3210")) {
		display->lane_map.lane_map_v1 = DSI_LANE_MAP_3210;
		__set_lane_map_v2(display->lane_map.lane_map_v2,
			DSI_PHYSICAL_LANE_3,
			DSI_PHYSICAL_LANE_2,
			DSI_PHYSICAL_LANE_1,
			DSI_PHYSICAL_LANE_0);
	} else {
		DSI_WARN("%s: invalid lane map %s specified. defaulting to lane_map0123\n",
			__func__, data);
		goto set_default;
	}
	return 0;

set_default:
	/* default lane mapping */
	__set_lane_map_v2(display->lane_map.lane_map_v2, DSI_PHYSICAL_LANE_0,
		DSI_PHYSICAL_LANE_1, DSI_PHYSICAL_LANE_2, DSI_PHYSICAL_LANE_3);
	display->lane_map.lane_map_v1 = DSI_LANE_MAP_0123;
	return 0;
}

static int dsi_display_get_phandle_index(
			struct dsi_display *display,
			const char *propname, int count, int index)
{
	struct device_node *disp_node = display->panel_node;
	u32 *val = NULL;
	int rc = 0;

	val = kcalloc(count, sizeof(*val), GFP_KERNEL);
	if (ZERO_OR_NULL_PTR(val)) {
		rc = -ENOMEM;
		goto end;
	}

	if (index >= count)
		goto end;

	if (display->fw)
		rc = dsi_parser_read_u32_array(display->parser_node,
			propname, val, count);
	else
		rc = of_property_read_u32_array(disp_node, propname,
			val, count);
	if (rc)
		goto end;

	rc = val[index];

	DSI_DEBUG("%s index=%d\n", propname, rc);
end:
	kfree(val);
	return rc;
}

static int dsi_display_get_phandle_count(struct dsi_display *display,
			const char *propname)
{
	if (display->fw)
		return dsi_parser_count_u32_elems(display->parser_node,
				propname);
	else
		return of_property_count_u32_elems(display->panel_node,
				propname);
}

static int dsi_display_parse_dt(struct dsi_display *display)
{
	int i, rc = 0;
	u32 phy_count = 0;
	struct device_node *of_node = display->pdev->dev.of_node;
	char *dsi_ctrl_name, *dsi_phy_name;

	if (!strcmp(display->display_type, "primary")) {
		dsi_ctrl_name = "qcom,dsi-ctrl-num";
		dsi_phy_name = "qcom,dsi-phy-num";
	} else {
		dsi_ctrl_name = "qcom,dsi-sec-ctrl-num";
		dsi_phy_name = "qcom,dsi-sec-phy-num";
	}

	display->ctrl_count = dsi_display_get_phandle_count(display,
					dsi_ctrl_name);
	phy_count = dsi_display_get_phandle_count(display, dsi_phy_name);

	DSI_DEBUG("ctrl count=%d, phy count=%d\n",
			display->ctrl_count, phy_count);

	if (!phy_count || !display->ctrl_count) {
		DSI_ERR("no ctrl/phys found\n");
		rc = -ENODEV;
		goto error;
	}

	if (phy_count != display->ctrl_count) {
		DSI_ERR("different ctrl and phy counts\n");
		rc = -ENODEV;
		goto error;
	}

	display_for_each_ctrl(i, display) {
		struct dsi_display_ctrl *ctrl = &display->ctrl[i];
		int index;
		index = dsi_display_get_phandle_index(display, dsi_ctrl_name,
			display->ctrl_count, i);
		ctrl->ctrl_of_node = of_parse_phandle(of_node,
				"qcom,dsi-ctrl", index);
		of_node_put(ctrl->ctrl_of_node);

		index = dsi_display_get_phandle_index(display, dsi_phy_name,
			display->ctrl_count, i);
		ctrl->phy_of_node = of_parse_phandle(of_node,
				"qcom,dsi-phy", index);
		of_node_put(ctrl->phy_of_node);
	}

	/* Parse TE data */
	dsi_display_parse_te_data(display);

	display->needs_clk_src_reset = of_property_read_bool(of_node,
				"qcom,needs-clk-src-reset");

	display->needs_ctrl_vreg_disable = of_property_read_bool(of_node,
				"qcom,needs-ctrl-vreg-disable");

	/* Parse all external bridges from port 0 */
	display_for_each_ctrl(i, display) {
		display->ext_bridge[i].node_of =
			of_graph_get_remote_node(of_node, 0, i);
		if (display->ext_bridge[i].node_of)
			display->ext_bridge_cnt++;
		else
			break;
	}

	DSI_DEBUG("success\n");
error:
	return rc;
}

static int dsi_display_res_init(struct dsi_display *display)
{
	int rc = 0;
	int i;
	struct dsi_display_ctrl *ctrl;

	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		ctrl->ctrl = dsi_ctrl_get(ctrl->ctrl_of_node);
		if (IS_ERR_OR_NULL(ctrl->ctrl)) {
			rc = PTR_ERR(ctrl->ctrl);
			DSI_ERR("failed to get dsi controller, rc=%d\n", rc);
			ctrl->ctrl = NULL;
			goto error_ctrl_put;
		}

		ctrl->phy = dsi_phy_get(ctrl->phy_of_node);
		if (IS_ERR_OR_NULL(ctrl->phy)) {
			rc = PTR_ERR(ctrl->phy);
			DSI_ERR("failed to get phy controller, rc=%d\n", rc);
			dsi_ctrl_put(ctrl->ctrl);
			ctrl->phy = NULL;
			goto error_ctrl_put;
		}
	}

	if (!strcmp(display->display_type, "primary"))
		display->panel_idx = 0;
	else if (!strcmp(display->display_type, "secondary"))
		display->panel_idx = 1;
	else {
		DSI_WARN("Unalbe to find the display type");
		display->panel_idx = 0;
	}
	display->panel = dsi_panel_get(&display->pdev->dev,
				display->panel_node,
				display->parser_node,
				display->display_type,
				display->cmdline_topology,
				display->trusted_vm_env,
				display->panel_idx);
	if (IS_ERR_OR_NULL(display->panel)) {
		rc = PTR_ERR(display->panel);
		DSI_ERR("failed to get panel, rc=%d\n", rc);
		display->panel = NULL;
		goto error_ctrl_put;
	}

	display_for_each_ctrl(i, display) {
		struct msm_dsi_phy *phy = display->ctrl[i].phy;

		phy->cfg.force_clk_lane_hs =
			display->panel->host_config.force_hs_clk_lane;
		phy->cfg.phy_type =
			display->panel->host_config.phy_type;
	}

	dsi_panel_parse_panel_cfg(display->panel,
				!strcmp(display->display_type, "primary"));

	rc = dsi_display_parse_lane_map(display);
	if (rc) {
		DSI_ERR("Lane map not found, rc=%d\n", rc);
		goto error_ctrl_put;
	}

	rc = dsi_display_clocks_init(display);
	if (rc) {
		DSI_ERR("Failed to parse clock data, rc=%d\n", rc);
		goto error_ctrl_put;
	}

	/**
	 * In trusted vm, the connectors will not be enabled
	 * until the HW resources are assigned and accepted.
	 */
	if (display->trusted_vm_env) {
		display->is_active = false;
		display->hw_ownership = false;
	} else {
		display->is_active = true;
		display->hw_ownership = true;
	}

	return 0;
error_ctrl_put:
	for (i = i - 1; i >= 0; i--) {
		ctrl = &display->ctrl[i];
		dsi_ctrl_put(ctrl->ctrl);
		dsi_phy_put(ctrl->phy);
	}
	return rc;
}

static int dsi_display_res_deinit(struct dsi_display *display)
{
	int rc = 0;
	int i;
	struct dsi_display_ctrl *ctrl;

	rc = dsi_display_clocks_deinit(display);
	if (rc)
		DSI_ERR("clocks deinit failed, rc=%d\n", rc);

	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		dsi_phy_put(ctrl->phy);
		dsi_ctrl_put(ctrl->ctrl);
	}

	if (display->panel)
		dsi_panel_put(display->panel);

	return rc;
}

static int dsi_display_validate_mode_set(struct dsi_display *display,
					 struct dsi_display_mode *mode,
					 u32 flags)
{
	int rc = 0;
	int i;
	struct dsi_display_ctrl *ctrl;

	/*
	 * To set a mode:
	 * 1. Controllers should be turned off.
	 * 2. Link clocks should be off.
	 * 3. Phy should be disabled.
	 */

	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		if ((ctrl->power_state > DSI_CTRL_POWER_VREG_ON) ||
		    (ctrl->phy_enabled)) {
			rc = -EINVAL;
			goto error;
		}
	}

error:
	return rc;
}

static bool dsi_display_is_seamless_dfps_possible(
		const struct dsi_display *display,
		const struct dsi_display_mode *tgt,
		const enum dsi_dfps_type dfps_type)
{
	struct dsi_display_mode *cur;

	if (!display || !tgt || !display->panel) {
		DSI_ERR("Invalid params\n");
		return false;
	}

	cur = display->panel->cur_mode;

	if (cur->timing.h_active != tgt->timing.h_active) {
		DSI_DEBUG("timing.h_active differs %d %d\n",
				cur->timing.h_active, tgt->timing.h_active);
		return false;
	}

	if (cur->timing.h_back_porch != tgt->timing.h_back_porch) {
		DSI_DEBUG("timing.h_back_porch differs %d %d\n",
				cur->timing.h_back_porch,
				tgt->timing.h_back_porch);
		return false;
	}

	if (cur->timing.h_sync_width != tgt->timing.h_sync_width) {
		DSI_DEBUG("timing.h_sync_width differs %d %d\n",
				cur->timing.h_sync_width,
				tgt->timing.h_sync_width);
		return false;
	}

	if (cur->timing.h_front_porch != tgt->timing.h_front_porch) {
		DSI_DEBUG("timing.h_front_porch differs %d %d\n",
				cur->timing.h_front_porch,
				tgt->timing.h_front_porch);
		if (dfps_type != DSI_DFPS_IMMEDIATE_HFP)
			return false;
	}

	if (cur->timing.h_skew != tgt->timing.h_skew) {
		DSI_DEBUG("timing.h_skew differs %d %d\n",
				cur->timing.h_skew,
				tgt->timing.h_skew);
		return false;
	}

	/* skip polarity comparison */

	if (cur->timing.v_active != tgt->timing.v_active) {
		DSI_DEBUG("timing.v_active differs %d %d\n",
				cur->timing.v_active,
				tgt->timing.v_active);
		return false;
	}

	if (cur->timing.v_back_porch != tgt->timing.v_back_porch) {
		DSI_DEBUG("timing.v_back_porch differs %d %d\n",
				cur->timing.v_back_porch,
				tgt->timing.v_back_porch);
		return false;
	}

	if (cur->timing.v_sync_width != tgt->timing.v_sync_width) {
		DSI_DEBUG("timing.v_sync_width differs %d %d\n",
				cur->timing.v_sync_width,
				tgt->timing.v_sync_width);
		return false;
	}

	if (cur->timing.v_front_porch != tgt->timing.v_front_porch) {
		DSI_DEBUG("timing.v_front_porch differs %d %d\n",
				cur->timing.v_front_porch,
				tgt->timing.v_front_porch);
		if (dfps_type != DSI_DFPS_IMMEDIATE_VFP)
			return false;
	}

	/* skip polarity comparison */

	if (cur->timing.refresh_rate == tgt->timing.refresh_rate)
		DSI_DEBUG("timing.refresh_rate identical %d %d\n",
				cur->timing.refresh_rate,
				tgt->timing.refresh_rate);

	if (cur->pixel_clk_khz != tgt->pixel_clk_khz)
		DSI_DEBUG("pixel_clk_khz differs %d %d\n",
				cur->pixel_clk_khz, tgt->pixel_clk_khz);

	if (cur->dsi_mode_flags != tgt->dsi_mode_flags)
		DSI_DEBUG("flags differs %d %d\n",
				cur->dsi_mode_flags, tgt->dsi_mode_flags);

	return true;
}

void dsi_display_update_byte_intf_div(struct dsi_display *display)
{
	struct dsi_host_common_cfg *config;
	struct dsi_display_ctrl *m_ctrl;
	int phy_ver;

	m_ctrl = &display->ctrl[display->cmd_master_idx];
	config = &display->panel->host_config;

	phy_ver = dsi_phy_get_version(m_ctrl->phy);
	if (phy_ver <= DSI_PHY_VERSION_2_0)
		config->byte_intf_clk_div = 1;
	else
		config->byte_intf_clk_div = 2;
}

static int dsi_display_update_dsi_bitrate(struct dsi_display *display,
					  u32 bit_clk_rate)
{
	int rc = 0;
	int i;

	DSI_DEBUG("%s:bit rate:%d\n", __func__, bit_clk_rate);
	if (!display->panel) {
		DSI_ERR("Invalid params\n");
		return -EINVAL;
	}

	if (bit_clk_rate == 0) {
		DSI_ERR("Invalid bit clock rate\n");
		return -EINVAL;
	}

	display->config.bit_clk_rate_hz = bit_clk_rate;

	display_for_each_ctrl(i, display) {
		struct dsi_display_ctrl *dsi_disp_ctrl = &display->ctrl[i];
		struct dsi_ctrl *ctrl = dsi_disp_ctrl->ctrl;
		u32 num_of_lanes = 0, bpp, byte_intf_clk_div;
		u64 bit_rate, pclk_rate, bit_rate_per_lane, byte_clk_rate,
				byte_intf_clk_rate;
		u32 bits_per_symbol = 16, num_of_symbols = 7; /* For Cphy */
		struct dsi_host_common_cfg *host_cfg;

		mutex_lock(&ctrl->ctrl_lock);

		host_cfg = &display->panel->host_config;
		if (host_cfg->data_lanes & DSI_DATA_LANE_0)
			num_of_lanes++;
		if (host_cfg->data_lanes & DSI_DATA_LANE_1)
			num_of_lanes++;
		if (host_cfg->data_lanes & DSI_DATA_LANE_2)
			num_of_lanes++;
		if (host_cfg->data_lanes & DSI_DATA_LANE_3)
			num_of_lanes++;

		if (num_of_lanes == 0) {
			DSI_ERR("Invalid lane count\n");
			rc = -EINVAL;
			goto error;
		}

		bpp = dsi_pixel_format_to_bpp(host_cfg->dst_format);

		bit_rate = display->config.bit_clk_rate_hz * num_of_lanes;
		bit_rate_per_lane = bit_rate;
		do_div(bit_rate_per_lane, num_of_lanes);
		pclk_rate = bit_rate;
		do_div(pclk_rate, bpp);
		if (host_cfg->phy_type == DSI_PHY_TYPE_DPHY) {
			bit_rate_per_lane = bit_rate;
			do_div(bit_rate_per_lane, num_of_lanes);
			byte_clk_rate = bit_rate_per_lane;
			do_div(byte_clk_rate, 8);
			byte_intf_clk_rate = byte_clk_rate;
			byte_intf_clk_div = host_cfg->byte_intf_clk_div;
			do_div(byte_intf_clk_rate, byte_intf_clk_div);
		} else {
			bit_rate_per_lane = bit_clk_rate;
			pclk_rate *= bits_per_symbol;
			do_div(pclk_rate, num_of_symbols);
			byte_clk_rate = bit_clk_rate;
			do_div(byte_clk_rate, num_of_symbols);

			/* For CPHY, byte_intf_clk is same as byte_clk */
			byte_intf_clk_rate = byte_clk_rate;
		}

		DSI_DEBUG("bit_clk_rate = %llu, bit_clk_rate_per_lane = %llu\n",
			 bit_rate, bit_rate_per_lane);
		DSI_DEBUG("byte_clk_rate = %llu, byte_intf_clk_rate = %llu\n",
			  byte_clk_rate, byte_intf_clk_rate);
		DSI_DEBUG("pclk_rate = %llu\n", pclk_rate);
		SDE_EVT32(i, bit_rate, byte_clk_rate, pclk_rate);

		ctrl->clk_freq.byte_clk_rate = byte_clk_rate;
		ctrl->clk_freq.byte_intf_clk_rate = byte_intf_clk_rate;
		ctrl->clk_freq.pix_clk_rate = pclk_rate;
		rc = dsi_clk_set_link_frequencies(display->dsi_clk_handle,
			ctrl->clk_freq, ctrl->cell_index);
		if (rc) {
			DSI_ERR("Failed to update link frequencies\n");
			goto error;
		}

		ctrl->host_config.bit_clk_rate_hz = bit_clk_rate;
error:
		mutex_unlock(&ctrl->ctrl_lock);

		/* TODO: recover ctrl->clk_freq in case of failure */
		if (rc)
			return rc;
	}

	return 0;
}

static void _dsi_display_calc_pipe_delay(struct dsi_display *display,
				    struct dsi_dyn_clk_delay *delay,
				    struct dsi_display_mode *mode)
{
	u32 esc_clk_rate_hz;
	u32 pclk_to_esc_ratio, byte_to_esc_ratio, hr_bit_to_esc_ratio;
	u32 hsync_period = 0;
	struct dsi_display_ctrl *m_ctrl;
	struct dsi_ctrl *dsi_ctrl;
	struct dsi_phy_cfg *cfg;
	int phy_ver;

	m_ctrl = &display->ctrl[display->clk_master_idx];
	dsi_ctrl = m_ctrl->ctrl;

	cfg = &(m_ctrl->phy->cfg);

	esc_clk_rate_hz = dsi_ctrl->clk_freq.esc_clk_rate;
	pclk_to_esc_ratio = (dsi_ctrl->clk_freq.pix_clk_rate /
			     esc_clk_rate_hz);
	byte_to_esc_ratio = (dsi_ctrl->clk_freq.byte_clk_rate /
			     esc_clk_rate_hz);
	hr_bit_to_esc_ratio = ((dsi_ctrl->clk_freq.byte_clk_rate * 4) /
					esc_clk_rate_hz);

	hsync_period = dsi_h_total_dce(&mode->timing);
	delay->pipe_delay = (hsync_period + 1) / pclk_to_esc_ratio;
	if (!display->panel->video_config.eof_bllp_lp11_en)
		delay->pipe_delay += (17 / pclk_to_esc_ratio) +
			((21 + (display->config.common_config.t_clk_pre + 1) +
			  (display->config.common_config.t_clk_post + 1)) /
			 byte_to_esc_ratio) +
			((((cfg->timing.lane_v3[8] >> 1) + 1) +
			((cfg->timing.lane_v3[6] >> 1) + 1) +
			((cfg->timing.lane_v3[3] * 4) +
			 (cfg->timing.lane_v3[5] >> 1) + 1) +
			((cfg->timing.lane_v3[7] >> 1) + 1) +
			((cfg->timing.lane_v3[1] >> 1) + 1) +
			((cfg->timing.lane_v3[4] >> 1) + 1)) /
			 hr_bit_to_esc_ratio);

	delay->pipe_delay2 = 0;
	if (display->panel->host_config.force_hs_clk_lane)
		delay->pipe_delay2 = (6 / byte_to_esc_ratio) +
			((((cfg->timing.lane_v3[1] >> 1) + 1) +
			  ((cfg->timing.lane_v3[4] >> 1) + 1)) /
			 hr_bit_to_esc_ratio);

	/*
	 * 100us pll delay recommended for phy ver 2.0 and 3.0
	 * 25us pll delay recommended for phy ver 4.0
	 */
	phy_ver = dsi_phy_get_version(m_ctrl->phy);
	if (phy_ver <= DSI_PHY_VERSION_3_0)
		delay->pll_delay = 100;
	else
		delay->pll_delay = 25;

	delay->pll_delay = ((delay->pll_delay * esc_clk_rate_hz) / 1000000);
}

/*
 * dsi_display_is_type_cphy - check if panel type is cphy
 * @display: Pointer to private display structure
 * Returns: True if panel type is cphy
 */
static inline bool dsi_display_is_type_cphy(struct dsi_display *display)
{
	return (display->panel->host_config.phy_type ==
		DSI_PHY_TYPE_CPHY) ? true : false;
}

static int _dsi_display_dyn_update_clks(struct dsi_display *display,
					struct link_clk_freq *bkp_freq)
{
	int rc = 0, i;
	u8 ctrl_version;
	struct dsi_display_ctrl *m_ctrl, *ctrl;
	struct dsi_dyn_clk_caps *dyn_clk_caps;
	struct dsi_clk_link_set *parent_clk, *enable_clk;

	m_ctrl = &display->ctrl[display->clk_master_idx];
	dyn_clk_caps = &(display->panel->dyn_clk_caps);
	ctrl_version = m_ctrl->ctrl->version;

	if (dsi_display_is_type_cphy(display)) {
		enable_clk = &display->clock_info.cphy_clks;
		parent_clk = &display->clock_info.shadow_cphy_clks;
	} else {
		enable_clk = &display->clock_info.src_clks;
		parent_clk = &display->clock_info.shadow_clks;
	}

	dsi_clk_prepare_enable(enable_clk);

	rc = dsi_clk_update_parent(parent_clk,
				&display->clock_info.mux_clks);
	if (rc) {
		DSI_ERR("failed to update mux parent\n");
		goto exit;
	}

	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		if (!ctrl->ctrl)
			continue;
		rc = dsi_clk_set_byte_clk_rate(display->dsi_clk_handle,
				ctrl->ctrl->clk_freq.byte_clk_rate,
				ctrl->ctrl->clk_freq.byte_intf_clk_rate, i);
		if (rc) {
			DSI_ERR("failed to set byte rate for index:%d\n", i);
			goto recover_byte_clk;
		}
		rc = dsi_clk_set_pixel_clk_rate(display->dsi_clk_handle,
				   ctrl->ctrl->clk_freq.pix_clk_rate, i);
		if (rc) {
			DSI_ERR("failed to set pix rate for index:%d\n", i);
			goto recover_pix_clk;
		}
	}

	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		if (ctrl == m_ctrl)
			continue;
		dsi_phy_dynamic_refresh_trigger(ctrl->phy, false);
	}
	dsi_phy_dynamic_refresh_trigger(m_ctrl->phy, true);

	/*
	 * Don't wait for dynamic refresh done for dsi ctrl greater than 2.5
	 * and with constant fps, as dynamic refresh will applied with
	 * next mdp intf ctrl flush.
	 */
	if ((ctrl_version >= DSI_CTRL_VERSION_2_5) &&
			(dyn_clk_caps->maintain_const_fps))
		goto defer_dfps_wait;

	/* wait for dynamic refresh done */
	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		rc = dsi_ctrl_wait4dynamic_refresh_done(ctrl->ctrl);
		if (rc) {
			DSI_ERR("wait4dynamic refresh failed for dsi:%d\n", i);
			goto recover_pix_clk;
		} else {
			DSI_INFO("dynamic refresh done on dsi: %s\n",
				i ? "slave" : "master");
		}
	}

	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		dsi_phy_dynamic_refresh_clear(ctrl->phy);
	}

	rc = dsi_clk_update_parent(enable_clk,
			&display->clock_info.mux_clks);
	if (rc)
		DSI_ERR("could not switch back to src clks %d\n", rc);

	dsi_clk_disable_unprepare(enable_clk);

	return rc;

recover_pix_clk:
	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		if (!ctrl->ctrl)
			continue;
		dsi_clk_set_pixel_clk_rate(display->dsi_clk_handle,
					   bkp_freq->pix_clk_rate, i);
	}

recover_byte_clk:
	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		if (!ctrl->ctrl)
			continue;
		dsi_clk_set_byte_clk_rate(display->dsi_clk_handle,
					bkp_freq->byte_clk_rate,
					bkp_freq->byte_intf_clk_rate, i);
	}

exit:
	dsi_clk_disable_unprepare(&display->clock_info.src_clks);

defer_dfps_wait:
	return rc;
}

void dsi_display_dfps_update_parent(struct dsi_display *display)
{
	int rc = 0;

	rc = dsi_clk_update_parent(&display->clock_info.src_clks,
			      &display->clock_info.mux_clks);
	if (rc)
		DSI_ERR("could not switch back to src clks %d\n", rc);

	dsi_clk_disable_unprepare(&display->clock_info.src_clks);
}

static int dsi_display_dynamic_clk_switch_vid(struct dsi_display *display,
					  struct dsi_display_mode *mode)
{
	int rc = 0, mask, i;
	struct dsi_display_ctrl *m_ctrl, *ctrl;
	struct dsi_dyn_clk_delay delay;
	struct link_clk_freq bkp_freq;

	dsi_panel_acquire_panel_lock(display->panel);

	m_ctrl = &display->ctrl[display->clk_master_idx];

	dsi_display_clk_ctrl(display->dsi_clk_handle, DSI_ALL_CLKS, DSI_CLK_ON);

	/* mask PLL unlock, FIFO overflow and underflow errors */
	mask = BIT(DSI_PLL_UNLOCK_ERR) | BIT(DSI_FIFO_UNDERFLOW) |
		BIT(DSI_FIFO_OVERFLOW);
	dsi_display_mask_ctrl_error_interrupts(display, mask, true);

	/* update the phy timings based on new mode */
	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		dsi_phy_update_phy_timings(ctrl->phy, &display->config);
	}

	/* back up existing rates to handle failure case */
	bkp_freq.byte_clk_rate = m_ctrl->ctrl->clk_freq.byte_clk_rate;
	bkp_freq.byte_intf_clk_rate = m_ctrl->ctrl->clk_freq.byte_intf_clk_rate;
	bkp_freq.pix_clk_rate = m_ctrl->ctrl->clk_freq.pix_clk_rate;
	bkp_freq.esc_clk_rate = m_ctrl->ctrl->clk_freq.esc_clk_rate;

	rc = dsi_display_update_dsi_bitrate(display, mode->timing.clk_rate_hz);
	if (rc) {
		DSI_ERR("failed set link frequencies %d\n", rc);
		goto exit;
	}

	/* calculate pipe delays */
	_dsi_display_calc_pipe_delay(display, &delay, mode);

	/* configure dynamic refresh ctrl registers */
	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		if (!ctrl->phy)
			continue;
		if (ctrl == m_ctrl)
			dsi_phy_config_dynamic_refresh(ctrl->phy, &delay, true);
		else
			dsi_phy_config_dynamic_refresh(ctrl->phy, &delay,
						       false);
	}

	rc = _dsi_display_dyn_update_clks(display, &bkp_freq);

exit:
	dsi_display_mask_ctrl_error_interrupts(display, mask, false);

	dsi_display_clk_ctrl(display->dsi_clk_handle, DSI_ALL_CLKS,
			     DSI_CLK_OFF);

	/* store newly calculated phy timings in mode private info */
	dsi_phy_dyn_refresh_cache_phy_timings(m_ctrl->phy,
					      mode->priv_info->phy_timing_val,
					      mode->priv_info->phy_timing_len);

	dsi_panel_release_panel_lock(display->panel);

	return rc;
}

static int dsi_display_dynamic_clk_configure_cmd(struct dsi_display *display,
		int clk_rate)
{
	int rc = 0;

	if (clk_rate <= 0) {
		DSI_ERR("%s: bitrate should be greater than 0\n", __func__);
		return -EINVAL;
	}

	if (clk_rate == display->cached_clk_rate) {
		DSI_INFO("%s: ignore duplicated DSI clk setting\n", __func__);
		return rc;
	}

	display->cached_clk_rate = clk_rate;

	rc = dsi_display_update_dsi_bitrate(display, clk_rate);
	if (!rc) {
		DSI_DEBUG("%s: bit clk is ready to be configured to '%d'\n",
				__func__, clk_rate);
		atomic_set(&display->clkrate_change_pending, 1);
	} else {
		DSI_ERR("%s: Failed to prepare to configure '%d'. rc = %d\n",
				__func__, clk_rate, rc);
		/* Caching clock failed, so don't go on doing so. */
		atomic_set(&display->clkrate_change_pending, 0);
		display->cached_clk_rate = 0;
	}

	return rc;
}

static int dsi_display_dfps_update(struct dsi_display *display,
				   struct dsi_display_mode *dsi_mode)
{
	struct dsi_mode_info *timing;
	struct dsi_display_ctrl *m_ctrl, *ctrl;
	struct dsi_display_mode *panel_mode;
	struct dsi_dfps_capabilities dfps_caps;
	int rc = 0;
	int i = 0;
	struct dsi_dyn_clk_caps *dyn_clk_caps;

	if (!display || !dsi_mode || !display->panel) {
		DSI_ERR("Invalid params\n");
		return -EINVAL;
	}
	timing = &dsi_mode->timing;

	dsi_panel_get_dfps_caps(display->panel, &dfps_caps);
	dyn_clk_caps = &(display->panel->dyn_clk_caps);
	if (!dfps_caps.dfps_support && !dyn_clk_caps->maintain_const_fps) {
		DSI_ERR("dfps or constant fps not supported\n");
		return -ENOTSUPP;
	}

	if (dfps_caps.type == DSI_DFPS_IMMEDIATE_CLK) {
		DSI_ERR("dfps clock method not supported\n");
		return -ENOTSUPP;
	}

	/* For split DSI, update the clock master first */

	DSI_DEBUG("configuring seamless dynamic fps\n\n");
	SDE_EVT32(SDE_EVTLOG_FUNC_ENTRY);

	m_ctrl = &display->ctrl[display->clk_master_idx];
	rc = dsi_ctrl_async_timing_update(m_ctrl->ctrl, timing);
	if (rc) {
		DSI_ERR("[%s] failed to dfps update host_%d, rc=%d\n",
				display->name, i, rc);
		goto error;
	}

	/* Update the rest of the controllers */
	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		if (!ctrl->ctrl || (ctrl == m_ctrl))
			continue;

		rc = dsi_ctrl_async_timing_update(ctrl->ctrl, timing);
		if (rc) {
			DSI_ERR("[%s] failed to dfps update host_%d, rc=%d\n",
					display->name, i, rc);
			goto error;
		}
	}

	panel_mode = display->panel->cur_mode;
	memcpy(panel_mode, dsi_mode, sizeof(*panel_mode));
	/*
	 * dsi_mode_flags flags are used to communicate with other drm driver
	 * components, and are transient. They aren't inherently part of the
	 * display panel's mode and shouldn't be saved into the cached currently
	 * active mode.
	 */
	panel_mode->dsi_mode_flags = 0;

error:
	SDE_EVT32(SDE_EVTLOG_FUNC_EXIT);
	return rc;
}

static int dsi_display_dfps_calc_front_porch(
		u32 old_fps,
		u32 new_fps,
		u32 a_total,
		u32 b_total,
		u32 b_fp,
		u32 *b_fp_out)
{
	s32 b_fp_new;
	int add_porches, diff;

	if (!b_fp_out) {
		DSI_ERR("Invalid params\n");
		return -EINVAL;
	}

	if (!a_total || !new_fps) {
		DSI_ERR("Invalid pixel total or new fps in mode request\n");
		return -EINVAL;
	}

	/*
	 * Keep clock, other porches constant, use new fps, calc front porch
	 * new_vtotal = old_vtotal * (old_fps / new_fps )
	 * new_vfp - old_vfp = new_vtotal - old_vtotal
	 * new_vfp = old_vfp + old_vtotal * ((old_fps - new_fps)/ new_fps)
	 */
	diff = abs(old_fps - new_fps);
	add_porches = mult_frac(b_total, diff, new_fps);

	if (old_fps > new_fps)
		b_fp_new = b_fp + add_porches;
	else
		b_fp_new = b_fp - add_porches;

	DSI_DEBUG("fps %u a %u b %u b_fp %u new_fp %d\n",
			new_fps, a_total, b_total, b_fp, b_fp_new);

	if (b_fp_new < 0) {
		DSI_ERR("Invalid new_hfp calcluated%d\n", b_fp_new);
		return -EINVAL;
	}

	/**
	 * TODO: To differentiate from clock method when communicating to the
	 * other components, perhaps we should set clk here to original value
	 */
	*b_fp_out = b_fp_new;

	return 0;
}

/**
 * dsi_display_get_dfps_timing() - Get the new dfps values.
 * @display:         DSI display handle.
 * @adj_mode:        Mode value structure to be changed.
 *                   It contains old timing values and latest fps value.
 *                   New timing values are updated based on new fps.
 * @curr_refresh_rate:  Current fps rate.
 *                      If zero , current fps rate is taken from
 *                      display->panel->cur_mode.
 * Return: error code.
 */
static int dsi_display_get_dfps_timing(struct dsi_display *display,
			struct dsi_display_mode *adj_mode,
				u32 curr_refresh_rate)
{
	struct dsi_dfps_capabilities dfps_caps;
	struct dsi_display_mode per_ctrl_mode;
	struct dsi_mode_info *timing;
	struct dsi_ctrl *m_ctrl;

	int rc = 0;

	if (!display || !adj_mode) {
		DSI_ERR("Invalid params\n");
		return -EINVAL;
	}
	m_ctrl = display->ctrl[display->clk_master_idx].ctrl;

	dsi_panel_get_dfps_caps(display->panel, &dfps_caps);
	if (!dfps_caps.dfps_support) {
		DSI_ERR("dfps not supported by panel\n");
		return -EINVAL;
	}

	per_ctrl_mode = *adj_mode;
	adjust_timing_by_ctrl_count(display, &per_ctrl_mode);

	if (!curr_refresh_rate) {
		if (!dsi_display_is_seamless_dfps_possible(display,
				&per_ctrl_mode, dfps_caps.type)) {
			DSI_ERR("seamless dynamic fps not supported for mode\n");
			return -EINVAL;
		}
		if (display->panel->cur_mode) {
			curr_refresh_rate =
				display->panel->cur_mode->timing.refresh_rate;
		} else {
			DSI_ERR("cur_mode is not initialized\n");
			return -EINVAL;
		}
	}
	/* TODO: Remove this direct reference to the dsi_ctrl */
	timing = &per_ctrl_mode.timing;

	switch (dfps_caps.type) {
	case DSI_DFPS_IMMEDIATE_VFP:
		rc = dsi_display_dfps_calc_front_porch(
				curr_refresh_rate,
				timing->refresh_rate,
				dsi_h_total_dce(timing),
				DSI_V_TOTAL(timing),
				timing->v_front_porch,
				&adj_mode->timing.v_front_porch);
		SDE_EVT32(SDE_EVTLOG_FUNC_CASE1, DSI_DFPS_IMMEDIATE_VFP,
			curr_refresh_rate, timing->refresh_rate,
			timing->v_front_porch, adj_mode->timing.v_front_porch);
		break;

	case DSI_DFPS_IMMEDIATE_HFP:
		rc = dsi_display_dfps_calc_front_porch(
				curr_refresh_rate,
				timing->refresh_rate,
				DSI_V_TOTAL(timing),
				dsi_h_total_dce(timing),
				timing->h_front_porch,
				&adj_mode->timing.h_front_porch);
		SDE_EVT32(SDE_EVTLOG_FUNC_CASE2, DSI_DFPS_IMMEDIATE_HFP,
			curr_refresh_rate, timing->refresh_rate,
			timing->h_front_porch, adj_mode->timing.h_front_porch);
		if (!rc)
			adj_mode->timing.h_front_porch *= display->ctrl_count;
		break;

	default:
		DSI_ERR("Unsupported DFPS mode %d\n", dfps_caps.type);
		rc = -ENOTSUPP;
	}

	return rc;
}

static bool dsi_display_validate_mode_seamless(struct dsi_display *display,
		struct dsi_display_mode *adj_mode)
{
	int rc = 0;

	if (!display || !adj_mode) {
		DSI_ERR("Invalid params\n");
		return false;
	}

	/* Currently the only seamless transition is dynamic fps */
	rc = dsi_display_get_dfps_timing(display, adj_mode, 0);
	if (rc) {
		DSI_DEBUG("Dynamic FPS not supported for seamless\n");
	} else {
		DSI_DEBUG("Mode switch is seamless Dynamic FPS\n");
		adj_mode->dsi_mode_flags |= DSI_MODE_FLAG_DFPS |
				DSI_MODE_FLAG_VBLANK_PRE_MODESET;
	}

	return rc;
}

static void dsi_display_validate_dms_fps(struct dsi_display_mode *cur_mode,
		struct dsi_display_mode *to_mode)
{
	u32 cur_fps, to_fps;
	u32 cur_h_active, to_h_active;
	u32 cur_v_active, to_v_active;

	cur_fps = cur_mode->timing.refresh_rate;
	to_fps = to_mode->timing.refresh_rate;
	cur_h_active = cur_mode->timing.h_active;
	cur_v_active = cur_mode->timing.v_active;
	to_h_active = to_mode->timing.h_active;
	to_v_active = to_mode->timing.v_active;

	if ((cur_h_active == to_h_active) && (cur_v_active == to_v_active) &&
			(cur_fps != to_fps)) {
		to_mode->dsi_mode_flags |= DSI_MODE_FLAG_DMS_FPS;
		DSI_DEBUG("DMS Modeset with FPS change\n");
	} else {
		to_mode->dsi_mode_flags &= ~DSI_MODE_FLAG_DMS_FPS;
	}
}


static int dsi_display_set_mode_sub(struct dsi_display *display,
				    struct dsi_display_mode *mode,
				    u32 flags)
{
	int rc = 0, clk_rate = 0;
	int i;
	struct dsi_display_ctrl *ctrl;
	struct dsi_display_ctrl *mctrl;
	struct dsi_display_mode_priv_info *priv_info;
	bool commit_phy_timing = false;
	struct dsi_dyn_clk_caps *dyn_clk_caps;

	priv_info = mode->priv_info;
	if (!priv_info) {
		DSI_ERR("[%s] failed to get private info of the display mode\n",
			display->name);
		return -EINVAL;
	}

	SDE_EVT32(mode->dsi_mode_flags, mode->panel_mode);

	display->panel->panel_mode = mode->panel_mode;
	rc = dsi_panel_get_host_cfg_for_mode(display->panel,
					     mode,
					     &display->config);
	if (rc) {
		DSI_ERR("[%s] failed to get host config for mode, rc=%d\n",
		       display->name, rc);
		goto error;
	}

	memcpy(&display->config.lane_map, &display->lane_map,
	       sizeof(display->lane_map));

	mctrl = &display->ctrl[display->clk_master_idx];
	dyn_clk_caps = &(display->panel->dyn_clk_caps);

	if (mode->dsi_mode_flags &
			(DSI_MODE_FLAG_DFPS | DSI_MODE_FLAG_VRR)) {
		display_for_each_ctrl(i, display) {
			ctrl = &display->ctrl[i];

			if (!ctrl->ctrl || (ctrl != mctrl))
				continue;

			ctrl->ctrl->hw.ops.set_timing_db(&ctrl->ctrl->hw,
					true);
			dsi_phy_dynamic_refresh_clear(ctrl->phy);

			if ((ctrl->ctrl->version >= DSI_CTRL_VERSION_2_5) &&
					(dyn_clk_caps->maintain_const_fps)) {
				dsi_phy_dynamic_refresh_trigger_sel(ctrl->phy,
						true);
			}
		}

		if (display->panel->dfps_caps.dfps_send_cmd_with_te_async) {
			if ((display->panel->dfps_caps.current_fps != 60)&&(display->panel->dfps_caps.current_fps != 90))
				dsi_display_status_check_te(display,1);
		}
		rc = dsi_display_dfps_update(display, mode);
		if (rc) {
			DSI_ERR("[%s]DSI dfps update failed, rc=%d\n",
					display->name, rc);
			goto error;
		}
		display_for_each_ctrl(i, display) {
			ctrl = &display->ctrl[i];
			rc = dsi_ctrl_update_host_config(ctrl->ctrl,
				&display->config, mode, mode->dsi_mode_flags,
					display->dsi_clk_handle);
			if (rc) {
				DSI_ERR("failed to update ctrl config\n");
				goto error;
			}
		}
		if (priv_info->phy_timing_len) {
			display_for_each_ctrl(i, display) {
				ctrl = &display->ctrl[i];
				rc = dsi_phy_set_timing_params(ctrl->phy,
						priv_info->phy_timing_val,
						priv_info->phy_timing_len,
						commit_phy_timing);
				if (rc)
					DSI_ERR("Fail to add timing params\n");
			}
		}

		if (display->panel->dfps_caps.dfps_send_cmd_support)
			dsi_panel_dfps_send_cmd(display->panel);

		if (!(mode->dsi_mode_flags & DSI_MODE_FLAG_DYN_CLK))
			return rc;
	}

	if (mode->dsi_mode_flags & DSI_MODE_FLAG_DYN_CLK) {
		if (display->panel->panel_mode == DSI_OP_VIDEO_MODE) {
			rc = dsi_display_dynamic_clk_switch_vid(display, mode);
			if (rc)
				DSI_ERR("dynamic clk change failed %d\n", rc);
			/*
			 * skip rest of the opearations since
			 * dsi_display_dynamic_clk_switch_vid() already takes
			 * care of them.
			 */
			return rc;
		} else if (display->panel->panel_mode == DSI_OP_CMD_MODE) {
			clk_rate = mode->timing.clk_rate_hz;
			rc = dsi_display_dynamic_clk_configure_cmd(display,
					clk_rate);
			if (rc) {
				DSI_ERR("Failed to configure dynamic clk\n");
				return rc;
			}
		}
	}

	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		rc = dsi_ctrl_update_host_config(ctrl->ctrl, &display->config,
				mode, mode->dsi_mode_flags,
				display->dsi_clk_handle);
		if (rc) {
			DSI_ERR("[%s] failed to update ctrl config, rc=%d\n",
			       display->name, rc);
			goto error;
		}
	}

	if ((mode->dsi_mode_flags & DSI_MODE_FLAG_DMS) &&
			(display->panel->panel_mode == DSI_OP_CMD_MODE)) {
		u64 cur_bitclk = display->panel->cur_mode->timing.clk_rate_hz;
		u64 to_bitclk = mode->timing.clk_rate_hz;
		commit_phy_timing = true;

		/* No need to set clkrate pending flag if clocks are same */
		if ((!cur_bitclk && !to_bitclk) || (cur_bitclk != to_bitclk))
			atomic_set(&display->clkrate_change_pending, 1);

		dsi_display_validate_dms_fps(display->panel->cur_mode, mode);
	}

	if (priv_info->phy_drive_strength) {
		display_for_each_ctrl(i, display) {
			ctrl = &display->ctrl[i];
			rc = dsi_phy_set_drive_strength_params(ctrl->phy,
					priv_info->phy_drive_strength);
			if (rc)
				DSI_ERR("Fail to add drive strength params\n");
		}
	}

	if (priv_info->phy_timing_len) {
		display_for_each_ctrl(i, display) {
			ctrl = &display->ctrl[i];
			 rc = dsi_phy_set_timing_params(ctrl->phy,
				priv_info->phy_timing_val,
				priv_info->phy_timing_len,
				commit_phy_timing);
			if (rc)
				DSI_ERR("failed to add DSI PHY timing params\n");
		}
	}
error:
	return rc;
}

/**
 * _dsi_display_dev_init - initializes the display device
 * Initialization will acquire references to the resources required for the
 * display hardware to function.
 * @display:         Handle to the display
 * Returns:          Zero on success
 */
static int _dsi_display_dev_init(struct dsi_display *display)
{
	int rc = 0;

	if (!display) {
		DSI_ERR("invalid display\n");
		return -EINVAL;
	}

	if (!display->panel_node && !display->fw)
		return 0;

	mutex_lock(&display->display_lock);

	display->parser = dsi_parser_get(&display->pdev->dev);
	if (display->fw && display->parser)
		display->parser_node = dsi_parser_get_head_node(
				display->parser, display->fw->data,
				display->fw->size);

	rc = dsi_display_parse_dt(display);
	if (rc) {
		DSI_ERR("[%s] failed to parse dt, rc=%d\n", display->name, rc);
		goto error;
	}

	rc = dsi_display_res_init(display);
	if (rc) {
		DSI_ERR("[%s] failed to initialize resources, rc=%d\n",
		       display->name, rc);
		goto error;
	}
error:
	mutex_unlock(&display->display_lock);
	return rc;
}

/**
 * _dsi_display_dev_deinit - deinitializes the display device
 * All the resources acquired during device init will be released.
 * @display:        Handle to the display
 * Returns:         Zero on success
 */
static int _dsi_display_dev_deinit(struct dsi_display *display)
{
	int rc = 0;

	if (!display) {
		DSI_ERR("invalid display\n");
		return -EINVAL;
	}

	mutex_lock(&display->display_lock);

	rc = dsi_display_res_deinit(display);
	if (rc)
		DSI_ERR("[%s] failed to deinitialize resource, rc=%d\n",
		       display->name, rc);

	mutex_unlock(&display->display_lock);

	return rc;
}

/**
 * dsi_display_cont_splash_res_disable() - Disable resource votes added in probe
 * @dsi_display:    Pointer to dsi display
 * Returns:     Zero on success
 */
int dsi_display_cont_splash_res_disable(void *dsi_display)
{
	struct dsi_display *display = dsi_display;
	int rc = 0;

	/* Remove the panel vote that was added during dsi display probe */
	rc = dsi_pwr_enable_regulator(&display->panel->power_info, false);
	if (rc)
		DSI_ERR("[%s] failed to disable vregs, rc=%d\n",
				display->panel->name, rc);

	return rc;
}

/**
 * dsi_display_cont_splash_config() - Initialize resources for continuous splash
 * @dsi_display:    Pointer to dsi display
 * Returns:     Zero on success
 */
int dsi_display_cont_splash_config(void *dsi_display)
{
	struct dsi_display *display = dsi_display;
	int rc = 0;

	/* Vote for gdsc required to read register address space */
	if (!display) {
		DSI_ERR("invalid input display param\n");
		return -EINVAL;
	}

	rc = pm_runtime_get_sync(display->drm_dev->dev);
	if (rc < 0) {
		DSI_ERR("failed to vote gdsc for continuous splash, rc=%d\n",
							rc);
		return rc;
	}

	mutex_lock(&display->display_lock);

	display->is_cont_splash_enabled = true;

	/* Update splash status for clock manager */
	dsi_display_clk_mngr_update_splash_status(display->clk_mngr,
				display->is_cont_splash_enabled);
	SDE_EVT32(SDE_EVTLOG_FUNC_ENTRY, display->is_cont_splash_enabled);

	/* Set up ctrl isr before enabling core clk */
	dsi_display_ctrl_isr_configure(display, true);

	/* Vote for Core clk and link clk. Votes on ctrl and phy
	 * regulator are inplicit from  pre clk on callback
	 */
	rc = dsi_display_clk_ctrl(display->dsi_clk_handle,
			DSI_ALL_CLKS, DSI_CLK_ON);
	if (rc) {
		DSI_ERR("[%s] failed to enable DSI link clocks, rc=%d\n",
		       display->name, rc);
		goto clk_manager_update;
	}

	mutex_unlock(&display->display_lock);

	/* Set the current brightness level */
	dsi_panel_bl_handoff(display->panel);

	return rc;

clk_manager_update:
	dsi_display_ctrl_isr_configure(display, false);
	/* Update splash status for clock manager */
	dsi_display_clk_mngr_update_splash_status(display->clk_mngr,
				false);
	pm_runtime_put_sync(display->drm_dev->dev);
	display->is_cont_splash_enabled = false;
	mutex_unlock(&display->display_lock);
	return rc;
}

/**
 * dsi_display_splash_res_cleanup() - cleanup for continuous splash
 * @display:    Pointer to dsi display
 * Returns:     Zero on success
 */
int dsi_display_splash_res_cleanup(struct  dsi_display *display)
{
	int rc = 0;

	if (!display->is_cont_splash_enabled)
		return 0;

	rc = dsi_display_clk_ctrl(display->dsi_clk_handle,
			DSI_ALL_CLKS, DSI_CLK_OFF);
	if (rc)
		DSI_ERR("[%s] failed to disable DSI link clocks, rc=%d\n",
		       display->name, rc);

	pm_runtime_put_sync(display->drm_dev->dev);

	display->is_cont_splash_enabled = false;
	/* Update splash status for clock manager */
	dsi_display_clk_mngr_update_splash_status(display->clk_mngr,
				display->is_cont_splash_enabled);

	SDE_EVT32(SDE_EVTLOG_FUNC_EXIT, display->is_cont_splash_enabled);
	return rc;
}

static int dsi_display_force_update_dsi_clk(struct dsi_display *display)
{
	int rc = 0;

	rc = dsi_display_link_clk_force_update_ctrl(display->dsi_clk_handle);

	if (!rc) {
		DSI_DEBUG("dsi bit clk has been configured to %d\n",
			display->cached_clk_rate);

		atomic_set(&display->clkrate_change_pending, 0);
	} else {
		DSI_ERR("Failed to configure dsi bit clock '%d'. rc = %d\n",
			display->cached_clk_rate, rc);
	}

	return rc;
}

static int dsi_display_validate_split_link(struct dsi_display *display)
{
	int i, rc = 0;
	struct dsi_display_ctrl *ctrl;
	struct dsi_host_common_cfg *host = &display->panel->host_config;

	if (!host->split_link.split_link_enabled)
		return 0;

	if (display->panel->panel_mode == DSI_OP_CMD_MODE) {
		DSI_ERR("[%s] split link is not supported in command mode\n",
			display->name);
		rc = -ENOTSUPP;
		goto error;
	}

	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		if (!ctrl->ctrl->split_link_supported) {
			DSI_ERR("[%s] split link is not supported by hw\n",
				display->name);
			rc = -ENOTSUPP;
			goto error;
		}

		set_bit(DSI_PHY_SPLIT_LINK, ctrl->phy->hw.feature_map);
	}

	DSI_DEBUG("Split link is enabled\n");
	return 0;

error:
	host->split_link.split_link_enabled = false;
	return rc;
}

static int dsi_display_get_io_resources(struct msm_io_res *io_res, void *data)
{
	int rc = 0;
	struct dsi_display *display;

	if (!data)
		return -EINVAL;

	rc = dsi_ctrl_get_io_resources(io_res);
	if (rc)
		goto end;

	rc = dsi_phy_get_io_resources(io_res);
	if (rc)
		goto end;

	display = (struct dsi_display *)data;
	rc = dsi_panel_get_io_resources(display->panel, io_res);

end:
	return rc;
}

static int dsi_display_pre_release(void *data)
{
	struct dsi_display *display;

	if (!data)
		return -EINVAL;

	display = (struct dsi_display *)data;
	mutex_lock(&display->display_lock);
	display->hw_ownership = false;
	mutex_unlock(&display->display_lock);

	dsi_display_ctrl_irq_update(display, false);

	return 0;
}

static int dsi_display_pre_acquire(void *data)
{
	struct dsi_display *display;

	if (!data)
		return -EINVAL;

	display = (struct dsi_display *)data;
	mutex_lock(&display->display_lock);
	display->hw_ownership = true;
	mutex_unlock(&display->display_lock);

	dsi_display_ctrl_irq_update((struct dsi_display *)data, true);

	return 0;
}

/**
 * dsi_display_bind - bind dsi device with controlling device
 * @dev:        Pointer to base of platform device
 * @master:     Pointer to container of drm device
 * @data:       Pointer to private data
 * Returns:     Zero on success
 */
static int dsi_display_bind(struct device *dev,
		struct device *master,
		void *data)
{
	struct dsi_display_ctrl *display_ctrl;
	struct drm_device *drm;
	struct dsi_display *display;
	struct dsi_clk_info info;
	struct clk_ctrl_cb clk_cb;
	void *handle = NULL;
	struct platform_device *pdev = to_platform_device(dev);
	char *client1 = "dsi_clk_client";
	char *client2 = "mdp_event_client";
	struct msm_vm_ops vm_event_ops = {
		.vm_get_io_resources = dsi_display_get_io_resources,
		.vm_pre_hw_release = dsi_display_pre_release,
		.vm_post_hw_acquire = dsi_display_pre_acquire,
	};
	int i, rc = 0;

	if (!dev || !pdev || !master) {
		DSI_ERR("invalid param(s), dev %pK, pdev %pK, master %pK\n",
				dev, pdev, master);
		return -EINVAL;
	}

	drm = dev_get_drvdata(master);
	display = platform_get_drvdata(pdev);
	if (!drm || !display) {
		DSI_ERR("invalid param(s), drm %pK, display %pK\n",
				drm, display);
		return -EINVAL;
	}
	if (!display->panel_node && !display->fw)
		return 0;

	if (!display->fw)
		display->name = display->panel_node->name;

	/* defer bind if ext bridge driver is not loaded */
	if (display->panel && display->panel->host_config.ext_bridge_mode) {
		for (i = 0; i < display->ext_bridge_cnt; i++) {
			if (!of_drm_find_bridge(
					display->ext_bridge[i].node_of)) {
				DSI_DEBUG("defer for bridge[%d] %s\n", i,
				  display->ext_bridge[i].node_of->full_name);
				return -EPROBE_DEFER;
			}
		}
	}

	mutex_lock(&display->display_lock);

	rc = dsi_display_validate_split_link(display);
	if (rc) {
		DSI_ERR("[%s] split link validation failed, rc=%d\n",
						 display->name, rc);
		goto error;
	}

	rc = dsi_display_debugfs_init(display);
	if (rc) {
		DSI_ERR("[%s] debugfs init failed, rc=%d\n", display->name, rc);
		goto error;
	}

	atomic_set(&display->clkrate_change_pending, 0);
	display->cached_clk_rate = 0;

	memset(&info, 0x0, sizeof(info));

	display_for_each_ctrl(i, display) {
		display_ctrl = &display->ctrl[i];
		rc = dsi_ctrl_drv_init(display_ctrl->ctrl, display->root);
		if (rc) {
			DSI_ERR("[%s] failed to initialize ctrl[%d], rc=%d\n",
			       display->name, i, rc);
			goto error_ctrl_deinit;
		}
		display_ctrl->ctrl->horiz_index = i;

		rc = dsi_phy_drv_init(display_ctrl->phy);
		if (rc) {
			DSI_ERR("[%s] Failed to initialize phy[%d], rc=%d\n",
				display->name, i, rc);
			(void)dsi_ctrl_drv_deinit(display_ctrl->ctrl);
			goto error_ctrl_deinit;
		}

		display_ctrl->ctrl->dma_cmd_workq = display->dma_cmd_workq;
		memcpy(&info.c_clks[i],
				(&display_ctrl->ctrl->clk_info.core_clks),
				sizeof(struct dsi_core_clk_info));
		memcpy(&info.l_hs_clks[i],
				(&display_ctrl->ctrl->clk_info.hs_link_clks),
				sizeof(struct dsi_link_hs_clk_info));
		memcpy(&info.l_lp_clks[i],
				(&display_ctrl->ctrl->clk_info.lp_link_clks),
				sizeof(struct dsi_link_lp_clk_info));

		info.c_clks[i].drm = drm;
		info.ctrl_index[i] = display_ctrl->ctrl->cell_index;
	}

	info.pre_clkoff_cb = dsi_pre_clkoff_cb;
	info.pre_clkon_cb = dsi_pre_clkon_cb;
	info.post_clkoff_cb = dsi_post_clkoff_cb;
	info.post_clkon_cb = dsi_post_clkon_cb;
	info.priv_data = display;
	info.master_ndx = display->clk_master_idx;
	info.dsi_ctrl_count = display->ctrl_count;
	snprintf(info.name, MAX_STRING_LEN,
			"DSI_MNGR-%s", display->name);

	display->clk_mngr = dsi_display_clk_mngr_register(&info);
	if (IS_ERR_OR_NULL(display->clk_mngr)) {
		rc = PTR_ERR(display->clk_mngr);
		display->clk_mngr = NULL;
		DSI_ERR("dsi clock registration failed, rc = %d\n", rc);
		goto error_ctrl_deinit;
	}

	handle = dsi_register_clk_handle(display->clk_mngr, client1);
	if (IS_ERR_OR_NULL(handle)) {
		rc = PTR_ERR(handle);
		DSI_ERR("failed to register %s client, rc = %d\n",
		       client1, rc);
		goto error_clk_deinit;
	} else {
		display->dsi_clk_handle = handle;
	}

	handle = dsi_register_clk_handle(display->clk_mngr, client2);
	if (IS_ERR_OR_NULL(handle)) {
		rc = PTR_ERR(handle);
		DSI_ERR("failed to register %s client, rc = %d\n",
		       client2, rc);
		goto error_clk_client_deinit;
	} else {
		display->mdp_clk_handle = handle;
	}

	clk_cb.priv = display;
	clk_cb.dsi_clk_cb = dsi_display_clk_ctrl_cb;

	display_for_each_ctrl(i, display) {
		display_ctrl = &display->ctrl[i];

		rc = dsi_ctrl_clk_cb_register(display_ctrl->ctrl, &clk_cb);
		if (rc) {
			DSI_ERR("[%s] failed to register ctrl clk_cb[%d], rc=%d\n",
			       display->name, i, rc);
			goto error_ctrl_deinit;
		}

		rc = dsi_phy_clk_cb_register(display_ctrl->phy, &clk_cb);
		if (rc) {
			DSI_ERR("[%s] failed to register phy clk_cb[%d], rc=%d\n",
			       display->name, i, rc);
			goto error_ctrl_deinit;
		}
	}

	dsi_display_update_byte_intf_div(display);
	rc = dsi_display_mipi_host_init(display);
	if (rc) {
		DSI_ERR("[%s] failed to initialize mipi host, rc=%d\n",
		       display->name, rc);
		goto error_ctrl_deinit;
	}

	rc = dsi_panel_drv_init(display->panel, &display->host);
	if (rc) {
		if (rc != -EPROBE_DEFER)
			DSI_ERR("[%s] failed to initialize panel driver, rc=%d\n",
			       display->name, rc);
		goto error_host_deinit;
	}

	DSI_INFO("Successfully bind display panel '%s'\n", display->name);
	dsi_display_is_probed(display, 0);
	display->drm_dev = drm;

	display_for_each_ctrl(i, display) {
		display_ctrl = &display->ctrl[i];
		if (!display_ctrl->phy || !display_ctrl->ctrl)
			continue;

		display_ctrl->ctrl->drm_dev = drm;
		rc = dsi_phy_set_clk_freq(display_ctrl->phy,
				&display_ctrl->ctrl->clk_freq);
		if (rc) {
			DSI_ERR("[%s] failed to set phy clk freq, rc=%d\n",
					display->name, rc);
			goto error;
		}
	}

	/* register te irq handler */
	dsi_display_register_te_irq(display);

	msm_register_vm_event(master, dev, &vm_event_ops, (void *)display);

	goto error;

error_host_deinit:
	(void)dsi_display_mipi_host_deinit(display);
error_clk_client_deinit:
	(void)dsi_deregister_clk_handle(display->dsi_clk_handle);
error_clk_deinit:
	(void)dsi_display_clk_mngr_deregister(display->clk_mngr);
error_ctrl_deinit:
	for (i = i - 1; i >= 0; i--) {
		display_ctrl = &display->ctrl[i];
		(void)dsi_phy_drv_deinit(display_ctrl->phy);
		(void)dsi_ctrl_drv_deinit(display_ctrl->ctrl);
		dsi_ctrl_put(display_ctrl->ctrl);
		dsi_phy_put(display_ctrl->phy);
	}
	(void)dsi_display_debugfs_deinit(display);
error:
	mutex_unlock(&display->display_lock);
	return rc;
}

/**
 * dsi_display_unbind - unbind dsi from controlling device
 * @dev:        Pointer to base of platform device
 * @master:     Pointer to container of drm device
 * @data:       Pointer to private data
 */
static void dsi_display_unbind(struct device *dev,
		struct device *master, void *data)
{
	struct dsi_display_ctrl *display_ctrl;
	struct dsi_display *display;
	struct platform_device *pdev = to_platform_device(dev);
	int i, rc = 0;

	if (!dev || !pdev || !master) {
		DSI_ERR("invalid param(s)\n");
		return;
	}

	display = platform_get_drvdata(pdev);
	if (!display || !display->panel_node) {
		DSI_ERR("invalid display\n");
		return;
	}

	mutex_lock(&display->display_lock);

	rc = dsi_display_mipi_host_deinit(display);
	if (rc)
		DSI_ERR("[%s] failed to deinit mipi hosts, rc=%d\n",
		       display->name,
		       rc);

	display_for_each_ctrl(i, display) {
		display_ctrl = &display->ctrl[i];

		rc = dsi_phy_drv_deinit(display_ctrl->phy);
		if (rc)
			DSI_ERR("[%s] failed to deinit phy%d driver, rc=%d\n",
			       display->name, i, rc);

		display->ctrl->ctrl->dma_cmd_workq = NULL;
		rc = dsi_ctrl_drv_deinit(display_ctrl->ctrl);
		if (rc)
			DSI_ERR("[%s] failed to deinit ctrl%d driver, rc=%d\n",
			       display->name, i, rc);
	}

	atomic_set(&display->clkrate_change_pending, 0);
	(void)dsi_display_debugfs_deinit(display);

	mutex_unlock(&display->display_lock);
}

static const struct component_ops dsi_display_comp_ops = {
	.bind = dsi_display_bind,
	.unbind = dsi_display_unbind,
};

static struct platform_driver dsi_display_driver = {
	.probe = dsi_display_dev_probe,
	.remove = dsi_display_dev_remove,
	.driver = {
		.name = "msm-dsi-display",
		.of_match_table = dsi_display_dt_match,
		.suppress_bind_attrs = true,
	},
	.shutdown = dsi_display_dev_shutdown,
};

void dsi_display_dev_shutdown(struct platform_device *pdev)
{
	int rc = 0;
	struct dsi_display *display;
	struct dsi_panel *panel;

	if (!pdev) {
		DSI_ERR("Invalid device\n");
		return;
	}

	display = platform_get_drvdata(pdev);

	if (display == NULL || display->panel == NULL) {
		DSI_ERR("Invalid device\n");
		return;
	}

	panel = display->panel;

	if (!panel->need_execute_shutdown) {
		return;
	}

	if (gpio_is_valid(panel->reset_config.disp_en_gpio))
		gpio_set_value(panel->reset_config.disp_en_gpio, 0);

	if (gpio_is_valid(panel->reset_config.reset_gpio))
		gpio_set_value(panel->reset_config.reset_gpio, 0);

	if (gpio_is_valid(panel->reset_config.lcd_mode_sel_gpio))
		gpio_set_value(panel->reset_config.lcd_mode_sel_gpio, 0);

	if (gpio_is_valid(panel->panel_test_gpio)) {
		rc = gpio_direction_input(panel->panel_test_gpio);
		if (rc)
			DSI_WARN("set dir for panel test gpio failed rc=%d\n", rc);
	}

	rc = dsi_pwr_enable_regulator(&panel->power_info, false);
	if (rc)
		DSI_ERR("[%s] failed to enable vregs, rc=%d\n", panel->name, rc);

}

static int dsi_display_init(struct dsi_display *display)
{
	int rc = 0;
	struct platform_device *pdev = display->pdev;

	mutex_init(&display->display_lock);

	rc = _dsi_display_dev_init(display);
	if (rc) {
		DSI_ERR("device init failed, rc=%d\n", rc);
		goto end;
	}

	/*
	 * Vote on panel regulator is added to make sure panel regulators
	 * are ON for cont-splash enabled usecase.
	 * This panel regulator vote will be removed only in:
	 *	1) device suspend when cont-splash is enabled.
	 *	2) cont_splash_res_disable() when cont-splash is disabled.
	 * For GKI, adding this vote will make sure that sync_state
	 * kernel driver doesn't disable the panel regulators after
	 * dsi probe is complete.
	 */
	if (display->panel) {
		rc = dsi_pwr_enable_regulator(&display->panel->power_info,
								true);
		if (rc) {
			DSI_ERR("[%s] failed to enable vregs, rc=%d\n",
					display->panel->name, rc);
			return rc;
		}
	}

	rc = component_add(&pdev->dev, &dsi_display_comp_ops);
	if (rc)
		DSI_ERR("component add failed, rc=%d\n", rc);

	DSI_DEBUG("component add success: %s\n", display->name);
end:
	return rc;
}

static void dsi_display_firmware_display(const struct firmware *fw,
				void *context)
{
	struct dsi_display *display = context;

	if (fw) {
		DSI_INFO("reading data from firmware, size=%zd\n",
			fw->size);

		display->fw = fw;

		if (!strcmp(display->display_type, "primary"))
			display->name = "dsi_firmware_display";

		else if (!strcmp(display->display_type, "secondary"))
			display->name = "dsi_firmware_display_secondary";

	} else {
		DSI_INFO("no firmware available, fallback to device node\n");
	}

	if (dsi_display_init(display))
		return;

	DSI_DEBUG("success\n");
}

/*
	Add /sys/class/drm path, other module can get panel info from this path.
*/
struct dsi_display *display_panel = NULL;
static ssize_t panelId_show(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	int written = 0;

	mutex_lock(&display_panel->drm_conn->dev->mode_config.mutex);
	written = snprintf(buf, PAGE_SIZE, "0x%016llx\n",
	display_panel->panel->panel_id);
	mutex_unlock(&display_panel->drm_conn->dev->mode_config.mutex);

	return written;
}

static ssize_t panelVer_show(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	int written = 0;

	mutex_lock(&display_panel->drm_conn->dev->mode_config.mutex);
	written = snprintf(buf, PAGE_SIZE, "0x%016llx\n",
	display_panel->panel->panel_ver);
	mutex_unlock(&display_panel->drm_conn->dev->mode_config.mutex);

	return written;
}

static ssize_t panelName_show(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	int written = 0;

	mutex_lock(&display_panel->drm_conn->dev->mode_config.mutex);
	written = snprintf(buf, PAGE_SIZE, "%s\n", display_panel->panel->panel_name);
	mutex_unlock(&display_panel->drm_conn->dev->mode_config.mutex);

	return written;
}

static ssize_t panelRegDA_show(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	int written = 0;

	mutex_lock(&display_panel->drm_conn->dev->mode_config.mutex);
	written = snprintf(buf, PAGE_SIZE, "0x%02x\n", display_panel->panel->panel_regDA);
	mutex_unlock(&display_panel->drm_conn->dev->mode_config.mutex);

	return written;
}

static ssize_t panelSupplier_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	int written = 0;

	mutex_lock(&display_panel->drm_conn->dev->mode_config.mutex);
	written = scnprintf(buf, PAGE_SIZE, display_panel->panel->panel_supplier);
	mutex_unlock(&display_panel->drm_conn->dev->mode_config.mutex);

	return written;
}

static struct device_attribute panel_attributes[] = {
	__ATTR_RO(panelId),
	__ATTR_RO(panelVer),
	__ATTR_RO(panelName),
	__ATTR_RO(panelRegDA),
	__ATTR_RO(panelSupplier),
	__ATTR_NULL
};

#define DRM_RETRY_TIMES 3
static int panel_class_create(struct platform_device *pdev)
{
	struct device_attribute *attrs = panel_attributes;
	int i, j, error = 0;

	display_panel = platform_get_drvdata(pdev);

	for (j = 0; j < DRM_RETRY_TIMES; j++) {
		if (display_panel->drm_conn->kdev) {
			for (i = 0; attrs[i].attr.name != NULL; ++i) {
				error = device_create_file(display_panel->drm_conn->kdev, &attrs[i]);
				if (error)
					break;
			}
		} else {
			DSI_ERR("drm_conn->kdev is NULL, retry %d times\n", j);
		}
	}

	if (error)
		goto device_destroy;

	return 0;

device_destroy:
	for (--i; i >= 0; --i)
		device_remove_file(display_panel->drm_conn->kdev, &attrs[i]);
	DSI_ERR("creating panel class failed\n");

	return -ENODEV;
}

int dsi_display_dev_probe(struct platform_device *pdev)
{
	struct dsi_display *display = NULL;
	struct device_node *node = NULL, *panel_node = NULL, *mdp_node = NULL;
	int rc = 0, index = DSI_PRIMARY;
	bool firm_req = false;
	struct dsi_display_boot_param *boot_disp;

	if (!pdev || !pdev->dev.of_node) {
		DSI_ERR("pdev not found\n");
		rc = -ENODEV;
		goto end;
	}

	display = devm_kzalloc(&pdev->dev, sizeof(*display), GFP_KERNEL);
	if (!display) {
		rc = -ENOMEM;
		goto end;
	}

	display->dma_cmd_workq = create_singlethread_workqueue(
			"dsi_dma_cmd_workq");
	if (!display->dma_cmd_workq)  {
		DSI_ERR("failed to create work queue\n");
		rc =  -EINVAL;
		goto end;
	}

	mdp_node = of_parse_phandle(pdev->dev.of_node, "qcom,mdp", 0);
	if (!mdp_node) {
		DSI_ERR("mdp_node not found\n");
		rc = -ENODEV;
		goto end;
	}

	display->trusted_vm_env = of_property_read_bool(mdp_node,
						"qcom,sde-trusted-vm-env");
	if (display->trusted_vm_env)
		DSI_INFO("Display enabled with trusted vm path\n");

	/* initialize panel id to UINT64_MAX */
	display->panel_id = ~0x0;

	display->display_type = of_get_property(pdev->dev.of_node,
				"label", NULL);
	if (!display->display_type)
		display->display_type = "primary";

	if (!strcmp(display->display_type, "secondary"))
		index = DSI_SECONDARY;

	boot_disp = &boot_displays[index];
	node = pdev->dev.of_node;
	if (boot_disp->boot_disp_en) {
		/* The panel name should be same as UEFI name index */
		panel_node = of_find_node_by_name(mdp_node, boot_disp->name);
		if (!panel_node)
			DSI_WARN("panel_node %s not found\n", boot_disp->name);
	} else {
		panel_node = of_parse_phandle(node,
				"qcom,dsi-default-panel", 0);
		if (!panel_node)
			DSI_WARN("default panel not found\n");
	}

	boot_disp->node = pdev->dev.of_node;
	boot_disp->disp = display;

	display->panel_node = panel_node;
	display->pdev = pdev;
	display->boot_disp = boot_disp;

	dsi_display_parse_cmdline_topology(display, index);

	platform_set_drvdata(pdev, display);

	/* initialize display in firmware callback */
	if (!(boot_displays[DSI_PRIMARY].boot_disp_en ||
			boot_displays[DSI_SECONDARY].boot_disp_en) &&
			IS_ENABLED(CONFIG_DSI_PARSER) &&
			!display->trusted_vm_env) {
		if (!strcmp(display->display_type, "primary"))
			firm_req = !request_firmware_nowait(
				THIS_MODULE, 1, "dsi_prop",
				&pdev->dev, GFP_KERNEL, display,
				dsi_display_firmware_display);

		else if (!strcmp(display->display_type, "secondary"))
			firm_req = !request_firmware_nowait(
				THIS_MODULE, 1, "dsi_prop_sec",
				&pdev->dev, GFP_KERNEL, display,
				dsi_display_firmware_display);
	}

	if (!firm_req) {
		rc = dsi_display_init(display);
		if (rc)
			goto end;
	}

	panel_class_create(pdev);

	return 0;
end:
	if (display)
		devm_kfree(&pdev->dev, display);

	return rc;
}

int dsi_display_dev_remove(struct platform_device *pdev)
{
	int rc = 0, i = 0;
	struct dsi_display *display;
	struct dsi_display_ctrl *ctrl;

	if (!pdev) {
		DSI_ERR("Invalid device\n");
		return -EINVAL;
	}

	display = platform_get_drvdata(pdev);
	if (!display || !display->panel_node) {
		DSI_ERR("invalid display\n");
		return -EINVAL;
	}

	/* decrement ref count */
	of_node_put(display->panel_node);

	if (display->dma_cmd_workq) {
		flush_workqueue(display->dma_cmd_workq);
		destroy_workqueue(display->dma_cmd_workq);
		display->dma_cmd_workq = NULL;
		display_for_each_ctrl(i, display) {
			ctrl = &display->ctrl[i];
			if (!ctrl->ctrl)
				continue;
			ctrl->ctrl->dma_cmd_workq = NULL;
		}
	}

	(void)_dsi_display_dev_deinit(display);

	platform_set_drvdata(pdev, NULL);
	devm_kfree(&pdev->dev, display);
	return rc;
}

int dsi_display_get_num_of_displays(void)
{
	int i, count = 0;

	for (i = 0; i < MAX_DSI_ACTIVE_DISPLAY; i++) {
		struct dsi_display *display = boot_displays[i].disp;

		if ((display && display->panel_node) ||
					(display && display->fw))
			count++;
	}

	return count;
}

bool dsi_display_all_displays_dead(void)
{
	int index =0, num_displays =0, num_dead_displays =0;

	for (index = 0; index < MAX_DSI_ACTIVE_DISPLAY; index++) {
		struct dsi_display *display = boot_displays[index].disp;

		if (display && display->panel_node) {
			num_displays++;

			if (display->panel->is_panel_dead)
				num_dead_displays++;
		}
	}

	pr_info("%d Display Dead(%d)\n", num_dead_displays, num_displays);

	return num_dead_displays == num_displays;
}
EXPORT_SYMBOL(dsi_display_all_displays_dead);

int dsi_display_get_active_displays(void **display_array, u32 max_display_count)
{
	int index = 0, count = 0;

	if (!display_array || !max_display_count) {
		DSI_ERR("invalid params\n");
		return 0;
	}

	for (index = 0; index < MAX_DSI_ACTIVE_DISPLAY; index++) {
		struct dsi_display *display = boot_displays[index].disp;

		if ((display && display->panel_node) ||
					(display && display->fw))
			display_array[count++] = display;
	}

	return count;
}

void dsi_display_set_active_state(struct dsi_display *display, bool is_active)
{
	if (!display)
		return;

	mutex_lock(&display->display_lock);
	display->is_active = is_active;
	mutex_unlock(&display->display_lock);
}

int dsi_display_drm_bridge_init(struct dsi_display *display,
		struct drm_encoder *enc)
{
	int rc = 0;
	struct dsi_bridge *bridge;
	struct msm_drm_private *priv = NULL;

	if (!display || !display->drm_dev || !enc) {
		DSI_ERR("invalid param(s)\n");
		return -EINVAL;
	}

	mutex_lock(&display->display_lock);
	priv = display->drm_dev->dev_private;

	if (!priv) {
		DSI_ERR("Private data is not present\n");
		rc = -EINVAL;
		goto error;
	}

	if (display->bridge) {
		DSI_ERR("display is already initialize\n");
		goto error;
	}

	bridge = dsi_drm_bridge_init(display, display->drm_dev, enc);
	if (IS_ERR_OR_NULL(bridge)) {
		rc = PTR_ERR(bridge);
		DSI_ERR("[%s] brige init failed, %d\n", display->name, rc);
		goto error;
	}

	display->bridge = bridge;
	priv->bridges[priv->num_bridges++] = &bridge->base;

	if (display->tx_cmd_buf == NULL) {
		rc = dsi_host_alloc_cmd_tx_buffer(display);
		if (rc)
			DSI_ERR("failed to allocate cmd tx buffer memory\n");
	}

error:
	mutex_unlock(&display->display_lock);
	return rc;
}

int dsi_display_drm_bridge_deinit(struct dsi_display *display)
{
	int rc = 0;

	if (!display) {
		DSI_ERR("Invalid params\n");
		return -EINVAL;
	}

	mutex_lock(&display->display_lock);

	dsi_drm_bridge_cleanup(display->bridge);
	display->bridge = NULL;

	mutex_unlock(&display->display_lock);
	return rc;
}

/* Hook functions to call external connector, pointer validation is
 * done in dsi_display_drm_ext_bridge_init.
 */
static enum drm_connector_status dsi_display_drm_ext_detect(
		struct drm_connector *connector,
		bool force,
		void *disp)
{
	struct dsi_display *display = disp;

	return display->ext_conn->funcs->detect(display->ext_conn, force);
}

static int dsi_display_drm_ext_get_modes(
		struct drm_connector *connector, void *disp,
		const struct msm_resource_caps_info *avail_res)
{
	struct dsi_display *display = disp;
	struct drm_display_mode *pmode, *pt;
	int count;

	/* if there are modes defined in panel, ignore external modes */
	if (display->panel->num_timing_nodes)
		return dsi_connector_get_modes(connector, disp, avail_res);

	count = display->ext_conn->helper_private->get_modes(
			display->ext_conn);

	list_for_each_entry_safe(pmode, pt,
			&display->ext_conn->probed_modes, head) {
		list_move_tail(&pmode->head, &connector->probed_modes);
	}

	connector->display_info = display->ext_conn->display_info;

	return count;
}

static enum drm_mode_status dsi_display_drm_ext_mode_valid(
		struct drm_connector *connector,
		struct drm_display_mode *mode,
		void *disp, const struct msm_resource_caps_info *avail_res)
{
	struct dsi_display *display = disp;
	enum drm_mode_status status;

	/* always do internal mode_valid check */
	status = dsi_conn_mode_valid(connector, mode, disp, avail_res);
	if (status != MODE_OK)
		return status;

	return display->ext_conn->helper_private->mode_valid(
			display->ext_conn, mode);
}

static int dsi_display_drm_ext_atomic_check(struct drm_connector *connector,
		void *disp,
		struct drm_atomic_state *state)
{
	struct dsi_display *display = disp;
	struct drm_connector_state *c_state;

	c_state = drm_atomic_get_new_connector_state(state, connector);

	return display->ext_conn->helper_private->atomic_check(
			display->ext_conn, state);
}

static int dsi_display_ext_get_info(struct drm_connector *connector,
	struct msm_display_info *info, void *disp)
{
	struct dsi_display *display;
	int i;

	if (!info || !disp) {
		DSI_ERR("invalid params\n");
		return -EINVAL;
	}

	display = disp;
	if (!display->panel) {
		DSI_ERR("invalid display panel\n");
		return -EINVAL;
	}

	if (display->panel->num_timing_nodes)
		return dsi_display_get_info(connector, info, disp);

	mutex_lock(&display->display_lock);

	memset(info, 0, sizeof(struct msm_display_info));

	info->intf_type = DRM_MODE_CONNECTOR_DSI;
	info->num_of_h_tiles = display->ctrl_count;
	for (i = 0; i < info->num_of_h_tiles; i++)
		info->h_tile_instance[i] = display->ctrl[i].ctrl->cell_index;

	info->is_connected = connector->status != connector_status_disconnected;

	if (!strcmp(display->display_type, "primary"))
		info->display_type = SDE_CONNECTOR_PRIMARY;
	else if (!strcmp(display->display_type, "secondary"))
		info->display_type = SDE_CONNECTOR_SECONDARY;

	info->capabilities |= (MSM_DISPLAY_CAP_VID_MODE |
			MSM_DISPLAY_CAP_EDID | MSM_DISPLAY_CAP_HOT_PLUG);
	info->curr_panel_mode = MSM_DISPLAY_VIDEO_MODE;

	mutex_unlock(&display->display_lock);
	return 0;
}

static int dsi_display_ext_get_mode_info(struct drm_connector *connector,
	const struct drm_display_mode *drm_mode,
	struct msm_mode_info *mode_info,
	void *display, const struct msm_resource_caps_info *avail_res)
{
	struct msm_display_topology *topology;
	struct dsi_display *ext_display = (struct dsi_display *)display;

	if (!drm_mode || !mode_info ||
			!avail_res || !avail_res->max_mixer_width)
		return -EINVAL;

	if (ext_display->panel->num_timing_nodes)
		return dsi_conn_get_mode_info(connector, drm_mode,
			mode_info, display, avail_res);

	memset(mode_info, 0, sizeof(*mode_info));
	mode_info->frame_rate = drm_mode->vrefresh;
	mode_info->vtotal = drm_mode->vtotal;
	mode_info->comp_info.comp_type = MSM_DISPLAY_COMPRESSION_NONE;

	topology = &mode_info->topology;
	topology->num_lm = ext_display->ctrl_count;

	topology->num_enc = 0;
	topology->num_intf = topology->num_lm;

	return 0;
}

static struct dsi_display_ext_bridge *dsi_display_ext_get_bridge(
		struct drm_bridge *bridge)
{
	struct msm_drm_private *priv;
	struct sde_kms *sde_kms;
	struct drm_connector *conn;
	struct drm_connector_list_iter conn_iter;
	struct sde_connector *sde_conn;
	struct dsi_display *display;
	struct dsi_display_ext_bridge *dsi_bridge = NULL;
	int i;

	if (!bridge || !bridge->encoder) {
		SDE_ERROR("invalid argument\n");
		return NULL;
	}

	priv = bridge->dev->dev_private;
	sde_kms = to_sde_kms(priv->kms);

	drm_connector_list_iter_begin(sde_kms->dev, &conn_iter);
	drm_for_each_connector_iter(conn, &conn_iter) {
		sde_conn = to_sde_connector(conn);
		if (sde_conn->encoder == bridge->encoder) {
			display = sde_conn->display;
			display_for_each_ctrl(i, display) {
				if (display->ext_bridge[i].bridge == bridge) {
					dsi_bridge = &display->ext_bridge[i];
					break;
				}
			}
		}
	}
	drm_connector_list_iter_end(&conn_iter);

	return dsi_bridge;
}

static void dsi_display_drm_ext_adjust_timing(
		const struct dsi_display *display,
		struct drm_display_mode *mode)
{
	mode->hdisplay /= display->ctrl_count;
	mode->hsync_start /= display->ctrl_count;
	mode->hsync_end /= display->ctrl_count;
	mode->htotal /= display->ctrl_count;
	mode->hskew /= display->ctrl_count;
	mode->clock /= display->ctrl_count;
}

static enum drm_mode_status dsi_display_drm_ext_bridge_mode_valid(
		struct drm_bridge *bridge,
		const struct drm_display_mode *mode)
{
	struct dsi_display_ext_bridge *ext_bridge;
	struct drm_display_mode tmp;

	ext_bridge = dsi_display_ext_get_bridge(bridge);
	if (!ext_bridge)
		return MODE_ERROR;

	tmp = *mode;
	dsi_display_drm_ext_adjust_timing(ext_bridge->display, &tmp);
	return ext_bridge->orig_funcs->mode_valid(bridge, &tmp);
}

static bool dsi_display_drm_ext_bridge_mode_fixup(
		struct drm_bridge *bridge,
		const struct drm_display_mode *mode,
		struct drm_display_mode *adjusted_mode)
{
	struct dsi_display_ext_bridge *ext_bridge;
	struct drm_display_mode tmp;

	ext_bridge = dsi_display_ext_get_bridge(bridge);
	if (!ext_bridge)
		return false;

	tmp = *mode;
	dsi_display_drm_ext_adjust_timing(ext_bridge->display, &tmp);
	return ext_bridge->orig_funcs->mode_fixup(bridge, &tmp, &tmp);
}

static void dsi_display_drm_ext_bridge_mode_set(
		struct drm_bridge *bridge,
		const struct drm_display_mode *mode,
		const struct drm_display_mode *adjusted_mode)
{
	struct dsi_display_ext_bridge *ext_bridge;
	struct drm_display_mode tmp;

	ext_bridge = dsi_display_ext_get_bridge(bridge);
	if (!ext_bridge)
		return;

	tmp = *mode;
	dsi_display_drm_ext_adjust_timing(ext_bridge->display, &tmp);
	ext_bridge->orig_funcs->mode_set(bridge, &tmp, &tmp);
}

static int dsi_host_ext_attach(struct mipi_dsi_host *host,
			   struct mipi_dsi_device *dsi)
{
	struct dsi_display *display = to_dsi_display(host);
	struct dsi_panel *panel;

	if (!host || !dsi || !display->panel) {
		DSI_ERR("Invalid param\n");
		return -EINVAL;
	}

	DSI_DEBUG("DSI[%s]: channel=%d, lanes=%d, format=%d, mode_flags=%lx\n",
		dsi->name, dsi->channel, dsi->lanes,
		dsi->format, dsi->mode_flags);

	panel = display->panel;
	panel->host_config.data_lanes = 0;
	if (dsi->lanes > 0)
		panel->host_config.data_lanes |= DSI_DATA_LANE_0;
	if (dsi->lanes > 1)
		panel->host_config.data_lanes |= DSI_DATA_LANE_1;
	if (dsi->lanes > 2)
		panel->host_config.data_lanes |= DSI_DATA_LANE_2;
	if (dsi->lanes > 3)
		panel->host_config.data_lanes |= DSI_DATA_LANE_3;

	switch (dsi->format) {
	case MIPI_DSI_FMT_RGB888:
		panel->host_config.dst_format = DSI_PIXEL_FORMAT_RGB888;
		break;
	case MIPI_DSI_FMT_RGB666:
		panel->host_config.dst_format = DSI_PIXEL_FORMAT_RGB666_LOOSE;
		break;
	case MIPI_DSI_FMT_RGB666_PACKED:
		panel->host_config.dst_format = DSI_PIXEL_FORMAT_RGB666;
		break;
	case MIPI_DSI_FMT_RGB565:
	default:
		panel->host_config.dst_format = DSI_PIXEL_FORMAT_RGB565;
		break;
	}

	if (dsi->mode_flags & MIPI_DSI_MODE_VIDEO) {
		panel->panel_mode = DSI_OP_VIDEO_MODE;

		if (dsi->mode_flags & MIPI_DSI_MODE_VIDEO_BURST)
			panel->video_config.traffic_mode =
					DSI_VIDEO_TRAFFIC_BURST_MODE;
		else if (dsi->mode_flags & MIPI_DSI_MODE_VIDEO_SYNC_PULSE)
			panel->video_config.traffic_mode =
					DSI_VIDEO_TRAFFIC_SYNC_PULSES;
		else
			panel->video_config.traffic_mode =
					DSI_VIDEO_TRAFFIC_SYNC_START_EVENTS;

		panel->video_config.hsa_lp11_en =
			dsi->mode_flags & MIPI_DSI_MODE_VIDEO_HSA;
		panel->video_config.hbp_lp11_en =
			dsi->mode_flags & MIPI_DSI_MODE_VIDEO_HBP;
		panel->video_config.hfp_lp11_en =
			dsi->mode_flags & MIPI_DSI_MODE_VIDEO_HFP;
		panel->video_config.pulse_mode_hsa_he =
			dsi->mode_flags & MIPI_DSI_MODE_VIDEO_HSE;
		panel->video_config.bllp_lp11_en =
			dsi->mode_flags & MIPI_DSI_MODE_VIDEO_BLLP;
		panel->video_config.eof_bllp_lp11_en =
			dsi->mode_flags & MIPI_DSI_MODE_VIDEO_EOF_BLLP;
	} else {
		panel->panel_mode = DSI_OP_CMD_MODE;
		DSI_ERR("command mode not supported by ext bridge\n");
		return -ENOTSUPP;
	}

	panel->bl_config.type = DSI_BACKLIGHT_UNKNOWN;

	return 0;
}

static struct mipi_dsi_host_ops dsi_host_ext_ops = {
	.attach = dsi_host_ext_attach,
	.detach = dsi_host_detach,
	.transfer = dsi_host_transfer,
};

struct drm_panel *dsi_display_get_drm_panel(struct dsi_display *display)
{
	if (!display || !display->panel) {
		pr_err("invalid param(s)\n");
		return NULL;
	}

	return &display->panel->drm_panel;
}

int dsi_display_drm_ext_bridge_init(struct dsi_display *display,
		struct drm_encoder *encoder, struct drm_connector *connector)
{
	struct drm_device *drm;
	struct drm_bridge *bridge;
	struct drm_bridge *ext_bridge;
	struct drm_connector *ext_conn;
	struct sde_connector *sde_conn;
	struct drm_bridge *prev_bridge;
	int rc = 0, i;

	if (!display || !encoder || !connector)
		return -EINVAL;

	drm = encoder->dev;
	bridge = encoder->bridge;
	sde_conn = to_sde_connector(connector);
	prev_bridge = bridge;

	if (display->panel && !display->panel->host_config.ext_bridge_mode)
		return 0;

	for (i = 0; i < display->ext_bridge_cnt; i++) {
		struct dsi_display_ext_bridge *ext_bridge_info =
				&display->ext_bridge[i];

		/* return if ext bridge is already initialized */
		if (ext_bridge_info->bridge)
			return 0;

		ext_bridge = of_drm_find_bridge(ext_bridge_info->node_of);
		if (IS_ERR_OR_NULL(ext_bridge)) {
			rc = PTR_ERR(ext_bridge);
			DSI_ERR("failed to find ext bridge\n");
			goto error;
		}

		/* override functions for mode adjustment */
		if (display->ext_bridge_cnt > 1) {
			ext_bridge_info->bridge_funcs = *ext_bridge->funcs;
			if (ext_bridge->funcs->mode_fixup)
				ext_bridge_info->bridge_funcs.mode_fixup =
					dsi_display_drm_ext_bridge_mode_fixup;
			if (ext_bridge->funcs->mode_valid)
				ext_bridge_info->bridge_funcs.mode_valid =
					dsi_display_drm_ext_bridge_mode_valid;
			if (ext_bridge->funcs->mode_set)
				ext_bridge_info->bridge_funcs.mode_set =
					dsi_display_drm_ext_bridge_mode_set;
			ext_bridge_info->orig_funcs = ext_bridge->funcs;
			ext_bridge->funcs = &ext_bridge_info->bridge_funcs;
		}

		rc = drm_bridge_attach(encoder, ext_bridge, prev_bridge);
		if (rc) {
			DSI_ERR("[%s] ext brige attach failed, %d\n",
				display->name, rc);
			goto error;
		}

		ext_bridge_info->display = display;
		ext_bridge_info->bridge = ext_bridge;
		prev_bridge = ext_bridge;

		/* ext bridge will init its own connector during attach,
		 * we need to extract it out of the connector list
		 */
		spin_lock_irq(&drm->mode_config.connector_list_lock);
		ext_conn = list_last_entry(&drm->mode_config.connector_list,
			struct drm_connector, head);
		if (ext_conn && ext_conn != connector &&
			ext_conn->encoder_ids[0] == bridge->encoder->base.id) {
			list_del_init(&ext_conn->head);
			display->ext_conn = ext_conn;
		}
		spin_unlock_irq(&drm->mode_config.connector_list_lock);

		/* if there is no valid external connector created, or in split
		 * mode, default setting is used from panel defined in DT file.
		 */
		if (!display->ext_conn ||
		    !display->ext_conn->funcs ||
		    !display->ext_conn->helper_private ||
		    display->ext_bridge_cnt > 1) {
			display->ext_conn = NULL;
			continue;
		}

		/* otherwise, hook up the functions to use external connector */
		if (display->ext_conn->funcs->detect)
			sde_conn->ops.detect = dsi_display_drm_ext_detect;

		if (display->ext_conn->helper_private->get_modes)
			sde_conn->ops.get_modes =
				dsi_display_drm_ext_get_modes;

		if (display->ext_conn->helper_private->mode_valid)
			sde_conn->ops.mode_valid =
				dsi_display_drm_ext_mode_valid;

		if (display->ext_conn->helper_private->atomic_check)
			sde_conn->ops.atomic_check =
				dsi_display_drm_ext_atomic_check;

		sde_conn->ops.get_info =
				dsi_display_ext_get_info;
		sde_conn->ops.get_mode_info =
				dsi_display_ext_get_mode_info;

		/* add support to attach/detach */
		display->host.ops = &dsi_host_ext_ops;
	}

	return 0;
error:
	return rc;
}

int dsi_display_get_info(struct drm_connector *connector,
		struct msm_display_info *info, void *disp)
{
	struct dsi_display *display;
	struct dsi_panel_phy_props phy_props;
	struct dsi_host_common_cfg *host;
	int i, rc;

	if (!info || !disp) {
		DSI_ERR("invalid params\n");
		return -EINVAL;
	}

	display = disp;
	if (!display->panel) {
		DSI_ERR("invalid display panel\n");
		return -EINVAL;
	}

	mutex_lock(&display->display_lock);
	rc = dsi_panel_get_phy_props(display->panel, &phy_props);
	if (rc) {
		DSI_ERR("[%s] failed to get panel phy props, rc=%d\n",
		       display->name, rc);
		goto error;
	}

	memset(info, 0, sizeof(struct msm_display_info));
	info->intf_type = DRM_MODE_CONNECTOR_DSI;
	info->num_of_h_tiles = display->ctrl_count;
	for (i = 0; i < info->num_of_h_tiles; i++)
		info->h_tile_instance[i] = display->ctrl[i].ctrl->cell_index;

	info->is_connected = display->is_active;

	if (!strcmp(display->display_type, "primary"))
		info->display_type = SDE_CONNECTOR_PRIMARY;
	else if (!strcmp(display->display_type, "secondary"))
		info->display_type = SDE_CONNECTOR_SECONDARY;

	info->width_mm = phy_props.panel_width_mm;
	info->height_mm = phy_props.panel_height_mm;
	info->max_width = 1920;
	info->max_height = 1080;
	info->qsync_min_fps =
		display->panel->qsync_caps.qsync_min_fps;
	info->has_qsync_min_fps_list =
		(display->panel->qsync_caps.qsync_min_fps_list_len > 0) ?
		true : false;
	info->poms_align_vsync = display->panel->poms_align_vsync;

	info->panel_id = display->panel->panel_id;
	info->panel_ver = display->panel->panel_ver;
	info->panel_regDA = display->panel->panel_regDA;
	strncpy(info->panel_name, display->panel->panel_name,
				sizeof(display->panel->panel_name));
	strncpy(info->panel_supplier, display->panel->panel_supplier,
				sizeof(display->panel->panel_supplier));

	switch (display->panel->panel_mode) {
	case DSI_OP_VIDEO_MODE:
		info->curr_panel_mode = MSM_DISPLAY_VIDEO_MODE;
		info->capabilities |= MSM_DISPLAY_CAP_VID_MODE;
		if (display->panel->panel_mode_switch_enabled)
			info->capabilities |= MSM_DISPLAY_CAP_CMD_MODE;
		break;
	case DSI_OP_CMD_MODE:
		info->curr_panel_mode = MSM_DISPLAY_CMD_MODE;
		info->capabilities |= MSM_DISPLAY_CAP_CMD_MODE;
		if (display->panel->panel_mode_switch_enabled)
			info->capabilities |= MSM_DISPLAY_CAP_VID_MODE;
		info->is_te_using_watchdog_timer =
			display->panel->te_using_watchdog_timer |
			display->sw_te_using_wd;
		break;
	default:
		DSI_ERR("unknwown dsi panel mode %d\n",
				display->panel->panel_mode);
		break;
	}

	if (display->panel->esd_config.esd_enabled &&
			!display->sw_te_using_wd)
		info->capabilities |= MSM_DISPLAY_ESD_ENABLED;

	info->te_source = display->te_source;

	host = &display->panel->host_config;
	if (host->split_link.split_link_enabled)
		info->capabilities |= MSM_DISPLAY_SPLIT_LINK;

	info->dsc_count = display->panel->dsc_count;
	info->lm_count = display->panel->lm_count;
error:
	mutex_unlock(&display->display_lock);
	return rc;
}

int dsi_display_get_mode_count(struct dsi_display *display,
			u32 *count)
{
	if (!display || !display->panel) {
		DSI_ERR("invalid display:%d panel:%d\n", display != NULL,
			display ? display->panel != NULL : 0);
		return -EINVAL;
	}

	mutex_lock(&display->display_lock);
	*count = display->panel->num_display_modes;
	mutex_unlock(&display->display_lock);

	return 0;
}

void dsi_display_adjust_mode_timing(struct dsi_display *display,
			struct dsi_display_mode *dsi_mode,
			int lanes, int bpp)
{
	u64 new_htotal, new_vtotal, htotal, vtotal, old_htotal, div;
	struct dsi_dyn_clk_caps *dyn_clk_caps;
	u32 bits_per_symbol = 16, num_of_symbols = 7; /* For Cphy */

	dyn_clk_caps = &(display->panel->dyn_clk_caps);

	/* Constant FPS is not supported on command mode */
	if (dsi_mode->panel_mode == DSI_OP_CMD_MODE)
		return;

	if (!dyn_clk_caps->maintain_const_fps)
		return;
	/*
	 * When there is a dynamic clock switch, there is small change
	 * in FPS. To compensate for this difference in FPS, hfp or vfp
	 * is adjusted. It has been assumed that the refined porch values
	 * are supported by the panel. This logic can be enhanced further
	 * in future by taking min/max porches supported by the panel.
	 */
	switch (dyn_clk_caps->type) {
	case DSI_DYN_CLK_TYPE_CONST_FPS_ADJUST_HFP:
		vtotal = DSI_V_TOTAL(&dsi_mode->timing);
		old_htotal = dsi_h_total_dce(&dsi_mode->timing);
		do_div(old_htotal, display->ctrl_count);
		new_htotal = dsi_mode->timing.clk_rate_hz * lanes;
		div = bpp * vtotal * dsi_mode->timing.refresh_rate;
		if (dsi_display_is_type_cphy(display)) {
			new_htotal = new_htotal * bits_per_symbol;
			div = div * num_of_symbols;
		}
		do_div(new_htotal, div);
		if (old_htotal > new_htotal)
			dsi_mode->timing.h_front_porch -=
			((old_htotal - new_htotal) * display->ctrl_count);
		else
			dsi_mode->timing.h_front_porch +=
			((new_htotal - old_htotal) * display->ctrl_count);
		break;

	case DSI_DYN_CLK_TYPE_CONST_FPS_ADJUST_VFP:
		htotal = dsi_h_total_dce(&dsi_mode->timing);
		do_div(htotal, display->ctrl_count);
		new_vtotal = dsi_mode->timing.clk_rate_hz * lanes;
		div = bpp * htotal * dsi_mode->timing.refresh_rate;
		if (dsi_display_is_type_cphy(display)) {
			new_vtotal = new_vtotal * bits_per_symbol;
			div = div * num_of_symbols;
		}
		do_div(new_vtotal, div);
		dsi_mode->timing.v_front_porch = new_vtotal -
				dsi_mode->timing.v_back_porch -
				dsi_mode->timing.v_sync_width -
				dsi_mode->timing.v_active;
		break;

	default:
		break;
	}
}

static void _dsi_display_populate_bit_clks(struct dsi_display *display,
					   int start, int end, u32 *mode_idx)
{
	struct dsi_dyn_clk_caps *dyn_clk_caps;
	struct dsi_display_mode *src, *dst;
	struct dsi_host_common_cfg *cfg;
	struct dsi_display_mode_priv_info *priv_info;
	int i, j, total_modes, bpp, lanes = 0;
	size_t size = 0;

	if (!display || !mode_idx)
		return;

	dyn_clk_caps = &(display->panel->dyn_clk_caps);
	if (!dyn_clk_caps->dyn_clk_support)
		return;

	cfg = &(display->panel->host_config);
	bpp = dsi_pixel_format_to_bpp(cfg->dst_format);

	if (cfg->data_lanes & DSI_DATA_LANE_0)
		lanes++;
	if (cfg->data_lanes & DSI_DATA_LANE_1)
		lanes++;
	if (cfg->data_lanes & DSI_DATA_LANE_2)
		lanes++;
	if (cfg->data_lanes & DSI_DATA_LANE_3)
		lanes++;

	total_modes = display->panel->num_display_modes;

	for (i = start; i < end; i++) {
		src = &display->modes[i];
		if (!src)
			return;
		/*
		 * TODO: currently setting the first bit rate in
		 * the list as preferred rate. But ideally should
		 * be based on user or device tree preferrence.
		 */
		src->timing.clk_rate_hz = dyn_clk_caps->bit_clk_list[0];

		dsi_display_adjust_mode_timing(display, src, lanes, bpp);

		src->pixel_clk_khz =
			div_u64(src->timing.clk_rate_hz * lanes, bpp);
		src->pixel_clk_khz /= 1000;
		src->pixel_clk_khz *= display->ctrl_count;
	}

	for (i = 1; i < dyn_clk_caps->bit_clk_list_len; i++) {
		if (*mode_idx >= total_modes)
			return;
		for (j = start; j < end; j++) {
			src = &display->modes[j];
			dst = &display->modes[*mode_idx];

			if (!src || !dst) {
				DSI_ERR("invalid mode index\n");
				return;
			}
			memcpy(dst, src, sizeof(struct dsi_display_mode));

			size = sizeof(struct dsi_display_mode_priv_info);
			priv_info = kzalloc(size, GFP_KERNEL);
			dst->priv_info = priv_info;
			if (dst->priv_info)
				memcpy(dst->priv_info, src->priv_info, size);

			dst->timing.clk_rate_hz = dyn_clk_caps->bit_clk_list[i];

			dsi_display_adjust_mode_timing(display, dst, lanes,
									bpp);

			dst->pixel_clk_khz =
				div_u64(dst->timing.clk_rate_hz * lanes, bpp);
			dst->pixel_clk_khz /= 1000;
			dst->pixel_clk_khz *= display->ctrl_count;
			(*mode_idx)++;
		}
	}
}

void dsi_display_put_mode(struct dsi_display *display,
	struct dsi_display_mode *mode)
{
	dsi_panel_put_mode(mode);
}

int dsi_display_get_modes(struct dsi_display *display,
			  struct dsi_display_mode **out_modes)
{
	struct dsi_dfps_capabilities dfps_caps;
	struct dsi_display_ctrl *ctrl;
	struct dsi_host_common_cfg *host = &display->panel->host_config;
	bool is_split_link, is_cmd_mode;
	u32 num_dfps_rates, timing_mode_count, display_mode_count;
	u32 sublinks_count, mode_idx, array_idx = 0;
	struct dsi_dyn_clk_caps *dyn_clk_caps;
	int i, start, end, rc = -EINVAL;

	if (!display || !out_modes) {
		DSI_ERR("Invalid params\n");
		return -EINVAL;
	}

	*out_modes = NULL;
	ctrl = &display->ctrl[0];

	mutex_lock(&display->display_lock);

	if (display->modes)
		goto exit;

	display_mode_count = display->panel->num_display_modes;

	display->modes = kcalloc(display_mode_count, sizeof(*display->modes),
			GFP_KERNEL);
	if (!display->modes) {
		rc = -ENOMEM;
		goto error;
	}

	rc = dsi_panel_get_dfps_caps(display->panel, &dfps_caps);
	if (rc) {
		DSI_ERR("[%s] failed to get dfps caps from panel\n",
				display->name);
		goto error;
	}

	dyn_clk_caps = &(display->panel->dyn_clk_caps);

	timing_mode_count = display->panel->num_timing_nodes;

	/* Validate command line timing */
	if ((display->cmdline_timing != NO_OVERRIDE) &&
		(display->cmdline_timing >= timing_mode_count))
		display->cmdline_timing = NO_OVERRIDE;

	for (mode_idx = 0; mode_idx < timing_mode_count; mode_idx++) {
		struct dsi_display_mode display_mode;
		int topology_override = NO_OVERRIDE;
		bool is_preferred = false;
		u32 frame_threshold_us = ctrl->ctrl->frame_threshold_time_us;

		if (display->cmdline_timing == mode_idx) {
			topology_override = display->cmdline_topology;
			is_preferred = true;
		}

		memset(&display_mode, 0, sizeof(display_mode));

		rc = dsi_panel_get_mode(display->panel, mode_idx,
						&display_mode,
						topology_override);
		if (rc) {
			DSI_ERR("[%s] failed to get mode idx %d from panel\n",
				   display->name, mode_idx);
			goto error;
		}

		/*
		 * Update the host_config.dst_format for compressed RGB101010
		 * pixel format.
		 */
		if (display->panel->host_config.dst_format ==
			DSI_PIXEL_FORMAT_RGB101010 &&
			display_mode.timing.dsc_enabled) {
			display->panel->host_config.dst_format =
				DSI_PIXEL_FORMAT_RGB888;
			DSI_DEBUG("updated dst_format from %d to %d\n",
				DSI_PIXEL_FORMAT_RGB101010,
				display->panel->host_config.dst_format);
		}

		is_cmd_mode = (display_mode.panel_mode == DSI_OP_CMD_MODE);

		/* Setup widebus support */
		display_mode.priv_info->widebus_support =
				ctrl->ctrl->hw.widebus_support;
		num_dfps_rates = ((!dfps_caps.dfps_support ||
			is_cmd_mode) ? 1 : dfps_caps.dfps_list_len);

		/* Calculate dsi frame transfer time */
		if (is_cmd_mode) {
			dsi_panel_calc_dsi_transfer_time(
					&display->panel->host_config,
					&display_mode, frame_threshold_us);
			display_mode.priv_info->dsi_transfer_time_us =
				display_mode.timing.dsi_transfer_time_us;
			display_mode.priv_info->min_dsi_clk_hz =
				display_mode.timing.min_dsi_clk_hz;

			display_mode.priv_info->mdp_transfer_time_us =
				display_mode.timing.mdp_transfer_time_us;
		}

		is_split_link = host->split_link.split_link_enabled;
		sublinks_count = host->split_link.num_sublinks;
		if (is_split_link && sublinks_count > 1) {
			display_mode.timing.h_active *= sublinks_count;
			display_mode.timing.h_front_porch *= sublinks_count;
			display_mode.timing.h_sync_width *= sublinks_count;
			display_mode.timing.h_back_porch *= sublinks_count;
			display_mode.timing.h_skew *= sublinks_count;
			display_mode.pixel_clk_khz *= sublinks_count;
		} else {
			display_mode.timing.h_active *= display->ctrl_count;
			display_mode.timing.h_front_porch *=
						display->ctrl_count;
			display_mode.timing.h_sync_width *=
						display->ctrl_count;
			display_mode.timing.h_back_porch *=
						display->ctrl_count;
			display_mode.timing.h_skew *= display->ctrl_count;
			display_mode.pixel_clk_khz *= display->ctrl_count;
		}

		start = array_idx;
		for (i = 0; i < num_dfps_rates; i++) {
			struct dsi_display_mode *sub_mode =
					&display->modes[array_idx];
			u32 curr_refresh_rate;

			if (!sub_mode) {
				DSI_ERR("invalid mode data\n");
				rc = -EFAULT;
				goto error;
			}

			memcpy(sub_mode, &display_mode, sizeof(display_mode));
			array_idx++;

			if (!dfps_caps.dfps_support || is_cmd_mode)
				continue;

			curr_refresh_rate = sub_mode->timing.refresh_rate;
			sub_mode->timing.refresh_rate = dfps_caps.dfps_list[i];

			dsi_display_get_dfps_timing(display, sub_mode,
					curr_refresh_rate);
		}
		end = array_idx;
		/*
		 * if POMS is enabled and boot up mode is video mode,
		 * skip bit clk rates update for command mode,
		 * else if dynamic clk switch is supported then update all
		 * the bit clk rates.
		 */

		if (is_cmd_mode &&
			(display->panel->panel_mode == DSI_OP_VIDEO_MODE))
			continue;

		_dsi_display_populate_bit_clks(display, start, end, &array_idx);
		if (is_preferred) {
			/* Set first timing sub mode as preferred mode */
			display->modes[start].is_preferred = true;
		}
	}

exit:
	*out_modes = display->modes;
	rc = 0;

error:
	if (rc)
		kfree(display->modes);

	mutex_unlock(&display->display_lock);
	return rc;
}

int dsi_display_get_panel_vfp(void *dsi_display,
	int h_active, int v_active)
{
	int i, rc = 0;
	u32 count, refresh_rate = 0;
	struct dsi_dfps_capabilities dfps_caps;
	struct dsi_display *display = (struct dsi_display *)dsi_display;
	struct dsi_host_common_cfg *host;

	if (!display || !display->panel)
		return -EINVAL;

	mutex_lock(&display->display_lock);

	count = display->panel->num_display_modes;

	if (display->panel->cur_mode)
		refresh_rate = display->panel->cur_mode->timing.refresh_rate;

	dsi_panel_get_dfps_caps(display->panel, &dfps_caps);
	if (dfps_caps.dfps_support)
		refresh_rate = dfps_caps.max_refresh_rate;

	if (!refresh_rate) {
		mutex_unlock(&display->display_lock);
		DSI_ERR("Null Refresh Rate\n");
		return -EINVAL;
	}

	host = &display->panel->host_config;
	if (host->split_link.split_link_enabled)
		h_active *= host->split_link.num_sublinks;
	else
		h_active *= display->ctrl_count;

	for (i = 0; i < count; i++) {
		struct dsi_display_mode *m = &display->modes[i];

		if (m && v_active == m->timing.v_active &&
			h_active == m->timing.h_active &&
			refresh_rate == m->timing.refresh_rate) {
			rc = m->timing.v_front_porch;
			break;
		}
	}
	mutex_unlock(&display->display_lock);

	return rc;
}

int dsi_display_get_default_lms(void *dsi_display, u32 *num_lm)
{
	struct dsi_display *display = (struct dsi_display *)dsi_display;
	u32 count, i;
	int rc = 0;

	*num_lm = 0;

	mutex_lock(&display->display_lock);
	count = display->panel->num_display_modes;
	mutex_unlock(&display->display_lock);

	if (!display->modes) {
		struct dsi_display_mode *m;

		rc = dsi_display_get_modes(display, &m);
		if (rc)
			return rc;
	}

	mutex_lock(&display->display_lock);
	for (i = 0; i < count; i++) {
		struct dsi_display_mode *m = &display->modes[i];

		*num_lm = max(m->priv_info->topology.num_lm, *num_lm);
	}
	mutex_unlock(&display->display_lock);

	return rc;
}

int dsi_display_get_qsync_min_fps(void *display_dsi, u32 mode_fps)
{
	struct dsi_display *display = (struct dsi_display *)display_dsi;
	struct dsi_panel *panel;
	u32 i;

	if (display == NULL || display->panel == NULL)
		return -EINVAL;

	panel = display->panel;
	for (i = 0; i < panel->dfps_caps.dfps_list_len; i++) {
		if (panel->dfps_caps.dfps_list[i] == mode_fps)
			return panel->qsync_caps.qsync_min_fps_list[i];
	}
	SDE_EVT32(mode_fps);
	DSI_DEBUG("Invalid mode_fps %d\n", mode_fps);
	return -EINVAL;
}

int dsi_display_find_mode(struct dsi_display *display,
		const struct dsi_display_mode *cmp,
		struct dsi_display_mode **out_mode)
{
	u32 count, i;
	int rc;

	if (!display || !out_mode)
		return -EINVAL;

	*out_mode = NULL;

	mutex_lock(&display->display_lock);
	count = display->panel->num_display_modes;
	mutex_unlock(&display->display_lock);

	if (!display->modes) {
		struct dsi_display_mode *m;

		rc = dsi_display_get_modes(display, &m);
		if (rc)
			return rc;
	}

	mutex_lock(&display->display_lock);
	for (i = 0; i < count; i++) {
		struct dsi_display_mode *m = &display->modes[i];

		if (cmp->timing.v_active == m->timing.v_active &&
			cmp->timing.h_active == m->timing.h_active &&
			cmp->timing.refresh_rate == m->timing.refresh_rate &&
			cmp->panel_mode == m->panel_mode &&
			cmp->pixel_clk_khz == m->pixel_clk_khz) {
			*out_mode = m;
			rc = 0;
			break;
		}
	}
	mutex_unlock(&display->display_lock);

	if (!*out_mode) {
		DSI_ERR("[%s] failed to find mode for v_active %u h_active %u fps %u pclk %u\n",
				display->name, cmp->timing.v_active,
				cmp->timing.h_active, cmp->timing.refresh_rate,
				cmp->pixel_clk_khz);
		rc = -ENOENT;
	}

	return rc;
}

static inline bool dsi_display_mode_switch_dfps(struct dsi_display_mode *cur,
						struct dsi_display_mode *adj)
{
	/*
	 * If there is a change in the hfp or vfp of the current and adjoining
	 * mode,then either it is a dfps mode switch or dynamic clk change with
	 * constant fps.
	 */
	if ((cur->timing.h_front_porch != adj->timing.h_front_porch) ||
		(cur->timing.v_front_porch != adj->timing.v_front_porch))
		return true;
	else
		return false;
}

/**
 * dsi_display_validate_mode_change() - Validate mode change case.
 * @display:     DSI display handle.
 * @cur_mode:    Current mode.
 * @adj_mode:    Mode to be set.
 *               MSM_MODE_FLAG_SEAMLESS_VRR flag is set if there
 *               is change in hfp or vfp but vactive and hactive are same.
 *               DSI_MODE_FLAG_DYN_CLK flag is set if there
 *               is change in clk but vactive and hactive are same.
 * Return: error code.
 */
int dsi_display_validate_mode_change(struct dsi_display *display,
			struct dsi_display_mode *cur_mode,
			struct dsi_display_mode *adj_mode)
{
	int rc = 0;
	struct dsi_dfps_capabilities dfps_caps;
	struct dsi_dyn_clk_caps *dyn_clk_caps;

	if (!display || !adj_mode) {
		DSI_ERR("Invalid params\n");
		return -EINVAL;
	}

	if (!display->panel || !display->panel->cur_mode) {
		DSI_DEBUG("Current panel mode not set\n");
		return rc;
	}

	mutex_lock(&display->display_lock);
	dyn_clk_caps = &(display->panel->dyn_clk_caps);
	if ((cur_mode->timing.v_active == adj_mode->timing.v_active) &&
		(cur_mode->timing.h_active == adj_mode->timing.h_active) &&
		(cur_mode->panel_mode == adj_mode->panel_mode)) {
		/* dfps and dynamic clock with const fps use case */
		if (dsi_display_mode_switch_dfps(cur_mode, adj_mode)) {
			dsi_panel_get_dfps_caps(display->panel, &dfps_caps);
			if (dfps_caps.dfps_support ||
				dyn_clk_caps->maintain_const_fps) {
				DSI_DEBUG("Mode switch is seamless variable refresh\n");
				adj_mode->dsi_mode_flags |= DSI_MODE_FLAG_VRR;
				SDE_EVT32(SDE_EVTLOG_FUNC_CASE1,
					cur_mode->timing.refresh_rate,
					adj_mode->timing.refresh_rate,
					cur_mode->timing.h_front_porch,
					adj_mode->timing.h_front_porch,
					cur_mode->timing.v_front_porch,
					adj_mode->timing.v_front_porch);
			}
		}

		/* dynamic clk change use case */
		if (cur_mode->pixel_clk_khz != adj_mode->pixel_clk_khz) {
			if (dyn_clk_caps->dyn_clk_support) {
				DSI_DEBUG("dynamic clk change detected\n");
				if ((adj_mode->dsi_mode_flags &
					DSI_MODE_FLAG_VRR) &&
					(!dyn_clk_caps->maintain_const_fps)) {
					DSI_ERR("dfps and dyn clk not supported in same commit\n");
					rc = -ENOTSUPP;
					goto error;
				}

				adj_mode->dsi_mode_flags |=
						DSI_MODE_FLAG_DYN_CLK;
				SDE_EVT32(SDE_EVTLOG_FUNC_CASE2,
					cur_mode->pixel_clk_khz,
					adj_mode->pixel_clk_khz);
			}
		}
	}

error:
	mutex_unlock(&display->display_lock);
	return rc;
}

int dsi_display_validate_mode(struct dsi_display *display,
			      struct dsi_display_mode *mode,
			      u32 flags)
{
	int rc = 0;
	int i;
	struct dsi_display_ctrl *ctrl;
	struct dsi_display_mode adj_mode;

	if (!display || !mode) {
		DSI_ERR("Invalid params\n");
		return -EINVAL;
	}

	mutex_lock(&display->display_lock);

	adj_mode = *mode;
	adjust_timing_by_ctrl_count(display, &adj_mode);

	rc = dsi_panel_validate_mode(display->panel, &adj_mode);
	if (rc) {
		DSI_ERR("[%s] panel mode validation failed, rc=%d\n",
		       display->name, rc);
		goto error;
	}

	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		rc = dsi_ctrl_validate_timing(ctrl->ctrl, &adj_mode.timing);
		if (rc) {
			DSI_ERR("[%s] ctrl mode validation failed, rc=%d\n",
			       display->name, rc);
			goto error;
		}

		rc = dsi_phy_validate_mode(ctrl->phy, &adj_mode.timing);
		if (rc) {
			DSI_ERR("[%s] phy mode validation failed, rc=%d\n",
			       display->name, rc);
			goto error;
		}
	}

	if ((flags & DSI_VALIDATE_FLAG_ALLOW_ADJUST) &&
			(mode->dsi_mode_flags & DSI_MODE_FLAG_SEAMLESS)) {
		rc = dsi_display_validate_mode_seamless(display, mode);
		if (rc) {
			DSI_ERR("[%s] seamless not possible rc=%d\n",
				display->name, rc);
			goto error;
		}
	}

error:
	mutex_unlock(&display->display_lock);
	return rc;
}

int dsi_display_set_mode(struct dsi_display *display,
			 struct dsi_display_mode *mode,
			 u32 flags)
{
	int rc = 0;
	struct dsi_display_mode adj_mode;
	struct dsi_mode_info timing;

	if (!display || !mode || !display->panel) {
		DSI_ERR("Invalid params\n");
		return -EINVAL;
	}

	mutex_lock(&display->display_lock);

	adj_mode = *mode;
	timing = adj_mode.timing;
	adjust_timing_by_ctrl_count(display, &adj_mode);

	if (!display->panel->cur_mode) {
		display->panel->cur_mode =
			kzalloc(sizeof(struct dsi_display_mode), GFP_KERNEL);
		if (!display->panel->cur_mode) {
			rc = -ENOMEM;
			goto error;
		}
	}

	/*For dynamic DSI setting, use specified clock rate */
	if (display->cached_clk_rate > 0)
		adj_mode.priv_info->clk_rate_hz = display->cached_clk_rate;

	rc = dsi_display_validate_mode_set(display, &adj_mode, flags);
	if (rc) {
		DSI_ERR("[%s] mode cannot be set\n", display->name);
		goto error;
	}

	rc = dsi_display_set_mode_sub(display, &adj_mode, flags);
	if (rc) {
		DSI_ERR("[%s] failed to set mode\n", display->name);
		goto error;
	}

	DSI_INFO("mdp_transfer_time=%d, hactive=%d, vactive=%d, fps=%d\n",
			adj_mode.priv_info->mdp_transfer_time_us,
			timing.h_active, timing.v_active, timing.refresh_rate);
	SDE_EVT32(adj_mode.priv_info->mdp_transfer_time_us,
			timing.h_active, timing.v_active, timing.refresh_rate);

	memcpy(display->panel->cur_mode, &adj_mode, sizeof(adj_mode));
error:
	mutex_unlock(&display->display_lock);
	return rc;
}

int dsi_display_set_tpg_state(struct dsi_display *display, bool enable)
{
	int rc = 0;
	int i;
	struct dsi_display_ctrl *ctrl;

	if (!display) {
		DSI_ERR("Invalid params\n");
		return -EINVAL;
	}

	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		rc = dsi_ctrl_set_tpg_state(ctrl->ctrl, enable);
		if (rc) {
			DSI_ERR("[%s] failed to set tpg state for host_%d\n",
			       display->name, i);
			goto error;
		}
	}

	display->is_tpg_enabled = enable;
error:
	return rc;
}

static int dsi_display_pre_switch(struct dsi_display *display)
{
	int rc = 0;

	rc = dsi_display_clk_ctrl(display->dsi_clk_handle,
			DSI_CORE_CLK, DSI_CLK_ON);
	if (rc) {
		DSI_ERR("[%s] failed to enable DSI core clocks, rc=%d\n",
		       display->name, rc);
		goto error;
	}

	rc = dsi_display_ctrl_update(display);
	if (rc) {
		DSI_ERR("[%s] failed to update DSI controller, rc=%d\n",
			   display->name, rc);
		goto error_ctrl_clk_off;
	}

	if (!display->trusted_vm_env) {
		rc = dsi_display_set_clk_src(display);
		if (rc) {
			DSI_ERR(
			"[%s] failed to set DSI link clock source, rc=%d\n",
			display->name, rc);
			goto error_ctrl_deinit;
		}
	}

	rc = dsi_display_clk_ctrl(display->dsi_clk_handle,
			DSI_LINK_CLK, DSI_CLK_ON);
	if (rc) {
		DSI_ERR("[%s] failed to enable DSI link clocks, rc=%d\n",
			   display->name, rc);
		goto error_ctrl_deinit;
	}

	goto error;

error_ctrl_deinit:
	(void)dsi_display_ctrl_deinit(display);
error_ctrl_clk_off:
	(void)dsi_display_clk_ctrl(display->dsi_clk_handle,
			DSI_CORE_CLK, DSI_CLK_OFF);
error:
	return rc;
}

static bool _dsi_display_validate_host_state(struct dsi_display *display)
{
	int i;
	struct dsi_display_ctrl *ctrl;

	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		if (!ctrl->ctrl)
			continue;
		if (!dsi_ctrl_validate_host_state(ctrl->ctrl))
			return false;
	}

	return true;
}

static void dsi_display_handle_fifo_underflow(struct work_struct *work)
{
	struct dsi_display *display = NULL;

	display = container_of(work, struct dsi_display, fifo_underflow_work);
	if (!display || !display->panel ||
	    atomic_read(&display->panel->esd_recovery_pending)) {
		DSI_DEBUG("Invalid recovery use case\n");
		return;
	}

	mutex_lock(&display->display_lock);

	if (!_dsi_display_validate_host_state(display)) {
		mutex_unlock(&display->display_lock);
		return;
	}

	DSI_INFO("handle DSI FIFO underflow error\n");
	SDE_EVT32(SDE_EVTLOG_FUNC_ENTRY);

	dsi_display_clk_ctrl(display->dsi_clk_handle,
			DSI_ALL_CLKS, DSI_CLK_ON);
	dsi_display_soft_reset(display);
	dsi_display_clk_ctrl(display->dsi_clk_handle,
			DSI_ALL_CLKS, DSI_CLK_OFF);
	SDE_EVT32(SDE_EVTLOG_FUNC_EXIT);

	mutex_unlock(&display->display_lock);
}

static void dsi_display_handle_fifo_overflow(struct work_struct *work)
{
	struct dsi_display *display = NULL;
	struct dsi_display_ctrl *ctrl;
	int i, rc;
	int mask = BIT(20); /* clock lane */
	int (*cb_func)(void *event_usr_ptr,
		uint32_t event_idx, uint32_t instance_idx,
		uint32_t data0, uint32_t data1,
		uint32_t data2, uint32_t data3);
	void *data;
	u32 version = 0;

	display = container_of(work, struct dsi_display, fifo_overflow_work);
	if (!display || !display->panel ||
	    (display->panel->panel_mode != DSI_OP_VIDEO_MODE) ||
	    atomic_read(&display->panel->esd_recovery_pending)) {
		DSI_DEBUG("Invalid recovery use case\n");
		return;
	}

	mutex_lock(&display->display_lock);

	if (!_dsi_display_validate_host_state(display)) {
		mutex_unlock(&display->display_lock);
		return;
	}

	DSI_INFO("handle DSI FIFO overflow error\n");
	SDE_EVT32(SDE_EVTLOG_FUNC_ENTRY);

	dsi_display_clk_ctrl(display->dsi_clk_handle,
			DSI_ALL_CLKS, DSI_CLK_ON);

	/*
	 * below recovery sequence is not applicable to
	 * hw version 2.0.0, 2.1.0 and 2.2.0, so return early.
	 */
	ctrl = &display->ctrl[display->clk_master_idx];
	version = dsi_ctrl_get_hw_version(ctrl->ctrl);
	if (!version || (version < 0x20020001))
		goto end;

	/* reset ctrl and lanes */
	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		rc = dsi_ctrl_reset(ctrl->ctrl, mask);
		rc = dsi_phy_lane_reset(ctrl->phy);
	}

	/* wait for display line count to be in active area */
	ctrl = &display->ctrl[display->clk_master_idx];
	if (ctrl->ctrl->recovery_cb.event_cb) {
		cb_func = ctrl->ctrl->recovery_cb.event_cb;
		data = ctrl->ctrl->recovery_cb.event_usr_ptr;
		rc = cb_func(data, SDE_CONN_EVENT_VID_FIFO_OVERFLOW,
				display->clk_master_idx, 0, 0, 0, 0);
		if (rc < 0) {
			DSI_DEBUG("sde callback failed\n");
			goto end;
		}
	}

	/* Enable Video mode for DSI controller */
	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		dsi_ctrl_vid_engine_en(ctrl->ctrl, true);
	}
	/*
	 * Add sufficient delay to make sure
	 * pixel transmission has started
	 */
	udelay(200);
end:
	dsi_display_clk_ctrl(display->dsi_clk_handle,
			DSI_ALL_CLKS, DSI_CLK_OFF);
	SDE_EVT32(SDE_EVTLOG_FUNC_EXIT);

	mutex_unlock(&display->display_lock);
}

static void dsi_display_handle_lp_rx_timeout(struct work_struct *work)
{
	struct dsi_display *display = NULL;
	struct dsi_display_ctrl *ctrl;
	int i, rc;
	int mask = (BIT(20) | (0xF << 16)); /* clock lane and 4 data lane */
	int (*cb_func)(void *event_usr_ptr,
		uint32_t event_idx, uint32_t instance_idx,
		uint32_t data0, uint32_t data1,
		uint32_t data2, uint32_t data3);
	void *data;
	u32 version = 0;

	display = container_of(work, struct dsi_display, lp_rx_timeout_work);
	if (!display || !display->panel ||
	    (display->panel->panel_mode != DSI_OP_VIDEO_MODE) ||
	    atomic_read(&display->panel->esd_recovery_pending)) {
		DSI_DEBUG("Invalid recovery use case\n");
		return;
	}

	mutex_lock(&display->display_lock);

	if (!_dsi_display_validate_host_state(display)) {
		mutex_unlock(&display->display_lock);
		return;
	}

	DSI_INFO("handle DSI LP RX Timeout error\n");
	SDE_EVT32(SDE_EVTLOG_FUNC_ENTRY);

	dsi_display_clk_ctrl(display->dsi_clk_handle,
			DSI_ALL_CLKS, DSI_CLK_ON);

	/*
	 * below recovery sequence is not applicable to
	 * hw version 2.0.0, 2.1.0 and 2.2.0, so return early.
	 */
	ctrl = &display->ctrl[display->clk_master_idx];
	version = dsi_ctrl_get_hw_version(ctrl->ctrl);
	if (!version || (version < 0x20020001))
		goto end;

	/* reset ctrl and lanes */
	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		rc = dsi_ctrl_reset(ctrl->ctrl, mask);
		rc = dsi_phy_lane_reset(ctrl->phy);
	}

	ctrl = &display->ctrl[display->clk_master_idx];
	if (ctrl->ctrl->recovery_cb.event_cb) {
		cb_func = ctrl->ctrl->recovery_cb.event_cb;
		data = ctrl->ctrl->recovery_cb.event_usr_ptr;
		rc = cb_func(data, SDE_CONN_EVENT_VID_FIFO_OVERFLOW,
				display->clk_master_idx, 0, 0, 0, 0);
		if (rc < 0) {
			DSI_DEBUG("Target is in suspend/shutdown\n");
			goto end;
		}
	}

	/* Enable Video mode for DSI controller */
	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		dsi_ctrl_vid_engine_en(ctrl->ctrl, true);
	}

	/*
	 * Add sufficient delay to make sure
	 * pixel transmission as started
	 */
	udelay(200);
end:
	dsi_display_clk_ctrl(display->dsi_clk_handle,
			DSI_ALL_CLKS, DSI_CLK_OFF);
	SDE_EVT32(SDE_EVTLOG_FUNC_EXIT);

	mutex_unlock(&display->display_lock);
}

static int dsi_display_cb_error_handler(void *data,
		uint32_t event_idx, uint32_t instance_idx,
		uint32_t data0, uint32_t data1,
		uint32_t data2, uint32_t data3)
{
	struct dsi_display *display =  data;

	if (!display || !(display->err_workq))
		return -EINVAL;

	switch (event_idx) {
	case DSI_FIFO_UNDERFLOW:
		queue_work(display->err_workq, &display->fifo_underflow_work);
		break;
	case DSI_FIFO_OVERFLOW:
		queue_work(display->err_workq, &display->fifo_overflow_work);
		break;
	case DSI_LP_Rx_TIMEOUT:
		queue_work(display->err_workq, &display->lp_rx_timeout_work);
		break;
	default:
		DSI_WARN("unhandled error interrupt: %d\n", event_idx);
		break;
	}

	return 0;
}

static void dsi_display_register_error_handler(struct dsi_display *display)
{
	int i = 0;
	struct dsi_display_ctrl *ctrl;
	struct dsi_event_cb_info event_info;

	if (!display)
		return;

	display->err_workq = create_singlethread_workqueue("dsi_err_workq");
	if (!display->err_workq) {
		DSI_ERR("failed to create dsi workq!\n");
		return;
	}

	INIT_WORK(&display->fifo_underflow_work,
				dsi_display_handle_fifo_underflow);
	INIT_WORK(&display->fifo_overflow_work,
				dsi_display_handle_fifo_overflow);
	INIT_WORK(&display->lp_rx_timeout_work,
				dsi_display_handle_lp_rx_timeout);

	memset(&event_info, 0, sizeof(event_info));

	event_info.event_cb = dsi_display_cb_error_handler;
	event_info.event_usr_ptr = display;

	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		ctrl->ctrl->irq_info.irq_err_cb = event_info;
	}
}

static void dsi_display_unregister_error_handler(struct dsi_display *display)
{
	int i = 0;
	struct dsi_display_ctrl *ctrl;

	if (!display)
		return;

	display_for_each_ctrl(i, display) {
		ctrl = &display->ctrl[i];
		memset(&ctrl->ctrl->irq_info.irq_err_cb,
		       0, sizeof(struct dsi_event_cb_info));
	}

	if (display->err_workq) {
		destroy_workqueue(display->err_workq);
		display->err_workq = NULL;
	}
}

int dsi_display_prepare(struct dsi_display *display)
{
	int rc = 0;
	struct dsi_display_mode *mode;

	if (!display) {
		DSI_ERR("Invalid params\n");
		return -EINVAL;
	}

	if (!display->panel->cur_mode) {
		DSI_ERR("no valid mode set for the display\n");
		return -EINVAL;
	}

	DSI_INFO("%s(%s)+\n", __func__, display->drm_conn->name);
	SDE_EVT32(SDE_EVTLOG_FUNC_ENTRY);
	mutex_lock(&display->display_lock);

	display->hw_ownership = true;
	mode = display->panel->cur_mode;

	dsi_display_set_ctrl_esd_check_flag(display, false);

	/* Set up ctrl isr before enabling core clk */
	if (!display->trusted_vm_env)
		dsi_display_ctrl_isr_configure(display, true);

	if (mode->dsi_mode_flags & DSI_MODE_FLAG_DMS) {
		if (display->is_cont_splash_enabled &&
		    display->config.panel_mode == DSI_OP_VIDEO_MODE) {
			DSI_ERR("DMS not supported on first frame\n");
			rc = -EINVAL;
			goto error;
		}

		if (!is_skip_op_required(display)) {
			/* update dsi ctrl for new mode */
			rc = dsi_display_pre_switch(display);
			if (rc)
				DSI_ERR("[%s] panel pre-switch failed, rc=%d\n",
					display->name, rc);
			goto error;
		}
	}

	if (!(mode->dsi_mode_flags & DSI_MODE_FLAG_POMS) &&
		(!is_skip_op_required(display))) {
		/*
		 * For continuous splash/trusted vm, we skip panel
		 * pre prepare since the regulator vote is already
		 * taken care in splash resource init
		 */
		rc = dsi_panel_pre_prepare(display->panel);
		if (rc) {
			DSI_ERR("[%s] panel pre-prepare failed, rc=%d\n",
					display->name, rc);
			goto error;
		}
	}

	rc = dsi_display_clk_ctrl(display->dsi_clk_handle,
			DSI_CORE_CLK, DSI_CLK_ON);
	if (rc) {
		DSI_ERR("[%s] failed to enable DSI core clocks, rc=%d\n",
		       display->name, rc);
		goto error_panel_post_unprep;
	}

	/*
	 * If ULPS during suspend feature is enabled, then DSI PHY was
	 * left on during suspend. In this case, we do not need to reset/init
	 * PHY. This would have already been done when the CORE clocks are
	 * turned on. However, if cont splash is disabled, the first time DSI
	 * is powered on, phy init needs to be done unconditionally.
	 */
	if (!display->panel->ulps_suspend_enabled || !display->ulps_enabled) {
		rc = dsi_display_phy_sw_reset(display);
		if (rc) {
			DSI_ERR("[%s] failed to reset phy, rc=%d\n",
				display->name, rc);
			goto error_ctrl_clk_off;
		}

		rc = dsi_display_phy_enable(display);
		if (rc) {
			DSI_ERR("[%s] failed to enable DSI PHY, rc=%d\n",
			       display->name, rc);
			goto error_ctrl_clk_off;
		}
	}

	if (!display->trusted_vm_env) {
		rc = dsi_display_set_clk_src(display);
		if (rc) {
			DSI_ERR(
			"[%s] failed to set DSI link clock source, rc=%d\n",
				display->name, rc);
			goto error_phy_disable;
		}
	}

	rc = dsi_display_ctrl_init(display);
	if (rc) {
		DSI_ERR("[%s] failed to setup DSI controller, rc=%d\n",
		       display->name, rc);
		goto error_phy_disable;
	}
	/* Set up DSI ERROR event callback */
	dsi_display_register_error_handler(display);

	rc = dsi_display_ctrl_host_enable(display);
	if (rc) {
		DSI_ERR("[%s] failed to enable DSI host, rc=%d\n",
		       display->name, rc);
		goto error_ctrl_deinit;
	}

	rc = dsi_display_clk_ctrl(display->dsi_clk_handle,
			DSI_LINK_CLK, DSI_CLK_ON);
	if (rc) {
		DSI_ERR("[%s] failed to enable DSI link clocks, rc=%d\n",
		       display->name, rc);
		goto error_host_engine_off;
	}

	if (!is_skip_op_required(display)) {
		/*
		 * For continuous splash/trusted vm, skip panel prepare and
		 * ctl reset since the pnael and ctrl is already in active
		 * state and panel on commands are not needed
		 */
		rc = dsi_display_soft_reset(display);
		if (rc) {
			DSI_ERR("[%s] failed soft reset, rc=%d\n",
					display->name, rc);
			goto error_ctrl_link_off;
		}

		if (!(mode->dsi_mode_flags & DSI_MODE_FLAG_POMS)) {
			rc = dsi_panel_prepare(display->panel);
			if (rc) {
				DSI_ERR("[%s] panel prepare failed, rc=%d\n",
						display->name, rc);
				goto error_ctrl_link_off;
			}
		}
	}
	goto error;

error_ctrl_link_off:
	(void)dsi_display_clk_ctrl(display->dsi_clk_handle,
			DSI_LINK_CLK, DSI_CLK_OFF);
error_host_engine_off:
	(void)dsi_display_ctrl_host_disable(display);
error_ctrl_deinit:
	(void)dsi_display_ctrl_deinit(display);
error_phy_disable:
	(void)dsi_display_phy_disable(display);
error_ctrl_clk_off:
	(void)dsi_display_clk_ctrl(display->dsi_clk_handle,
			DSI_CORE_CLK, DSI_CLK_OFF);
error_panel_post_unprep:
	if (!display->is_cont_splash_enabled)
		(void)dsi_panel_post_unprepare(display->panel);
error:
	mutex_unlock(&display->display_lock);
	SDE_EVT32(SDE_EVTLOG_FUNC_EXIT);
	return rc;
}

static int dsi_display_calc_ctrl_roi(const struct dsi_display *display,
		const struct dsi_display_ctrl *ctrl,
		const struct msm_roi_list *req_rois,
		struct dsi_rect *out_roi)
{
	const struct dsi_rect *bounds = &ctrl->ctrl->mode_bounds;
	struct dsi_display_mode *cur_mode;
	struct msm_roi_caps *roi_caps;
	struct dsi_rect req_roi = { 0 };
	int rc = 0;

	cur_mode = display->panel->cur_mode;
	if (!cur_mode)
		return 0;

	roi_caps = &cur_mode->priv_info->roi_caps;
	if (req_rois->num_rects > roi_caps->num_roi) {
		DSI_ERR("request for %d rois greater than max %d\n",
				req_rois->num_rects,
				roi_caps->num_roi);
		rc = -EINVAL;
		goto exit;
	}

	/**
	 * if no rois, user wants to reset back to full resolution
	 * note: h_active is already divided by ctrl_count
	 */
	if (!req_rois->num_rects) {
		*out_roi = *bounds;
		goto exit;
	}

	/* intersect with the bounds */
	req_roi.x = req_rois->roi[0].x1;
	req_roi.y = req_rois->roi[0].y1;
	req_roi.w = req_rois->roi[0].x2 - req_rois->roi[0].x1;
	req_roi.h = req_rois->roi[0].y2 - req_rois->roi[0].y1;
	dsi_rect_intersect(&req_roi, bounds, out_roi);

exit:
	/* adjust the ctrl origin to be top left within the ctrl */
	out_roi->x = out_roi->x - bounds->x;

	DSI_DEBUG("ctrl%d:%d: req (%d,%d,%d,%d) bnd (%d,%d,%d,%d) out (%d,%d,%d,%d)\n",
			ctrl->dsi_ctrl_idx, ctrl->ctrl->cell_index,
			req_roi.x, req_roi.y, req_roi.w, req_roi.h,
			bounds->x, bounds->y, bounds->w, bounds->h,
			out_roi->x, out_roi->y, out_roi->w, out_roi->h);

	return rc;
}

static int dsi_display_qsync(struct dsi_display *display, bool enable)
{
	int i;
	int rc = 0;

	if (!display->panel->qsync_caps.qsync_min_fps) {
		DSI_ERR("%s:ERROR: qsync set, but no fps\n", __func__);
		return 0;
	}

	mutex_lock(&display->display_lock);

	display_for_each_ctrl(i, display) {
		if (enable) {
			/* send the commands to enable qsync */
			rc = dsi_panel_send_qsync_on_dcs(display->panel, i);
			if (rc) {
				DSI_ERR("fail qsync ON cmds rc:%d\n", rc);
				goto exit;
			}
		} else {
			/* send the commands to enable qsync */
			rc = dsi_panel_send_qsync_off_dcs(display->panel, i);
			if (rc) {
				DSI_ERR("fail qsync OFF cmds rc:%d\n", rc);
				goto exit;
			}
		}

		dsi_ctrl_setup_avr(display->ctrl[i].ctrl, enable);
	}

exit:
	SDE_EVT32(enable, display->panel->qsync_caps.qsync_min_fps, rc);
	mutex_unlock(&display->display_lock);
	return rc;
}

static int dsi_display_set_roi(struct dsi_display *display,
		struct msm_roi_list *rois)
{
	struct dsi_display_mode *cur_mode;
	struct msm_roi_caps *roi_caps;
	int rc = 0;
	int i;

	if (!display || !rois || !display->panel)
		return -EINVAL;

	cur_mode = display->panel->cur_mode;
	if (!cur_mode)
		return 0;

	roi_caps = &cur_mode->priv_info->roi_caps;
	if (!roi_caps->enabled)
		return 0;

	display_for_each_ctrl(i, display) {
		struct dsi_display_ctrl *ctrl = &display->ctrl[i];
		struct dsi_rect ctrl_roi;
		bool changed = false;

		rc = dsi_display_calc_ctrl_roi(display, ctrl, rois, &ctrl_roi);
		if (rc) {
			DSI_ERR("dsi_display_calc_ctrl_roi failed rc %d\n", rc);
			return rc;
		}

		rc = dsi_ctrl_set_roi(ctrl->ctrl, &ctrl_roi, &changed);
		if (rc) {
			DSI_ERR("dsi_ctrl_set_roi failed rc %d\n", rc);
			return rc;
		}

		if (!changed)
			continue;

		/* send the new roi to the panel via dcs commands */
		rc = dsi_panel_send_roi_dcs(display->panel, i, &ctrl_roi);
		if (rc) {
			DSI_ERR("dsi_panel_set_roi failed rc %d\n", rc);
			return rc;
		}

		/* re-program the ctrl with the timing based on the new roi */
		rc = dsi_ctrl_timing_setup(ctrl->ctrl);
		if (rc) {
			DSI_ERR("dsi_ctrl_setup failed rc %d\n", rc);
			return rc;
		}
	}

	return rc;
}

int dsi_display_pre_kickoff(struct drm_connector *connector,
		struct dsi_display *display,
		struct msm_display_kickoff_params *params)
{
	int rc = 0, ret = 0;
	int i;

	/* check and setup MISR */
	if (display->misr_enable)
		_dsi_display_setup_misr(display);

	/* dynamic DSI clock setting */
	if (atomic_read(&display->clkrate_change_pending)) {
		mutex_lock(&display->display_lock);
		/*
		 * acquire panel_lock to make sure no commands are in progress
		 */
		dsi_panel_acquire_panel_lock(display->panel);

		/*
		 * Wait for DSI command engine not to be busy sending data
		 * from display engine.
		 * If waiting fails, return "rc" instead of below "ret" so as
		 * not to impact DRM commit. The clock updating would be
		 * deferred to the next DRM commit.
		 */
		display_for_each_ctrl(i, display) {
			struct dsi_ctrl *ctrl = display->ctrl[i].ctrl;

			ret = dsi_ctrl_wait_for_cmd_mode_mdp_idle(ctrl);
			if (ret)
				goto wait_failure;
		}

		/*
		 * Don't check the return value so as not to impact DRM commit
		 * when error occurs.
		 */
		(void)dsi_display_force_update_dsi_clk(display);
wait_failure:
		/* release panel_lock */
		dsi_panel_release_panel_lock(display->panel);
		mutex_unlock(&display->display_lock);
	}

	if (!ret)
		rc = dsi_display_set_roi(display, params->rois);

	return rc;
}

int dsi_display_config_ctrl_for_cont_splash(struct dsi_display *display)
{
	int rc = 0;

	if (!display || !display->panel) {
		DSI_ERR("Invalid params\n");
		return -EINVAL;
	}

	if (!display->panel->cur_mode) {
		DSI_ERR("no valid mode set for the display\n");
		return -EINVAL;
	}

	if (display->config.panel_mode == DSI_OP_VIDEO_MODE) {
		rc = dsi_display_vid_engine_enable(display);
		if (rc) {
			DSI_ERR("[%s]failed to enable DSI video engine, rc=%d\n",
			       display->name, rc);
			goto error_out;
		}
	} else if (display->config.panel_mode == DSI_OP_CMD_MODE) {
		rc = dsi_display_cmd_engine_enable(display);
		if (rc) {
			DSI_ERR("[%s]failed to enable DSI cmd engine, rc=%d\n",
			       display->name, rc);
			goto error_out;
		}
	} else {
		DSI_ERR("[%s] Invalid configuration\n", display->name);
		rc = -EINVAL;
	}

error_out:
	return rc;
}

int dsi_display_pre_commit(void *display,
		struct msm_display_conn_params *params)
{
	bool enable = false;
	int rc = 0;

	if (!display || !params) {
		pr_err("Invalid params\n");
		return -EINVAL;
	}

	if (params->qsync_update) {
		enable = (params->qsync_mode > 0) ? true : false;
		rc = dsi_display_qsync(display, enable);
		if (rc)
			pr_err("%s failed to send qsync commands\n",
				__func__);
		SDE_EVT32(params->qsync_mode, rc);
	}

	return rc;
}

static void dsi_display_panel_id_notification(struct dsi_display *display)
{
	if (display->panel_id != ~0x0 &&
		display->ctrl[0].ctrl->panel_id_cb.event_cb) {
		display->ctrl[0].ctrl->panel_id_cb.event_cb(
			display->ctrl[0].ctrl->panel_id_cb.event_usr_ptr,
			display->ctrl[0].ctrl->panel_id_cb.event_idx,
			0, ((display->panel_id & 0xffffffff00000000) >> 31),
			(display->panel_id & 0xffffffff), 0, 0);
	}
}

struct dsi_enable_status {
	struct dsi_display *display;
	int probed;
	bool enable;
	char pname[DSI_PANEL_MAX_PANEL_LEN];
};

static struct dsi_enable_status enable_status[2] = {
	{
		.probed = -ENODEV,
	},
	{
		.probed = -ENODEV,
	},
};

static void dsi_display_is_probed (struct dsi_display *display,
					int probe_status)

{
	int enable_idx = 0;

	if (!display->display_type)
		enable_idx = 0;

	if (!strcmp(display->display_type, "secondary"))
		enable_idx = 1;

	enable_status[enable_idx].probed = probe_status;
	strncpy(enable_status[enable_idx].pname, display->name,
			sizeof(enable_status[enable_idx].pname));
	DSI_DEBUG("display->drm_conn[%d] set: probe =%d, name =%s\n",
			enable_idx, probe_status, display->name);
}


int dsi_display_trigger_panel_dead_event(struct dsi_display *display)
{
	bool panel_dead;
	struct drm_event event;
	struct drm_connector *drm_conn = display->drm_conn;

	panel_dead = true;
	event.type = DRM_EVENT_PANEL_DEAD;
	event.length = sizeof(u32);
	msm_mode_object_event_notify(&drm_conn->base,
				drm_conn->dev, &event, (u8 *)&panel_dead);

	DSI_WARN("ESD is not recovered. Start ESD recovery again\n");
	return 0;
}

static int dsi_display_chk_esd_recovery(struct dsi_display *display)
{
	int ret = 0;
	u32 status_mode;
	static int disp_esd_trigger = 0;

	status_mode = display->panel->esd_config.status_mode;
	if (status_mode == ESD_MODE_TE_CHK_REG_RD) {
		/*
		 * ESD detection is to check the TE signal and reag_status
		* therefore when disp_esd_chk_underway is set, which means the
		* ESD recovery is running, therefore after the panel is on
		* then check if the ESD detection can be recovered or not.
		* if it is not, then trigger the ESD again
		*/
		disp_esd_trigger++;
		if (dsi_display_status_check_te(display, 1) <= 0) {
			DSI_ERR("ESD: TE Check is still failed. disp_esd_trigger=%d\n",
						disp_esd_trigger);
		} else if (dsi_display_status_reg_read(display) <= 0) {
			DSI_ERR("ESD: REG_READ is still failed. disp_esd_trigger=%d\n",
						disp_esd_trigger);
		} else {
			display->disp_esd_chk_underway = false;
			DSI_INFO("ESD_MODE_TE_CHK_REG_RD is good\n");
			disp_esd_trigger = 0;
			display->disp_esd_chk_underway = false;
		}

		if (disp_esd_trigger > 0 && disp_esd_trigger < MAX_ESD_RECOVERY_RETRY) {
			DSI_WARN("ESD: disp_esd_trigger=%d, trigger ESD again\n");
			dsi_display_trigger_panel_dead_event(display);
		} else if (disp_esd_trigger >= MAX_ESD_RECOVERY_RETRY) {
			DSI_ERR("ESD: disp_esd_trigger=%d is Max, calling BUG\n",
										disp_esd_trigger);
			BUG();
		}
		ret = -EIO;
	} else
		ret = 0;

	return ret;
}

static int dsi_display_enable_status (struct dsi_display *display, bool enable)
{
	int ret = 0;

	if (!strncmp("DSI-1", display->drm_conn->name, 5)) {
		DSI_DEBUG("display->drm_conn->name=%s enable = %d MAIN_DISPLAY\n",
			display->drm_conn->name, enable);
		enable_status[0].display = display;
		enable_status[0].enable = enable;
	} else if (!strncmp("DSI-2", display->drm_conn->name, 5)) {
		DSI_DEBUG("display->drm_conn->name=%s enable = %d CLI_DISPLAY\n",
			display->drm_conn->name, enable);
		enable_status[1].display = display;
		enable_status[1].enable = enable;
	} else {
		DSI_ERR("display->drm_conn: invalid name =%s\n",
					display->drm_conn->name);
		ret =  -EINVAL;
	}

	return ret;
}

bool dsi_display_is_panel_enable (int panel_index, int *probe_status,
							char **pname)
{
	struct dsi_display *display;
	bool enable = false;

	if (panel_index > 1) {
		pr_err("display->drm_conn: invalid panel index =%d\n",
								panel_index);
		if (probe_status)
			*probe_status = -ENODEV;
		return false;
	}

	if (probe_status)
		*probe_status = enable_status[panel_index].probed;
	if (pname)
		*pname = &enable_status[panel_index].pname[0];

	display = enable_status[panel_index].display;
	if (display) {
		mutex_lock(&display->display_lock);
		enable = enable_status[panel_index].enable;
		mutex_unlock(&display->display_lock);
	}

	return enable;
}
EXPORT_SYMBOL(dsi_display_is_panel_enable);

int dsi_display_enable(struct dsi_display *display)
{
	int rc = 0;
	struct dsi_display_mode *mode;

	if (!display || !display->panel) {
		DSI_ERR("Invalid params\n");
		return -EINVAL;
	}

	if (!display->panel->cur_mode) {
		DSI_ERR("no valid mode set for the display\n");
		return -EINVAL;
	}
	SDE_EVT32(SDE_EVTLOG_FUNC_ENTRY);

	/*
	 * Engine states and panel states are populated during splash
	 * resource/trusted vm and hence we return early
	 */
	if (is_skip_op_required(display)) {

		dsi_display_config_ctrl_for_cont_splash(display);

		rc = dsi_display_splash_res_cleanup(display);
		if (rc) {
			DSI_ERR("Continuous splash res cleanup failed, rc=%d\n",
				rc);
			return -EINVAL;
		}

		dsi_display_enable_status(display, 1);
		display->panel->panel_initialized = true;
		DSI_DEBUG("cont splash enabled, display enable not required\n");
		dsi_display_panel_id_notification(display);

		return 0;
	}

	mutex_lock(&display->display_lock);

	mode = display->panel->cur_mode;

	if (mode->dsi_mode_flags & DSI_MODE_FLAG_DMS) {
		rc = dsi_panel_post_switch(display->panel);
		if (rc) {
			DSI_ERR("[%s] failed to switch DSI panel mode, rc=%d\n",
				   display->name, rc);
			goto error;
		}
	} else if (!(display->panel->cur_mode->dsi_mode_flags &
			DSI_MODE_FLAG_POMS)){
		rc = dsi_panel_enable(display->panel);
		if (rc) {
			DSI_ERR("[%s] failed to enable DSI panel, rc=%d\n",
			       display->name, rc);
			goto error;
		}
		dsi_panel_reset_param(display->panel);

		if (display->panel->dfps_caps.dfps_send_cmd_support) {
			display->panel->dfps_caps.current_fps = display->panel->dfps_caps.panel_on_fps;
			if (mode->timing.refresh_rate != display->panel->dfps_caps.panel_on_fps) {
				DSI_INFO("[%s] dst_refresh_rate %d panel_on_fps %d\n", __func__, mode->timing.refresh_rate, display->panel->dfps_caps.panel_on_fps);
				dsi_panel_dfps_send_cmd(display->panel);
			}
		}

		dsi_display_enable_status(display, true);
	}
	dsi_display_panel_id_notification(display);
	/* Block sending pps command if modeset is due to fps difference */
	if ((mode->priv_info->dsc_enabled ||
			mode->priv_info->vdc_enabled) &&
		!(mode->dsi_mode_flags & DSI_MODE_FLAG_DMS_FPS)) {
		if(!mode->priv_info->panel_dsc_update_pps_disable){
		  	rc = dsi_panel_update_pps(display->panel);
		  	if (rc) {
				DSI_ERR("[%s] panel pps cmd update failed, rc=%d\n",
					display->name, rc);
				goto error;
			}
		}
	}

	if (mode->dsi_mode_flags & DSI_MODE_FLAG_DMS) {
		rc = dsi_panel_switch(display->panel);
		if (rc)
			DSI_ERR("[%s] failed to switch DSI panel mode, rc=%d\n",
				   display->name, rc);

		goto error;
	}

	if (display->config.panel_mode == DSI_OP_VIDEO_MODE) {
		DSI_DEBUG("%s:enable video timing eng\n", __func__);
		rc = dsi_display_vid_engine_enable(display);
		if (rc) {
			DSI_ERR("[%s]failed to enable DSI video engine, rc=%d\n",
			       display->name, rc);
			goto error_disable_panel;
		}
	} else if (display->config.panel_mode == DSI_OP_CMD_MODE) {
		DSI_DEBUG("%s:enable command timing eng\n", __func__);
		rc = dsi_display_cmd_engine_enable(display);
		if (rc) {
			DSI_ERR("[%s]failed to enable DSI cmd engine, rc=%d\n",
			       display->name, rc);
			goto error_disable_panel;
		}
	} else {
		DSI_ERR("[%s] Invalid configuration\n", display->name);
		rc = -EINVAL;
		goto error_disable_panel;
	}

	if (display->disp_esd_chk_underway)
		if (dsi_display_chk_esd_recovery(display))
			goto error;

	goto error;

error_disable_panel:
	(void)dsi_panel_disable(display->panel);
error:
	mutex_unlock(&display->display_lock);
	SDE_EVT32(SDE_EVTLOG_FUNC_EXIT);
	return rc;
}

int dsi_display_post_enable(struct dsi_display *display)
{
	int rc = 0;

	if (!display) {
		DSI_ERR("Invalid params\n");
		return -EINVAL;
	}

	mutex_lock(&display->display_lock);

	if (display->panel->panel_hbm_dim_off)
		dsi_display_read_elvss_volt(display);

	if (display->panel->cur_mode->dsi_mode_flags & DSI_MODE_FLAG_POMS) {
		if (display->config.panel_mode == DSI_OP_CMD_MODE)
			dsi_panel_mode_switch_to_cmd(display->panel);

		if (display->config.panel_mode == DSI_OP_VIDEO_MODE)
			dsi_panel_mode_switch_to_vid(display->panel);
	} else {
		rc = dsi_panel_post_enable(display->panel);
		if (rc)
			DSI_ERR("[%s] panel post-enable failed, rc=%d\n",
				display->name, rc);
	}

	/* remove the clk vote for CMD mode panels */
	if (display->config.panel_mode == DSI_OP_CMD_MODE)
		dsi_display_clk_ctrl(display->dsi_clk_handle,
			DSI_ALL_CLKS, DSI_CLK_OFF);

	mutex_unlock(&display->display_lock);
	return rc;
}

int dsi_display_pre_disable(struct dsi_display *display)
{
	int rc = 0;

	if (!display) {
		DSI_ERR("Invalid params\n");
		return -EINVAL;
	}

	mutex_lock(&display->display_lock);

	/* enable the clk vote for CMD mode panels */
	if (display->config.panel_mode == DSI_OP_CMD_MODE)
		dsi_display_clk_ctrl(display->dsi_clk_handle,
			DSI_ALL_CLKS, DSI_CLK_ON);
	if (display->poms_pending) {
		if (display->config.panel_mode == DSI_OP_CMD_MODE)
			dsi_panel_pre_mode_switch_to_video(display->panel);

		if (display->config.panel_mode == DSI_OP_VIDEO_MODE) {
			/*
			 * Add unbalanced vote for clock & cmd engine to enable
			 * async trigger of pre video to cmd mode switch.
			 */
			rc = dsi_display_clk_ctrl(display->dsi_clk_handle,
					DSI_ALL_CLKS, DSI_CLK_ON);
			if (rc) {
				DSI_ERR("[%s]failed to enable all clocks,rc=%d",
						display->name, rc);
				goto exit;
			}

			rc = dsi_display_cmd_engine_enable(display);
			if (rc) {
				DSI_ERR("[%s]failed to enable cmd engine,rc=%d",
						display->name, rc);
				goto error_disable_clks;
			}

			dsi_panel_pre_mode_switch_to_cmd(display->panel);
		}
	} else {
		rc = dsi_panel_pre_disable(display->panel);
		if (rc)
			DSI_ERR("[%s] panel pre-disable failed, rc=%d\n",
				display->name, rc);
	}
	goto exit;

error_disable_clks:
	rc = dsi_display_clk_ctrl(display->dsi_clk_handle,
			DSI_ALL_CLKS, DSI_CLK_OFF);
	if (rc)
		DSI_ERR("[%s] failed to disable all DSI clocks, rc=%d\n",
		       display->name, rc);

exit:
	mutex_unlock(&display->display_lock);
	return rc;
}

static void dsi_display_handle_poms_te(struct work_struct *work)
{
	struct dsi_display *display = NULL;
	struct delayed_work *dw = to_delayed_work(work);
	struct mipi_dsi_device *dsi = NULL;
	struct dsi_panel *panel = NULL;
	int rc = 0;

	display = container_of(dw, struct dsi_display, poms_te_work);
	if (!display || !display->panel) {
		DSI_ERR("Invalid params\n");
		return;
	}

	panel = display->panel;
	mutex_lock(&panel->panel_lock);
	if (!dsi_panel_initialized(panel)) {
		rc = -EINVAL;
		goto error;
	}

	dsi = &panel->mipi_device;
	rc = mipi_dsi_dcs_set_tear_off(dsi);

error:
	mutex_unlock(&panel->panel_lock);
	if (rc < 0)
		DSI_ERR("failed to set tear off\n");
}

int dsi_display_disable(struct dsi_display *display)
{
	int rc = 0;

	if (!display) {
		DSI_ERR("Invalid params\n");
		return -EINVAL;
	}

	DSI_INFO("%s(%s)+\n", __func__, display->drm_conn->name);
	SDE_EVT32(SDE_EVTLOG_FUNC_ENTRY);
	mutex_lock(&display->display_lock);

	/* cancel delayed work */
	if (display->poms_pending &&
			display->panel->poms_align_vsync)
		cancel_delayed_work_sync(&display->poms_te_work);

	rc = dsi_display_wake_up(display);
	if (rc)
		DSI_ERR("[%s] display wake up failed, rc=%d\n",
		       display->name, rc);
	else
		dsi_display_enable_status(display, false);

	if (display->config.panel_mode == DSI_OP_VIDEO_MODE) {
		rc = dsi_display_vid_engine_disable(display);
		if (rc)
			DSI_ERR("[%s]failed to disable DSI vid engine, rc=%d\n",
			       display->name, rc);
	} else if (display->config.panel_mode == DSI_OP_CMD_MODE) {
		/**
		 * On POMS request , disable panel TE through
		 * delayed work queue.
		 */
		if (display->poms_pending &&
				display->panel->poms_align_vsync) {
			INIT_DELAYED_WORK(&display->poms_te_work,
					dsi_display_handle_poms_te);
			queue_delayed_work(system_wq,
					&display->poms_te_work,
					msecs_to_jiffies(100));
		}
		rc = dsi_display_cmd_engine_disable(display);
		if (rc)
			DSI_ERR("[%s]failed to disable DSI cmd engine, rc=%d\n",
			       display->name, rc);
	} else {
		DSI_ERR("[%s] Invalid configuration\n", display->name);
		rc = -EINVAL;
	}

	if (!display->poms_pending && !is_skip_op_required(display)) {
		rc = dsi_panel_disable(display->panel);
		if (rc)
			DSI_ERR("[%s] failed to disable DSI panel, rc=%d\n",
				display->name, rc);
	}

	if (is_skip_op_required(display)) {
		/* applicable only for trusted vm */
		display->panel->panel_initialized = false;
		display->panel->power_mode = SDE_MODE_DPMS_OFF;
	}
	mutex_unlock(&display->display_lock);
	SDE_EVT32(SDE_EVTLOG_FUNC_EXIT);
	return rc;
}

int dsi_display_update_pps(char *pps_cmd, void *disp)
{
	struct dsi_display *display;

	if (pps_cmd == NULL || disp == NULL) {
		DSI_ERR("Invalid parameter\n");
		return -EINVAL;
	}

	display = disp;
	mutex_lock(&display->display_lock);
	memcpy(display->panel->dce_pps_cmd, pps_cmd, DSI_CMD_PPS_SIZE);
	mutex_unlock(&display->display_lock);

	return 0;
}

int dsi_display_dump_clks_state(struct dsi_display *display)
{
	int rc = 0;

	if (!display) {
		DSI_ERR("invalid display argument\n");
		return -EINVAL;
	}

	if (!display->clk_mngr) {
		DSI_ERR("invalid clk manager\n");
		return -EINVAL;
	}

	if (!display->dsi_clk_handle || !display->mdp_clk_handle) {
		DSI_ERR("invalid clk handles\n");
		return -EINVAL;
	}

	mutex_lock(&display->display_lock);
	rc = dsi_display_dump_clk_handle_state(display->dsi_clk_handle);
	if (rc) {
		DSI_ERR("failed to dump dsi clock state\n");
		goto end;
	}

	rc = dsi_display_dump_clk_handle_state(display->mdp_clk_handle);
	if (rc) {
		DSI_ERR("failed to dump mdp clock state\n");
		goto end;
	}

end:
	mutex_unlock(&display->display_lock);

	return rc;
}

int dsi_display_unprepare(struct dsi_display *display)
{
	int rc = 0, i;
	struct dsi_display_ctrl *ctrl;

	if (!display) {
		DSI_ERR("Invalid params\n");
		return -EINVAL;
	}

	SDE_EVT32(SDE_EVTLOG_FUNC_ENTRY);
	mutex_lock(&display->display_lock);

	rc = dsi_display_wake_up(display);
	if (rc)
		DSI_ERR("[%s] display wake up failed, rc=%d\n",
		       display->name, rc);
	if (!display->poms_pending && !is_skip_op_required(display)) {
		rc = dsi_panel_unprepare(display->panel);
		if (rc)
			DSI_ERR("[%s] panel unprepare failed, rc=%d\n",
			       display->name, rc);
	}

	/* Remove additional vote added for pre_mode_switch_to_cmd */
	if (display->poms_pending &&
			display->config.panel_mode == DSI_OP_VIDEO_MODE) {
		display_for_each_ctrl(i, display) {
			ctrl = &display->ctrl[i];
			if (!ctrl->ctrl || !ctrl->ctrl->dma_wait_queued)
				continue;
			flush_workqueue(display->dma_cmd_workq);
			cancel_work_sync(&ctrl->ctrl->dma_cmd_wait);
			ctrl->ctrl->dma_wait_queued = false;
		}

		dsi_display_cmd_engine_disable(display);
		dsi_display_clk_ctrl(display->dsi_clk_handle,
				DSI_ALL_CLKS, DSI_CLK_OFF);
	}

	rc = dsi_display_ctrl_host_disable(display);
	if (rc)
		DSI_ERR("[%s] failed to disable DSI host, rc=%d\n",
		       display->name, rc);

	rc = dsi_display_clk_ctrl(display->dsi_clk_handle,
			DSI_LINK_CLK, DSI_CLK_OFF);
	if (rc)
		DSI_ERR("[%s] failed to disable Link clocks, rc=%d\n",
		       display->name, rc);

	/* set to dsi clocks to xo clocks */
	rc = dsi_display_unset_clk_src(display);
	if (rc)
		DSI_ERR("[%s] failed to unset clocks, rc=%d\n",
		       display->name, rc);

	rc = dsi_display_ctrl_deinit(display);
	if (rc)
		DSI_ERR("[%s] failed to deinit controller, rc=%d\n",
		       display->name, rc);

	if (!display->panel->ulps_suspend_enabled) {
		rc = dsi_display_phy_disable(display);
		if (rc)
			DSI_ERR("[%s] failed to disable DSI PHY, rc=%d\n",
			       display->name, rc);
	}

	rc = dsi_display_clk_ctrl(display->dsi_clk_handle,
			DSI_CORE_CLK, DSI_CLK_OFF);
	if (rc)
		DSI_ERR("[%s] failed to disable DSI clocks, rc=%d\n",
		       display->name, rc);

	/* destrory dsi isr set up */
	dsi_display_ctrl_isr_configure(display, false);

	if (!display->poms_pending && !is_skip_op_required(display)) {
		rc = dsi_panel_post_unprepare(display->panel);
		if (rc)
			DSI_ERR("[%s] panel post-unprepare failed, rc=%d\n",
			       display->name, rc);
	}
	display->hw_ownership = false;

	mutex_unlock(&display->display_lock);

	/* Free up DSI ERROR event callback */
	dsi_display_unregister_error_handler(display);

	SDE_EVT32(SDE_EVTLOG_FUNC_EXIT);
	return rc;
}

void __init dsi_display_register(void)
{
	dsi_phy_drv_register();
	dsi_ctrl_drv_register();

	dsi_display_parse_boot_display_selection();

	platform_driver_register(&dsi_display_driver);
}

void __exit dsi_display_unregister(void)
{
	platform_driver_unregister(&dsi_display_driver);
	dsi_ctrl_drv_unregister();
	dsi_phy_drv_unregister();
}
module_param_string(dsi_display0, dsi_display_primary, MAX_CMDLINE_PARAM_LEN,
								0600);
MODULE_PARM_DESC(dsi_display0,
	"msm_drm.dsi_display0=<display node>:<configX> where <display node> is 'primary dsi display node name' and <configX> where x represents index in the topology list");
module_param_string(dsi_display1, dsi_display_secondary, MAX_CMDLINE_PARAM_LEN,
								0600);
MODULE_PARM_DESC(dsi_display1,
	"msm_drm.dsi_display1=<display node>:<configX> where <display node> is 'secondary dsi display node name' and <configX> where x represents index in the topology list");
