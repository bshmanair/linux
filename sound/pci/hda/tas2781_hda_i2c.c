// SPDX-License-Identifier: GPL-2.0
//
// TAS2781 HDA I2C driver
//
// Copyright 2023 - 2024 Texas Instruments, Inc.
//
// Author: Shenghao Ding <shenghao-ding@ti.com>
// Current maintainer: Baojun Xu <baojun.xu@ti.com>

#include <linux/unaligned.h>
#include <linux/acpi.h>
#include <linux/crc8.h>
#include <linux/crc32.h>
#include <linux/efi.h>
#include <linux/firmware.h>
#include <linux/i2c.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/pci_ids.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <sound/hda_codec.h>
#include <sound/soc.h>
#include <sound/tas2781.h>
#include <sound/tlv.h>
#include <sound/tas2781-tlv.h>

#include "hda_local.h"
#include "hda_auto_parser.h"
#include "hda_component.h"
#include "hda_jack.h"
#include "hda_generic.h"

#define TASDEVICE_SPEAKER_CALIBRATION_SIZE	20

/* No standard control callbacks for SNDRV_CTL_ELEM_IFACE_CARD
 * Define two controls, one is Volume control callbacks, the other is
 * flag setting control callbacks.
 */

/* Volume control callbacks for tas2781 */
#define ACARD_SINGLE_RANGE_EXT_TLV(xname, xreg, xshift, xmin, xmax, xinvert, \
	xhandler_get, xhandler_put, tlv_array) \
{	.iface = SNDRV_CTL_ELEM_IFACE_CARD, .name = (xname),\
	.access = SNDRV_CTL_ELEM_ACCESS_TLV_READ |\
		 SNDRV_CTL_ELEM_ACCESS_READWRITE,\
	.tlv.p = (tlv_array), \
	.info = snd_soc_info_volsw, \
	.get = xhandler_get, .put = xhandler_put, \
	.private_value = (unsigned long)&(struct soc_mixer_control) \
		{.reg = xreg, .rreg = xreg, .shift = xshift, \
		 .rshift = xshift, .min = xmin, .max = xmax, \
		 .invert = xinvert} }

/* Flag control callbacks for tas2781 */
#define ACARD_SINGLE_BOOL_EXT(xname, xdata, xhandler_get, xhandler_put) \
{	.iface = SNDRV_CTL_ELEM_IFACE_CARD, .name = xname, \
	.info = snd_ctl_boolean_mono_info, \
	.get = xhandler_get, .put = xhandler_put, \
	.private_value = xdata }

enum calib_data {
	R0_VAL = 0,
	INV_R0,
	R0LOW,
	POWER,
	TLIM,
	CALIB_MAX
};

#define TAS2563_MAX_CHANNELS	4

#define TAS2563_CAL_POWER	TASDEVICE_REG(0, 0x0d, 0x3c)
#define TAS2563_CAL_R0		TASDEVICE_REG(0, 0x0f, 0x34)
#define TAS2563_CAL_INVR0	TASDEVICE_REG(0, 0x0f, 0x40)
#define TAS2563_CAL_R0_LOW	TASDEVICE_REG(0, 0x0f, 0x48)
#define TAS2563_CAL_TLIM	TASDEVICE_REG(0, 0x10, 0x14)
#define TAS2563_CAL_N		5
#define TAS2563_CAL_DATA_SIZE	4
#define TAS2563_CAL_CH_SIZE	20
#define TAS2563_CAL_ARRAY_SIZE	80

static unsigned int cal_regs[TAS2563_CAL_N] = {
	TAS2563_CAL_POWER, TAS2563_CAL_R0, TAS2563_CAL_INVR0,
	TAS2563_CAL_R0_LOW, TAS2563_CAL_TLIM,
};


struct tas2781_hda {
	struct device *dev;
	struct tasdevice_priv *priv;
	struct snd_kcontrol *dsp_prog_ctl;
	struct snd_kcontrol *dsp_conf_ctl;
	struct snd_kcontrol *prof_ctl;
	struct snd_kcontrol *snd_ctls[2];
};

static int tas2781_get_i2c_res(struct acpi_resource *ares, void *data)
{
	struct tasdevice_priv *tas_priv = data;
	struct acpi_resource_i2c_serialbus *sb;

	if (i2c_acpi_get_i2c_resource(ares, &sb)) {
		if (tas_priv->ndev < TASDEVICE_MAX_CHANNELS &&
			sb->slave_address != tas_priv->global_addr) {
			tas_priv->tasdevice[tas_priv->ndev].dev_addr =
				(unsigned int)sb->slave_address;
			tas_priv->ndev++;
		}
	}
	return 1;
}

static const struct acpi_gpio_params speakerid_gpios = { 0, 0, false };

