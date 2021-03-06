/* Copyright (c) 2010, Code Aurora Forum. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/spinlock.h>
#include <linux/platform_device.h>
#include <linux/regulator/driver.h>
#include <linux/mfd/pmic8901.h>

#include "rpm-regulator.h"
#include "rpm.h"

#define MICRO_TO_MILLI(uV)			((uV) / 1000)
#define MILLI_TO_MICRO(mV)			((mV) * 1000)

/* LDO register word 1 */
#define LDO_VOLTAGE				0x00000FFF
#define LDO_VOLTAGE_SHIFT			0
#define LDO_PEAK_CURRENT			0x00FFF000
#define LDO_PEAK_CURRENT_SHIFT			12
#define LDO_MODE				0x03000000
#define LDO_MODE_SHIFT				24
#define LDO_PIN_CTRL				0x3C000000
#define LDO_PIN_CTRL_SHIFT			26
#define LDO_PIN_FN				0xC0000000
#define LDO_PIN_FN_SHIFT			30

/* LDO register word 2 */
#define LDO_PULL_DOWN_ENABLE			0x00000001
#define LDO_PULL_DOWN_ENABLE_SHIFT		0
#define LDO_AVG_CURRENT				0x00001FFE
#define LDO_AVG_CURRENT_SHIFT			1

/* SMPS register word 1 */
#define SMPS_VOLTAGE				0x00000FFF
#define SMPS_VOLTAGE_SHIFT			0
#define SMPS_PEAK_CURRENT			0x00FFF000
#define SMPS_PEAK_CURRENT_SHIFT			12
#define SMPS_MODE				0x03000000
#define SMPS_MODE_SHIFT				24
#define SMPS_PIN_CTRL				0x3C000000
#define SMPS_PIN_CTRL_SHIFT			26
#define SMPS_PIN_FN				0xC0000000
#define SMPS_PIN_FN_SHIFT			30

/* SMPS register word 2 */
#define SMPS_PULL_DOWN_ENABLE			0x00000001
#define SMPS_PULL_DOWN_ENABLE_SHIFT		0
#define SMPS_AVG_CURRENT			0x00001FFE
#define SMPS_AVG_CURRENT_SHIFT			1
#define SMPS_FREQ				0x001FE000
#define SMPS_FREQ_SHIFT				13
#define SMPS_CLK_SRC				0x00600000
#define SMPS_CLK_SRC_SHIFT			21

/* SWITCH register word 1 */
#define SWITCH_STATE				0x0001
#define SWITCH_STATE_SHIFT			0
#define SWITCH_PULL_DOWN_ENABLE			0x0002
#define SWITCH_PULL_DOWN_ENABLE_SHIFT		1
#define SWITCH_PIN_CTRL				0x003C
#define SWITCH_PIN_CTRL_SHIFT			2
#define SWITCH_PIN_FN				0x00C0
#define SWITCH_PIN_FN_SHIFT			6

/* NCP register word 1 */
#define NCP_VOLTAGE				0x0FFF
#define NCP_VOLTAGE_SHIFT			0
#define NCP_STATE				0x1000
#define NCP_STATE_SHIFT				12

/* Max low power mode loads in uA */
#define LDO_50_LPM_MAX_LOAD			50000
#define LDO_150_LPM_MAX_LOAD			50000
#define LDO_300_LPM_MAX_LOAD			50000
#define SMPS_LPM_MAX_LOAD			50000
#define FTSMPS_LPM_MAX_LOAD			50000

/*
 * This is used when voting for LPM or HPM by subtracting or adding to the
 * lpm_max_load of a regulator.  It has units of uA.
 */
#define LOAD_THRESHOLD_STEP			1000

/* This is the maximum uA load that can be passed to the RPM. */
#define MAX_POSSIBLE_LOAD			(MILLI_TO_MICRO(0xFFF))

/* Voltage regulator types */
#define IS_LDO(id)	((id >= RPM_VREG_ID_PM8058_L0 && \
			  id <= RPM_VREG_ID_PM8058_L25) || \
			 (id >= RPM_VREG_ID_PM8901_L0 && \
			  id <= RPM_VREG_ID_PM8901_L6))
#define IS_SMPS(id)	((id >= RPM_VREG_ID_PM8058_S0 && \
			  id <= RPM_VREG_ID_PM8058_S4) || \
			 (id >= RPM_VREG_ID_PM8901_S0 && \
			  id <= RPM_VREG_ID_PM8901_S4))
#define IS_SWITCH(id)	((id >= RPM_VREG_ID_PM8058_LVS0 && \
			  id <= RPM_VREG_ID_PM8058_LVS1) || \
			 (id >= RPM_VREG_ID_PM8901_LVS0 && \
			  id <= RPM_VREG_ID_PM8901_LVS3) || \
			 (id == RPM_VREG_ID_PM8901_MVS0))
#define IS_NCP(id)	(id == RPM_VREG_ID_PM8058_NCP)

struct vreg {
	struct msm_rpm_iv_pair	req[2];
	struct msm_rpm_iv_pair	prev_req[2];
	struct rpm_vreg_pdata	*pdata;
	int			save_uV;
	const int		lpm_max_load;
	unsigned		pc_vote;
	unsigned		optimum;
	unsigned		mode_initialized;
	enum rpm_vreg_id	id;
};

