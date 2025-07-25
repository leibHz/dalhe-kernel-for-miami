/*
 * aw882xx.c   aw882xx codec module
 *
 * Version: v0.1.18
 *
 * keep same with AW882XX_VERSION
 *
 * Copyright (c) 2019 AWINIC Technology CO., LTD
 *
 *  Author: Nick Li <liweilei@awinic.com.cn>
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 */
#ifdef CONFIG_AW882XX_CODEC

#include <linux/module.h>
#include <linux/i2c.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <linux/of_gpio.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/miscdevice.h>
#include <linux/device.h>
#include <linux/firmware.h>
#include <linux/i2c.h>
#include <linux/debugfs.h>
#include <linux/version.h>
#include <linux/input.h>
#include <linux/timer.h>
#include <linux/workqueue.h>
#include <linux/hrtimer.h>
#include <linux/syscalls.h>
#include <sound/tlv.h>
#include <linux/uaccess.h>
#include "aw882xx.h"
#include "aw882xx_reg.h"

/******************************************************
 *
 * Marco
 *
 ******************************************************/
#define AW882XX_I2C_NAME "aw882xx_smartpa"

#define AW882XX_VERSION "v0.1.18"

#define AW882XX_RATES SNDRV_PCM_RATE_8000_48000
#define AW882XX_FORMATS (SNDRV_PCM_FMTBIT_S16_LE | \
						SNDRV_PCM_FMTBIT_S24_LE | \
						SNDRV_PCM_FMTBIT_S32_LE)


#define AW_I2C_RETRIES				5	/* 5 times */
#define AW_I2C_RETRY_DELAY			5	/* 5 ms */
#define AW_READ_CHIPID_RETRIES		5	/* 5 times */
#define AW_READ_CHIPID_RETRY_DELAY	5	/* 5 ms */


#define DELAY_TIME_MAX 300
#define AWINIC_CALI_FILE  "aw_cali.bin"

static DEFINE_MUTEX(g_msg_dsp_lock);

#ifdef CONFIG_AW882XX_DSP
extern int aw_send_afe_cal_apr(uint32_t rx_port_id, uint32_t tx_port_id,
                        uint32_t param_id, void *buf, int cmd_size, bool write);
extern int aw_send_afe_rx_module_enable(uint32_t rx_port_id, void *buf, int cmd_size);
extern int aw_send_afe_tx_module_enable(uint32_t tx_port_id, void *buf, int cmd_size);
extern int aw_adm_param_enable(int port_id, int module_id, int param_id, int enable);
#else
static int aw_send_afe_cal_apr(uint32_t rx_port_id,uint32_t tx_port_id,
                        uint32_t param_id, void *buf, int cmd_size, bool write) {
    return 0;
}

static int aw_send_afe_rx_module_enable(uint32_t rx_port_id, void *buf, int cmd_size)
{
	return 0;
}

static int aw_send_afe_tx_module_enable(uint32_t tx_port_id, void *buf, int cmd_size)
{
	return 0;
}

static int aw_adm_param_enable(int port_id, int module_id, int param_id, int enable)
{
	return 0;
}
#endif

static int aw882xx_get_cali_re_from_nv(struct aw882xx *aw882xx, uint32_t *cali_re);
static int aw882xx_set_cali_re(struct aw882xx *aw882xx, uint32_t cali_re);
static int aw882xx_load_profile_params(struct aw882xx *aw882xx);
static int aw882xx_skt_set_dsp(int value);
static int aw882xx_send_profile_params_to_dsp(struct aw882xx *aw882xx, int profile_id, bool is_fade);
static void aw882xx_fade_in_out(struct aw882xx *aw882xx);
static void aw882xx_volume_set(struct aw882xx *aw882xx, unsigned int value);

/*monitor  voltage and temperature table*/
static struct aw882xx_low_vol vol_down_table[] = {
		{3500, IPEAK_2P50_A, GAIN_NEG_1P5_DB},
		{3700, IPEAK_2P75_A, GAIN_NEG_1P0_DB},
		{3900, IPEAK_3P00_A, GAIN_NEG_0P5_DB},
	};
static struct aw882xx_low_vol vol_up_table[] = {
		{4000, IPEAK_3P50_A, GAIN_NEG_0P0_DB},
		{3800, IPEAK_3P00_A, GAIN_NEG_0P5_DB},
		{3600, IPEAK_2P75_A, GAIN_NEG_1P0_DB},
	};
static struct aw882xx_low_temp temp_down_table[] = {
		{-5, IPEAK_2P50_A, GAIN_NEG_6P0_DB, VMAX_063_PERCENTAGE},
		{ 0, IPEAK_2P75_A, GAIN_NEG_4P5_DB, VMAX_075_PERCENTAGE},
		{ 5, IPEAK_3P00_A, GAIN_NEG_3P0_DB, VMAX_086_PERCENTAGE},
	};
static struct aw882xx_low_temp temp_up_table[] = {
		{ 7, IPEAK_3P50_A, GAIN_NEG_0P0_DB, VMAX_100_PERCENTAGE},
		{ 2, IPEAK_3P00_A, GAIN_NEG_3P0_DB, VMAX_086_PERCENTAGE},
		{-2, IPEAK_2P75_A, GAIN_NEG_4P5_DB, VMAX_075_PERCENTAGE},
	};

static int aw882xx_monitor_start(struct aw882xx_monitor *monitor);
static int aw882xx_monitor_stop(struct aw882xx_monitor *monitor);

/******************************************************
 *
 * Value
 *
 ******************************************************/
static int aw882xx_spk_control;
static int aw882xx_rcv_control;

#ifdef AW882XX_RUNIN_TEST
static atomic_t g_runin_test;
#endif
static atomic_t g_algo_rx_enable;
static atomic_t g_algo_tx_enable;
static atomic_t g_skt_disable;
static struct aw882xx *g_aw882xx = NULL;
static int8_t g_aw882xx_cali_flag = 0;
static int8_t g_aw882xx_profile_flag = 0;

#define AW882XX_CFG_NAME_MAX		64
static char aw882xx_cfg_name[][AW882XX_CFG_NAME_MAX] = {
	{"aw882xx_spk_reg.bin"},
	{"aw882xx_rcv_reg.bin"},
};

static unsigned int aw882xx_mode_cfg_shift[AW882XX_MODE_SHIFT_MAX] = {
	AW882XX_MODE_SPK_SHIFT,
	AW882XX_MODE_RCV_SHIFT,
};

/******************************************************
 *
 * aw882xx i2c write/read
 *
 ******************************************************/
static int aw882xx_i2c_writes(struct aw882xx *aw882xx,
	unsigned char reg_addr, unsigned char *buf, unsigned int len)
{
	int ret = -1;
	unsigned char *data;

	data = kmalloc(len+1, GFP_KERNEL);
	if (data == NULL) {
		pr_err("%s: can not allocate memory\n", __func__);
		return -ENOMEM;
	}

	data[0] = reg_addr;
	memcpy(&data[1], buf, len);

	ret = i2c_master_send(aw882xx->i2c, data, len+1);
	if (ret < 0)
		pr_err("%s: i2c master send error\n", __func__);

	kfree(data);

	return ret;
}

static int aw882xx_i2c_reads(struct aw882xx *aw882xx,
	unsigned char reg_addr, unsigned char *data_buf, unsigned int data_len)
{
	int ret;
	struct i2c_msg msg[] = {
		[0] = {
			.addr = aw882xx->i2c->addr,
			.flags = 0,
			.len = sizeof(uint8_t),
			.buf = &reg_addr,
			},
		[1] = {
			.addr = aw882xx->i2c->addr,
			.flags = I2C_M_RD,
			.len = data_len,
			.buf = data_buf,
			},
	};

	ret = i2c_transfer(aw882xx->i2c->adapter, msg, ARRAY_SIZE(msg));
	if (ret < 0) {
		pr_err("%s: i2c master send error, ret=%d\n",
			__func__, ret);
		return ret;
	} else if (ret != AW882XX_I2C_READ_MSG_NUM) {
		pr_err("%s: couldn't read registers, return %d bytes\n",
			__func__, ret);
		return -ENXIO;
	}

	return 0;
}

static int aw882xx_i2c_write(struct aw882xx *aw882xx,
	unsigned char reg_addr, unsigned int reg_data)
{
	int ret = -1;
	unsigned char cnt = 0;
	unsigned char buf[2];

	buf[0] = (reg_data&0xff00)>>8;
	buf[1] = (reg_data&0x00ff)>>0;
	while (cnt < AW_I2C_RETRIES) {
		ret = aw882xx_i2c_writes(aw882xx, reg_addr, buf, 2);
		if (ret < 0)
			pr_err("%s: i2c_write cnt=%d error=%d\n",
				__func__, cnt, ret);
		else
			break;
		cnt++;
	}

	return ret;
}

static int aw882xx_i2c_read(struct aw882xx *aw882xx,
	unsigned char reg_addr, unsigned int *reg_data)
{
	int ret = -1;
	unsigned char cnt = 0;
	unsigned char buf[2];

	while (cnt < AW_I2C_RETRIES) {
		ret = aw882xx_i2c_reads(aw882xx, reg_addr, buf, 2);
		if (ret < 0) {
			pr_err("%s: i2c_read cnt=%d error=%d\n",
				__func__, cnt, ret);
		} else {
			*reg_data = (buf[0]<<8) | (buf[1]<<0);
			break;
		}
		cnt++;
	}

	return ret;
}

static int aw882xx_i2c_write_bits(struct aw882xx *aw882xx,
	unsigned char reg_addr, unsigned int mask, unsigned int reg_data)
{
	int ret = -1;
	unsigned int reg_val = 0;

	ret = aw882xx_i2c_read(aw882xx, reg_addr, &reg_val);
	if (ret < 0) {
		pr_err("%s: i2c read error, ret=%d\n", __func__, ret);
		return ret;
	}

	reg_val &= mask;
	reg_val |= reg_data;
	ret = aw882xx_i2c_write(aw882xx, reg_addr, reg_val);
	if (ret < 0) {
		pr_err("%s: i2c read error, ret=%d\n", __func__, ret);
		return ret;
	}

	return 0;
}

/******************************************************
 *
 * aw882xx control
 *
 ******************************************************/
static void aw882xx_run_mute(struct aw882xx *aw882xx, bool mute)
{
	pr_debug("%s: enter\n", __func__);

	if (mute) {
		if (aw882xx->need_fade) {
			aw882xx->is_fade_in = 0;
			aw882xx->fade_work_start = 1;
			cancel_delayed_work_sync(&aw882xx->fade_work);
			aw882xx_fade_in_out(aw882xx);
		}
		aw882xx_i2c_write_bits(aw882xx, AW882XX_SYSCTRL2_REG,
				AW882XX_HMUTE_MASK,
				AW882XX_HMUTE_ENABLE_VALUE);
	} else {
		aw882xx_i2c_write_bits(aw882xx, AW882XX_SYSCTRL2_REG,
				AW882XX_HMUTE_MASK,
				AW882XX_HMUTE_DISABLE_VALUE);
		if (aw882xx->need_fade) {
			aw882xx->is_fade_in = 1;
			aw882xx->fade_work_start = 1;
			aw882xx_volume_set(aw882xx, VOLUME_MIN_NEG_90_DB);
			schedule_delayed_work(&aw882xx->fade_work,
							msecs_to_jiffies(aw882xx->delayed_time));
		}
	}
}

#if 0
static bool aw882xx_get_power_status(struct aw882xx *aw882xx)
{
	unsigned int reg_value = 0;
	int ret = 0;
	pr_debug("%s: enter\n", __func__);

	ret = aw882xx_i2c_read(aw882xx, AW882XX_SYSCTRL_REG, &reg_value);
	if (ret < 0) {
		pr_err("%s: read reg %d failed \n", __func__, AW882XX_SYSCTRL_REG);
		return false;
	}
	/*bit 0: 1 power off, 0 power on*/
	if (reg_value & 0x01) {
		return false;
	} else {
		return true;
	}
}
#endif

static void aw882xx_run_pwd(struct aw882xx *aw882xx, bool pwd)
{
	pr_debug("%s: enter\n", __func__);

	if (pwd) {
		aw882xx_i2c_write_bits(aw882xx, AW882XX_SYSCTRL_REG,
				AW882XX_PWDN_MASK,
				AW882XX_PWDN_POWER_DOWN_VALUE);
	} else {
		aw882xx_i2c_write_bits(aw882xx, AW882XX_SYSCTRL_REG,
				AW882XX_PWDN_MASK,
				AW882XX_PWDN_NORMAL_WORKING_VALUE);
	}
}

static int aw882xx_sysst_check(struct aw882xx *aw882xx)
{
	int ret = -1;
	unsigned char i;
	unsigned int reg_val = 0;

	for (i = 0; i < AW882XX_SYSST_CHECK_MAX; i++) {
		aw882xx_i2c_read(aw882xx, AW882XX_SYSST_REG, &reg_val);
		if (((reg_val & (~AW882XX_SYSST_CHECK_MASK)) & AW882XX_SYSST_CHECK) ==
			AW882XX_SYSST_CHECK) {
			ret = 0;
			break;
		} else {
			pr_debug("%s: check fail, cnt=%d, reg_val=0x%04x\n",
				__func__, i, reg_val);
			msleep(2);
		}
	}
	if (ret < 0)
		pr_info("%s: check fail\n", __func__);

	return ret;
}

/*
static int aw882xx_get_sysint(struct aw882xx *aw882xx, unsigned int *sysint)
{
	int ret = -1;
	unsigned int reg_val = 0;

	ret = aw882xx_i2c_read(aw882xx, AW882XX_SYSINT_REG, &reg_val);
	if (ret < 0)
		pr_info("%s: read sysint fail, ret=%d\n", __func__, ret);
	else
		*sysint = reg_val;

	return ret;
}

static int aw882xx_get_iis_status(struct aw882xx *aw882xx)
{
	int ret = -1;
	unsigned int reg_val = 0;

	pr_debug("%s: enter\n", __func__);

	aw882xx_i2c_read(aw882xx, AW882XX_SYSST_REG, &reg_val);
	if (reg_val & AW882XX_PLLS_LOCKED_VALUE)
		ret = 0;

	return ret;
}
*/

static int aw882xx_get_icalk(struct aw882xx *aw882xx, int16_t *icalk)
{
	int ret = -1;
	unsigned int reg_val = 0;
	uint16_t reg_icalk = 0;

	ret = aw882xx_i2c_read(aw882xx, AW882XX_EFRM1_REG, &reg_val);
	reg_icalk = (uint16_t)reg_val & AW882XX_EF_ISN_GESLP_MASK;

	if (reg_icalk & AW882XX_EF_ISN_GESLP_SIGN_MASK)
		reg_icalk = reg_icalk | AW882XX_EF_ISN_GESLP_NEG;

	*icalk = (int16_t)reg_icalk;

	return ret;
}

static int aw882xx_get_vcalk(struct aw882xx *aw882xx, int16_t *vcalk)
{
	int ret = -1;
	unsigned int reg_val = 0;
	uint16_t reg_vcalk = 0;

	ret = aw882xx_i2c_read(aw882xx, AW882XX_EFRH_REG, &reg_val);

	reg_vcalk = (uint16_t)reg_val & AW882XX_EF_VSN_GESLP_MASK;

	if (reg_vcalk & AW882XX_EF_VSN_GESLP_SIGN_MASK)
		reg_vcalk = reg_vcalk | AW882XX_EF_VSN_GESLP_NEG;

	*vcalk = (int16_t)reg_vcalk;

	return ret;
}

static int aw882xx_set_vcalb(struct aw882xx *aw882xx)
{
	int ret = -1;
	unsigned int reg_val;
	int vcalb;
	int icalk;
	int vcalk;
	int16_t icalk_val = 0;
	int16_t vcalk_val = 0;

	ret = aw882xx_get_icalk(aw882xx, &icalk_val);
	ret = aw882xx_get_vcalk(aw882xx, &vcalk_val);

	icalk = AW882XX_CABL_BASE_VALUE + AW882XX_ICABLK_FACTOR * icalk_val;
	vcalk = AW882XX_CABL_BASE_VALUE + AW882XX_VCABLK_FACTOR * vcalk_val;

	vcalb = AW882XX_VCAL_FACTOR * icalk / vcalk;

	reg_val = (unsigned int)vcalb;
	pr_debug("%s: icalk=%d, vcalk=%d, vcalb=%d, reg_val=%d\n",
		__func__, icalk, vcalk, vcalb, reg_val);

	ret = aw882xx_i2c_write(aw882xx, AW882XX_VTMCTRL3_REG, reg_val);

	return ret;
}