static const struct acpi_gpio_mapping tas2781_speaker_id_gpios[] = {
	{ "speakerid-gpios", &speakerid_gpios, 1 },
	{ }
};

static int tas2781_read_acpi(struct tasdevice_priv *p, const char *hid)
{
	struct acpi_device *adev;
	struct device *physdev;
	LIST_HEAD(resources);
	const char *sub;
	uint32_t subid;
	int ret;

	adev = acpi_dev_get_first_match_dev(hid, NULL, -1);
	if (!adev) {
		dev_err(p->dev,
			"Failed to find an ACPI device for %s\n", hid);
		return -ENODEV;
	}

	physdev = get_device(acpi_get_first_physical_node(adev));
	ret = acpi_dev_get_resources(adev, &resources, tas2781_get_i2c_res, p);
	if (ret < 0) {
		dev_err(p->dev, "Failed to get ACPI resource.\n");
		goto err;
	}
	sub = acpi_get_subsystem_id(ACPI_HANDLE(physdev));
	if (IS_ERR(sub)) {
		/* No subsys id in older tas2563 projects. */
		if (!strncmp(hid, "INT8866", sizeof("INT8866")))
			goto end_2563;
		dev_err(p->dev, "Failed to get SUBSYS ID.\n");
		ret = PTR_ERR(sub);
		goto err;
	}
	/* Speaker id was needed for ASUS projects. */
	ret = kstrtou32(sub, 16, &subid);
	if (!ret && upper_16_bits(subid) == PCI_VENDOR_ID_ASUSTEK) {
		ret = devm_acpi_dev_add_driver_gpios(p->dev,
			tas2781_speaker_id_gpios);
		if (ret < 0)
			dev_err(p->dev, "Failed to add driver gpio %d.\n",
				ret);
		p->speaker_id = devm_gpiod_get(p->dev, "speakerid", GPIOD_IN);
		if (IS_ERR(p->speaker_id)) {
			dev_err(p->dev, "Failed to get Speaker id.\n");
			ret = PTR_ERR(p->speaker_id);
			goto err;
		}
	} else {
		p->speaker_id = NULL;
	}

end_2563:
	acpi_dev_free_resource_list(&resources);
	strscpy(p->dev_name, hid, sizeof(p->dev_name));
	put_device(physdev);
	acpi_dev_put(adev);

	return 0;

err:
	dev_err(p->dev, "read acpi error, ret: %d\n", ret);
	put_device(physdev);
	acpi_dev_put(adev);

	return ret;
}

static void tas2781_hda_playback_hook(struct device *dev, int action)
{
	struct tas2781_hda *tas_hda = dev_get_drvdata(dev);

	dev_dbg(tas_hda->dev, "%s: action = %d\n", __func__, action);
	switch (action) {
	case HDA_GEN_PCM_ACT_OPEN:
		pm_runtime_get_sync(dev);
		mutex_lock(&tas_hda->priv->codec_lock);
		tasdevice_tuning_switch(tas_hda->priv, 0);
		tas_hda->priv->playback_started = true;
		mutex_unlock(&tas_hda->priv->codec_lock);
		break;
	case HDA_GEN_PCM_ACT_CLOSE:
		mutex_lock(&tas_hda->priv->codec_lock);
		tasdevice_tuning_switch(tas_hda->priv, 1);
		tas_hda->priv->playback_started = false;
		mutex_unlock(&tas_hda->priv->codec_lock);

		pm_runtime_mark_last_busy(dev);
		pm_runtime_put_autosuspend(dev);
		break;
	default:
		break;
	}
}

static int tasdevice_info_profile(struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_info *uinfo)
{
	struct tasdevice_priv *tas_priv = snd_kcontrol_chip(kcontrol);

	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 1;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = tas_priv->rcabin.ncfgs - 1;

	return 0;
}

static int tasdevice_get_profile_id(struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_value *ucontrol)
{
	struct tasdevice_priv *tas_priv = snd_kcontrol_chip(kcontrol);

	mutex_lock(&tas_priv->codec_lock);

	ucontrol->value.integer.value[0] = tas_priv->rcabin.profile_cfg_id;

	dev_dbg(tas_priv->dev, "%s: kcontrol %s: %d\n",
		__func__, kcontrol->id.name, tas_priv->rcabin.profile_cfg_id);

	mutex_unlock(&tas_priv->codec_lock);

	return 0;
}

static int tasdevice_set_profile_id(struct snd_kcontrol *kcontrol,
		struct snd_ctl_elem_value *ucontrol)
{
	struct tasdevice_priv *tas_priv = snd_kcontrol_chip(kcontrol);
	int nr_profile = ucontrol->value.integer.value[0];
	int max = tas_priv->rcabin.ncfgs - 1;
	int val, ret = 0;

	val = clamp(nr_profile, 0, max);