#define VREG_2(_vreg_id, _rpm_id, _lpm_max_load) \
	[RPM_VREG_ID_##_vreg_id] = { \
		.req = { \
			[0] = { .id = MSM_RPM_ID_##_rpm_id##_0, }, \
			[1] = { .id = MSM_RPM_ID_##_rpm_id##_1, }, \
		}, \
		.lpm_max_load = _lpm_max_load, \
	}

#define VREG_1(_vreg_id, _rpm_id) \
	[RPM_VREG_ID_##_vreg_id] = { \
		.req = { \
			[0] = { .id = MSM_RPM_ID_##_rpm_id, }, \
			[1] = { .id = -1, }, \
		}, \
	}

static struct vreg vregs[RPM_VREG_ID_MAX] = {
	VREG_2(PM8058_L0, LDO0, LDO_150_LPM_MAX_LOAD),
	VREG_2(PM8058_L1, LDO1, LDO_300_LPM_MAX_LOAD),
	VREG_2(PM8058_L2, LDO2, LDO_300_LPM_MAX_LOAD),
	VREG_2(PM8058_L3, LDO3, LDO_150_LPM_MAX_LOAD),
	VREG_2(PM8058_L4, LDO4, LDO_50_LPM_MAX_LOAD),
	VREG_2(PM8058_L5, LDO5, LDO_300_LPM_MAX_LOAD),
	VREG_2(PM8058_L6, LDO6, LDO_50_LPM_MAX_LOAD),
	VREG_2(PM8058_L7, LDO7, LDO_50_LPM_MAX_LOAD),
	VREG_2(PM8058_L8, LDO8, LDO_300_LPM_MAX_LOAD),
	VREG_2(PM8058_L9, LDO9, LDO_300_LPM_MAX_LOAD),
	VREG_2(PM8058_L10, LDO10, LDO_300_LPM_MAX_LOAD),
	VREG_2(PM8058_L11, LDO11, LDO_150_LPM_MAX_LOAD),
	VREG_2(PM8058_L12, LDO12, LDO_150_LPM_MAX_LOAD),
	VREG_2(PM8058_L13, LDO13, LDO_300_LPM_MAX_LOAD),
	VREG_2(PM8058_L14, LDO14, LDO_300_LPM_MAX_LOAD),
	VREG_2(PM8058_L15, LDO15, LDO_300_LPM_MAX_LOAD),
	VREG_2(PM8058_L16, LDO16, LDO_300_LPM_MAX_LOAD),
	VREG_2(PM8058_L17, LDO17, LDO_150_LPM_MAX_LOAD),
	VREG_2(PM8058_L18, LDO18, LDO_150_LPM_MAX_LOAD),
	VREG_2(PM8058_L19, LDO19, LDO_150_LPM_MAX_LOAD),
	VREG_2(PM8058_L20, LDO20, LDO_150_LPM_MAX_LOAD),
	VREG_2(PM8058_L21, LDO21, LDO_150_LPM_MAX_LOAD),
	VREG_2(PM8058_L22, LDO22, LDO_300_LPM_MAX_LOAD),
	VREG_2(PM8058_L23, LDO23, LDO_300_LPM_MAX_LOAD),
	VREG_2(PM8058_L24, LDO24, LDO_150_LPM_MAX_LOAD),
	VREG_2(PM8058_L25, LDO25, LDO_150_LPM_MAX_LOAD),

	VREG_2(PM8058_S0, SMPS0, SMPS_LPM_MAX_LOAD),
	VREG_2(PM8058_S1, SMPS1, SMPS_LPM_MAX_LOAD),
	VREG_2(PM8058_S2, SMPS2, SMPS_LPM_MAX_LOAD),
	VREG_2(PM8058_S3, SMPS3, SMPS_LPM_MAX_LOAD),
	VREG_2(PM8058_S4, SMPS4, SMPS_LPM_MAX_LOAD),

	VREG_1(PM8058_LVS0, LVS0),
	VREG_1(PM8058_LVS1, LVS1),

	VREG_2(PM8058_NCP, NCP, 0),

	VREG_2(PM8901_L0, LDO0B, LDO_300_LPM_MAX_LOAD),
	VREG_2(PM8901_L1, LDO1B, LDO_300_LPM_MAX_LOAD),
	VREG_2(PM8901_L2, LDO2B, LDO_300_LPM_MAX_LOAD),
	VREG_2(PM8901_L3, LDO3B, LDO_300_LPM_MAX_LOAD),
	VREG_2(PM8901_L4, LDO4B, LDO_300_LPM_MAX_LOAD),
	VREG_2(PM8901_L5, LDO5B, LDO_300_LPM_MAX_LOAD),
	VREG_2(PM8901_L6, LDO6B, LDO_300_LPM_MAX_LOAD),

	VREG_2(PM8901_S0, SMPS0B, FTSMPS_LPM_MAX_LOAD),
	VREG_2(PM8901_S1, SMPS1B, FTSMPS_LPM_MAX_LOAD),
	VREG_2(PM8901_S2, SMPS2B, FTSMPS_LPM_MAX_LOAD),
	VREG_2(PM8901_S3, SMPS3B, FTSMPS_LPM_MAX_LOAD),
	VREG_2(PM8901_S4, SMPS4B, FTSMPS_LPM_MAX_LOAD),

	VREG_1(PM8901_LVS0, LVS0B),
	VREG_1(PM8901_LVS1, LVS1B),
	VREG_1(PM8901_LVS2, LVS2B),
	VREG_1(PM8901_LVS3, LVS3B),

	VREG_1(PM8901_MVS0, MVS),
};

#define REG_IS_PM8058_S1(rpm_id) ((rpm_id) == MSM_RPM_ID_SMPS1_0 || \
				  (rpm_id) == MSM_RPM_ID_SMPS1_1)

static int vreg_set_pm8058_s1(enum pm8058_s1_vote_client voter, unsigned mask0,
		       unsigned val0, unsigned mask1, unsigned val1,
		       unsigned cnt)
{
	struct vreg *vreg = &vregs[RPM_VREG_ID_PM8058_S1];
	static DEFINE_SPINLOCK(pm8058_s1_lock);
	static int min_mV_votes[PM8058_S1_VOTE_NUM_VOTERS];
	unsigned prev0 = 0, prev1 = 0;
	int rc = 0, i, max_mV_vote;
	unsigned long flags;

	if (voter < 0 || voter >= PM8058_S1_VOTE_NUM_VOTERS)
		return -EINVAL;

	spin_lock_irqsave(&pm8058_s1_lock, flags);

	prev0 = vreg->req[0].value;
	vreg->req[0].value &= ~mask0;
	vreg->req[0].value |= val0 & mask0;

	prev1 = vreg->req[1].value;
	vreg->req[1].value &= ~mask1;
	vreg->req[1].value |= val1 & mask1;

	min_mV_votes[voter] = (vreg->req[0].value & SMPS_VOLTAGE) >>
			      SMPS_VOLTAGE_SHIFT;

	/* Find the highest voltage voted for and use it. */
	max_mV_vote = 0;
	for (i = 0; i < PM8058_S1_VOTE_NUM_VOTERS; i++)
		max_mV_vote = max(max_mV_vote, min_mV_votes[i]);
	vreg->req[0].value &= ~SMPS_VOLTAGE;
	vreg->req[0].value |= (max_mV_vote << SMPS_VOLTAGE_SHIFT) &
			      SMPS_VOLTAGE;

	/* Ignore duplicate requests */
	if (vreg->req[0].value == vreg->prev_req[0].value &&
	    vreg->req[1].value == vreg->prev_req[1].value)
		goto done;

	rc = msm_rpm_set_noirq(MSM_RPM_CTX_SET_0, vreg->req, cnt);
	if (rc) {
		vreg->req[0].value = prev0;
		vreg->req[1].value = prev1;

		pr_err("%s: msm_rpm_set_noirq fail id=%d, rc=%d\n",
				__func__, vreg->req[0].id, rc);
	} else {
		vreg->prev_req[0].value = vreg->req[0].value;
		vreg->prev_req[1].value = vreg->req[1].value;
	}

done:
	spin_unlock_irqrestore(&pm8058_s1_lock, flags);

	return rc;
}

/**
 * pm8058_s1_set_min_uv_noirq - vote for a min_uV value of regualtor pm8058_s1
 * @voter: ID for the voter
 * @min_uV: minimum acceptable voltage (in uV) that is voted for
 *
 * Returns 0 on success or less than 0 on error.
 *
 * This function is used to vote for the voltage of regulator pm8058_s1 without
 * using the regulator framework.  It is needed by consumers which hold spin
 * locks or have interrupts disabled because the regulator framework can sleep.
 */
int pm8058_s1_set_min_uv_noirq(enum pm8058_s1_vote_client voter, int min_uV)
{
	int rc;

	rc = vreg_set_pm8058_s1(voter, SMPS_VOLTAGE,
			MICRO_TO_MILLI(min_uV) << SMPS_VOLTAGE_SHIFT,
			0, 0, 2);
	if (!rc)
		return rc;

	/* only save if nonzero (or not disabling) */
	if (min_uV)
		vregs[RPM_VREG_ID_PM8058_S1].save_uV = min_uV;

	return rc;
}
EXPORT_SYMBOL_GPL(pm8058_s1_set_min_uv_noirq);

#define IS_PMIC_8901_V1(rev)		((rev) == PM_8901_REV_1p0 || \
					 (rev) == PM_8901_REV_1p1)

#define PMIC_8901_V1_SCALE(uV)		((((uV) - 62100) * 23) / 25)

static inline int vreg_hpm_min_uA(struct vreg *vreg)
{
	return vreg->lpm_max_load + LOAD_THRESHOLD_STEP;
}

static inline int vreg_lpm_max_uA(struct vreg *vreg)
{
	return vreg->lpm_max_load - LOAD_THRESHOLD_STEP;
}

static inline unsigned saturate_load(unsigned load_uA)
{
	return (load_uA > MAX_POSSIBLE_LOAD ? MAX_POSSIBLE_LOAD : load_uA);
}

static int vreg_set(struct vreg *vreg, unsigned mask0, unsigned val0,
		unsigned mask1, unsigned val1, unsigned cnt)
{
	unsigned prev0 = 0, prev1 = 0;
	int rc;

	/* bypass normal route for PM8058_S1 */
	if (REG_IS_PM8058_S1(vreg->req[0].id))
		return vreg_set_pm8058_s1(PM8058_S1_VOTE_REG_FRAMEWORK, mask0,
					  val0, mask1, val1, cnt);

	prev0 = vreg->req[0].value;
	vreg->req[0].value &= ~mask0;
	vreg->req[0].value |= val0 & mask0;

	prev1 = vreg->req[1].value;
	vreg->req[1].value &= ~mask1;
	vreg->req[1].value |= val1 & mask1;

	/* Ignore duplicate requests */
	if (vreg->req[0].value == vreg->prev_req[0].value &&
	    vreg->req[1].value == vreg->prev_req[1].value)
		return 0;

	rc = msm_rpm_set(MSM_RPM_CTX_SET_0, vreg->req, cnt);
	if (rc) {
		vreg->req[0].value = prev0;
		vreg->req[1].value = prev1;

		pr_err("%s: msm_rpm_set fail id=%d, rc=%d\n",
				__func__, vreg->req[0].id, rc);
	} else {
		vreg->prev_req[0].value = vreg->req[0].value;
		vreg->prev_req[1].value = vreg->req[1].value;
	}

	return rc;
}

static int smps_set_voltage(struct regulator_dev *dev, int min_uV, int max_uV)
{
	struct vreg *vreg = rdev_get_drvdata(dev);
	int rc;

	rc = vreg_set(vreg, SMPS_VOLTAGE,
			MICRO_TO_MILLI(min_uV) << SMPS_VOLTAGE_SHIFT,
			0, 0, 2);
	if (!rc)
		return rc;

	/* only save if nonzero (or not disabling) */
	if (min_uV)
		vreg->save_uV = min_uV;

	return rc;
}

/*
 * This secondary set_voltage callback is needed to handle v1 PMIC 8901 SMPS
 * voltage compensation on the Linux side.  The RPM does not correct for the
 * output voltage error present in v1 PMIC 8901 chips.
 */
static int smps_8901_set_voltage(struct regulator_dev *dev, int min_uV,
				 int max_uV)
{
	struct vreg *vreg = rdev_get_drvdata(dev);
	int scaled_min_uV = min_uV;
	int rc;
	static int pmic8901_rev;

	/* Scale input request voltage down if using v1 PMIC 8901. */
	if (min_uV) {
		if (pmic8901_rev <= 0)
			pmic8901_rev = pm8901_rev(NULL);

		if (pmic8901_rev < 0)
			pr_err("%s: setting %s to %d uV; PMIC 8901 revision "
				"unavailable, no scaling can be performed.\n",
				__func__, dev->desc->name, min_uV);
		else if (IS_PMIC_8901_V1(pmic8901_rev))
			scaled_min_uV = PMIC_8901_V1_SCALE(min_uV);
	}

	rc = vreg_set(vreg, SMPS_VOLTAGE, MICRO_TO_MILLI(scaled_min_uV) <<
			SMPS_VOLTAGE_SHIFT, 0, 0, 2);
	if (!rc)
		return rc;

	/* only save if nonzero (or not disabling) */
	if (min_uV)
		vreg->save_uV = min_uV;

	return rc;
}

static int smps_get_voltage(struct regulator_dev *dev)
{
	struct vreg *vreg = rdev_get_drvdata(dev);
	return vreg->save_uV;
}

static int smps_enable(struct regulator_dev *dev)
{
	int rc = 0;
	struct vreg *vreg = rdev_get_drvdata(dev);

	/* enable by setting voltage */
	if (MICRO_TO_MILLI(vreg->save_uV) > 0)
		rc = smps_set_voltage(dev, vreg->save_uV, vreg->save_uV);
	return rc;
}

static int smps_disable(struct regulator_dev *dev)
{
	/* disable by setting voltage to zero */
	return smps_set_voltage(dev, 0, 0);
}

static int smps_is_enabled(struct regulator_dev *dev)
{
	struct vreg *vreg = rdev_get_drvdata(dev);
	return ((vreg->req[0].value & SMPS_VOLTAGE) >> SMPS_VOLTAGE_SHIFT) != 0;
}

/*
 * Optimum mode programming:
 * REGULATOR_MODE_FAST: Go to HPM (highest priority)
 * REGULATOR_MODE_STANDBY: Go to pin ctrl mode if there are any pin ctrl
 * votes, else go to LPM
 *
 * Pin ctrl mode voting via regulator set_mode:
 * REGULATOR_MODE_IDLE: Go to pin ctrl mode if the optimum mode is LPM, else
 * go to HPM
 * REGULATOR_MODE_NORMAL: Go to LPM if it is the optimum mode, else go to HPM
 *
 * Pin ctrl mode takes priority on the RPM when force mode is not set;
 * therefore, pin ctrl bits must be cleared if LPM or HPM is being voted for.
 */
static int smps_set_mode(struct regulator_dev *dev, unsigned int mode)
{
	struct vreg *vreg = rdev_get_drvdata(dev);
	unsigned optimum = vreg->optimum;
	unsigned pc_vote = vreg->pc_vote;
	unsigned mode_initialized = vreg->mode_initialized;
	unsigned mask0 = 0, val0 = 0, mask1 = 0, val1 = 0;
	int peak_uA;
	int rc = 0;

	peak_uA = MILLI_TO_MICRO((vreg->req[0].value & SMPS_PEAK_CURRENT) >>
		  SMPS_PEAK_CURRENT_SHIFT);

	switch (mode) {
	case REGULATOR_MODE_FAST:
		if (peak_uA < vreg_hpm_min_uA(vreg)) {
			mask0 = SMPS_PEAK_CURRENT;
			mask1 = SMPS_AVG_CURRENT;
			val0 = (MICRO_TO_MILLI(vreg_hpm_min_uA(vreg)) <<
				SMPS_PEAK_CURRENT_SHIFT) & SMPS_PEAK_CURRENT;
			val1 = (MICRO_TO_MILLI(vreg_hpm_min_uA(vreg)) <<
				SMPS_AVG_CURRENT_SHIFT) & SMPS_AVG_CURRENT;
		}
		/* clear pin control */
		mask0 |= SMPS_PIN_CTRL;
		optimum = mode;
		mode_initialized = 1;
		break;

	case REGULATOR_MODE_STANDBY:
		if (peak_uA > vreg_lpm_max_uA(vreg)) {
			mask0 = SMPS_PEAK_CURRENT;
			mask1 = SMPS_AVG_CURRENT;
			val0 = (MICRO_TO_MILLI(vreg_lpm_max_uA(vreg)) <<
			   SMPS_PEAK_CURRENT_SHIFT) & SMPS_PEAK_CURRENT;
			val1 = (MICRO_TO_MILLI(vreg_lpm_max_uA(vreg)) <<
			     SMPS_AVG_CURRENT_SHIFT) & SMPS_AVG_CURRENT;
		}
		if (pc_vote) {
			mask0 |= SMPS_PIN_CTRL;
			val0 |= vreg->pdata->pin_ctrl << SMPS_PIN_CTRL_SHIFT;
		} else {
			/* clear pin control */
			mask0 |= SMPS_PIN_CTRL;
		}
		optimum = mode;
		mode_initialized = 1;
		break;

	case REGULATOR_MODE_IDLE:
		if (pc_vote++)
			goto done; /* already taken care of */

		if (mode_initialized && optimum == REGULATOR_MODE_FAST) {
			if (peak_uA < vreg_hpm_min_uA(vreg)) {
				mask0 = SMPS_PEAK_CURRENT;
				mask1 = SMPS_AVG_CURRENT;
				val0 = (MICRO_TO_MILLI(vreg_hpm_min_uA(vreg)) <<
				   SMPS_PEAK_CURRENT_SHIFT) & SMPS_PEAK_CURRENT;
				val1 = (MICRO_TO_MILLI(vreg_hpm_min_uA(vreg)) <<
				     SMPS_AVG_CURRENT_SHIFT) & SMPS_AVG_CURRENT;
			}
			/* clear pin control */
			mask0 |= SMPS_PIN_CTRL;
		} else {
			mask0 = SMPS_PIN_CTRL;
			val0 = vreg->pdata->pin_ctrl << SMPS_PIN_CTRL_SHIFT;
		}
		break;

	case REGULATOR_MODE_NORMAL:
		if (pc_vote && --pc_vote)
			goto done; /* already taken care of */

		if (optimum == REGULATOR_MODE_STANDBY) {
			if (peak_uA > vreg_lpm_max_uA(vreg)) {
				mask0 = SMPS_PEAK_CURRENT;
				mask1 = SMPS_AVG_CURRENT;
				val0 = (MICRO_TO_MILLI(vreg_lpm_max_uA(vreg)) <<
				   SMPS_PEAK_CURRENT_SHIFT) & SMPS_PEAK_CURRENT;
				val1 = (MICRO_TO_MILLI(vreg_lpm_max_uA(vreg)) <<
				     SMPS_AVG_CURRENT_SHIFT) & SMPS_AVG_CURRENT;
			}
		} else {
			if (peak_uA < vreg_hpm_min_uA(vreg)) {
				mask0 = SMPS_PEAK_CURRENT;
				mask1 = SMPS_AVG_CURRENT;
				val0 = (MICRO_TO_MILLI(vreg_hpm_min_uA(vreg)) <<
				   SMPS_PEAK_CURRENT_SHIFT) & SMPS_PEAK_CURRENT;
				val1 = (MICRO_TO_MILLI(vreg_hpm_min_uA(vreg)) <<
				     SMPS_AVG_CURRENT_SHIFT) & SMPS_AVG_CURRENT;
			}
		}
		/* clear pin control */
		mask0 |= SMPS_PIN_CTRL;
		break;

	default:
		return -EINVAL;
	}

	rc = vreg_set(rdev_get_drvdata(dev), mask0, val0, mask1, val1, 2);
	if (rc)
		return rc;

done:
	vreg->mode_initialized = mode_initialized;
	vreg->optimum = optimum;
	vreg->pc_vote = pc_vote;

	return 0;
}

static unsigned int smps_get_mode(struct regulator_dev *dev)
{
	struct vreg *vreg = rdev_get_drvdata(dev);

	if (vreg->optimum == REGULATOR_MODE_FAST)
		return REGULATOR_MODE_FAST;
	else if (vreg->pc_vote)
		return REGULATOR_MODE_IDLE;
	else if (vreg->optimum == REGULATOR_MODE_STANDBY)
		return REGULATOR_MODE_STANDBY;
	return REGULATOR_MODE_FAST;
}

unsigned int smps_get_optimum_mode(struct regulator_dev *dev, int input_uV,
		int output_uV, int load_uA)
{
	struct vreg *vreg = rdev_get_drvdata(dev);

	if (MICRO_TO_MILLI(load_uA) > 0) {
		vreg->req[0].value &= ~SMPS_PEAK_CURRENT;
		vreg->req[0].value |= (MICRO_TO_MILLI(saturate_load(load_uA)) <<
				   SMPS_PEAK_CURRENT_SHIFT) & SMPS_PEAK_CURRENT;
		vreg->req[1].value &= ~SMPS_AVG_CURRENT;
		vreg->req[1].value |= (MICRO_TO_MILLI(saturate_load(load_uA)) <<
				     SMPS_AVG_CURRENT_SHIFT) & SMPS_AVG_CURRENT;
	} else {
		/*
		 * smps_get_optimum_mode is being called before consumers have
		 * specified their load currents via regulator_set_optimum_mode.
		 * Return whatever the existing mode is.
		 */
		return smps_get_mode(dev);
	}

	if (load_uA > vreg->lpm_max_load)
		return REGULATOR_MODE_FAST;
	return REGULATOR_MODE_STANDBY;
}

static int ldo_set_voltage(struct regulator_dev *dev, int min_uV, int max_uV)
{
	struct vreg *vreg = rdev_get_drvdata(dev);
	int rc;

	rc = vreg_set(vreg, LDO_VOLTAGE,
			MICRO_TO_MILLI(min_uV) << LDO_VOLTAGE_SHIFT,
			0, 0, 2);
	if (!rc)
		return rc;

	/* only save if nonzero (or not disabling) */
	if (min_uV)
		vreg->save_uV = min_uV;

	return rc;
}

static int ldo_get_voltage(struct regulator_dev *dev)
{
	struct vreg *vreg = rdev_get_drvdata(dev);
	return vreg->save_uV;
}

static int ldo_enable(struct regulator_dev *dev)
{
	int rc = 0;
	struct vreg *vreg = rdev_get_drvdata(dev);

	/* enable by setting voltage */
	if (MICRO_TO_MILLI(vreg->save_uV) > 0)
		rc = ldo_set_voltage(dev, vreg->save_uV, vreg->save_uV);
	return rc;
}

static int ldo_disable(struct regulator_dev *dev)
{
	/* disable by setting voltage to zero */
	return ldo_set_voltage(dev, 0, 0);
}

static int ldo_is_enabled(struct regulator_dev *dev)
{
	struct vreg *vreg = rdev_get_drvdata(dev);
	return ((vreg->req[0].value & LDO_VOLTAGE) >> LDO_VOLTAGE_SHIFT) != 0;
}

/*
 * Optimum mode programming:
 * REGULATOR_MODE_FAST: Go to HPM (highest priority)
 * REGULATOR_MODE_STANDBY: Go to pin ctrl mode if there are any pin ctrl
 * votes, else go to LPM
 *
 * Pin ctrl mode voting via regulator set_mode:
 * REGULATOR_MODE_IDLE: Go to pin ctrl mode if the optimum mode is LPM, else
 * go to HPM
 * REGULATOR_MODE_NORMAL: Go to LPM if it is the optimum mode, else go to HPM
 *
 * Pin ctrl mode takes priority on the RPM when force mode is not set;
 * therefore, pin ctrl bits must be cleared if LPM or HPM is being voted for.
 */
static int ldo_set_mode(struct regulator_dev *dev, unsigned int mode)
{
	struct vreg *vreg = rdev_get_drvdata(dev);
	unsigned optimum = vreg->optimum;
	unsigned pc_vote = vreg->pc_vote;
	unsigned mode_initialized = vreg->mode_initialized;
	unsigned mask0 = 0, val0 = 0, mask1 = 0, val1 = 0;
	int peak_uA;
	int rc = 0;

	peak_uA = MILLI_TO_MICRO((vreg->req[0].value & LDO_PEAK_CURRENT) >>
		  LDO_PEAK_CURRENT_SHIFT);

	switch (mode) {
	case REGULATOR_MODE_FAST:
		if (peak_uA < vreg_hpm_min_uA(vreg)) {
			mask0 = LDO_PEAK_CURRENT;
			mask1 = LDO_AVG_CURRENT;
			val0 = (MICRO_TO_MILLI(vreg_hpm_min_uA(vreg)) <<
				LDO_PEAK_CURRENT_SHIFT) & LDO_PEAK_CURRENT;
			val1 = (MICRO_TO_MILLI(vreg_hpm_min_uA(vreg)) <<
				LDO_AVG_CURRENT_SHIFT) & LDO_AVG_CURRENT;
		}
		/* clear pin control */
		mask0 |= LDO_PIN_CTRL;
		optimum = mode;
		mode_initialized = 1;
		break;

	case REGULATOR_MODE_STANDBY:
		if (peak_uA > vreg_lpm_max_uA(vreg)) {
			mask0 = LDO_PEAK_CURRENT;
			mask1 = LDO_AVG_CURRENT;
			val0 = (MICRO_TO_MILLI(vreg_lpm_max_uA(vreg)) <<
				LDO_PEAK_CURRENT_SHIFT) & LDO_PEAK_CURRENT;
			val1 = (MICRO_TO_MILLI(vreg_lpm_max_uA(vreg)) <<
				LDO_AVG_CURRENT_SHIFT) & LDO_AVG_CURRENT;
		}
		if (pc_vote) {
			mask0 |= LDO_PIN_CTRL;
			val0 |= vreg->pdata->pin_ctrl << LDO_PIN_CTRL_SHIFT;
		} else {
			/* clear pin control */
			mask0 |= LDO_PIN_CTRL;
		}
		optimum = mode;
		mode_initialized = 1;
		break;

	case REGULATOR_MODE_IDLE:
		if (pc_vote++)
			goto done; /* already taken care of */

		if (mode_initialized && optimum == REGULATOR_MODE_FAST) {
			if (peak_uA < vreg_hpm_min_uA(vreg)) {
				mask0 = LDO_PEAK_CURRENT;
				mask1 = LDO_AVG_CURRENT;
				val0 = (MICRO_TO_MILLI(vreg_hpm_min_uA(vreg)) <<
				   LDO_PEAK_CURRENT_SHIFT) & LDO_PEAK_CURRENT;
				val1 = (MICRO_TO_MILLI(vreg_hpm_min_uA(vreg)) <<
				     LDO_AVG_CURRENT_SHIFT) & LDO_AVG_CURRENT;
			}
			/* clear pin control */
			mask0 |= LDO_PIN_CTRL;
		} else {
			mask0 = LDO_PIN_CTRL;
			val0 = vreg->pdata->pin_ctrl << LDO_PIN_CTRL_SHIFT;
		}
		break;

	case REGULATOR_MODE_NORMAL:
		if (pc_vote && --pc_vote)
			goto done; /* already taken care of */

		if (optimum == REGULATOR_MODE_STANDBY) {
			if (peak_uA > vreg_lpm_max_uA(vreg)) {
				mask0 = LDO_PEAK_CURRENT;
				mask1 = LDO_AVG_CURRENT;
				val0 = (MICRO_TO_MILLI(vreg_lpm_max_uA(vreg)) <<
				   LDO_PEAK_CURRENT_SHIFT) & LDO_PEAK_CURRENT;
				val1 = (MICRO_TO_MILLI(vreg_lpm_max_uA(vreg)) <<
				     LDO_AVG_CURRENT_SHIFT) & LDO_AVG_CURRENT;
			}
		} else {
			if (peak_uA < vreg_hpm_min_uA(vreg)) {
				mask0 = LDO_PEAK_CURRENT;
				mask1 = LDO_AVG_CURRENT;
				val0 = (MICRO_TO_MILLI(vreg_hpm_min_uA(vreg)) <<
				   LDO_PEAK_CURRENT_SHIFT) & LDO_PEAK_CURRENT;
				val1 = (MICRO_TO_MILLI(vreg_hpm_min_uA(vreg)) <<
				     LDO_AVG_CURRENT_SHIFT) & LDO_AVG_CURRENT;
			}
		}
		/* clear pin control */
		mask0 |= LDO_PIN_CTRL;
		break;

	default:
		return -EINVAL;
	}

	rc = vreg_set(rdev_get_drvdata(dev), mask0, val0, mask1, val1, 2);
	if (rc)
		return rc;

done:
	vreg->mode_initialized = mode_initialized;
	vreg->optimum = optimum;
	vreg->pc_vote = pc_vote;

	return 0;
}

static unsigned int ldo_get_mode(struct regulator_dev *dev)
{
	struct vreg *vreg = rdev_get_drvdata(dev);

	if (vreg->optimum == REGULATOR_MODE_FAST)
		return REGULATOR_MODE_FAST;
	else if (vreg->pc_vote)
		return REGULATOR_MODE_IDLE;
	else if (vreg->optimum == REGULATOR_MODE_STANDBY)
		return REGULATOR_MODE_STANDBY;
	return REGULATOR_MODE_FAST;
}

unsigned int ldo_get_optimum_mode(struct regulator_dev *dev, int input_uV,
		int output_uV, int load_uA)
{
	struct vreg *vreg = rdev_get_drvdata(dev);

	if (MICRO_TO_MILLI(load_uA) > 0) {
		vreg->req[0].value &= ~LDO_PEAK_CURRENT;
		vreg->req[0].value |= (MICRO_TO_MILLI(saturate_load(load_uA)) <<
				     LDO_PEAK_CURRENT_SHIFT) & LDO_PEAK_CURRENT;
		vreg->req[1].value &= ~LDO_AVG_CURRENT;
		vreg->req[1].value |= (MICRO_TO_MILLI(saturate_load(load_uA)) <<
				       LDO_AVG_CURRENT_SHIFT) & LDO_AVG_CURRENT;
	} else {
		/*
		 * ldo_get_optimum_mode is being called before consumers have
		 * specified their load currents via regulator_set_optimum_mode.
		 * Return whatever the existing mode is.
		 */
		return ldo_get_mode(dev);
	}

	if (load_uA > vreg->lpm_max_load)
		return REGULATOR_MODE_FAST;
	return REGULATOR_MODE_STANDBY;
}

static int switch_enable(struct regulator_dev *dev)
{
	return vreg_set(rdev_get_drvdata(dev), SWITCH_STATE,
			RPM_VREG_STATE_ON << SWITCH_STATE_SHIFT, 0, 0, 1);
}

static int switch_disable(struct regulator_dev *dev)
{
	return vreg_set(rdev_get_drvdata(dev), SWITCH_STATE,
			RPM_VREG_STATE_OFF << SWITCH_STATE_SHIFT, 0, 0, 1);
}

static int switch_is_enabled(struct regulator_dev *dev)
{
	struct vreg *vreg = rdev_get_drvdata(dev);
	enum rpm_vreg_state state;

	state = (vreg->req[0].value & SWITCH_STATE) >> SWITCH_STATE_SHIFT;

	return state == RPM_VREG_STATE_ON;
}

/*
 * Pin ctrl mode voting via regulator set_mode:
 * REGULATOR_MODE_IDLE: Go to pin ctrl mode if the optimum mode is LPM, else
 * go to HPM
 * REGULATOR_MODE_NORMAL: Go to LPM if it is the optimum mode, else go to HPM
 */
static int switch_set_mode(struct regulator_dev *dev, unsigned int mode)
{
	struct vreg *vreg = rdev_get_drvdata(dev);
	unsigned pc_vote = vreg->pc_vote;
	unsigned mask, val;
	int rc;

	switch (mode) {
	case REGULATOR_MODE_IDLE:
		if (pc_vote++)
			goto done; /* already taken care of */

		mask = SWITCH_PIN_CTRL;
		val = vreg->pdata->pin_ctrl << SWITCH_PIN_CTRL_SHIFT;
		break;

	case REGULATOR_MODE_NORMAL:
		if (--pc_vote)
			goto done; /* already taken care of */

		mask = SWITCH_PIN_CTRL;
		val = RPM_VREG_PIN_CTRL_NONE << SWITCH_PIN_CTRL_SHIFT;
		break;

	default:
		return -EINVAL;
	}

	rc = vreg_set(rdev_get_drvdata(dev), mask, val, 0, 0, 2);
	if (rc)
		return rc;

done:
	vreg->pc_vote = pc_vote;
	return 0;
}

static unsigned int switch_get_mode(struct regulator_dev *dev)
{
	struct vreg *vreg = rdev_get_drvdata(dev);

	if (vreg->pc_vote)
		return REGULATOR_MODE_IDLE;
	return REGULATOR_MODE_NORMAL;
}

static int ncp_enable(struct regulator_dev *dev)
{
	return vreg_set(rdev_get_drvdata(dev), NCP_STATE,
			RPM_VREG_STATE_ON << NCP_STATE_SHIFT, 0, 0, 2);
}

static int ncp_disable(struct regulator_dev *dev)
{
	return vreg_set(rdev_get_drvdata(dev), NCP_STATE,
			RPM_VREG_STATE_OFF << NCP_STATE_SHIFT, 0, 0, 2);
}

static int ncp_is_enabled(struct regulator_dev *dev)
{
	struct vreg *vreg = rdev_get_drvdata(dev);
	enum rpm_vreg_state state;

	state = (vreg->req[0].value & NCP_STATE) >> NCP_STATE_SHIFT;

	return state == RPM_VREG_STATE_ON;
}

static int ncp_set_voltage(struct regulator_dev *dev,
		int min_uV, int max_uV)
{
	return vreg_set(rdev_get_drvdata(dev), NCP_VOLTAGE,
			MICRO_TO_MILLI(min_uV) << NCP_VOLTAGE_SHIFT, 0, 0, 2);
}

static int ncp_get_voltage(struct regulator_dev *dev)
{
	struct vreg *vreg = rdev_get_drvdata(dev);

	return MILLI_TO_MICRO((vreg->req[0].value & NCP_VOLTAGE) >>
			NCP_VOLTAGE_SHIFT);
}

static struct regulator_ops ldo_ops = {
	.enable = ldo_enable,
	.disable = ldo_disable,
	.is_enabled = ldo_is_enabled,
	.set_voltage = ldo_set_voltage,
	.get_voltage = ldo_get_voltage,
	.set_mode = ldo_set_mode,
	.get_optimum_mode = ldo_get_optimum_mode,
	.get_mode = ldo_get_mode,
};

static struct regulator_ops smps_ops = {
	.enable = smps_enable,
	.disable = smps_disable,
	.is_enabled = smps_is_enabled,
	.set_voltage = smps_set_voltage,
	.get_voltage = smps_get_voltage,
	.set_mode = smps_set_mode,
	.get_optimum_mode = smps_get_optimum_mode,
	.get_mode = smps_get_mode,
};

static struct regulator_ops smps_8901_ops = {
	.enable = smps_enable,
	.disable = smps_disable,
	.is_enabled = smps_is_enabled,
	.set_voltage = smps_8901_set_voltage,
	.get_voltage = smps_get_voltage,
	.set_mode = smps_set_mode,
	.get_optimum_mode = smps_get_optimum_mode,
	.get_mode = smps_get_mode,
};

static struct regulator_ops switch_ops = {
	.enable = switch_enable,
	.disable = switch_disable,
	.is_enabled = switch_is_enabled,
	.set_mode = switch_set_mode,
	.get_mode = switch_get_mode,
};

static struct regulator_ops ncp_ops = {
	.enable = ncp_enable,
	.disable = ncp_disable,
	.is_enabled = ncp_is_enabled,
	.set_voltage = ncp_set_voltage,
	.get_voltage = ncp_get_voltage,
};

#define DESC(_id, _name, _ops) \
	[_id] = { \
		.id = _id, \
		.name = _name, \
		.ops = _ops, \
		.type = REGULATOR_VOLTAGE, \
		.owner = THIS_MODULE, \
	}

static struct regulator_desc vreg_descrip[RPM_VREG_ID_MAX] = {
	DESC(RPM_VREG_ID_PM8058_L0, "8058_l0", &ldo_ops),
	DESC(RPM_VREG_ID_PM8058_L1, "8058_l1", &ldo_ops),
	DESC(RPM_VREG_ID_PM8058_L2, "8058_l2", &ldo_ops),
	DESC(RPM_VREG_ID_PM8058_L3, "8058_l3", &ldo_ops),
	DESC(RPM_VREG_ID_PM8058_L4, "8058_l4", &ldo_ops),
	DESC(RPM_VREG_ID_PM8058_L5, "8058_l5", &ldo_ops),
	DESC(RPM_VREG_ID_PM8058_L6, "8058_l6", &ldo_ops),
	DESC(RPM_VREG_ID_PM8058_L7, "8058_l7", &ldo_ops),
	DESC(RPM_VREG_ID_PM8058_L8, "8058_l8", &ldo_ops),
	DESC(RPM_VREG_ID_PM8058_L9, "8058_l9", &ldo_ops),
	DESC(RPM_VREG_ID_PM8058_L10, "8058_l10", &ldo_ops),
	DESC(RPM_VREG_ID_PM8058_L11, "8058_l11", &ldo_ops),
	DESC(RPM_VREG_ID_PM8058_L12, "8058_l12", &ldo_ops),
	DESC(RPM_VREG_ID_PM8058_L13, "8058_l13", &ldo_ops),
	DESC(RPM_VREG_ID_PM8058_L14, "8058_l14", &ldo_ops),
	DESC(RPM_VREG_ID_PM8058_L15, "8058_l15", &ldo_ops),
	DESC(RPM_VREG_ID_PM8058_L16, "8058_l16", &ldo_ops),
	DESC(RPM_VREG_ID_PM8058_L17, "8058_l17", &ldo_ops),
	DESC(RPM_VREG_ID_PM8058_L18, "8058_l18", &ldo_ops),
	DESC(RPM_VREG_ID_PM8058_L19, "8058_l19", &ldo_ops),
	DESC(RPM_VREG_ID_PM8058_L20, "8058_l20", &ldo_ops),
	DESC(RPM_VREG_ID_PM8058_L21, "8058_l21", &ldo_ops),
	DESC(RPM_VREG_ID_PM8058_L22, "8058_l22", &ldo_ops),
	DESC(RPM_VREG_ID_PM8058_L23, "8058_l23", &ldo_ops),
	DESC(RPM_VREG_ID_PM8058_L24, "8058_l24", &ldo_ops),
	DESC(RPM_VREG_ID_PM8058_L25, "8058_l25", &ldo_ops),

	DESC(RPM_VREG_ID_PM8058_S0, "8058_s0", &smps_ops),
	DESC(RPM_VREG_ID_PM8058_S1, "8058_s1", &smps_ops),
	DESC(RPM_VREG_ID_PM8058_S2, "8058_s2", &smps_ops),
	DESC(RPM_VREG_ID_PM8058_S3, "8058_s3", &smps_ops),
	DESC(RPM_VREG_ID_PM8058_S4, "8058_s4", &smps_ops),

	DESC(RPM_VREG_ID_PM8058_LVS0, "8058_lvs0", &switch_ops),
	DESC(RPM_VREG_ID_PM8058_LVS1, "8058_lvs1", &switch_ops),

	DESC(RPM_VREG_ID_PM8058_NCP, "8058_ncp", &ncp_ops),

	DESC(RPM_VREG_ID_PM8901_L0, "8901_l0", &ldo_ops),
	DESC(RPM_VREG_ID_PM8901_L1, "8901_l1", &ldo_ops),
	DESC(RPM_VREG_ID_PM8901_L2, "8901_l2", &ldo_ops),
	DESC(RPM_VREG_ID_PM8901_L3, "8901_l3", &ldo_ops),
	DESC(RPM_VREG_ID_PM8901_L4, "8901_l4", &ldo_ops),
	DESC(RPM_VREG_ID_PM8901_L5, "8901_l5", &ldo_ops),
	DESC(RPM_VREG_ID_PM8901_L6, "8901_l6", &ldo_ops),

	DESC(RPM_VREG_ID_PM8901_S0, "8901_s0", &smps_8901_ops),
	DESC(RPM_VREG_ID_PM8901_S1, "8901_s1", &smps_8901_ops),
	DESC(RPM_VREG_ID_PM8901_S2, "8901_s2", &smps_8901_ops),
	DESC(RPM_VREG_ID_PM8901_S3, "8901_s3", &smps_8901_ops),
	DESC(RPM_VREG_ID_PM8901_S4, "8901_s4", &smps_8901_ops),

	DESC(RPM_VREG_ID_PM8901_LVS0, "8901_lvs0", &switch_ops),
	DESC(RPM_VREG_ID_PM8901_LVS1, "8901_lvs1", &switch_ops),
	DESC(RPM_VREG_ID_PM8901_LVS2, "8901_lvs2", &switch_ops),
	DESC(RPM_VREG_ID_PM8901_LVS3, "8901_lvs3", &switch_ops),

	DESC(RPM_VREG_ID_PM8901_MVS0, "8901_mvs0", &switch_ops),
};

static void ldo_init(struct vreg *vreg)
{
	vreg->req[0].value =
		MICRO_TO_MILLI(vreg->pdata->default_uV) <<
			LDO_VOLTAGE_SHIFT |
		MICRO_TO_MILLI(saturate_load(vreg->pdata->peak_uA)) <<
			LDO_PEAK_CURRENT_SHIFT |
		vreg->pdata->mode << LDO_MODE_SHIFT |
		vreg->pdata->pin_fn << LDO_PIN_FN_SHIFT;

	vreg->req[1].value =
		vreg->pdata->pull_down_enable << LDO_PULL_DOWN_ENABLE_SHIFT |
		MICRO_TO_MILLI(saturate_load(vreg->pdata->avg_uA)) <<
			LDO_AVG_CURRENT_SHIFT;
}

static void smps_init(struct vreg *vreg)
{
	vreg->req[0].value =
		MICRO_TO_MILLI(vreg->pdata->default_uV) << SMPS_VOLTAGE_SHIFT |
		MICRO_TO_MILLI(saturate_load(vreg->pdata->peak_uA)) <<
			SMPS_PEAK_CURRENT_SHIFT |
		vreg->pdata->mode << SMPS_MODE_SHIFT |
		vreg->pdata->pin_fn << SMPS_PIN_FN_SHIFT;


	vreg->req[1].value =
		vreg->pdata->pull_down_enable << SMPS_PULL_DOWN_ENABLE_SHIFT |
		MICRO_TO_MILLI(saturate_load(vreg->pdata->avg_uA)) <<
			SMPS_AVG_CURRENT_SHIFT |
		vreg->pdata->freq << SMPS_FREQ_SHIFT |
		0 << SMPS_CLK_SRC_SHIFT;
}

static void ncp_init(struct vreg *vreg)
{
	vreg->req[0].value =
		MICRO_TO_MILLI(vreg->pdata->default_uV) << NCP_VOLTAGE_SHIFT |
		vreg->pdata->state << NCP_STATE_SHIFT;
}

static void switch_init(struct vreg *vreg)
{
	vreg->req[0].value =
		vreg->pdata->state << SWITCH_STATE_SHIFT |
		vreg->pdata->pull_down_enable <<
			SWITCH_PULL_DOWN_ENABLE_SHIFT |
		vreg->pdata->pin_fn << SWITCH_PIN_FN_SHIFT;
}

static int vreg_init(enum rpm_vreg_id id, struct vreg *vreg)
{
	/* save_uV must be > 0 for regulator_set_optimum_mode to work. */
	vreg->save_uV = 1;

	if (vreg->pdata->peak_uA > vreg->lpm_max_load)
		vreg->optimum = REGULATOR_MODE_FAST;
	else
		vreg->optimum = REGULATOR_MODE_STANDBY;

	vreg->mode_initialized = 0;

	if (IS_LDO(id))
		ldo_init(vreg);
	else if (IS_SMPS(id))
		smps_init(vreg);
	else if (IS_NCP(id))
		ncp_init(vreg);
	else if (IS_SWITCH(id))
		switch_init(vreg);
	else
		return -EINVAL;

	return 0;
}

static int __devinit rpm_vreg_probe(struct platform_device *pdev)
{
	struct regulator_desc *rdesc;
	struct regulator_dev *rdev;
	struct vreg *vreg;
	int rc;

	if (pdev == NULL)
		return -EINVAL;

	if (pdev->id < 0 || pdev->id >= RPM_VREG_ID_MAX)
		return -ENODEV;

	vreg = &vregs[pdev->id];
	vreg->pdata = pdev->dev.platform_data;
	rdesc = &vreg_descrip[pdev->id];

	rc = vreg_init(pdev->id, vreg);
	if (rc) {
		pr_err("%s: vreg_init failed, rc=%d\n", __func__, rc);
		return rc;
	}

	rdev = regulator_register(rdesc, &pdev->dev,
			&vreg->pdata->init_data, vreg);
	if (IS_ERR(rdev)) {
		rc = PTR_ERR(rdev);
		pr_err("%s: id=%d, rc=%d\n", __func__,
				pdev->id, rc);
		return rc;
	}

	platform_set_drvdata(pdev, rdev);

	return rc;
}

static int __devexit rpm_vreg_remove(struct platform_device *pdev)
{
	struct regulator_dev *rdev = platform_get_drvdata(pdev);

	platform_set_drvdata(pdev, NULL);
	regulator_unregister(rdev);

	return 0;
}

static struct platform_driver rpm_vreg_driver = {
	.probe = rpm_vreg_probe,
	.remove = __devexit_p(rpm_vreg_remove),
	.driver = {
		.name = "rpm-regulator",
		.owner = THIS_MODULE,
	},
};

static int __init rpm_vreg_init(void)
{
	return platform_driver_register(&rpm_vreg_driver);
}

static void __exit rpm_vreg_exit(void)
{
	platform_driver_unregister(&rpm_vreg_driver);
}

postcore_initcall(rpm_vreg_init);
module_exit(rpm_vreg_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("rpm regulator driver");
MODULE_VERSION("1.0");
MODULE_ALIAS("platform:rpm-regulator");