static void aw882xx_send_cali_re_to_dsp(struct aw882xx *aw882xx)
{
	int ret = 0;
	if((aw882xx != NULL) && (aw882xx->default_re == 0)) {
		ret = aw_send_afe_cal_apr(aw882xx->afe_rx_portid, aw882xx->afe_tx_portid,
						AFE_PARAM_ID_AWDSP_RX_RE_L,
						&aw882xx->default_re, sizeof(int32_t), false);
		pr_info("aw882xx: default_re:%d\n", aw882xx->default_re);
	}
	if ((aw882xx != NULL) && (aw882xx->cali_re != ERRO_CALI_VALUE)) {
		ret = aw_send_afe_cal_apr(aw882xx->afe_rx_portid, aw882xx->afe_tx_portid,
					AFE_PARAM_ID_AWDSP_RX_RE_L, &aw882xx->cali_re, sizeof(int32_t), true);
	}
	if (ret)
		pr_err("%s : set cali re to dsp failed 0x%x\n",
			__func__ , AFE_PARAM_ID_AWDSP_RX_RE_L);
}

static void aw882xx_start(struct aw882xx *aw882xx)
{
	int ret = -1;
	uint32_t cali_re;
	pr_debug("%s: enter\n", __func__);

	ret = aw882xx_get_cali_re_from_nv(aw882xx, &cali_re);
	if (ret < 0) {
		cali_re = ERRO_CALI_VALUE;
		pr_err("%s: use default vaule %d", __func__ , ERRO_CALI_VALUE);
	}
	ret = aw882xx_set_cali_re(aw882xx, cali_re);
	if (ret < 0)
		pr_err("%s: set cali re failed: %d\n", __func__, ret);

	mutex_lock(&aw882xx->lock);
	pr_info("%s: monitor is_enable %d,spk_rcv_mode %d\n",
		__func__, aw882xx->monitor.is_enable, aw882xx->spk_rcv_mode);
	if (aw882xx->monitor.is_enable &&
		(aw882xx->spk_rcv_mode == AW882XX_SPEAKER_MODE)) {
		aw882xx->monitor.first_entry = AW_FIRST_ENTRY;
		aw882xx->monitor.pre_vol = 0;
		aw882xx->monitor.vol_count = 0;
		schedule_work(&aw882xx->monitor.work);
	}
	if (aw882xx->afe_profile) {
		ret = aw882xx_send_profile_params_to_dsp(aw882xx, aw882xx->profile.cur_profile, false);
		if (ret) {
			pr_err("%s: set profile %d failed \n", __func__,aw882xx->profile.cur_profile);
			aw882xx->profile.cur_profile = 0;
		}
	}
	aw882xx_send_cali_re_to_dsp(aw882xx);

	aw882xx_run_pwd(aw882xx, false);
	ret = aw882xx_sysst_check(aw882xx);
	if (ret < 0) {
		aw882xx_run_mute(aw882xx, true);
		aw882xx_run_pwd(aw882xx, true);
		aw882xx->init = AW882XX_INIT_NG;
		aw882xx->power_flag = AW882XX_POWER_DOWN;
	} else {
		aw882xx_run_mute(aw882xx, false);
		aw882xx->init = AW882XX_INIT_OK;
		aw882xx->power_flag = AW882XX_POWER_UP;
	}
#ifdef AW882XX_RUNIN_TEST
	schedule_delayed_work(&aw882xx->adsp_status, msecs_to_jiffies(50));
#endif
	mutex_unlock(&aw882xx->lock);
}

static void aw882xx_stop(struct aw882xx *aw882xx)
{
	pr_debug("%s: enter\n", __func__);

	mutex_lock(&aw882xx->lock);
	aw882xx->power_flag = AW882XX_POWER_DOWN;
	aw882xx_run_mute(aw882xx, true);
	aw882xx_run_pwd(aw882xx, true);
	if (aw882xx->afe_profile) {
		aw882xx->profile.cur_profile = 0;
	}
	if (aw882xx->monitor.is_enable)
		aw882xx_monitor_stop(&aw882xx->monitor);
	mutex_unlock(&aw882xx->lock);
}

/******************************************************
 *
 * aw882xx config
 *
 ******************************************************/
static int aw882xx_get_cali_re_from_nv(struct aw882xx *aw882xx, uint32_t *cali_re)
{
	/*custom add, if success return value is 0 , else -1*/
	int rc = -EINVAL;
	uint32_t read_re;
	const struct firmware *fw = NULL;

	/*open cali file*/
	if (request_firmware(&fw, AWINIC_CALI_FILE, aw882xx->dev)) {
		pr_err("%s: open %s failed!", __func__, AWINIC_CALI_FILE);
		return rc;
	}

	if (!fw || !fw->data || !fw->size) {
		pr_err("%s: invalid firmware", __func__);
		goto error;
	}

	/*get cali re value*/
	if (sscanf(fw->data, "%d", &read_re) != 1) {
		pr_err("%s: file read error", __func__);
		goto error;
	}

	*cali_re = read_re;
	pr_info("%s: %d", __func__, read_re);
	rc = 0;

error:
	/*close file*/
	release_firmware(fw);
	return rc;
}

static int aw882xx_set_cali_re(struct aw882xx *aw882xx, uint32_t cali_re)
{
	if (aw882xx == NULL)
		return -EINVAL;
	aw882xx->cali_re = cali_re;
	return 0;
}

static int aw882xx_reg_container_update(struct aw882xx *aw882xx,
	struct aw882xx_container *aw882xx_cont)
{
	int i = 0;
	int reg_addr = 0;
	int reg_val = 0;
	int ret = -1;

	pr_debug("%s: enter\n", __func__);

	for (i = 0; i < aw882xx_cont->len; i += 4) {
		reg_addr = (aw882xx_cont->data[i+1]<<8) +
			aw882xx_cont->data[i+0];
		reg_val = (aw882xx_cont->data[i+3]<<8) +
			aw882xx_cont->data[i+2];
		pr_debug("%s: reg=0x%04x, val = 0x%04x\n",
			__func__, reg_addr, reg_val);
		ret = aw882xx_i2c_write(aw882xx,
			(unsigned char)reg_addr,
			(unsigned int)reg_val);
		if (ret < 0)
			break;
	}

	pr_debug("%s: exit\n", __func__);

	return ret;
}

static void aw882xx_reg_loaded(const struct firmware *cont, void *context)
{
	struct aw882xx *aw882xx = context;
	struct aw882xx_container *aw882xx_cfg;
	int ret = -1;

	if (!cont) {
		pr_err("%s: failed to read %s\n", __func__,
			aw882xx_cfg_name[aw882xx->cfg_num]);
		release_firmware(cont);
		return;
	}

	pr_info("%s: loaded %s - size: %zu\n", __func__,
		aw882xx_cfg_name[aw882xx->cfg_num], cont ? cont->size : 0);

	aw882xx_cfg = kzalloc(cont->size+sizeof(int), GFP_KERNEL);
	if (!aw882xx_cfg) {
		release_firmware(cont);
		pr_err("%s: error allocating memory\n", __func__);
		return;
	}
	aw882xx_cfg->len = cont->size;
	memcpy(aw882xx_cfg->data, cont->data, cont->size);
	release_firmware(cont);

	mutex_lock(&aw882xx->lock);
	ret = aw882xx_reg_container_update(aw882xx, aw882xx_cfg);
	if (ret < 0) {
		pr_err("%s: reg update fail\n", __func__);
	} else {
		pr_err("%s: reg update sucess\n", __func__);
		//aw882xx_run_mute(aw882xx, true);
		aw882xx_volume_set(aw882xx, VOLUME_MIN_NEG_90_DB);
		aw882xx_i2c_write_bits(aw882xx, AW882XX_SYSCTRL2_REG,
				AW882XX_HMUTE_MASK,
				AW882XX_HMUTE_ENABLE_VALUE);
		aw882xx_set_vcalb(aw882xx);
	}
	mutex_unlock(&aw882xx->lock);
	kfree(aw882xx_cfg);
	if (aw882xx->afe_profile) {
		aw882xx_load_profile_params(aw882xx);
	}
	aw882xx->need_fade = 0;
	aw882xx_start(aw882xx);
	if(aw882xx->afe_profile || aw882xx->fade_flag) {
		aw882xx->need_fade = 1;
	}
}

static int aw882xx_load_reg(struct aw882xx *aw882xx)
{
	pr_info("%s: enter\n", __func__);

	return request_firmware_nowait(THIS_MODULE, FW_ACTION_HOTPLUG,
		aw882xx_cfg_name[aw882xx->cfg_num],
		aw882xx->dev, GFP_KERNEL,
		aw882xx, aw882xx_reg_loaded);
}

static void aw882xx_get_cfg_shift(struct aw882xx *aw882xx)
{
	aw882xx->cfg_num = aw882xx_mode_cfg_shift[aw882xx->spk_rcv_mode];
	pr_debug("%s: cfg_num=%d\n", __func__, aw882xx->cfg_num);
}

static void aw882xx_cold_start(struct aw882xx *aw882xx)
{
	int ret = -1;

	pr_info("%s: enter\n", __func__);

	aw882xx_get_cfg_shift(aw882xx);

	ret = aw882xx_load_reg(aw882xx);
	if (ret < 0)
		pr_err("%s: cfg loading requested failed: %d\n", __func__, ret);

}

static void aw882xx_smartpa_cfg(struct aw882xx *aw882xx, bool flag)
{
	pr_info("%s: flag = %d\n", __func__, flag);

	if (flag == true) {
		if ((aw882xx->init == AW882XX_INIT_ST) ||
			(aw882xx->init == AW882XX_INIT_NG)) {
			pr_info("%s: init = %d\n", __func__, aw882xx->init);
			aw882xx_cold_start(aw882xx);
		} else {
			aw882xx_start(aw882xx);
		}
	} else {
		aw882xx_stop(aw882xx);
	}
}

/******************************************************
 *
 * kcontrol
 *
 ******************************************************/
static const char *const switch_status[] = { "Off", "On" };
static const char *const awinic_algo[] = { "Disable", "Enable" };
static const char *const awinic_profile[AW_PROFILE_MAX] = {
"Music", "Ringtone", "Notification", "Voice"};
static const char *const awinic_profile_param_file_name[] = {
		"aw882xx_afe_params.bin",
	};

static const DECLARE_TLV_DB_SCALE(digital_gain, 0, 50, 0);

struct soc_mixer_control aw882xx_mixer = {
	.reg	= AW882XX_HAGCCFG4_REG,
	.shift	= AW882XX_VOL_START_BIT,
	.max	= AW882XX_VOLUME_MAX,
	.min	= AW882XX_VOLUME_MIN,
};

static int aw882xx_volume_info(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_info *uinfo)
{
	struct soc_mixer_control *mc =
		(struct soc_mixer_control *)kcontrol->private_value;

	/* set kcontrol info */
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 1;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = mc->max - mc->min;
	return 0;
}

static int aw882xx_volume_get(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *codec = snd_soc_kcontrol_component(kcontrol);
	struct aw882xx *aw882xx = snd_soc_component_get_drvdata(codec);
	unsigned int reg_val = 0;
	unsigned int value = 0;
	struct soc_mixer_control *mc =
		(struct soc_mixer_control *) kcontrol->private_value;

	aw882xx_i2c_read(aw882xx, AW882XX_HAGCCFG4_REG, &reg_val);
	ucontrol->value.integer.value[0] = (value >> mc->shift) &
		(AW882XX_VOL_MASK);
	return 0;
}

static int aw882xx_volume_put(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct soc_mixer_control *mc =
		(struct soc_mixer_control *) kcontrol->private_value;
	struct snd_soc_component *codec = snd_soc_kcontrol_component(kcontrol);
	struct aw882xx *aw882xx = snd_soc_component_get_drvdata(codec);
	unsigned int value = 0;
	unsigned int reg_value = 0;

	/* value is right */
	value = ucontrol->value.integer.value[0];
	if (value > (mc->max-mc->min) || value < 0) {
		pr_err("%s: value over range\n", __func__);
		return -ERANGE;
	}

	/* smartpa have clk */
	aw882xx_i2c_read(aw882xx, AW882XX_SYSST_REG, &reg_value);
	if (!(reg_value & AW882XX_PLLS_LOCKED_VALUE)) {
		pr_err("%s: NO I2S CLK ,cat not write reg\n", __func__);
		return 0;
	}

	/* cal real value */
	value = (value << mc->shift) & AW882XX_VOL_MASK;
	aw882xx_i2c_read(aw882xx, AW882XX_HAGCCFG4_REG, &reg_value);
	value = value | (reg_value & 0x00ff);

	/* write value */
	aw882xx->cur_gain = value >> AW882XX_BIT_HAGCCFG4_GAIN_SHIFT;
	aw882xx_i2c_write(aw882xx, AW882XX_HAGCCFG4_REG, value);

	return 0;
}

static struct snd_kcontrol_new aw882xx_volume = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "aw882xx_rx_volume",
	.access = SNDRV_CTL_ELEM_ACCESS_TLV_READ |
		SNDRV_CTL_ELEM_ACCESS_READWRITE,
	.tlv.p = (digital_gain),
	.info = aw882xx_volume_info,
	.get = aw882xx_volume_get,
	.put = aw882xx_volume_put,
	.private_value = (unsigned long)&aw882xx_mixer,
};

static int aw882xx_spk_get(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	pr_debug("%s: aw882xx_spk_control=%d\n",
		__func__, aw882xx_spk_control);
	ucontrol->value.integer.value[0] = aw882xx_spk_control;
	return 0;
}

static int aw882xx_spk_set(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *codec = snd_soc_kcontrol_component(kcontrol);
	struct aw882xx *aw882xx = snd_soc_component_get_drvdata(codec);

	pr_debug("%s: ucontrol->value.integer.value[0]=%ld\n",
		__func__, ucontrol->value.integer.value[0]);

	if (ucontrol->value.integer.value[0] == aw882xx_spk_control)
		return 1;

	aw882xx_spk_control = ucontrol->value.integer.value[0];

	if (AW882XX_SPEAKER_MODE != aw882xx->spk_rcv_mode) {
		aw882xx->spk_rcv_mode = AW882XX_SPEAKER_MODE;
		aw882xx->init = AW882XX_INIT_ST;
	}

	return 0;
}

static int aw882xx_rcv_get(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	pr_debug("%s: aw882xx_rcv_control=%d\n", __func__, aw882xx_rcv_control);
	ucontrol->value.integer.value[0] = aw882xx_rcv_control;
	return 0;
}

static int aw882xx_rcv_set(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *codec = snd_soc_kcontrol_component(kcontrol);
	struct aw882xx *aw882xx = snd_soc_component_get_drvdata(codec);

	pr_debug("%s: ucontrol->value.integer.value[0]=%ld\n",
		__func__, ucontrol->value.integer.value[0]);

	if (ucontrol->value.integer.value[0] == aw882xx_rcv_control)
		return 1;

	aw882xx_rcv_control = ucontrol->value.integer.value[0];

	if (AW882XX_RECEIVER_MODE != aw882xx->spk_rcv_mode) {
		aw882xx->spk_rcv_mode = AW882XX_RECEIVER_MODE;
		aw882xx->init = AW882XX_INIT_ST;
	}

	return 0;
}

static int aw882xx_algo_get(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	pr_debug("%s: aw882xx_algo enable=%d\n",
		__func__, atomic_read(&g_algo_rx_enable));

	ucontrol->value.integer.value[0] = atomic_read(&g_algo_rx_enable);

	return 0;
}

static int aw882xx_algo_set(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	int ret = -EINVAL;
	uint32_t ctrl_value = 0;

	pr_debug("%s: ucontrol->value.integer.value[0]=%ld\n",
		__func__, ucontrol->value.integer.value[0]);

	ctrl_value = ucontrol->value.integer.value[0];
	ret = aw_send_afe_rx_module_enable(g_aw882xx->afe_rx_portid, &ctrl_value, sizeof(uint32_t));
	if (ret)
		pr_err("%s: set algo %d failed, ret=%d\n",
			__func__, ctrl_value, ret);
	atomic_set(&g_algo_rx_enable, ctrl_value);
	return 0;
}

static int aw882xx_tx_get(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	pr_debug("%s: aw882xx_tx_control=%d\n", __func__,
		atomic_read(&g_algo_tx_enable));

	ucontrol->value.integer.value[0] = atomic_read(&g_algo_tx_enable);

	return 0;
}