	mutex_lock(&tas_priv->codec_lock);

	dev_dbg(tas_priv->dev, "%s: kcontrol %s: %d -> %d\n",
		__func__, kcontrol->id.name,
		tas_priv->rcabin.profile_cfg_id, val);

	if (tas_priv->rcabin.profile_cfg_id != val) {
		tas_priv->rcabin.profile_cfg_id = val;
		ret = 1;
	}

	mutex_unlock(&tas_priv->codec_lock);

	return ret;
}

static int tasdevice_info_programs(struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_info *uinfo)
{
	struct tasdevice_priv *tas_priv = snd_kcontrol_chip(kcontrol);
	struct tasdevice_fw *tas_fw = tas_priv->fmw;

	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 1;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = tas_fw->nr_programs - 1;

	return 0;
}

static int tasdevice_info_config(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_info *uinfo)
{
	struct tasdevice_priv *tas_priv = snd_kcontrol_chip(kcontrol);
	struct tasdevice_fw *tas_fw = tas_priv->fmw;

	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 1;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = tas_fw->nr_configurations - 1;

	return 0;
}

static int tasdevice_program_get(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct tasdevice_priv *tas_priv = snd_kcontrol_chip(kcontrol);

	mutex_lock(&tas_priv->codec_lock);

	ucontrol->value.integer.value[0] = tas_priv->cur_prog;

	dev_dbg(tas_priv->dev, "%s: kcontrol %s: %d\n",
		__func__, kcontrol->id.name, tas_priv->cur_prog);

	mutex_unlock(&tas_priv->codec_lock);

	return 0;
}

static int tasdevice_program_put(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct tasdevice_priv *tas_priv = snd_kcontrol_chip(kcontrol);
	struct tasdevice_fw *tas_fw = tas_priv->fmw;
	int nr_program = ucontrol->value.integer.value[0];
	int max = tas_fw->nr_programs - 1;
	int val, ret = 0;

	val = clamp(nr_program, 0, max);

	mutex_lock(&tas_priv->codec_lock);

	dev_dbg(tas_priv->dev, "%s: kcontrol %s: %d -> %d\n",
		__func__, kcontrol->id.name, tas_priv->cur_prog, val);

	if (tas_priv->cur_prog != val) {
		tas_priv->cur_prog = val;
		ret = 1;
	}

	mutex_unlock(&tas_priv->codec_lock);

	return ret;
}

static int tasdevice_config_get(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct tasdevice_priv *tas_priv = snd_kcontrol_chip(kcontrol);

	mutex_lock(&tas_priv->codec_lock);

	ucontrol->value.integer.value[0] = tas_priv->cur_conf;

	dev_dbg(tas_priv->dev, "%s: kcontrol %s: %d\n",
		__func__, kcontrol->id.name, tas_priv->cur_conf);

	mutex_unlock(&tas_priv->codec_lock);

	return 0;
}

static int tasdevice_config_put(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct tasdevice_priv *tas_priv = snd_kcontrol_chip(kcontrol);
	struct tasdevice_fw *tas_fw = tas_priv->fmw;
	int nr_config = ucontrol->value.integer.value[0];
	int max = tas_fw->nr_configurations - 1;
	int val, ret = 0;

	val = clamp(nr_config, 0, max);

	mutex_lock(&tas_priv->codec_lock);

	dev_dbg(tas_priv->dev, "%s: kcontrol %s: %d -> %d\n",
		__func__, kcontrol->id.name, tas_priv->cur_conf, val);

	if (tas_priv->cur_conf != val) {
		tas_priv->cur_conf = val;
		ret = 1;
	}

	mutex_unlock(&tas_priv->codec_lock);

	return ret;
}

static int tas2781_amp_getvol(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct tasdevice_priv *tas_priv = snd_kcontrol_chip(kcontrol);
	struct soc_mixer_control *mc =
		(struct soc_mixer_control *)kcontrol->private_value;
	int ret;

	mutex_lock(&tas_priv->codec_lock);

	ret = tasdevice_amp_getvol(tas_priv, ucontrol, mc);

	dev_dbg(tas_priv->dev, "%s: kcontrol %s: %ld\n",
		__func__, kcontrol->id.name, ucontrol->value.integer.value[0]);

	mutex_unlock(&tas_priv->codec_lock);

	return ret;
}

static int tas2781_amp_putvol(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct tasdevice_priv *tas_priv = snd_kcontrol_chip(kcontrol);
	struct soc_mixer_control *mc =
		(struct soc_mixer_control *)kcontrol->private_value;
	int ret;

	mutex_lock(&tas_priv->codec_lock);

	dev_dbg(tas_priv->dev, "%s: kcontrol %s: -> %ld\n",
		__func__, kcontrol->id.name, ucontrol->value.integer.value[0]);

	/* The check of the given value is in tasdevice_amp_putvol. */
	ret = tasdevice_amp_putvol(tas_priv, ucontrol, mc);

	mutex_unlock(&tas_priv->codec_lock);

	return ret;
}