static int aw882xx_tx_set(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	int ret = -EINVAL;
	uint32_t ctrl_value = 0;

	pr_debug("%s: ucontrol->value.integer.value[0]=%ld\n",
		__func__, ucontrol->value.integer.value[0]);

	ctrl_value = ucontrol->value.integer.value[0];
	ret = aw_send_afe_tx_module_enable(g_aw882xx->afe_tx_portid, &ctrl_value, sizeof(uint32_t));
	if (ret)
		pr_err("%s: set tx enable %d, ret=%d\n", __func__, ctrl_value, ret);
	atomic_set(&g_algo_tx_enable, ctrl_value);
	return 0;
}

static int aw882xx_profile_get(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *codec = snd_soc_kcontrol_component(kcontrol);
	struct aw882xx *aw882xx = snd_soc_component_get_drvdata(codec);

	pr_debug("%s: profile : %s \n", __func__,
			awinic_profile[aw882xx->profile.cur_profile]);
	if (aw882xx->afe_profile) {
		ucontrol->value.integer.value[0] = aw882xx->profile.cur_profile;
	}
	return 0;
}
static int aw882xx_profile_firmware_parse(struct aw882xx *aw882xx, const struct firmware *cont)
{
	aw_afe_params_hdr_t *hdr;
	char *fw_data = NULL;
	uint8_t sum = 0;
	uint32_t param_size = 0;
	char *param_data = NULL;
	int i;
	pr_info("%s: loaded %s - size: %zu\n", __func__,
		awinic_profile_param_file_name[0], cont ? cont->size : 0);

	if (cont->size == 0) {
		goto load_failed;
	}

	fw_data = kzalloc(cont->size, GFP_KERNEL);
	if (fw_data == NULL) {
		pr_err("%s: alloc memory failed \n", __func__);
		goto load_failed;
	}
	memcpy(fw_data, cont->data, cont->size);

	/*output bin info*/
	hdr = (aw_afe_params_hdr_t *)fw_data;
	pr_info("%s: fw v_%d.%d.%d \n", __func__,
		hdr->fw[0], hdr->fw[1], hdr->fw[2]);
	pr_info("%s: param v_%d.%d.%d \n", __func__,
		hdr->cfg[0], hdr->cfg[1], hdr->cfg[2]);
	pr_info("%s: project name : %s \n", __func__, hdr->project);
	pr_info("%s: param len: %d", __func__, hdr->params_len);

	param_data = fw_data + sizeof(aw_afe_params_hdr_t);
	param_size = cont->size - sizeof(aw_afe_params_hdr_t);

	for (i = 0; i < param_size; i++) {
		sum += param_data[i];
	}
	if (sum != hdr->check_sum) {
		pr_err("%s:data check sum failed : %d -- %d \n", __func__,
			sum, hdr->check_sum);
		kfree(fw_data);
		goto load_failed;
	}

	if (hdr->profile_num <  AW_PROFILE_MAX) {
		pr_err("%s: profile num is not match %d -- %d ", __func__,
			hdr->profile_num, AW_PROFILE_MAX);
		kfree(fw_data);
		goto load_failed;
	}

	if (hdr->params_len != (param_size / hdr->profile_num)) {
		pr_err("%s: profile params len  is not match %d ", __func__,
			hdr->params_len);
		kfree(fw_data);
		goto load_failed;
	}

	mutex_lock(&aw882xx->profile.lock);
	param_data = fw_data + sizeof(aw_afe_params_hdr_t);
	for (i = 0; i < AW_PROFILE_MAX; i++) {
		aw882xx->profile.data[i] =  kzalloc(hdr->params_len, GFP_KERNEL);
		if (aw882xx->profile.data[i] == NULL) {
			pr_err("%s: alloc failed !", __func__);
			kfree(fw_data);
			mutex_unlock(&aw882xx->profile.lock);
			goto load_failed;
		}
		memcpy(aw882xx->profile.data[i], param_data , hdr->params_len);
		param_data += hdr->params_len;
	}
	aw882xx->profile.len = hdr->params_len;
	aw882xx->profile.status = AW882XX_INIT_OK;
	mutex_unlock(&aw882xx->profile.lock);
	kfree(fw_data);
	return 0;

load_failed:
	mutex_lock(&aw882xx->profile.lock);
	for (i = 0; i < AW_PROFILE_MAX; i++) {
		if (aw882xx->profile.data[i]) {
			kfree(aw882xx->profile.data[i]);
			aw882xx->profile.data[i] = NULL;
		}
	}
	aw882xx->profile.status = AW882XX_INIT_NG;
	aw882xx->profile.len = 0;
	mutex_unlock(&aw882xx->profile.lock);
	pr_err("%s: load params bin failed !", __func__);
	return 0;
}

static int aw882xx_load_profile_params(struct aw882xx *aw882xx)
{
	const struct firmware *fw = NULL;
	int ret;
	pr_info("%s: enter\n", __func__);

	ret = request_firmware(&fw,
		awinic_profile_param_file_name[0],
		aw882xx->dev);
	if (ret) {
		pr_err("%s: load profile params failed !", __func__);
		return -EINVAL;
	}

	ret = aw882xx_profile_firmware_parse(aw882xx, fw);
	return ret;
}

static void aw882xx_volume_set(struct aw882xx *aw882xx, unsigned int value)
{
	unsigned int reg_value = 0;
	unsigned int real_value = ((value / VOLUME_STEP_DB) << 4) + (value % VOLUME_STEP_DB) * 2;

	aw882xx_i2c_read(aw882xx, AW882XX_HAGCCFG4_REG, &reg_value);
	if(real_value >= aw882xx->cur_gain) {
		real_value = (real_value << 8) | (reg_value & 0x00ff);
	} else {
		real_value = (aw882xx->cur_gain << 8) | (reg_value & 0x00ff);
	}
	/* write value */
	aw882xx_i2c_write(aw882xx, AW882XX_HAGCCFG4_REG, real_value);
}

static void aw882xx_fade_in_out(struct aw882xx *aw882xx)
{
	int i = 0;

	if(aw882xx == NULL) {
		aw882xx->fade_work_start = 0;
		return;
	}
	//volume up
	if (aw882xx->is_fade_in) {
		i = VOLUME_MIN_NEG_90_DB;
		do {
			aw882xx_volume_set(aw882xx, i);
			i -= FADE_STEP_DB;       //6
			usleep_range(1400,1600);
		} while (i >= 0);
	} else {  // volume down
		do {
			aw882xx_volume_set(aw882xx, i);
			i += FADE_STEP_DB;
			usleep_range(1400,1600);
		} while (i <= VOLUME_MIN_NEG_90_DB);
	}
	aw882xx->fade_work_start = 0;
}

static void aw882xx_fade_work_func(struct work_struct *work)
{
	struct delayed_work *d_work;
	struct aw882xx *aw882xx;

	d_work = to_delayed_work(work);
	aw882xx = container_of(d_work, struct aw882xx, fade_work);
	if(aw882xx == NULL) {
		pr_debug("%s: can not get aw882xx from fade_work\n", __func__);
		return;
	}
	pr_debug("%s: enter in: %d\n", __func__, aw882xx->is_fade_in);

	aw882xx_fade_in_out(aw882xx);
	pr_debug("%s: exit\n", __func__);
}

static int aw882xx_send_profile_params_to_dsp(struct aw882xx *aw882xx, int profile_id, bool is_fade)
{
	int ret ;
	int32_t params_id = AFE_PARAM_ID_AWDSP_RX_PARAMS;

	mutex_lock(&aw882xx->profile.lock);
	if (aw882xx->profile.data[profile_id] == NULL) {
		pr_err("%s: profile %s not data is NULL \n", __func__,
			awinic_profile[profile_id]);
		mutex_unlock(&aw882xx->profile.lock);
		return -EINVAL;
	}
	if (is_fade) {
		g_aw882xx_profile_flag = true;
		//fade out
		//aw882xx_fade_in_out(aw882xx, false);
		aw882xx_run_mute(aw882xx, true);
	}
	ret = aw_send_afe_cal_apr(aw882xx->afe_rx_portid, aw882xx->afe_tx_portid,
			params_id,
			aw882xx->profile.data[profile_id],
			aw882xx->profile.len, true);
	if (ret) {
		pr_err("%s: dsp_msg_write error: 0x%x\n",
			__func__, params_id);
		aw882xx_run_mute(aw882xx, false);
		//aw882xx_fade_in_out(aw882xx, true);
		g_aw882xx_profile_flag = false;
		mutex_unlock(&aw882xx->profile.lock);
		return ret;
	}
	if (is_fade) {
		//fade out
		aw882xx_run_mute(aw882xx, false);
		//aw882xx_fade_in_out(aw882xx, true);
		g_aw882xx_profile_flag = false;
	}
	mutex_unlock(&aw882xx->profile.lock);

	pr_debug("%s: set params done", __func__);
	return 0;
}

static int aw882xx_profile_set(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *codec = snd_soc_kcontrol_component(kcontrol);
	struct aw882xx *aw882xx = snd_soc_component_get_drvdata(codec);
	unsigned int ctl_value = 0;
	int ret;
	ctl_value = ucontrol->value.integer.value[0];

	if (aw882xx->afe_profile) {
		if (ctl_value >= AW_PROFILE_MAX) {
			pr_err("%s: unsuport profile %d", __func__, ctl_value);
			return -EINVAL;
		}
		pr_debug("%s: profile switch to %s \n",
			__func__, awinic_profile[ctl_value]);

		mutex_lock(&aw882xx->lock);
		if (aw882xx->profile.cur_profile != ctl_value) {
			/*PA have power on ,set params direct*/
		if (aw882xx->power_flag == AW882XX_POWER_UP) {
			ret = aw882xx_send_profile_params_to_dsp(aw882xx, ctl_value, true);
				if (ret < 0) {
				mutex_unlock(&aw882xx->lock);
					return -EINVAL;
				}
				aw882xx_send_cali_re_to_dsp(aw882xx);
				aw882xx->profile.cur_profile = ctl_value;
			} else {
				/*PA power only set params status*/
				aw882xx->profile.cur_profile = ctl_value;
			}
		}
		mutex_unlock(&aw882xx->lock);
	}
	return 0;
}

static int aw882xx_skt_disable_get(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	pr_debug("%s: aw882xx_skt_disable %d\n",
		__func__, atomic_read(&g_skt_disable));

	ucontrol->value.integer.value[0] = atomic_read(&g_skt_disable);

	return 0;
}

static int aw882xx_skt_set_dsp(int value)
{
        int ret;
	int port_id = g_aw882xx->afe_rx_portid;
	int module_id = AW_MODULE_ID_COPP;
	int param_id =  AW_MODULE_PARAMS_ID_COPP_ENABLE;

	ret = aw_adm_param_enable(port_id, module_id, param_id, value);
	if (ret) {
		pr_err("%s: set skt %d failed \n", __func__, value);
		return -EINVAL;
	}

	pr_info("%s: set skt %s", __func__, value == 1 ? "enable" : "disable");
	return 0;
}

static int aw882xx_skt_disable_set(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	int value = ucontrol->value.integer.value[0];

	pr_debug("%s: aw882xx_skt_disable %d \n", __func__, value);
	aw882xx_skt_set_dsp(!value);
	atomic_set(&g_skt_disable, value);

	return 0;
}

static int aw882xx_monitor_get(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *codec = snd_soc_kcontrol_component(kcontrol);
	struct aw882xx *aw882xx = snd_soc_component_get_drvdata(codec);


	pr_debug("%s: aw882xx_monitor_get %d\n",
		__func__, aw882xx->monitor.is_enable);

	ucontrol->value.integer.value[0] = aw882xx->monitor.is_enable;

	return 0;
}

static int aw882xx_monitor_set(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *codec = snd_soc_kcontrol_component(kcontrol);
	struct aw882xx *aw882xx = snd_soc_component_get_drvdata(codec);

	int enable = ucontrol->value.integer.value[0];

	pr_debug("%s: aw882xx_monitor_set %d \n", __func__, enable);

	if (enable == aw882xx->monitor.is_enable)
		return 1;

	aw882xx->monitor.is_enable = enable;
	if (enable)
		schedule_work(&aw882xx->monitor.work);

	return 0;
}

static int aw882xx_fade_dealyed_time_get(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *codec = snd_soc_kcontrol_component(kcontrol);
	struct aw882xx *aw882xx = snd_soc_component_get_drvdata(codec);


	pr_debug("%s: aw882xx_fade_dealyed_time_get %d\n",
		__func__, aw882xx->delayed_time);

	ucontrol->value.integer.value[0] = aw882xx->delayed_time;

	return 0;
}

static int aw882xx_fade_dealyed_time_set(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *codec = snd_soc_kcontrol_component(kcontrol);
	struct aw882xx *aw882xx = snd_soc_component_get_drvdata(codec);

	aw882xx->delayed_time = ucontrol->value.integer.value[0];

	pr_debug("%s: aw882xx_fade_dealyed_time_set %d ms \n", __func__, aw882xx->delayed_time);
	return 0;
}


#ifdef AW882XX_RUNIN_TEST
static int aw882xx_runin_test_get(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	pr_debug("%s: aw882xx_runin test %d\n",
		__func__, atomic_read(&g_runin_test));

	ucontrol->value.integer.value[0] = atomic_read(&g_runin_test);

	return 0;
}

static int aw882xx_runin_test_set(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	int value = ucontrol->value.integer.value[0];

	pr_debug("%s: aw882xx_runin_test %d \n", __func__, value);

	if (value > 1 ) {
		value = 0;
	}

	atomic_set(&g_runin_test, value);

	return 0;
}

static void aw882xx_set_adsp_module_status(struct work_struct *work)
{
	int ret = 0;
	int32_t set_value;

	/*no test reg set default value*/
	if (!atomic_read(&g_runin_test)) {
		return;
	}
	set_value = 0;
	/*set afe rx module*/
	ret = aw_send_afe_rx_module_enable(g_aw882xx->afe_rx_portid, &set_value, sizeof(uint32_t));
	if (ret) {
		pr_info("%s: disable afe rx module  %d falied , ret=%d\n",
				__func__, set_value, ret);
	}

	/*set skt module*/
	ret = aw882xx_skt_set_dsp(false);
	if (ret) {
		 pr_err("%s: disable skt failed !\n", __func__);
	}

	pr_info("%s: disable skt and  afe module \n", __func__);
}
#endif

static const struct soc_enum aw882xx_snd_enum[] = {
	SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(switch_status), switch_status),
	SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(awinic_algo), awinic_algo),
	SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(awinic_profile), awinic_profile),
};

static struct snd_kcontrol_new aw882xx_controls[] = {
	SOC_ENUM_EXT("aw882xx_speaker_switch", aw882xx_snd_enum[0],
		aw882xx_spk_get, aw882xx_spk_set),
	SOC_ENUM_EXT("aw882xx_receiver_switch", aw882xx_snd_enum[0],
		aw882xx_rcv_get, aw882xx_rcv_set),
	SOC_ENUM_EXT("aw882xx_rx_switch", aw882xx_snd_enum[1],
		aw882xx_algo_get, aw882xx_algo_set),
	SOC_ENUM_EXT("aw882xx_tx_switch", aw882xx_snd_enum[1],
		aw882xx_tx_get, aw882xx_tx_set),
	SOC_ENUM_EXT("aw882xx_skt_disable", aw882xx_snd_enum[0],
		aw882xx_skt_disable_get, aw882xx_skt_disable_set),
	SOC_ENUM_EXT("aw882xx_monitor_switch", aw882xx_snd_enum[1],
		aw882xx_monitor_get, aw882xx_monitor_set),
	SOC_SINGLE_EXT("aw882xx_fade_dealyed_time", SND_SOC_NOPM, 0, DELAY_TIME_MAX, 0,
		       aw882xx_fade_dealyed_time_get, aw882xx_fade_dealyed_time_set),
#ifdef AW882XX_RUNIN_TEST
	SOC_ENUM_EXT("aw882xx_runin_test", aw882xx_snd_enum[0],
		aw882xx_runin_test_get, aw882xx_runin_test_set),
#endif
};

static struct snd_kcontrol_new aw882xx_profile_controls[] = {
	SOC_ENUM_EXT("aw882xx_profile_switch", aw882xx_snd_enum[2],
		aw882xx_profile_get, aw882xx_profile_set),
};

static void aw882xx_add_codec_controls(struct aw882xx *aw882xx)
{
	pr_info("%s: enter\n", __func__);

	snd_soc_add_component_controls(aw882xx->component, aw882xx_controls,
		ARRAY_SIZE(aw882xx_controls));
	if(aw882xx->afe_profile) {
		snd_soc_add_component_controls(aw882xx->component, aw882xx_profile_controls,
		ARRAY_SIZE(aw882xx_profile_controls));
	}
	snd_soc_add_component_controls(aw882xx->component, &aw882xx_volume, 1);
}

/******************************************************
 *
 * Digital Audio Interface
 *
 ******************************************************/


static int aw882xx_startup(struct snd_pcm_substream *substream,
	struct snd_soc_dai *dai)
{
	struct snd_soc_component *component = dai->component;
	struct aw882xx *aw882xx = snd_soc_component_get_drvdata(component);

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		pr_info("%s: playback enter\n", __func__);
		/*aw882xx_run_pwd(aw882xx, false);*/
		aw882xx->power_flag = AW882XX_POWER_ING;
	} else {
		pr_info("%s: capture enter\n", __func__);
	}

	return 0;
}

static int aw882xx_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	/*struct aw882xx *aw882xx = snd_soc_component_get_drvdata(dai->codec);*/
	struct snd_soc_component *component = dai->component;

	pr_info("%s: fmt=0x%x\n", __func__, fmt);

	/* Supported mode: regular I2S, slave, or PDM */
	switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAIFMT_I2S:
		if ((fmt & SND_SOC_DAIFMT_MASTER_MASK) !=
			SND_SOC_DAIFMT_CBS_CFS) {
			dev_err(component->dev, "%s: invalid codec master mode\n",
				__func__);
			return -EINVAL;
		}
		break;
	default:
		dev_err(component->dev, "%s: unsupported DAI format %d\n",
			__func__, fmt & SND_SOC_DAIFMT_FORMAT_MASK);
		return -EINVAL;
	}
	return 0;
}

static int aw882xx_set_dai_sysclk(struct snd_soc_dai *codec_dai,
	int clk_id, unsigned int freq, int dir)
{
	struct aw882xx *aw882xx = snd_soc_component_get_drvdata(codec_dai->component);

	pr_info("%s: freq=%d\n", __func__, freq);

	aw882xx->sysclk = freq;
	return 0;
}

static int aw882xx_hw_params(struct snd_pcm_substream *substream,
	struct snd_pcm_hw_params *params,
	struct snd_soc_dai *dai)
{
	struct snd_soc_component *component = dai->component;
	struct aw882xx *aw882xx = snd_soc_component_get_drvdata(component);
	unsigned int rate = 0;
	int reg_value = 0;
	uint32_t cco_mux_value;
	int width = 0;

	if (substream->stream == SNDRV_PCM_STREAM_CAPTURE) {
		pr_debug("%s: requested rate: %d, sample size: %d\n",
			__func__, rate, snd_pcm_format_width(params_format(params)));
		return 0;
	}
	/* get rate param */
	aw882xx->rate = rate = params_rate(params);
	pr_debug("%s: requested rate: %d, sample size: %d\n",
		__func__, rate, snd_pcm_format_width(params_format(params)));

	/* match rate */
	switch (rate) {
	case 8000:
		reg_value = AW882XX_I2SSR_8KHZ_VALUE;
		cco_mux_value = AW882XX_I2S_CCO_MUX_8_16_32KHZ_VALUE;
		break;
	case 16000:
		reg_value = AW882XX_I2SSR_16KHZ_VALUE;
		cco_mux_value = AW882XX_I2S_CCO_MUX_8_16_32KHZ_VALUE;
		break;
	case 32000:
		reg_value = AW882XX_I2SSR_32KHZ_VALUE;
		cco_mux_value = AW882XX_I2S_CCO_MUX_8_16_32KHZ_VALUE;
		break;
	case 44100:
		reg_value = AW882XX_I2SSR_44P1KHZ_VALUE;
		cco_mux_value = AW882XX_I2S_CCO_MUX_EXC_8_16_32KHZ_VALUE;
		break;
	case 48000:
		reg_value = AW882XX_I2SSR_48KHZ_VALUE;
		cco_mux_value = AW882XX_I2S_CCO_MUX_EXC_8_16_32KHZ_VALUE;
		break;
	case 96000:
		reg_value = AW882XX_I2SSR_96KHZ_VALUE;
		cco_mux_value = AW882XX_I2S_CCO_MUX_EXC_8_16_32KHZ_VALUE;
		break;
	case 192000:
		reg_value = AW882XX_I2SSR_192KHZ_VALUE;
		cco_mux_value = AW882XX_I2S_CCO_MUX_EXC_8_16_32KHZ_VALUE;
		break;
	default:
		reg_value = AW882XX_I2SSR_48KHZ_VALUE;
		cco_mux_value = AW882XX_I2S_CCO_MUX_EXC_8_16_32KHZ_VALUE;
		pr_err("%s: rate can not support\n", __func__);
		break;
	}
	aw882xx_i2c_write_bits(aw882xx, AW882XX_PLLCTRL1_REG,
				AW882XX_I2S_CCO_MUX_MASK, cco_mux_value);

	/* set chip rate */
	if (-1 != reg_value) {
		aw882xx_i2c_write_bits(aw882xx, AW882XX_I2SCTRL_REG,
				AW882XX_I2SSR_MASK, reg_value);
	}

	/* get bit width */
	width = params_width(params);
	pr_debug("%s: width = %d\n", __func__, width);
	switch (width) {
	case 16:
		reg_value = AW882XX_I2SFS_16_BITS_VALUE;
		break;
	case 20:
		reg_value = AW882XX_I2SFS_20_BITS_VALUE;
		break;
	case 24:
		reg_value = AW882XX_I2SFS_24_BITS_VALUE;
		break;
	case 32:
		reg_value = AW882XX_I2SFS_32_BITS_VALUE;
		break;
	default:
		reg_value = AW882XX_I2SFS_16_BITS_VALUE;
		pr_err("%s: width can not support\n", __func__);
		break;
	}

	/* get width */
	if (-1 != reg_value) {
		aw882xx_i2c_write_bits(aw882xx, AW882XX_I2SCTRL_REG,
				AW882XX_I2SFS_MASK, reg_value);
	}

	return 0;
}

static int aw882xx_mute(struct snd_soc_dai *dai, int mute, int stream)
{
	struct snd_soc_component *component = dai->component;
	struct aw882xx *aw882xx = snd_soc_component_get_drvdata(component);

	pr_info("%s: mute state=%d\n", __func__, mute);

	if (!(aw882xx->flags & AW882XX_FLAG_START_ON_MUTE))
		return 0;

	if (mute) {
		if (stream == SNDRV_PCM_STREAM_PLAYBACK)
			aw882xx_smartpa_cfg(aw882xx, false);
		atomic_set(&g_algo_tx_enable, 0);
		atomic_set(&g_algo_rx_enable, 0);
		atomic_set(&g_skt_disable, 0);
	} else {
		if (stream == SNDRV_PCM_STREAM_PLAYBACK)
			aw882xx_smartpa_cfg(aw882xx, true);
	}

	return 0;
}

static void aw882xx_shutdown(struct snd_pcm_substream *substream,
	struct snd_soc_dai *dai)
{
	struct snd_soc_component *component = dai->component;
	struct aw882xx *aw882xx = snd_soc_component_get_drvdata(component);

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		aw882xx->rate = 0;
		aw882xx_run_pwd(aw882xx, true);
	}
}

static const struct snd_soc_dai_ops aw882xx_dai_ops = {
	.startup = aw882xx_startup,
	.set_fmt = aw882xx_set_fmt,
	.set_sysclk = aw882xx_set_dai_sysclk,
	.hw_params = aw882xx_hw_params,
	.mute_stream = aw882xx_mute,
	.shutdown = aw882xx_shutdown,
};

static struct snd_soc_dai_driver aw882xx_dai[] = {
	{
		.name = "aw882xx-aif",
		.id = 1,
		.playback = {
			.stream_name = "Speaker_Playback",
			.channels_min = 1,
			.channels_max = 2,
			.rates = AW882XX_RATES,
			.formats = AW882XX_FORMATS,
		},
		.capture = {
			.stream_name = "Speaker_Capture",
			.channels_min = 1,
			.channels_max = 2,
			.rates = AW882XX_RATES,
			.formats = AW882XX_FORMATS,
		 },
		.ops = &aw882xx_dai_ops,
		.symmetric_rates = 1,
#if 0
		.symmetric_channels = 1,
		.symmetric_samplebits = 1,
#endif
	},
};

/*****************************************************
 *
 * codec driver
 *
 *****************************************************/
static int aw882xx_probe(struct snd_soc_component *component)
{
	struct aw882xx *aw882xx = snd_soc_component_get_drvdata(component);
	int ret = -1;

	pr_info("%s: enter\n", __func__);

	aw882xx->component = component;

	aw882xx_add_codec_controls(aw882xx);

	//if (component->dev->of_node)
		//dev_set_name(component->dev, "%s", "aw882xx_smartpa");

	pr_info("%s: exit\n", __func__);

	ret = 0;
	return ret;
}

static void aw882xx_remove(struct snd_soc_component *component)
{
	/*struct aw882xx *aw882xx = snd_soc_component_get_drvdata(codec);*/
	pr_info("%s: enter\n", __func__);
	return;
}


static unsigned int aw882xx_codec_read(struct snd_soc_component *component,
	unsigned int reg)
{
	struct aw882xx *aw882xx = snd_soc_component_get_drvdata(component);
	unsigned int value = 0;
	int ret = -1;

	pr_debug("%s: enter\n", __func__);

	if (aw882xx_reg_access[reg] & REG_RD_ACCESS) {
		ret = aw882xx_i2c_read(aw882xx, reg, &value);
		if (ret < 0)
			pr_debug("%s: read register failed\n", __func__);
	} else {
		pr_debug("%s: register 0x%x no read access\n", __func__, reg);
	}
	return ret;
}

static int aw882xx_codec_write(struct snd_soc_component *component,
	unsigned int reg, unsigned int value)
{
	int ret = -1;
	struct aw882xx *aw882xx = snd_soc_component_get_drvdata(component);

	pr_debug("%s: enter ,reg is 0x%x value is 0x%x\n",
		__func__, reg, value);

	if (aw882xx_reg_access[reg]&REG_WR_ACCESS) {
		ret = aw882xx_i2c_write(aw882xx, reg, value);
	} else {
		pr_debug("%s: register 0x%x no write access\n",
			__func__, reg);
	}

	return ret;
}

static struct snd_soc_component_driver soc_component_dev_aw882xx = {
	.probe = aw882xx_probe,
	.remove = aw882xx_remove,
	.read = aw882xx_codec_read,
	.write = aw882xx_codec_write,
//	.reg_cache_size = AW882XX_REG_MAX,
//	.reg_word_size = 2,
};

/******************************************************
 *
 * irq
 *
 ******************************************************/
static void aw882xx_interrupt_setup(struct aw882xx *aw882xx)
{
	unsigned int reg_val;

	pr_info("%s: enter\n", __func__);

	aw882xx_i2c_read(aw882xx, AW882XX_SYSINTM_REG, &reg_val);
	reg_val &= (~AW882XX_UVLS_VDD_BELOW_2P8V_VALUE);
	reg_val &= (~AW882XX_NOCLKS_TRIG_VALUE);
	reg_val &= (~AW882XX_CLKS_TRIG_VALUE);
	aw882xx_i2c_write(aw882xx, AW882XX_SYSINTM_REG, reg_val);
}

static void aw882xx_interrupt_clear(struct aw882xx *aw882xx)
{
	unsigned int reg_val = 0;

	pr_info("%s: enter\n", __func__);

	aw882xx_i2c_read(aw882xx, AW882XX_SYSST_REG, &reg_val);
	pr_info("%s: reg SYSST=0x%x\n", __func__, reg_val);

	aw882xx_i2c_read(aw882xx, AW882XX_SYSINT_REG, &reg_val);
	pr_info("%s: reg SYSINT=0x%x\n", __func__, reg_val);

	aw882xx_i2c_read(aw882xx, AW882XX_SYSINTM_REG, &reg_val);
	pr_info("%s: reg SYSINTM=0x%x\n", __func__, reg_val);
}

static irqreturn_t aw882xx_irq(int irq, void *data)
{
	struct aw882xx *aw882xx = data;

	pr_info("%s: enter\n", __func__);

	aw882xx_interrupt_clear(aw882xx);

	pr_info("%s: exit\n", __func__);

	return IRQ_HANDLED;
}

/*****************************************************
 *
 * device tree
 *
 *****************************************************/
int aw882xx_parse_low_vol_cfg(struct device *dev, struct aw882xx *aw882xx,
		struct device_node *np, char *dir, struct aw882xx_low_vol **table, int *num)
{
	int ret;
	int i;
	uint32_t *cfg_value;
	char cfg_name[32] = {0};
	int cfg_len;

	snprintf(cfg_name, sizeof(cfg_name), "low-vol-table-%s", dir);

	cfg_len = of_property_count_u32_elems(np, cfg_name);
	if (cfg_len <= 0) {
		dev_info(dev, "%s: %s get cfg_len:%d error\n",
			__func__, cfg_name, cfg_len);
		goto use_default;
	}

	cfg_value = devm_kzalloc(dev, sizeof(uint32_t) * cfg_len, GFP_KERNEL);
	if (!cfg_value) {
		dev_info(dev,"%s: %s aw_cfg kzalloc failed\n",
				__func__, cfg_name);
		goto use_default;
	}

	ret = of_property_read_u32_array(np, cfg_name, cfg_value, cfg_len);
	if (ret != 0) {
		dev_info(dev, "%s: fail get %s dt\n",
			__func__, cfg_name);
		devm_kfree(dev, cfg_value);
		goto use_default;
	}
	*num = cfg_len / 3;
	*table = devm_kzalloc(dev, sizeof(struct aw882xx_low_vol) * (*num), GFP_KERNEL);
	if (!(*table)) {
		dev_info(dev, "%s: %s table kzalloc failed\n",
				__func__, cfg_name);
		devm_kfree(dev, cfg_value);
		goto use_default;
	}

	for(i = 0; i < (*num); i++) {
		(*table)[i].vol = cfg_value[3 * i];
		(*table)[i].ipeak = cfg_value[3 * i + 1];
		(*table)[i].gain = cfg_value[3 * i + 2];
	}

	devm_kfree(dev, cfg_value);

	return 0;

use_default:
	dev_info(dev, "%s: %s low-vol table use default cfg\n", __func__, cfg_name);
	if (!strcmp(dir,"up")) {
		*table = vol_up_table;
		*num = sizeof(vol_up_table) / sizeof(struct aw882xx_low_vol);
	} else if (!strcmp(dir,"down")) {
		*table = vol_down_table;
		*num = sizeof(vol_down_table) / sizeof(struct aw882xx_low_vol);
	} else {
		 dev_err(dev,"%s: unsupport dir %s\n",
			__func__, dir);
		return -EINVAL;
	}
	return 0;
}


int aw882xx_parse_low_temp_cfg(struct device *dev, struct aw882xx *aw882xx,
		struct device_node *np, char *dir, struct aw882xx_low_temp **table, int *num)
{
	int ret;
	int i;
	uint32_t *cfg_value;
	char cfg_name[32] = {0};
	int cfg_len;

	snprintf(cfg_name, sizeof(cfg_name), "low-temp-table-%s", dir);

	cfg_len = of_property_count_u32_elems(np, cfg_name);
	if (cfg_len <= 0) {
		dev_info(dev, "%s: %s get cfg_len:%d error\n",
			__func__, cfg_name, cfg_len);
		goto use_default;
	}

	cfg_value = devm_kzalloc(dev, sizeof(uint32_t) * cfg_len, GFP_KERNEL);
	if (!cfg_value) {
		dev_info(dev,"%s: %s aw_cfg kzalloc failed\n",
				__func__, cfg_name);
		goto use_default;
	}

	ret = of_property_read_u32_array(np, cfg_name, cfg_value, cfg_len);
	if (ret != 0) {
		dev_info(dev, "%s: fail get %s dt\n",
			__func__, cfg_name);
		devm_kfree(dev, cfg_value);
		goto use_default;
	}
	*num = cfg_len / 4;
	*table = devm_kzalloc(dev, sizeof(struct aw882xx_low_temp) * (*num), GFP_KERNEL);
	if (!(*table)) {
		dev_info(dev, "%s: %s table kzalloc failed\n",
				__func__, cfg_name);
		devm_kfree(dev, cfg_value);
		goto use_default;
	}