static int tas2781_force_fwload_get(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct tasdevice_priv *tas_priv = snd_kcontrol_chip(kcontrol);

	mutex_lock(&tas_priv->codec_lock);

	ucontrol->value.integer.value[0] = (int)tas_priv->force_fwload_status;
	dev_dbg(tas_priv->dev, "%s: kcontrol %s: %d\n",
		__func__, kcontrol->id.name, tas_priv->force_fwload_status);

	mutex_unlock(&tas_priv->codec_lock);

	return 0;
}

static int tas2781_force_fwload_put(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct tasdevice_priv *tas_priv = snd_kcontrol_chip(kcontrol);
	bool change, val = (bool)ucontrol->value.integer.value[0];

	mutex_lock(&tas_priv->codec_lock);

	dev_dbg(tas_priv->dev, "%s: kcontrol %s: %d -> %d\n",
		__func__, kcontrol->id.name,
		tas_priv->force_fwload_status, val);

	if (tas_priv->force_fwload_status == val)
		change = false;
	else {
		change = true;
		tas_priv->force_fwload_status = val;
	}

	mutex_unlock(&tas_priv->codec_lock);

	return change;
}

static const struct snd_kcontrol_new tas2781_snd_controls[] = {
	ACARD_SINGLE_RANGE_EXT_TLV("Speaker Analog Gain", TAS2781_AMP_LEVEL,
		1, 0, 20, 0, tas2781_amp_getvol,
		tas2781_amp_putvol, amp_vol_tlv),
	ACARD_SINGLE_BOOL_EXT("Speaker Force Firmware Load", 0,
		tas2781_force_fwload_get, tas2781_force_fwload_put),
};

static const struct snd_kcontrol_new tas2781_prof_ctrl = {
	.name = "Speaker Profile Id",
	.iface = SNDRV_CTL_ELEM_IFACE_CARD,
	.info = tasdevice_info_profile,
	.get = tasdevice_get_profile_id,
	.put = tasdevice_set_profile_id,
};

static const struct snd_kcontrol_new tas2781_dsp_prog_ctrl = {
	.name = "Speaker Program Id",
	.iface = SNDRV_CTL_ELEM_IFACE_CARD,
	.info = tasdevice_info_programs,
	.get = tasdevice_program_get,
	.put = tasdevice_program_put,
};

static const struct snd_kcontrol_new tas2781_dsp_conf_ctrl = {
	.name = "Speaker Config Id",
	.iface = SNDRV_CTL_ELEM_IFACE_CARD,
	.info = tasdevice_info_config,
	.get = tasdevice_config_get,
	.put = tasdevice_config_put,
};

static void tas2563_apply_calib(struct tasdevice_priv *tas_priv)
{
	int offset = 0;
	__be32 data;
	int ret;

	for (int i = 0; i < tas_priv->ndev; i++) {
		for (int j = 0; j < TAS2563_CAL_N; ++j) {
			data = cpu_to_be32(
				*(uint32_t *)&tas_priv->cali_data.data[offset]);
			ret = tasdevice_dev_bulk_write(tas_priv, i, cal_regs[j],
				(unsigned char *)&data, TAS2563_CAL_DATA_SIZE);
			if (ret)
				dev_err(tas_priv->dev,
					"Error writing calib regs\n");
			offset += TAS2563_CAL_DATA_SIZE;
		}
	}
}

static int tas2563_save_calibration(struct tasdevice_priv *tas_priv)
{
	static efi_guid_t efi_guid = EFI_GUID(0x1f52d2a1, 0xbb3a, 0x457d, 0xbc,
		0x09, 0x43, 0xa3, 0xf4, 0x31, 0x0a, 0x92);

	static efi_char16_t *efi_vars[TAS2563_MAX_CHANNELS][TAS2563_CAL_N] = {
		{ L"Power_1", L"R0_1", L"InvR0_1", L"R0_Low_1", L"TLim_1" },
		{ L"Power_2", L"R0_2", L"InvR0_2", L"R0_Low_2", L"TLim_2" },
		{ L"Power_3", L"R0_3", L"InvR0_3", L"R0_Low_3", L"TLim_3" },
		{ L"Power_4", L"R0_4", L"InvR0_4", L"R0_Low_4", L"TLim_4" },
	};

	unsigned long max_size = TAS2563_CAL_DATA_SIZE;
	unsigned int offset = 0;
	efi_status_t status;
	unsigned int attr;

	tas_priv->cali_data.data = devm_kzalloc(tas_priv->dev,
			TAS2563_CAL_ARRAY_SIZE, GFP_KERNEL);
	if (!tas_priv->cali_data.data)
		return -ENOMEM;

	for (int i = 0; i < tas_priv->ndev; ++i) {
		for (int j = 0; j < TAS2563_CAL_N; ++j) {
			status = efi.get_variable(efi_vars[i][j],
				&efi_guid, &attr, &max_size,
				&tas_priv->cali_data.data[offset]);
			if (status != EFI_SUCCESS ||
				max_size != TAS2563_CAL_DATA_SIZE) {
				dev_warn(tas_priv->dev,
				"Calibration data read failed %ld\n", status);
				return -EINVAL;
			}
			offset += TAS2563_CAL_DATA_SIZE;
		}
	}

	tas_priv->cali_data.total_sz = offset;
	tasdevice_apply_calibration(tas_priv);

	return 0;
}