	for(i = 0; i < (*num); i++) {
		(*table)[i].temp = cfg_value[4 * i];
		(*table)[i].ipeak = cfg_value[4 * i + 1];
		(*table)[i].gain = cfg_value[4 * i + 2];
		(*table)[i].vmax = cfg_value[4 * i + 3];
	}

	devm_kfree(dev, cfg_value);

	return 0;

use_default:
	dev_info(dev, "%s: %s temp table use default cfg\n", __func__, cfg_name);
	if (!strcmp(dir,"up")) {
		*table = temp_up_table;
		*num = sizeof(temp_up_table) / sizeof(struct aw882xx_low_temp);
	} else if (!strcmp(dir,"down")) {
		*table = temp_down_table;
		*num = sizeof(temp_down_table) / sizeof(struct aw882xx_low_temp);
	} else {
		dev_err(dev,"%s: unsupport dir %s\n",
			__func__, dir);
		return -EINVAL;
	}
	return 0;
}

static int aw882xx_parse_dt(struct device *dev, struct aw882xx *aw882xx,
		struct device_node *np)
{
	int ret = 0;
	int i = 0;
	struct aw882xx_monitor *monitor = &aw882xx->monitor;
	/* gpio */
	aw882xx->reset_gpio = of_get_named_gpio(np, "reset-gpio", 0);
	if (aw882xx->reset_gpio < 0) {
		dev_err(dev, "%s: no reset gpio provided, will not HW reset device\n",
			__func__);
	} else {
		dev_info(dev, "%s: reset gpio provided ok\n", __func__);
	}
	aw882xx->irq_gpio = of_get_named_gpio(np, "irq-gpio", 0);
	if (aw882xx->irq_gpio < 0)
		dev_info(dev, "%s: no irq gpio provided.\n", __func__);
	else
		dev_info(dev, "%s: irq gpio provided ok.\n", __func__);

	ret = of_property_read_u32(np, "monitor-flag", &monitor->is_enable);
	if (ret) {
		monitor->is_enable = AW882XX_MONITOR_DEFAULT_FLAG;
		dev_err(dev, "%s: monitor-flag get failed ,use default value!\n", __func__);
	} else {
		dev_info(dev, "%s: monitor-flag = %d\n",
			__func__, monitor->is_enable);
	}

	ret = of_property_read_u32(np, "monitor-timer-val", &monitor->timer_val);
	if (ret) {
		monitor->timer_val = AW882XX_MONITOR_DEFAULT_TIMER_VAL;
		dev_err(dev, "%s: monitor-timer-val get failed,use default value!\n", __func__);
	} else {
		dev_info(dev, "%s: monitor-timer-val = %d\n",
			__func__, monitor->timer_val);
	}

	ret = of_property_read_u32(np, "afe-rx-portid", &aw882xx->afe_rx_portid);
	if (ret) {
		aw882xx->afe_rx_portid = AW882XX_DEFAULT_AFE_RX_PROT_ID;
		dev_err(dev, "%s: afe_rx_portid get failed,use default port: 0x%x!\n",
			__func__, aw882xx->afe_rx_portid);
	} else {
		dev_info(dev, "%s: afe_rx_portid = 0x%x\n",
			__func__, aw882xx->afe_rx_portid);
	}

	ret = of_property_read_u32(np, "afe-tx-portid", &aw882xx->afe_tx_portid);
	if (ret) {
		aw882xx->afe_tx_portid = AW882XX_DEFAULT_AFE_TX_PROT_ID;
		dev_err(dev, "%s: afe_tx_portid get failed,use default port: 0x%x!\n",
			__func__, aw882xx->afe_tx_portid);
	} else {
		dev_info(dev, "%s: afe_tx_portid = 0x%x\n",
			__func__, aw882xx->afe_tx_portid);
	}

	ret = of_property_read_u32(np, "afe-profile", &aw882xx->afe_profile);
	if (ret) {
		aw882xx->afe_profile = 0;
		dev_err(dev, "%s: afe-profile get failed,use default value!\n", __func__);
	} else {
		dev_info(dev, "%s: afe-profile = %d\n",
			__func__, aw882xx->afe_profile);
	}

	ret = of_property_read_u32(np, "fade-flag", &aw882xx->fade_flag);
	if (ret) {
		aw882xx->fade_flag = 0;
		dev_err(dev, "%s: fade_flag get failed,use default value!\n", __func__);
	} else {
		dev_info(dev, "%s: fade_flag = %d\n",
			__func__, aw882xx->fade_flag);
	}

	/*get low vol table cfg*/
	ret = aw882xx_parse_low_vol_cfg(dev, aw882xx, np, "up",
			&monitor->vol_up_table, &monitor->vol_up_num);
	if (ret <0)
		return ret;

	ret = aw882xx_parse_low_vol_cfg(dev, aw882xx, np, "down",
			&monitor->vol_down_table, &monitor->vol_down_num);
	if (ret < 0)
		return ret;

	for(i = 0; i < monitor->vol_up_num; i++)
		dev_info(dev,"%s:vol_up_table vol:%d, %d, %d\n",
			__func__, monitor->vol_up_table[i].vol,
			monitor->vol_up_table[i].ipeak,
			monitor->vol_up_table[i].gain);

	for(i = 0; i < monitor->vol_down_num; i++)
		dev_info(dev, "%s:vol_down_table vol:%d, %d, %d\n",
			__func__, monitor->vol_down_table[i].vol,
			monitor->vol_down_table[i].ipeak,
			monitor->vol_down_table[i].gain);

	for(i = 0; i < monitor->vol_down_num; i++) {
		if(monitor->vol_down_table[i].vol >=
			monitor->vol_up_table[monitor->vol_up_num - 1 -i].vol) {
			dev_err(aw882xx->dev,"%s: use default table\n", __func__);
			monitor->vol_up_table = vol_up_table;
			monitor->vol_up_num= sizeof(vol_up_table) /
								sizeof(struct aw882xx_low_vol);
			monitor->vol_down_table = vol_down_table;
			monitor->vol_down_num = sizeof(vol_down_table) /
								sizeof(struct aw882xx_low_vol);
			break;
		}
	}

	for(i = 1; i < monitor->vol_down_num; i++) {
		if(monitor->vol_up_table[monitor->vol_up_num - i].vol >=
		   monitor->vol_down_table[i].vol) {
			dev_err(aw882xx->dev,"%s: use default table\n", __func__);
			monitor->vol_up_table = vol_up_table;
			monitor->vol_up_num= sizeof(vol_up_table) /
								sizeof(struct aw882xx_low_vol);
			monitor->vol_down_table = vol_down_table;
			monitor->vol_down_num = sizeof(vol_down_table) /
								sizeof(struct aw882xx_low_vol);
			break;
		}
	}
	/*get low temp table cfg*/
	ret = aw882xx_parse_low_temp_cfg(dev, aw882xx, np, "up",
			&monitor->temp_up_table, &monitor->temp_up_num);
	if (ret <0)
		return ret;

	ret = aw882xx_parse_low_temp_cfg(dev, aw882xx, np, "down",
			&monitor->temp_down_table, &monitor->temp_down_num);

	for(i = 0; i < monitor->temp_up_num; i++)
		dev_info(dev,"%s:temp_up_table temp:%d, 0x%x, 0x%x, 0x%x\n",
			__func__, monitor->temp_up_table[i].temp,
			monitor->temp_up_table[i].ipeak,
			monitor->temp_up_table[i].gain,
			monitor->temp_up_table[i].vmax);

	for(i = 0; i < monitor->temp_down_num; i++)
		dev_info(dev, "%s:temp_down_table temp:%d, 0x%x, 0x%x, 0x%x\n",
			__func__, monitor->temp_down_table[i].temp,
			monitor->temp_down_table[i].ipeak,
			monitor->temp_down_table[i].gain,
			monitor->temp_down_table[i].vmax);

	for(i = 0; i < monitor->temp_down_num; i++) {
		if(monitor->temp_down_table[i].temp >=
		   monitor->temp_up_table[monitor->temp_up_num - 1 -i].temp) {
			dev_err(aw882xx->dev,"%s: use default table\n", __func__);
			monitor->temp_up_table = temp_up_table;
			monitor->temp_up_num= sizeof(temp_up_table) /
								sizeof(struct aw882xx_low_temp);
			monitor->temp_down_table = temp_down_table;
			monitor->temp_down_num = sizeof(temp_down_table) /
								sizeof(struct aw882xx_low_temp);
			break;
		}
	}

	for(i = 1; i < monitor->temp_down_num; i++) {
		if(monitor->temp_up_table[monitor->temp_up_num - i].temp >=
		   monitor->temp_down_table[i].temp) {
			dev_err(aw882xx->dev,"%s: use default table\n", __func__);
			monitor->temp_up_table = temp_up_table;
			monitor->temp_up_num= sizeof(temp_up_table) /
								sizeof(struct aw882xx_low_temp);
			monitor->temp_down_table = temp_down_table;
			monitor->temp_down_num = sizeof(temp_down_table) /
								sizeof(struct aw882xx_low_temp);
			break;
		}
	}
	return 0;
}

int aw882xx_hw_reset(struct aw882xx *aw882xx)
{
	pr_info("%s: enter\n", __func__);

	if (aw882xx && gpio_is_valid(aw882xx->reset_gpio)) {
		gpio_set_value_cansleep(aw882xx->reset_gpio, 0);
		msleep(1);
		gpio_set_value_cansleep(aw882xx->reset_gpio, 1);
		msleep(2);
	} else {
		dev_err(aw882xx->dev, "%s: failed\n", __func__);
	}
	return 0;
}

/*****************************************************
 *
 * check chip id
 *
 *****************************************************/
int aw882xx_read_chipid(struct aw882xx *aw882xx)
{
	int ret = -1;
	unsigned int cnt = 0;
	unsigned int reg = 0;

	while (cnt < AW_READ_CHIPID_RETRIES) {
		ret = aw882xx_i2c_read(aw882xx, AW882XX_ID_REG, &reg);
		if (ret < 0) {
			dev_err(aw882xx->dev,
				"%s: failed to read REG_ID: %d\n",
				__func__, ret);
			return -EIO;
		}
		switch (reg) {
		case AW882XX_ID:
			pr_info("%s: aw882xx detected\n", __func__);
			aw882xx->flags |= AW882XX_FLAG_SKIP_INTERRUPTS;
			aw882xx->flags |= AW882XX_FLAG_START_ON_MUTE;
			aw882xx->chipid = AW882XX_ID;
			pr_info("%s: aw882xx->flags=0x%x\n",
				__func__, aw882xx->flags);
			return 0;
		default:
			pr_info("%s: unsupported device revision (0x%x)\n",
				__func__, reg);
			break;
		}
		cnt++;

		msleep(AW_READ_CHIPID_RETRY_DELAY);
	}

	return -EINVAL;
}

/******************************************************
 *
 * sys group attribute: monitor
 *
 ******************************************************/
static ssize_t aw882xx_monitor_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct aw882xx *aw882xx = dev_get_drvdata(dev);
	uint32_t enable = 0;
	int ret = -1;

	if (count == 0)
		return 0;

	ret = kstrtouint(buf, 0, &enable);
	if (ret < 0)
		return ret;

	dev_info(dev, "%s:monitor  enable set =%d\n",
		__func__, enable);
	aw882xx->monitor.is_enable = enable;
	if (enable)
		aw882xx_monitor_start(&aw882xx->monitor);

	return count;
}

static ssize_t aw882xx_monitor_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct aw882xx *aw882xx = dev_get_drvdata(dev);
	ssize_t len = 0;
	uint32_t local_enable;

	local_enable = aw882xx->monitor.is_enable;
	len += snprintf(buf+len, PAGE_SIZE-len,
		"aw882xx monitor enable: %d\n", local_enable);
	return len;
}

/******************************************************
 *
 * sys group attribute: reg
 *
 ******************************************************/
static ssize_t aw882xx_reg_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct aw882xx *aw882xx = dev_get_drvdata(dev);
	unsigned int databuf[2] = {0};

	if (2 == sscanf(buf, "%x %x", &databuf[0], &databuf[1]))
		aw882xx_i2c_write(aw882xx, databuf[0], databuf[1]);

	return count;
}

static ssize_t aw882xx_reg_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct aw882xx *aw882xx = dev_get_drvdata(dev);
	ssize_t len = 0;
	unsigned char i = 0;
	unsigned int reg_val = 0;

	for (i = 0; i < AW882XX_REG_MAX; i++) {
		if (aw882xx_reg_access[i]&REG_RD_ACCESS) {
			aw882xx_i2c_read(aw882xx, i, &reg_val);
			len += snprintf(buf+len, PAGE_SIZE-len,
				"reg:0x%02x=0x%04x\n", i, reg_val);
		}
	}
	return len;
}

static ssize_t aw882xx_rw_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct aw882xx *aw882xx = dev_get_drvdata(dev);

	unsigned int databuf[2] = {0};

	if (2 == sscanf(buf, "%x %x", &databuf[0], &databuf[1])) {
		aw882xx->reg_addr = (unsigned char)databuf[0];
		aw882xx_i2c_write(aw882xx, databuf[0], databuf[1]);
	} else if (1 == sscanf(buf, "%x", &databuf[0])) {
		aw882xx->reg_addr = (unsigned char)databuf[0];
	}

	return count;
}

static ssize_t aw882xx_rw_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct aw882xx *aw882xx = dev_get_drvdata(dev);
	ssize_t len = 0;
	unsigned int reg_val = 0;

	if (aw882xx_reg_access[aw882xx->reg_addr] & REG_RD_ACCESS) {
		aw882xx_i2c_read(aw882xx, aw882xx->reg_addr, &reg_val);
		len += snprintf(buf+len, PAGE_SIZE-len,
			"reg:0x%02x=0x%04x\n", aw882xx->reg_addr, reg_val);
	}
	return len;
}

static ssize_t aw882xx_spk_rcv_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct aw882xx *aw882xx = dev_get_drvdata(dev);

	int ret = -1;
	unsigned int databuf[2] = {0};

	ret = kstrtouint(buf, 0, &databuf[0]);
	if (ret < 0)
		return ret;

	aw882xx->spk_rcv_mode = databuf[0];

	return count;
}

static ssize_t aw882xx_spk_rcv_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct aw882xx *aw882xx = dev_get_drvdata(dev);
	ssize_t len = 0;

	if (aw882xx->spk_rcv_mode == AW882XX_SPEAKER_MODE)
		len += snprintf(buf+len, PAGE_SIZE-len,
			"aw882xx spk_rcv: %d, speaker mode\n",
			aw882xx->spk_rcv_mode);
	else if (aw882xx->spk_rcv_mode == AW882XX_RECEIVER_MODE)
		len += snprintf(buf+len, PAGE_SIZE-len,
			"aw882xx spk_rcv: %d, receiver mode\n",
			aw882xx->spk_rcv_mode);
	else
		len += snprintf(buf+len, PAGE_SIZE-len,
			"aw882xx spk_rcv: %d, unknown mode\n",
			aw882xx->spk_rcv_mode);

	return len;
}

static ssize_t aw882xx_mec_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	/*struct aw882xx *aw882xx = dev_get_drvdata(dev);*/
	uint32_t mec_ctr = 0;
	uint32_t param_id = AFE_PARAM_ID_AWDSP_RX_SET_ENABLE;
	int ret = -1;

	if (count == 0)
		return 0;

	ret = kstrtouint(buf, 0, &mec_ctr);
	if (ret < 0)
		return ret;

	pr_info("%s: mec_ctr=%d\n", __func__, mec_ctr);

	ret = aw_send_afe_cal_apr(g_aw882xx->afe_rx_portid, g_aw882xx->afe_tx_portid,
							param_id, &mec_ctr, sizeof(uint32_t), true);
	if (ret)
		pr_err("%s: dsp_msg error, ret=%d\n", __func__, ret);

	mdelay(2);

	return count;
}

static ssize_t aw882xx_mec_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	/*struct aw882xx *aw882xx = dev_get_drvdata(dev);*/
	ssize_t len = 0;
	int ret = -1;
	uint8_t *buffer = NULL;
	uint32_t mec_ctr = 0;
	uint32_t param_id = AFE_PARAM_ID_AWDSP_RX_SET_ENABLE;

	buffer = kmalloc(sizeof(uint32_t), GFP_KERNEL);
	if (buffer == NULL) {
		pr_err("%s can not allocate memory\n", __func__);
		return -ENOMEM;
	}

	ret = aw_send_afe_cal_apr(g_aw882xx->afe_rx_portid, g_aw882xx->afe_tx_portid,
							param_id, buffer, sizeof(uint32_t), false);
	if (ret) {
		pr_err("%s: dsp_msg_read error: %d\n", __func__, ret);
		kfree(buffer);
		return -EFAULT;
	}

	memcpy(&mec_ctr, buffer, sizeof(uint32_t));

	len += snprintf(buf+len, PAGE_SIZE-len,
		"aw882xx mec: %d\n", mec_ctr);

	kfree(buffer);

	return len;
}

static ssize_t aw882xx_default_re_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct aw882xx *aw882xx = dev_get_drvdata(dev);
	ssize_t len = 0;
	int ret = -1;

	if((aw882xx != NULL) && (aw882xx->default_re == 0)) {
		ret = aw_send_afe_cal_apr(aw882xx->afe_rx_portid, aw882xx->afe_tx_portid,
						AFE_PARAM_ID_AWDSP_RX_RE_L,
						&aw882xx->default_re, sizeof(int32_t), false);
		if (ret) {
			pr_err("%s: dsp_msg_read error: %d\n", __func__, ret);
			return -EFAULT;
		}
	}
	if(aw882xx != NULL) {
		len += snprintf(buf+len, PAGE_SIZE-len,
			"aw882xx default_re: %d\n", aw882xx->default_re);
	}
	return len;
}

static ssize_t aw882xx_spk_temp_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct aw882xx *aw882xx = dev_get_drvdata(dev);
	ssize_t len = 0;
	int ret = -1;
	int16_t data_len  = _IOC_SIZE(AW882XX_IOCTL_GET_CALI_DATA);
	int32_t spk_temp = 0;
	char *data_ptr = NULL;
	unsigned int reg_value = 0;

	if(aw882xx == NULL) {
		goto fail_temp;
	}
	data_ptr = kmalloc(data_len, GFP_KERNEL);
	if (data_ptr == NULL) {
		pr_err("%s : malloc failed !\n", __func__);
		goto fail_temp;
	}

	aw882xx_i2c_read(aw882xx, AW882XX_SYSST_REG, &reg_value);
	if (!(reg_value & AW882XX_PLLS_LOCKED_VALUE)) {
		pr_err("%s: NO I2S CLK\n", __func__);
		goto exit_temp;
	}

	ret = aw_send_afe_cal_apr(aw882xx->afe_rx_portid,
					aw882xx->afe_tx_portid,
					AFE_PARAM_ID_AWDSP_RX_REAL_DATA_L,
					data_ptr, data_len, false);
	if (ret) {
		pr_err("%s: dsp_msg_read error: %d\n", __func__, ret);
		goto exit_temp;
	}

	memcpy(&spk_temp, data_ptr + sizeof(int32_t), sizeof(int32_t));
	len += snprintf(buf+len, PAGE_SIZE-len,
			"aw882xx spk_temp: %d\n", spk_temp);
	return len;

exit_temp:
	kfree(data_ptr);
fail_temp:
	len += snprintf(buf+len, PAGE_SIZE-len,
			"aw882xx spk_temp: fail\n");
	return len;
}

#ifdef AW_DEBUG
static ssize_t aw882xx_vol_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct aw882xx *aw882xx = dev_get_drvdata(dev);
	uint32_t vol = 0;
	int ret = -1;

	if (count == 0)
		return 0;

	ret = kstrtouint(buf, 0, &vol);
	if (ret < 0)
		return ret;

	pr_info("%s: vol set =%d\n", __func__, vol);
	aw882xx->monitor.test_vol = vol;

	return count;
}

static ssize_t aw882xx_vol_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct aw882xx *aw882xx = dev_get_drvdata(dev);
	ssize_t len = 0;
	uint32_t local_vol = aw882xx->monitor.test_vol;

	len += snprintf(buf+len, PAGE_SIZE-len,
		"aw882xx vol: %d\n", local_vol);
	return len;
}
static ssize_t aw882xx_temp_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct aw882xx *aw882xx = dev_get_drvdata(dev);
	int32_t temp = 0;
	int ret = -1;

	if (count == 0)
		return 0;

	ret = kstrtoint(buf, 0, &temp);
	if (ret < 0)
		return ret;

	pr_info("%s: temp set =%d\n", __func__, temp);
	aw882xx->monitor.test_temp = temp;

	return count;
}

static ssize_t aw882xx_temp_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct aw882xx *aw882xx = dev_get_drvdata(dev);
	ssize_t len = 0;
	int32_t local_temp = aw882xx->monitor.test_temp;

	len += snprintf(buf+len, PAGE_SIZE-len,
		"aw882xx vol: %d\n", local_temp);
	return len;
}
#endif

static int aw_write_msg_to_dsp(struct aw882xx *aw882xx, int inline_id,
			char *data, int data_size)
{
	int32_t *dsp_msg = NULL;
	struct aw_dsp_msg_hdr *hdr = NULL;
	int ret;

	dsp_msg = kzalloc(sizeof(struct aw_dsp_msg_hdr) + data_size,
			GFP_KERNEL);
	if (!dsp_msg) {
		pr_err("%s: inline_id:0x%x kzalloc dsp_msg error\n",
			__func__, inline_id);
		return -ENOMEM;
	}
	hdr = (struct aw_dsp_msg_hdr *)dsp_msg;
	hdr->type = DSP_MSG_TYPE_DATA;
	hdr->opcode_id = inline_id;
	hdr->version = AWINIC_DSP_MSG_HDR_VER;

	memcpy(((char *)dsp_msg) + sizeof(struct aw_dsp_msg_hdr),
		data, data_size);

	ret = aw_send_afe_cal_apr(aw882xx->afe_rx_portid, aw882xx->afe_tx_portid,
				AFE_PARAM_ID_AWDSP_RX_MSG, (void *)dsp_msg,
				sizeof(struct aw_dsp_msg_hdr) + data_size, true);
	if (ret < 0) {
		pr_err("%s:inline_id:0x%x, write data failed\n",
			__func__, inline_id);
		kfree(dsp_msg);
		dsp_msg = NULL;
		return ret;
	}

	kfree(dsp_msg);
	dsp_msg = NULL;
	return 0;
}

static int aw_read_msg_from_dsp(struct aw882xx *aw882xx, int inline_id,
			char *data, int data_size)
{
	struct aw_dsp_msg_hdr dsp_msg;
	int ret;

	dsp_msg.type = DSP_MSG_TYPE_CMD;
	dsp_msg.opcode_id = inline_id;
	dsp_msg.version = AWINIC_DSP_MSG_HDR_VER;

	mutex_lock(&g_msg_dsp_lock);
	ret = aw_send_afe_cal_apr(aw882xx->afe_rx_portid, aw882xx->afe_tx_portid,
			AFE_PARAM_ID_AWDSP_RX_MSG,
			&dsp_msg, sizeof(struct aw_dsp_msg_hdr), true);
	if (ret < 0) {
		pr_err("%s:inline_id:0x%x, write cmd to dsp failed\n",
			__func__, inline_id);
		goto dsp_msg_failed;
	}

	ret = aw_send_afe_cal_apr(aw882xx->afe_rx_portid, aw882xx->afe_tx_portid,
			AFE_PARAM_ID_AWDSP_RX_MSG,
			data, data_size, false);
	if (ret < 0) {
		pr_err("%s:inline_id:0x%x, read data from dsp failed\n",
			__func__, inline_id);
		goto dsp_msg_failed;
	}

	mutex_unlock(&g_msg_dsp_lock);
	return 0;

dsp_msg_failed:
	mutex_unlock(&g_msg_dsp_lock);
	return ret;
}

static int aw_misc_ops_read_dsp(struct aw882xx *aw882xx, aw_ioctl_msg_t *msg)
{
	char __user* user_data = (char __user*)msg->data_buf;
	uint32_t dsp_msg_id = (uint32_t)msg->opcode_id;
	int data_len = msg->data_len;
	int ret;
	char *data_ptr;

	data_ptr = kmalloc(data_len, GFP_KERNEL);
	if (!data_ptr) {
		pr_err("%s : malloc failed !\n", __func__);
		return -ENOMEM;
	}

	ret = aw_read_msg_from_dsp(aw882xx, dsp_msg_id, data_ptr, data_len);
	if (ret) {
		pr_err("%s : write failed\n", __func__);
		goto exit;
	}

	if (copy_to_user((void __user *)user_data,
		data_ptr, data_len)) {
		ret = -EFAULT;
	}
exit:
	kfree(data_ptr);
	return ret;
}

static int aw_misc_ops_write_dsp(struct aw882xx *aw882xx, aw_ioctl_msg_t *msg)
{
	char __user* user_data = (char __user*)msg->data_buf;
	uint32_t dsp_msg_id = (uint32_t)msg->opcode_id;
	int data_len = msg->data_len;
	int ret;
	char *data_ptr;

	data_ptr = kmalloc(data_len, GFP_KERNEL);
	if (!data_ptr) {
		pr_err("%s : malloc failed !\n", __func__);
		return -ENOMEM;
	}

	if (copy_from_user(data_ptr, (void __user *)user_data, data_len)) {
		pr_err("%s : copy data failed\n", __func__);
		ret = -EFAULT;
		goto exit;
	}

	ret = aw_write_msg_to_dsp(aw882xx, dsp_msg_id, data_ptr, data_len);
	if (ret) {
		pr_err("%s : write failed\n", __func__);
	}
exit:
	kfree(data_ptr);
	return ret;
}

static int aw_misc_ops_msg(struct aw882xx *aw882xx, unsigned long arg)
{
	aw_ioctl_msg_t ioctl_msg;

	if (copy_from_user(&ioctl_msg, (void __user *)arg, sizeof(aw_ioctl_msg_t))) {
		return -EFAULT;
	}

	if(ioctl_msg.version != AW_IOCTL_MSG_VERSION) {
		pr_err("%s:unsupported msg version %d\n", __func__, ioctl_msg.version);
		return -EINVAL;
	}

	if (ioctl_msg.type == AW_IOCTL_MSG_RD_DSP) {
		return aw_misc_ops_read_dsp(aw882xx, &ioctl_msg);
	} else if (ioctl_msg.type == AW_IOCTL_MSG_WR_DSP) {
		return aw_misc_ops_write_dsp(aw882xx, &ioctl_msg);
	} else {
		pr_err("%s:unsupported msg type %d\n", __func__, ioctl_msg.type);
		return -EINVAL;
	}
}

static int aw882xx_cali_operation(struct aw882xx *aw882xx,
			unsigned int cmd, unsigned long arg)
{
	int16_t data_len  = _IOC_SIZE(cmd);
	int ret = 0;
	char *data_ptr = NULL;
	struct ptr_params_data *p_params;
	int32_t *p_data;


	data_ptr = kmalloc(data_len, GFP_KERNEL);
	if (data_ptr == NULL) {
		pr_err("%s : malloc failed !\n", __func__);
		return -EFAULT;
	}

	pr_info("cmd : %d, data_len%d\n", cmd , data_len);
	switch (cmd) {
		case AW882XX_IOCTL_ENABLE_CALI: {
			if (copy_from_user(data_ptr,
					(void __user *)arg, data_len)) {
				ret = -EFAULT;
				goto exit;
			}
			g_aw882xx_cali_flag = (int8_t)data_ptr[0];
			pr_info("%s:set cali %s", __func__,
				(g_aw882xx_cali_flag == 0) ? ("disable") : ("enable"));
		} break;
		case AW882XX_IOCTL_SET_CALI_CFG: {
			if (copy_from_user(data_ptr,
					(void __user *)arg, data_len)) {
				ret = -EFAULT;
				goto exit;
			}
			ret = aw_send_afe_cal_apr(aw882xx->afe_rx_portid, aw882xx->afe_tx_portid,
				AFE_PARAM_ID_AWDSP_RX_CALI_CFG_L,
							data_ptr, data_len, true);
			if (ret) {
				pr_err("%s: dsp_msg_write error: 0x%x\n",
					__func__, AFE_PARAM_ID_AWDSP_RX_CALI_CFG_L);
				ret =  -EFAULT;
				goto exit;
			}
		} break;
		case AW882XX_IOCTL_GET_CALI_CFG: {
			ret = aw_send_afe_cal_apr(aw882xx->afe_rx_portid, aw882xx->afe_tx_portid,
				AFE_PARAM_ID_AWDSP_RX_CALI_CFG_L,
						data_ptr, data_len, false);
			if (ret) {
				pr_err("%s: dsp_msg_read error: 0x%x\n",
					__func__, AFE_PARAM_ID_AWDSP_RX_CALI_CFG_L);
				ret = -EFAULT;
				goto exit;
			}
			if (copy_to_user((void __user *)arg,
				data_ptr, data_len)) {
				ret = -EFAULT;
				goto exit;
			}
		} break;
		case AW882XX_IOCTL_GET_CALI_DATA: {
			ret = aw_send_afe_cal_apr(aw882xx->afe_rx_portid, aw882xx->afe_tx_portid,
				AFE_PARAM_ID_AWDSP_RX_REAL_DATA_L,
						data_ptr, data_len, false);
			if (ret) {
				pr_err("%s: dsp_msg_read error: 0x%x\n",
					__func__, AFE_PARAM_ID_AWDSP_RX_REAL_DATA_L);
				ret = -EFAULT;
				goto exit;
			}
			if (copy_to_user((void __user *)arg,
				data_ptr, data_len)) {
				ret = -EFAULT;
				goto exit;
			}
		} break;
		case AW882XX_IOCTL_SET_NOISE: {
			if (copy_from_user(data_ptr,
				(void __user *)arg, data_len)) {
				ret = -EFAULT;
				goto exit;
			}
			ret = aw_send_afe_cal_apr(aw882xx->afe_rx_portid, aw882xx->afe_tx_portid,
						AFE_PARAM_ID_AWDSP_RX_NOISE_L,
						data_ptr, data_len, true);
			if (ret) {
				pr_err("%s: dsp_msg_write error: 0x%x\n",
					__func__, AFE_PARAM_ID_AWDSP_RX_NOISE_L);
				ret = -EFAULT;
				goto exit;
			}
		} break;
		case AW882XX_IOCTL_GET_F0: {
			ret = aw_send_afe_cal_apr(aw882xx->afe_rx_portid, aw882xx->afe_tx_portid,
						AFE_PARAM_ID_AWDSP_RX_F0_L,
						data_ptr, data_len, false);
			if (ret) {
				pr_err("%s: dsp_msg_read error: 0x%x\n",
					__func__, AFE_PARAM_ID_AWDSP_RX_F0_L);
				ret = -EFAULT;
				goto exit;
			}
			if (copy_to_user((void __user *)arg,
				data_ptr, data_len)) {
				ret = -EFAULT;
				goto exit;
			}
		} break;
		case AW882XX_IOCTL_SET_CALI_RE: {
			if (copy_from_user(data_ptr,
				(void __user *)arg, data_len)) {
				ret = -EFAULT;
				goto exit;
			}
			if(aw882xx->default_re == 0) {
				ret = aw_send_afe_cal_apr(aw882xx->afe_rx_portid, aw882xx->afe_tx_portid,
					AFE_PARAM_ID_AWDSP_RX_RE_L, &aw882xx->default_re, sizeof(int32_t), false);
				pr_info("aw882xx: default_re:%d\n", aw882xx->default_re);
			}
			ret = aw_send_afe_cal_apr(aw882xx->afe_rx_portid, aw882xx->afe_tx_portid,
						AFE_PARAM_ID_AWDSP_RX_RE_L,
						data_ptr, data_len, true);
			if (ret) {
				pr_err("%s: dsp_msg_write error: 0x%x\n",
					__func__, AFE_PARAM_ID_AWDSP_RX_RE_L);
				ret = -EFAULT;
				goto exit;
			}
			aw882xx_set_cali_re(aw882xx, *((int32_t *)data_ptr));
		} break;
		case AW882XX_IOCTL_GET_CALI_RE: {
			ret = aw_send_afe_cal_apr(aw882xx->afe_rx_portid, aw882xx->afe_tx_portid,
						AFE_PARAM_ID_AWDSP_RX_RE_L,
						data_ptr, data_len, false);
			if(aw882xx->default_re == 0) {
				aw882xx->default_re = *((uint32_t *)data_ptr);
				pr_info("aw882xx: default_re:%d\n", aw882xx->default_re);
			}
			if (ret) {
				pr_err("%s: dsp_msg_read error: 0x%x\n",
					__func__, AFE_PARAM_ID_AWDSP_RX_RE_L);
				ret = -EFAULT;
				goto exit;
			}
			if (copy_to_user((void __user *)arg,
					data_ptr, data_len)) {
				ret = -EFAULT;
				goto exit;
			}
		} break;
		case AW882XX_IOCTL_GET_VMAX: {
			ret = aw_send_afe_cal_apr(aw882xx->afe_rx_portid, aw882xx->afe_tx_portid,
				AFE_PARAM_ID_AWDSP_RX_VMAX_L,
				data_ptr, data_len, false);
			if (ret) {
				pr_err("%s: dsp_msg_read error:0x%x\n",
					__func__, AFE_PARAM_ID_AWDSP_RX_VMAX_L);
				ret = -EFAULT;
				goto exit;
			}
			if (copy_to_user((void __user *)arg,
				data_ptr, data_len)) {
				ret = -EFAULT;
				goto exit;
			}
		} break;
		case AW882XX_IOCTL_SET_VMAX: {
			if (copy_from_user(data_ptr,
				(void __user *)arg, data_len)) {
				ret = -EFAULT;
				goto exit;
			}
			ret = aw_send_afe_cal_apr(aw882xx->afe_rx_portid, aw882xx->afe_tx_portid,
						AFE_PARAM_ID_AWDSP_RX_VMAX_L,
						data_ptr, data_len, true);
			if (ret) {
				pr_err("%s: dsp_msg_write error: 0x%x\n",
					__func__, AFE_PARAM_ID_AWDSP_RX_VMAX_L);
				ret = -EFAULT;
				goto exit;
			}
		} break;
		case AW882XX_IOCTL_SET_PARAM: {
			if (copy_from_user(data_ptr,
				(void __user *)arg, data_len)) {
				ret = -EFAULT;
				goto exit;
			}
			ret = aw_send_afe_cal_apr(aw882xx->afe_rx_portid, aw882xx->afe_tx_portid,
						AFE_PARAM_ID_AWDSP_RX_PARAMS,
						data_ptr, data_len, true);
			if (ret) {
				pr_err("%s: dsp_msg_write error: 0x%x\n",
				__func__, AFE_PARAM_ID_AWDSP_RX_PARAMS);
				ret = -EFAULT;
				goto exit;
			}
			pr_debug("%s: set params done", __func__);
		} break;
		case AW882XX_IOCTL_SET_PTR_PARAM_NUM: {
			if (copy_from_user(data_ptr, (void __user *)arg, data_len)) {
				ret = -EFAULT;
				goto exit;
			}
			p_params = (struct ptr_params_data *)data_ptr;
			if (p_params->data == NULL || (!p_params->len)) {
				pr_err("%s: p_params error\n", __func__);
				ret = -EFAULT;
				goto exit;
			}
			p_data = kzalloc(p_params->len, GFP_KERNEL);
			if (!p_data) {
				pr_err("%s: error allocating memory\n", __func__);
				ret = -ENOMEM;
				goto exit;
			}

			if (copy_from_user(p_data, (void __user *)p_params->data,
					p_params->len)) {
				kfree(p_data);
				p_data = NULL;
				ret = -EFAULT;
				goto exit;
			}

			ret = aw_send_afe_cal_apr(aw882xx->afe_rx_portid,
			            aw882xx->afe_tx_portid,
			            AFE_PARAM_ID_AWDSP_RX_PARAMS,
			            p_data, p_params->len, true);
			if (ret) {
				pr_err("%s: dsp_msg_write error: 0x%x\n",
				      __func__, AFE_PARAM_ID_AWDSP_RX_PARAMS);
				kfree(p_data);
				p_data = NULL;
				ret =-EFAULT;
				goto exit;
			}
			kfree(p_data);
			p_data = NULL;
		} break;
		case AW882XX_IOCTL_MSG:
			ret = aw_misc_ops_msg(aw882xx, arg);
			if (ret < 0)
				goto exit;
			break;
		default: {
			pr_err("%s : cmd %d\n", __func__, cmd);
		} break;
	}
exit:
	kfree(data_ptr);
	return ret;
}