static void tas2781_apply_calib(struct tasdevice_priv *tas_priv)
{
	struct calidata *cali_data = &tas_priv->cali_data;
	struct cali_reg *r = &cali_data->cali_reg_array;
	unsigned int cali_reg[CALIB_MAX] = {
		TASDEVICE_REG(0, 0x17, 0x74),
		TASDEVICE_REG(0, 0x18, 0x0c),
		TASDEVICE_REG(0, 0x18, 0x14),
		TASDEVICE_REG(0, 0x13, 0x70),
		TASDEVICE_REG(0, 0x18, 0x7c),
	};
	int i, j, rc;
	int oft = 0;
	__be32 data;

	if (tas_priv->dspbin_typ != TASDEV_BASIC) {
		cali_reg[0] = r->r0_reg;
		cali_reg[1] = r->invr0_reg;
		cali_reg[2] = r->r0_low_reg;
		cali_reg[3] = r->pow_reg;
		cali_reg[4] = r->tlimit_reg;
	}

	for (i = 0; i < tas_priv->ndev; i++) {
		for (j = 0; j < CALIB_MAX; j++) {
			data = cpu_to_be32(
				*(uint32_t *)&tas_priv->cali_data.data[oft]);
			rc = tasdevice_dev_bulk_write(tas_priv, i,
				cali_reg[j], (unsigned char *)&data, 4);
			if (rc < 0)
				dev_err(tas_priv->dev,
					"chn %d calib %d bulk_wr err = %d\n",
					i, j, rc);
			oft += 4;
		}
	}
}

/* Update the calibration data, including speaker impedance, f0, etc, into algo.
 * Calibrate data is done by manufacturer in the factory. These data are used
 * by Algo for calculating the speaker temperature, speaker membrane excursion
 * and f0 in real time during playback.
 */
static int tas2781_save_calibration(struct tasdevice_priv *tas_priv)
{
	efi_guid_t efi_guid = EFI_GUID(0x02f9af02, 0x7734, 0x4233, 0xb4, 0x3d,
		0x93, 0xfe, 0x5a, 0xa3, 0x5d, 0xb3);
	static efi_char16_t efi_name[] = L"CALI_DATA";
	unsigned int attr, crc;
	unsigned int *tmp_val;
	efi_status_t status;

	/* Lenovo devices */
	if (tas_priv->catlog_id == LENOVO)
		efi_guid = EFI_GUID(0x1f52d2a1, 0xbb3a, 0x457d, 0xbc, 0x09,
			0x43, 0xa3, 0xf4, 0x31, 0x0a, 0x92);

	tas_priv->cali_data.total_sz = 0;
	/* Get real size of UEFI variable */
	status = efi.get_variable(efi_name, &efi_guid, &attr,
		&tas_priv->cali_data.total_sz, tas_priv->cali_data.data);
	if (status == EFI_BUFFER_TOO_SMALL) {
		/* Allocate data buffer of data_size bytes */
		tas_priv->cali_data.data = devm_kzalloc(tas_priv->dev,
			tas_priv->cali_data.total_sz, GFP_KERNEL);
		if (!tas_priv->cali_data.data)
			return -ENOMEM;
		/* Get variable contents into buffer */
		status = efi.get_variable(efi_name, &efi_guid, &attr,
			&tas_priv->cali_data.total_sz,
			tas_priv->cali_data.data);
	}
	if (status != EFI_SUCCESS)
		return -EINVAL;

	tmp_val = (unsigned int *)tas_priv->cali_data.data;

	crc = crc32(~0, tas_priv->cali_data.data, 84) ^ ~0;
	dev_dbg(tas_priv->dev, "cali crc 0x%08x PK tmp_val 0x%08x\n",
		crc, tmp_val[21]);

	if (crc == tmp_val[21]) {
		time64_t seconds = tmp_val[20];

		dev_dbg(tas_priv->dev, "%ptTsr\n", &seconds);
		tasdevice_apply_calibration(tas_priv);
	} else
		tas_priv->cali_data.total_sz = 0;

	return 0;
}