static DEVICE_ATTR(monitor, S_IWUSR | S_IRUGO,
	aw882xx_monitor_show, aw882xx_monitor_store);
static DEVICE_ATTR(reg, S_IWUSR | S_IRUGO,
	aw882xx_reg_show, aw882xx_reg_store);
static DEVICE_ATTR(rw, S_IWUSR | S_IRUGO,
	aw882xx_rw_show, aw882xx_rw_store);
static DEVICE_ATTR(spk_rcv, S_IWUSR | S_IRUGO,
	aw882xx_spk_rcv_show, aw882xx_spk_rcv_store);
static DEVICE_ATTR(mec, S_IWUSR | S_IRUGO,
	aw882xx_mec_show, aw882xx_mec_store);
static DEVICE_ATTR(default_re, S_IWUSR | S_IRUGO,
	aw882xx_default_re_show, NULL);
static DEVICE_ATTR(spk_temp, S_IWUSR | S_IRUGO,
	aw882xx_spk_temp_show, NULL);
#ifdef AW_DEBUG
static DEVICE_ATTR(vol, S_IWUSR | S_IRUGO,
	aw882xx_vol_show, aw882xx_vol_store);
static DEVICE_ATTR(temp, S_IWUSR | S_IRUGO,
	aw882xx_temp_show, aw882xx_temp_store);
#endif

static struct attribute *aw882xx_attributes[] = {
	&dev_attr_monitor.attr,
	&dev_attr_reg.attr,
	&dev_attr_rw.attr,
	&dev_attr_spk_rcv.attr,
	&dev_attr_mec.attr,
	&dev_attr_default_re.attr,
	&dev_attr_spk_temp.attr,
#ifdef AW_DEBUG
	&dev_attr_vol.attr,
	&dev_attr_temp.attr,
#endif
	NULL
};

static struct attribute_group aw882xx_attribute_group = {
	.attrs = aw882xx_attributes
};

#define AW882XX_SMARTPA_NAME "aw882xx_smartpa"
static int aw882xx_file_open(struct inode *inode, struct file *file)
{
	if (!try_module_get(THIS_MODULE))
		return -ENODEV;
	file->private_data = (void *)g_aw882xx;

	pr_debug("open success");
	return 0;
}

static int aw882xx_file_release(struct inode *inode, struct file *file)
{
	file->private_data = (void *)NULL;

	pr_debug("release successi\n");
	return 0;
}

static long aw882xx_file_unlocked_ioctl(struct file *file,
			unsigned int cmd, unsigned long arg)
{
	int ret = 0;
	struct aw882xx *aw882xx = NULL;

	if (((_IOC_TYPE(cmd)) != (AW882XX_IOCTL_MAGIC))) {
	    pr_err("%s: cmd magic err\n", __func__);
	    return -EINVAL;
	}
	aw882xx = (struct aw882xx *)file->private_data;
	ret = aw882xx_cali_operation(aw882xx, cmd, arg);
	if (ret)
		return -EINVAL;

	return 0;
}

static struct file_operations aw882xx_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = aw882xx_file_unlocked_ioctl,
	.open = aw882xx_file_open,
	.release = aw882xx_file_release,
};

static struct miscdevice aw882xx_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = AW882XX_SMARTPA_NAME,
	.fops = &aw882xx_fops,
};

void init_aw882xx_misc_driver(struct aw882xx *aw882xx)
{
	int ret;

	ret = misc_register(&aw882xx_misc);
	if (ret) {
		pr_err("%s: misc register fail: %d\n", __func__, ret);
		return;
	}
	pr_debug("%s: misc register success", __func__);
}

/*monitor*/
static int aw882xx_monitor_start(struct aw882xx_monitor *monitor)
{
	pr_debug("%s: enter\n", __func__);

	if (!hrtimer_active(&monitor->timer)) {
		pr_info("%s: start monitor\n", __func__);
		hrtimer_start(&monitor->timer,
			ktime_set(monitor->timer_val/1000,
			 (monitor->timer_val%1000)*1000000), HRTIMER_MODE_REL);
	}

	return 0;
}

static int aw882xx_monitor_stop(struct aw882xx_monitor *monitor)
{
	pr_info("%s: enter\n", __func__);

	if (hrtimer_active(&monitor->timer)) {
		pr_info("%s: stop monitor\n", __func__);
		hrtimer_cancel(&monitor->timer);
	}
	return 0;
}

static enum hrtimer_restart
	aw882xx_monitor_timer_func(struct hrtimer *timer)
{
	struct aw882xx_monitor *monitor =
		container_of(timer, struct aw882xx_monitor, timer);

	pr_debug("%s : enter\n", __func__);

	if (monitor->is_enable)
		schedule_work(&monitor->work);

	return HRTIMER_NORESTART;
}
static int aw882xx_monitor_get_voltage(struct aw882xx *aw882xx,
						unsigned int *vol)
{
	int ret = -1;
	uint16_t local_vol = 0;

	ret = aw882xx_i2c_read(aw882xx, AW882XX_VBAT_REG, vol);
	if (ret < 0) {
		pr_err("%s: read voltage failed !\n",  __func__);
		return ret;
	}
	local_vol = (*vol) * AW882XX_MONITOR_VBAT_RANGE
				 / AW882XX_MONITOR_INT_10BIT;

	if(aw882xx->monitor.vol_count < AW_VOL_COUNT) {
		aw882xx->monitor.vol_count++;
		if(aw882xx->monitor.pre_vol == 0) {
			*vol = local_vol;
		} else {
			*vol = (aw882xx->monitor.pre_vol * (AW_VOL_COUNT -1 ) +
					local_vol) / AW_VOL_COUNT;
		}
	} else {
		*vol = (aw882xx->monitor.pre_vol * (AW_VOL_COUNT -1 ) +
					local_vol) / AW_VOL_COUNT;
	}
	aw882xx->monitor.pre_vol = *vol;
	pr_debug("%s: cur voltage is %d, vol is %d\n", __func__, local_vol, *vol);
	return 0;
}
static int aw882xx_monitor_voltage(struct aw882xx *aw882xx,
				struct aw882xx_low_vol *vol_cfg)
{
	int ret = -1;
	int i;
	unsigned int voltage = 0;
	struct aw882xx_monitor *monitor = NULL;

	if (aw882xx == NULL || vol_cfg == NULL) {
		pr_err("%s: pointer is NUL\n", __func__);
		return ret;
	}
	monitor = &aw882xx->monitor;
#ifdef AW_DEBUG
	if (monitor->test_vol == 0) {
		ret = aw882xx_monitor_get_voltage(aw882xx, &voltage);
		if (ret < 0)
			return ret;
	} else {
		voltage = monitor->test_vol;
	}
#else
	ret = aw882xx_monitor_get_voltage(aw882xx, &voltage);
	if (ret < 0)
		return ret;
#endif

	if (monitor->first_entry == AW_FIRST_ENTRY) {
		for (i = 0; i < monitor->vol_down_num; i++) {
			if (voltage < monitor->vol_down_table[i].vol) {
				*vol_cfg = monitor->vol_down_table[i];
				return 0;
			}
		}

		*vol_cfg = monitor->vol_up_table[0];
		return 0;
	}

	if (voltage < monitor->vol_down_table[0].vol) {
		*vol_cfg = monitor->vol_down_table[0];
		return 0;
	}

	for (i = 1; i < monitor->vol_down_num; i++) {
		if ((monitor->vol_up_table[monitor->vol_down_num - i].vol < voltage) &&
			(voltage < monitor->vol_down_table[i].vol)) {
			*vol_cfg = monitor->vol_down_table[i];
			return 0;
		}
	}

	if (voltage > monitor->vol_up_table[0].vol) {
		*vol_cfg = monitor->vol_up_table[0];
		return 0;
	}

	return 0;
}
static int aw882xx_monitor_get_temperature(struct aw882xx *aw882xx,  int *temp)
{
	int ret = -1;
	unsigned int reg_val = 0;
	uint16_t local_temp;

	ret = aw882xx_i2c_read(aw882xx, AW882XX_TEMP_REG, &reg_val);
	if (ret < 0) {
		pr_err("%s: get temperature failed !\n", __func__);
		return ret;
	}

	local_temp = reg_val;
	if (local_temp & AW882XX_MONITOR_TEMP_SIGN_MASK)
		local_temp = local_temp | AW882XX_MONITOR_TEMP_NEG_MASK;

	*temp = (int)local_temp;
	pr_debug("%s: chip temperature = %d\n", __func__, local_temp);
	return 0;
}

static int aw882xx_monitor_temperature(struct aw882xx *aw882xx,
				struct aw882xx_low_temp *temp_cfg)
{
	int ret;
	int i;
	struct aw882xx_monitor *monitor = NULL;
	int  current_temp = 0;

	monitor = &aw882xx->monitor;
#ifdef AW_DEBUG
	if (monitor->test_temp == 0) {
		ret = aw882xx_monitor_get_temperature(aw882xx, &current_temp);
		if (ret)
			return ret;
	} else {
		current_temp = monitor->test_temp;
	}
#else
	ret = aw882xx_monitor_get_temperature(aw882xx, &current_temp);
	if (ret < 0)
		return ret;
#endif

	if (monitor->first_entry == AW_FIRST_ENTRY) {
		monitor->first_entry = AW_NOT_FIRST_ENTRY;
		for (i = 0; i < monitor->temp_down_num; i++) {
			if (current_temp < monitor->temp_down_table[i].temp) {
				temp_cfg->ipeak = monitor->temp_down_table[i].ipeak;
				temp_cfg->gain = monitor->temp_down_table[i].gain;
				temp_cfg->vmax = monitor->temp_down_table[i].vmax;
				return 0;
			}
		}

		temp_cfg->ipeak = monitor->temp_up_table[0].ipeak;
		temp_cfg->gain = monitor->temp_up_table[0].gain;
		temp_cfg->vmax = monitor->temp_up_table[0].vmax;
		return 0;
	}

	if (current_temp < monitor->temp_down_table[0].temp) {
		temp_cfg->ipeak = monitor->temp_down_table[0].ipeak;
		temp_cfg->gain = monitor->temp_down_table[0].gain;
		temp_cfg->vmax = monitor->temp_down_table[0].vmax;
		return 0;
	}

	for (i = 1; i < monitor->temp_down_num; i++) {
		if ((monitor->temp_up_table[monitor->temp_down_num - i].temp < current_temp)
			 && (current_temp < monitor->temp_down_table[i].temp)) {
			temp_cfg->ipeak = monitor->temp_down_table[i].ipeak;
			temp_cfg->gain  = monitor->temp_down_table[i].gain;
			temp_cfg->vmax  = monitor->temp_down_table[i].vmax;
			return 0;
		}
	}

	if (current_temp > monitor->temp_up_table[0].temp) {
		temp_cfg->ipeak = monitor->temp_up_table[0].ipeak;
		temp_cfg->gain  = monitor->temp_up_table[0].gain;
		temp_cfg->vmax  = monitor->temp_up_table[0].vmax;
		return 0;
	}

	return 0;
}

static void aw882xx_monitor_get_cfg(struct aw882xx_low_temp *temp,
					struct aw882xx_low_vol *vol)
{
	if (vol->ipeak == IPEAK_NONE)
		return;

	if (temp->ipeak == IPEAK_NONE) {
		temp->ipeak = vol->ipeak;
		temp->gain  = vol->gain;
		return;
	}

	/*get min ipeak*/
	if (temp->ipeak > vol->ipeak)
		temp->ipeak = vol->ipeak;