static void tas2781_hda_remove_controls(struct tas2781_hda *tas_hda)
{
	struct hda_codec *codec = tas_hda->priv->codec;

	snd_ctl_remove(codec->card, tas_hda->dsp_prog_ctl);
	snd_ctl_remove(codec->card, tas_hda->dsp_conf_ctl);

	for (int i = ARRAY_SIZE(tas_hda->snd_ctls) - 1; i >= 0; i--)
		snd_ctl_remove(codec->card, tas_hda->snd_ctls[i]);

	snd_ctl_remove(codec->card, tas_hda->prof_ctl);
}

static void tasdev_fw_ready(const struct firmware *fmw, void *context)
{
	struct tasdevice_priv *tas_priv = context;
	struct tas2781_hda *tas_hda = dev_get_drvdata(tas_priv->dev);
	struct hda_codec *codec = tas_priv->codec;
	int i, ret, spk_id;

	pm_runtime_get_sync(tas_priv->dev);
	mutex_lock(&tas_priv->codec_lock);

	ret = tasdevice_rca_parser(tas_priv, fmw);
	if (ret)
		goto out;

	tas_hda->prof_ctl = snd_ctl_new1(&tas2781_prof_ctrl, tas_priv);
	ret = snd_ctl_add(codec->card, tas_hda->prof_ctl);
	if (ret) {
		dev_err(tas_priv->dev,
			"Failed to add KControl %s = %d\n",
			tas2781_prof_ctrl.name, ret);
		goto out;
	}

	for (i = 0; i < ARRAY_SIZE(tas2781_snd_controls); i++) {
		tas_hda->snd_ctls[i] = snd_ctl_new1(&tas2781_snd_controls[i],
			tas_priv);
		ret = snd_ctl_add(codec->card, tas_hda->snd_ctls[i]);
		if (ret) {
			dev_err(tas_priv->dev,
				"Failed to add KControl %s = %d\n",
				tas2781_snd_controls[i].name, ret);
			goto out;
		}
	}

	tasdevice_dsp_remove(tas_priv);

	tas_priv->fw_state = TASDEVICE_DSP_FW_PENDING;
	if (tas_priv->speaker_id != NULL) {
		// Speaker id need to be checked for ASUS only.
		spk_id = gpiod_get_value(tas_priv->speaker_id);
		if (spk_id < 0) {
			// Speaker id is not valid, use default.
			dev_dbg(tas_priv->dev, "Wrong spk_id = %d\n", spk_id);
			spk_id = 0;
		}
		snprintf(tas_priv->coef_binaryname,
			  sizeof(tas_priv->coef_binaryname),
			  "TAS2XXX%04X%d.bin",
			  lower_16_bits(codec->core.subsystem_id),
			  spk_id);
	} else {
		snprintf(tas_priv->coef_binaryname,
			  sizeof(tas_priv->coef_binaryname),
			  "TAS2XXX%04X.bin",
			  lower_16_bits(codec->core.subsystem_id));
	}
	ret = tasdevice_dsp_parser(tas_priv);
	if (ret) {
		dev_err(tas_priv->dev, "dspfw load %s error\n",
			tas_priv->coef_binaryname);
		tas_priv->fw_state = TASDEVICE_DSP_FW_FAIL;
		goto out;
	}

	tas_hda->dsp_prog_ctl = snd_ctl_new1(&tas2781_dsp_prog_ctrl,
		tas_priv);
	ret = snd_ctl_add(codec->card, tas_hda->dsp_prog_ctl);
	if (ret) {
		dev_err(tas_priv->dev,
			"Failed to add KControl %s = %d\n",
			tas2781_dsp_prog_ctrl.name, ret);
		goto out;
	}

	tas_hda->dsp_conf_ctl = snd_ctl_new1(&tas2781_dsp_conf_ctrl,
		tas_priv);
	ret = snd_ctl_add(codec->card, tas_hda->dsp_conf_ctl);
	if (ret) {
		dev_err(tas_priv->dev,
			"Failed to add KControl %s = %d\n",
			tas2781_dsp_conf_ctrl.name, ret);
		goto out;
	}

	tas_priv->fw_state = TASDEVICE_DSP_FW_ALL_OK;
	tasdevice_prmg_load(tas_priv, 0);
	if (tas_priv->fmw->nr_programs > 0)
		tas_priv->cur_prog = 0;
	if (tas_priv->fmw->nr_configurations > 0)
		tas_priv->cur_conf = 0;

	/* If calibrated data occurs error, dsp will still works with default
	 * calibrated data inside algo.
	 */
	tasdevice_save_calibration(tas_priv);

	tasdevice_tuning_switch(tas_hda->priv, 0);
	tas_hda->priv->playback_started = true;

out:
	mutex_unlock(&tas_hda->priv->codec_lock);
	release_firmware(fmw);
	pm_runtime_mark_last_busy(tas_hda->dev);
	pm_runtime_put_autosuspend(tas_hda->dev);
}

static int tas2781_hda_bind(struct device *dev, struct device *master,
	void *master_data)
{
	struct tas2781_hda *tas_hda = dev_get_drvdata(dev);
	struct hda_component_parent *parent = master_data;
	struct hda_component *comp;
	struct hda_codec *codec;
	unsigned int subid;
	int ret;

	comp = hda_component_from_index(parent, tas_hda->priv->index);
	if (!comp)
		return -EINVAL;

	if (comp->dev)
		return -EBUSY;

	codec = parent->codec;
	subid = codec->core.subsystem_id >> 16;

	switch (subid) {
	case 0x17aa:
		tas_hda->priv->catlog_id = LENOVO;
		break;
	default:
		tas_hda->priv->catlog_id = OTHERS;
		break;
	}

	pm_runtime_get_sync(dev);

	comp->dev = dev;

	strscpy(comp->name, dev_name(dev), sizeof(comp->name));

	ret = tascodec_init(tas_hda->priv, codec, THIS_MODULE, tasdev_fw_ready);
	if (!ret)
		comp->playback_hook = tas2781_hda_playback_hook;

	pm_runtime_mark_last_busy(dev);
	pm_runtime_put_autosuspend(dev);

	return ret;
}

static void tas2781_hda_unbind(struct device *dev,
	struct device *master, void *master_data)
{
	struct tas2781_hda *tas_hda = dev_get_drvdata(dev);
	struct hda_component_parent *parent = master_data;
	struct hda_component *comp;

	comp = hda_component_from_index(parent, tas_hda->priv->index);
	if (comp && (comp->dev == dev)) {
		comp->dev = NULL;
		memset(comp->name, 0, sizeof(comp->name));
		comp->playback_hook = NULL;
	}

	tas2781_hda_remove_controls(tas_hda);

	tasdevice_config_info_remove(tas_hda->priv);
	tasdevice_dsp_remove(tas_hda->priv);

	tas_hda->priv->fw_state = TASDEVICE_DSP_FW_PENDING;
}

static const struct component_ops tas2781_hda_comp_ops = {
	.bind = tas2781_hda_bind,
	.unbind = tas2781_hda_unbind,
};

static void tas2781_hda_remove(struct device *dev)
{
	struct tas2781_hda *tas_hda = dev_get_drvdata(dev);

	component_del(tas_hda->dev, &tas2781_hda_comp_ops);

	pm_runtime_get_sync(tas_hda->dev);
	pm_runtime_disable(tas_hda->dev);

	pm_runtime_put_noidle(tas_hda->dev);

	tasdevice_remove(tas_hda->priv);
}

static int tas2781_hda_i2c_probe(struct i2c_client *clt)
{
	struct tas2781_hda *tas_hda;
	const char *device_name;
	int ret;


	tas_hda = devm_kzalloc(&clt->dev, sizeof(*tas_hda), GFP_KERNEL);
	if (!tas_hda)
		return -ENOMEM;

	dev_set_drvdata(&clt->dev, tas_hda);
	tas_hda->dev = &clt->dev;

	tas_hda->priv = tasdevice_kzalloc(clt);
	if (!tas_hda->priv)
		return -ENOMEM;

	if (strstr(dev_name(&clt->dev), "TIAS2781")) {
		device_name = "TIAS2781";
		tas_hda->priv->save_calibration = tas2781_save_calibration;
		tas_hda->priv->apply_calibration = tas2781_apply_calib;
		tas_hda->priv->global_addr = TAS2781_GLOBAL_ADDR;
	} else if (strstr(dev_name(&clt->dev), "INT8866")) {
		device_name = "INT8866";
		tas_hda->priv->save_calibration = tas2563_save_calibration;
		tas_hda->priv->apply_calibration = tas2563_apply_calib;
		tas_hda->priv->global_addr = TAS2563_GLOBAL_ADDR;
	} else
		return -ENODEV;

	tas_hda->priv->irq = clt->irq;
	ret = tas2781_read_acpi(tas_hda->priv, device_name);
	if (ret)
		return dev_err_probe(tas_hda->dev, ret,
			"Platform not supported\n");

	ret = tasdevice_init(tas_hda->priv);
	if (ret)
		goto err;

	pm_runtime_set_autosuspend_delay(tas_hda->dev, 3000);
	pm_runtime_use_autosuspend(tas_hda->dev);
	pm_runtime_mark_last_busy(tas_hda->dev);
	pm_runtime_set_active(tas_hda->dev);
	pm_runtime_enable(tas_hda->dev);

	tasdevice_reset(tas_hda->priv);

	ret = component_add(tas_hda->dev, &tas2781_hda_comp_ops);
	if (ret) {
		dev_err(tas_hda->dev, "Register component failed: %d\n", ret);
		pm_runtime_disable(tas_hda->dev);
	}

err:
	if (ret)
		tas2781_hda_remove(&clt->dev);
	return ret;
}