	/*get min gain*/
	if (temp->gain < vol->gain)
		temp->gain = vol->gain;

}
static void aw882xx_monitor_set_ipeak(struct aw882xx *aw882xx, uint8_t ipeak)
{
	unsigned int reg_val = 0;
	unsigned int read_reg_val;
	int ret;

	if (ipeak == IPEAK_NONE)
		return;

	ret = aw882xx_i2c_read(aw882xx, AW882XX_SYSCTRL2_REG, &reg_val);
	if (ret < 0) {
		pr_err("%s: read ipeak failed\n", __func__);
		return;
	}

	read_reg_val = reg_val;
	read_reg_val &= AW882XX_BIT_SYSCTRL2_BST_IPEAK_MASK;

	if (read_reg_val == ipeak) {
		pr_debug("%s: ipeak = 0x%x, no change\n",
					__func__, read_reg_val);
		return;
	}
	reg_val &= (~AW882XX_BIT_SYSCTRL2_BST_IPEAK_MASK);
	read_reg_val = ipeak;
	reg_val |= read_reg_val;

	ret = aw882xx_i2c_write(aw882xx, AW882XX_SYSCTRL2_REG, reg_val);
	if (ret < 0) {
		pr_err("%s: write ipeak failed\n", __func__);
		return;
	}
	pr_debug("%s: set reg val = 0x%x, ipeak = 0x%x\n",
					__func__, reg_val, ipeak);
}
static void aw882xx_monitor_set_gain(struct aw882xx *aw882xx, uint8_t gain)
{
	unsigned int reg_val = 0;
	unsigned int read_reg_val;
	int ret;

	if(aw882xx == NULL) {
		return;
	}

	if (gain == GAIN_NONE || (aw882xx->fade_work_start == 1))
		return;

	ret = aw882xx_i2c_read(aw882xx, AW882XX_HAGCCFG4_REG, &reg_val);
	if (ret < 0) {
		pr_err("%s: read gain failed\n", __func__);
		return;
	}

	read_reg_val = reg_val >> AW882XX_BIT_HAGCCFG4_GAIN_SHIFT;
	aw882xx->cur_gain = gain;
	if (read_reg_val == gain) {
		pr_debug("%s: gain = 0x%x, no change\n",
				__func__, read_reg_val);
		return;
	}
	reg_val &= AW882XX_BIT_HAGCCFG4_GAIN_MASK;
	read_reg_val = gain;
	reg_val |= (read_reg_val << AW882XX_BIT_HAGCCFG4_GAIN_SHIFT);

	ret = aw882xx_i2c_write(aw882xx, AW882XX_HAGCCFG4_REG, reg_val);
	if (ret < 0) {
		pr_err("%s: write gain failed\n", __func__);
		return;
	}
	pr_debug("%s: set reg val = 0x%x, gain = 0x%x\n",
			__func__, reg_val, gain);
}
static void aw882xx_monitor_set_vmax(struct aw882xx *aw882xx, uint32_t vmax)
{
	uint32_t local_vmax = vmax;
	int ret;

	if (vmax == VMAX_NONE)
		return;

	ret = aw_send_afe_cal_apr(aw882xx->afe_rx_portid, aw882xx->afe_tx_portid,
				AFE_PARAM_ID_AWDSP_RX_VMAX_L,
				&local_vmax, sizeof(uint32_t), true);
	if (ret)
		pr_err("%s: dsp_msg_write error: 0x%x\n",
			__func__, AFE_PARAM_ID_AWDSP_RX_VMAX_L);

	pr_debug("%s: set vmax = 0x%x\n", __func__, vmax);
}
static void aw882xx_monitor_work(struct aw882xx *aw882xx)
{
	struct aw882xx_low_vol *vol_cfg = &aw882xx->monitor.vol_cfg;
	struct aw882xx_low_temp *temp_cfg = &aw882xx->monitor.temp_cfg;
	struct aw882xx_low_temp set_cfg;
	int ret;

	if (aw882xx == NULL) {
		pr_err("%s: pointer is NULL\n", __func__);
		return;
	}
	if (g_aw882xx_cali_flag != 0 || g_aw882xx_profile_flag != 0) {
		pr_info("%s: done nothing while start cali", __func__);
		return;
	}

	ret = aw882xx_monitor_voltage(aw882xx, vol_cfg);
	if (ret < 0) {
		pr_err("%s: monitor voltage failed\n", __func__);
		return;
	}

	ret = aw882xx_monitor_temperature(aw882xx, temp_cfg);
	if (ret < 0) {
		pr_err("%s: monitor temperature failed\n", __func__);
		return;
	}
	pr_debug("%s: vol: ipeak = 0x%x, gain = 0x%x\n",
			__func__, vol_cfg->ipeak, vol_cfg->gain);
	pr_debug("%s: temp: ipeak = 0x%x, gain = 0x%x, vmax = 0x%x\n",
		__func__, temp_cfg->ipeak, temp_cfg->gain, temp_cfg->vmax);

	memcpy(&set_cfg, temp_cfg, sizeof(struct aw882xx_low_temp));

	aw882xx_monitor_get_cfg(&set_cfg, vol_cfg);

	aw882xx_monitor_set_ipeak(aw882xx, set_cfg.ipeak);

	aw882xx_monitor_set_gain(aw882xx, set_cfg.gain);

	aw882xx_monitor_set_vmax(aw882xx, set_cfg.vmax);
}

static void aw882xx_monitor_init_gain(struct aw882xx *aw882xx)
{
	struct aw882xx_low_vol *vol_cfg = &aw882xx->monitor.vol_cfg;
	struct aw882xx_low_temp *temp_cfg = &aw882xx->monitor.temp_cfg;
	struct aw882xx_low_temp set_cfg;
	int ret;

	if (aw882xx == NULL) {
		pr_err("%s: pointer is NULL\n", __func__);
		return;
	}

	ret = aw882xx_monitor_voltage(aw882xx, vol_cfg);
	if (ret < 0) {
		pr_err("%s: monitor voltage failed\n", __func__);
		return;
	}

	ret = aw882xx_monitor_temperature(aw882xx, temp_cfg);
	if (ret < 0) {
		pr_err("%s: monitor temperature failed\n", __func__);
		return;
	}

	memcpy(&set_cfg, temp_cfg, sizeof(struct aw882xx_low_temp));

	aw882xx_monitor_get_cfg(&set_cfg, vol_cfg);
	aw882xx->cur_gain = set_cfg.gain;
}

static int aw882xx_get_hmute(struct aw882xx *aw882xx)
{
	unsigned int reg_val = 0;
	int ret;

	pr_debug("%s: enter\n", __func__);

	aw882xx_i2c_read(aw882xx, AW882XX_SYSCTRL2_REG, &reg_val);
	if ((~AW882XX_HMUTE_MASK) & reg_val)
		ret = 1;
	else
		ret = 0;

	return ret;
}
static void aw882xx_monitor_work_func(struct work_struct *work)
{
	struct aw882xx_monitor *monitor = container_of(work,
				struct aw882xx_monitor, work);
	struct aw882xx *aw882xx = container_of(monitor,
				struct aw882xx, monitor);

	pr_debug("%s: enter\n", __func__);
	mutex_lock(&aw882xx->lock);
	if (!aw882xx_get_hmute(aw882xx)) {
		aw882xx_monitor_work(aw882xx);
		aw882xx_monitor_start(monitor);
	}
	mutex_unlock(&aw882xx->lock);
}

void init_aw882xx_monitor(struct aw882xx_monitor *monitor)
{
	pr_info("%s: enter\n", __func__);
	hrtimer_init(&monitor->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	monitor->timer.function = aw882xx_monitor_timer_func;
	INIT_WORK(&monitor->work, aw882xx_monitor_work_func);
	monitor->pre_vol = 0;
	monitor->vol_count = 0;
	monitor->vol_cfg.ipeak = monitor->vol_up_table[0].ipeak;
	monitor->vol_cfg.gain = monitor->vol_up_table[0].gain;
	monitor->temp_cfg.ipeak = monitor->temp_up_table[0].ipeak;
	monitor->temp_cfg.gain = monitor->temp_up_table[0].gain;
	monitor->temp_cfg.vmax = monitor->temp_up_table[0].vmax;
#ifdef AW_DEBUG
	 monitor->test_vol = 0;
	 monitor->test_temp = 0;
#endif
}

/******************************************************
 *
 * i2c driver
 *
 ******************************************************/
static int aw882xx_i2c_probe(struct i2c_client *i2c,
	const struct i2c_device_id *id)
{
	struct snd_soc_dai_driver *dai;
	struct aw882xx *aw882xx;
	struct device_node *np = i2c->dev.of_node;
	int irq_flags = 0;
	int ret = -1;
	int i;

	pr_info("%s: enter\n", __func__);

	if (!i2c_check_functionality(i2c->adapter, I2C_FUNC_I2C)) {
		dev_err(&i2c->dev, "check_functionality failed\n");
		return -EIO;
	}

	aw882xx = devm_kzalloc(&i2c->dev, sizeof(struct aw882xx), GFP_KERNEL);
	if (aw882xx == NULL)
		return -ENOMEM;

	aw882xx->dev = &i2c->dev;
	aw882xx->i2c = i2c;

	i2c_set_clientdata(i2c, aw882xx);
	mutex_init(&aw882xx->lock);

	/* aw882xx rst & int */
	if (np) {
		ret = aw882xx_parse_dt(&i2c->dev, aw882xx, np);
		if (ret) {
			dev_err(&i2c->dev,
				"%s: failed to parse device tree node\n",
				__func__);
			goto err_parse_dt;
		}
	} else {
		aw882xx->reset_gpio = -1;
		aw882xx->irq_gpio = -1;
	}

	if (gpio_is_valid(aw882xx->reset_gpio)) {
		ret = devm_gpio_request_one(&i2c->dev, aw882xx->reset_gpio,
			GPIOF_OUT_INIT_LOW, "aw882xx_rst");
		if (ret) {
			dev_err(&i2c->dev, "%s: rst request failed\n",
				__func__);
			goto err_reset_gpio_request;
		}
	}

	if (gpio_is_valid(aw882xx->irq_gpio)) {
		ret = devm_gpio_request_one(&i2c->dev, aw882xx->irq_gpio,
			GPIOF_DIR_IN, "aw882xx_int");
		if (ret) {
			dev_err(&i2c->dev, "%s: int request failed\n",
				__func__);
			goto err_irq_gpio_request;
		}
	}

	/* hardware reset */
	aw882xx_hw_reset(aw882xx);

	/* aw882xx chip id */
	ret = aw882xx_read_chipid(aw882xx);
	if (ret < 0) {
		dev_err(&i2c->dev, "%s: aw882xx_read_chipid failed ret=%d\n",
			__func__, ret);
		goto err_id;
	}

	/* aw882xx device name */
	//if (i2c->dev.of_node)
	//	dev_set_name(&i2c->dev, "%s", "aw882xx_smartpa");
	//else
	//	dev_err(&i2c->dev, "%s failed to set device name: %d\n",
	//		__func__, ret);

	/* register codec */
	dai = devm_kzalloc(&i2c->dev, sizeof(aw882xx_dai), GFP_KERNEL);
	if (!dai)
		goto err_dai_kzalloc;

	memcpy(dai, aw882xx_dai, sizeof(aw882xx_dai));
	pr_info("%s: dai->name(%s)\n", __func__, dai->name);

	ret = snd_soc_register_component(&i2c->dev, &soc_component_dev_aw882xx,
			dai, ARRAY_SIZE(aw882xx_dai));
	if (ret < 0) {
		dev_err(&i2c->dev, "%s failed to register aw882xx: %d\n",
			__func__, ret);
		goto err_register_codec;
	}

	/* aw882xx irq */
	if (gpio_is_valid(aw882xx->irq_gpio) &&
		!(aw882xx->flags & AW882XX_FLAG_SKIP_INTERRUPTS)) {
		aw882xx_interrupt_setup(aw882xx);
		/* register irq handler */
		irq_flags = IRQF_TRIGGER_FALLING | IRQF_ONESHOT;
		ret = devm_request_threaded_irq(&i2c->dev,
				gpio_to_irq(aw882xx->irq_gpio),
				NULL, aw882xx_irq, irq_flags,
				"aw882xx", aw882xx);
		if (ret != 0) {
			dev_err(&i2c->dev, "failed to request IRQ %d: %d\n",
				gpio_to_irq(aw882xx->irq_gpio), ret);
			goto err_irq;
		}
	} else {
		dev_info(&i2c->dev, "%s skipping IRQ registration\n", __func__);
		/* disable feature support if gpio was invalid */
		aw882xx->flags |= AW882XX_FLAG_SKIP_INTERRUPTS;
	}

	dev_set_drvdata(&i2c->dev, aw882xx);
	ret = sysfs_create_group(&i2c->dev.kobj, &aw882xx_attribute_group);
	if (ret < 0) {
		dev_info(&i2c->dev,
			"%s error creating sysfs attr files\n",
			__func__);
		goto err_sysfs;
	}

	init_aw882xx_monitor(&aw882xx->monitor);
	aw882xx_monitor_init_gain(aw882xx);
	init_aw882xx_misc_driver(aw882xx);
	g_aw882xx = aw882xx;

	aw882xx->default_re = 0;
	aw882xx->need_fade = 0;
	aw882xx->fade_work_start = 0;
	aw882xx->delayed_time = 0;
	aw882xx->is_fade_in = 0;

	/*init profile*/
	mutex_init(&aw882xx->profile.lock);
	aw882xx->profile.cur_profile = 0;
	aw882xx->profile.status = AW882XX_INIT_ST;
	aw882xx->profile.len = 0;
	for (i = 0; i < AW_PROFILE_MAX; i++) {
		aw882xx->profile.data[i] = NULL;
	}

	INIT_DELAYED_WORK(&aw882xx->fade_work, aw882xx_fade_work_func);
	/*global init*/
	atomic_set(&g_algo_rx_enable, 0);
	atomic_set(&g_algo_tx_enable, 0);
	atomic_set(&g_skt_disable, 0);
#ifdef AW882XX_RUNIN_TEST
	atomic_set(&g_runin_test, 0);
	INIT_DELAYED_WORK(&aw882xx->adsp_status, aw882xx_set_adsp_module_status);
#endif
	pr_info("%s: probe completed successfully!\n", __func__);

	return 0;

err_sysfs:
	devm_free_irq(&i2c->dev, gpio_to_irq(aw882xx->irq_gpio), aw882xx);
err_irq:
	snd_soc_unregister_component(&i2c->dev);
err_register_codec:
	devm_kfree(&i2c->dev, dai);
	dai = NULL;
err_dai_kzalloc:
err_id:
	if (gpio_is_valid(aw882xx->irq_gpio))
		devm_gpio_free(&i2c->dev, aw882xx->irq_gpio);
err_irq_gpio_request:
	if (gpio_is_valid(aw882xx->reset_gpio))
		devm_gpio_free(&i2c->dev, aw882xx->reset_gpio);
err_reset_gpio_request:
err_parse_dt:
	devm_kfree(&i2c->dev, aw882xx);
	aw882xx = NULL;
	g_aw882xx = NULL;
	return ret;
}

static int aw882xx_i2c_remove(struct i2c_client *i2c)
{
	struct aw882xx *aw882xx = i2c_get_clientdata(i2c);

	pr_info("%s: enter\n", __func__);
	misc_deregister(&aw882xx_misc);
	if (gpio_to_irq(aw882xx->irq_gpio))
		devm_free_irq(&i2c->dev,
			gpio_to_irq(aw882xx->irq_gpio),
			aw882xx);

	snd_soc_unregister_component(&i2c->dev);

	if (gpio_is_valid(aw882xx->irq_gpio))
		devm_gpio_free(&i2c->dev, aw882xx->irq_gpio);
	if (gpio_is_valid(aw882xx->reset_gpio))
		devm_gpio_free(&i2c->dev, aw882xx->reset_gpio);

	devm_kfree(&i2c->dev, aw882xx);
	aw882xx = NULL;
	g_aw882xx = NULL;
	return 0;
}

static const struct i2c_device_id aw882xx_i2c_id[] = {
	{ AW882XX_I2C_NAME, 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, aw882xx_i2c_id);

static struct of_device_id aw882xx_dt_match[] = {
	{ .compatible = "awinic,aw882xx_smartpa" },
	{ },
};

static struct i2c_driver aw882xx_i2c_driver = {
	.driver = {
		.name = AW882XX_I2C_NAME,
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(aw882xx_dt_match),
	},
	.probe = aw882xx_i2c_probe,
	.remove = aw882xx_i2c_remove,
	.id_table = aw882xx_i2c_id,
};

static int __init aw882xx_i2c_init(void)
{
	int ret = -1;

	pr_info("%s: aw882xx driver version %s\n", __func__, AW882XX_VERSION);

	ret = i2c_add_driver(&aw882xx_i2c_driver);
	if (ret)
		pr_err("%s: fail to add aw882xx device into i2c\n", __func__);

	return ret;
}

#if KERNEL_VERSION(5, 4, 0) <= LINUX_VERSION_CODE
MODULE_IMPORT_NS(VFS_internal_I_am_really_a_filesystem_and_am_NOT_a_driver);
#endif

module_init(aw882xx_i2c_init);


static void __exit aw882xx_i2c_exit(void)
{
	i2c_del_driver(&aw882xx_i2c_driver);
}
module_exit(aw882xx_i2c_exit);

MODULE_DESCRIPTION("ASoC AW882XX Smart PA Driver");
MODULE_LICENSE("GPL v2");

#endif /* CONFIG_AW882XX_CODEC */