static void tas2781_hda_i2c_remove(struct i2c_client *clt)
{
	tas2781_hda_remove(&clt->dev);
}

static int tas2781_runtime_suspend(struct device *dev)
{
	struct tas2781_hda *tas_hda = dev_get_drvdata(dev);

	dev_dbg(tas_hda->dev, "Runtime Suspend\n");

	mutex_lock(&tas_hda->priv->codec_lock);

	/* The driver powers up the amplifiers at module load time.
	 * Stop the playback if it's unused.
	 */
	if (tas_hda->priv->playback_started) {
		tasdevice_tuning_switch(tas_hda->priv, 1);
		tas_hda->priv->playback_started = false;
	}

	mutex_unlock(&tas_hda->priv->codec_lock);

	return 0;
}

static int tas2781_runtime_resume(struct device *dev)
{
	struct tas2781_hda *tas_hda = dev_get_drvdata(dev);

	dev_dbg(tas_hda->dev, "Runtime Resume\n");

	mutex_lock(&tas_hda->priv->codec_lock);

	tasdevice_prmg_load(tas_hda->priv, tas_hda->priv->cur_prog);

	/* If calibrated data occurs error, dsp will still works with default
	 * calibrated data inside algo.
	 */
	tasdevice_apply_calibration(tas_hda->priv);

	mutex_unlock(&tas_hda->priv->codec_lock);

	return 0;
}

static int tas2781_system_suspend(struct device *dev)
{
	struct tas2781_hda *tas_hda = dev_get_drvdata(dev);

	dev_dbg(tas_hda->priv->dev, "System Suspend\n");

	mutex_lock(&tas_hda->priv->codec_lock);

	/* Shutdown chip before system suspend */
	if (tas_hda->priv->playback_started)
		tasdevice_tuning_switch(tas_hda->priv, 1);

	mutex_unlock(&tas_hda->priv->codec_lock);

	/*
	 * Reset GPIO may be shared, so cannot reset here.
	 * However beyond this point, amps may be powered down.
	 */
	return 0;
}

static int tas2781_system_resume(struct device *dev)
{
	struct tas2781_hda *tas_hda = dev_get_drvdata(dev);
	int i;

	dev_dbg(tas_hda->priv->dev, "System Resume\n");

	mutex_lock(&tas_hda->priv->codec_lock);

	for (i = 0; i < tas_hda->priv->ndev; i++) {
		tas_hda->priv->tasdevice[i].cur_book = -1;
		tas_hda->priv->tasdevice[i].cur_prog = -1;
		tas_hda->priv->tasdevice[i].cur_conf = -1;
	}
	tasdevice_reset(tas_hda->priv);
	tasdevice_prmg_load(tas_hda->priv, tas_hda->priv->cur_prog);

	/* If calibrated data occurs error, dsp will still work with default
	 * calibrated data inside algo.
	 */
	tasdevice_apply_calibration(tas_hda->priv);

	if (tas_hda->priv->playback_started)
		tasdevice_tuning_switch(tas_hda->priv, 0);

	mutex_unlock(&tas_hda->priv->codec_lock);

	return 0;
}

static const struct dev_pm_ops tas2781_hda_pm_ops = {
	RUNTIME_PM_OPS(tas2781_runtime_suspend, tas2781_runtime_resume, NULL)
	SYSTEM_SLEEP_PM_OPS(tas2781_system_suspend, tas2781_system_resume)
};

static const struct i2c_device_id tas2781_hda_i2c_id[] = {
	{ "tas2781-hda" },
	{}
};

static const struct acpi_device_id tas2781_acpi_hda_match[] = {
	{"TIAS2781", 0 },
	{"INT8866", 0 },
	{}
};
MODULE_DEVICE_TABLE(acpi, tas2781_acpi_hda_match);

static struct i2c_driver tas2781_hda_i2c_driver = {
	.driver = {
		.name		= "tas2781-hda",
		.acpi_match_table = tas2781_acpi_hda_match,
		.pm		= &tas2781_hda_pm_ops,
	},
	.id_table	= tas2781_hda_i2c_id,
	.probe		= tas2781_hda_i2c_probe,
	.remove		= tas2781_hda_i2c_remove,
};
module_i2c_driver(tas2781_hda_i2c_driver);

MODULE_DESCRIPTION("TAS2781 HDA Driver");
MODULE_AUTHOR("Shenghao Ding, TI, <shenghao-ding@ti.com>");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("SND_SOC_TAS2781_FMWLIB");
