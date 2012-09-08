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
 *
 */
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/mutex.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/err.h>
#include <linux/m_adc.h>
#include <linux/pmic8058-xoadc.h>
#include <linux/mfd/pmic8058.h>
#include <linux/interrupt.h>
#include <linux/slab.h>

#include <mach/mpp.h>

#define ADC_DRIVER_NAME			"pm8058-xoadc"

#define MAX_QUEUE_LENGTH        0X15
#define MAX_CHANNEL_PROPERTIES_QUEUE    0X7
#define MAX_QUEUE_SLOT		0x1

/* User Processor */
#define ADC_ARB_USRP_CNRTL                      0x197
#define ADC_ARB_USRP_AMUX_CNTRL         0x198
#define ADC_ARB_USRP_ANA_PARAM          0x199
#define ADC_ARB_USRP_DIG_PARAM          0x19A
#define ADC_ARB_USRP_RSV                        0x19B

#define ADC_ARB_USRP_DATA0                      0x19D
#define ADC_ARB_USRP_DATA1                      0x19C

struct pmic8058_adc {
	struct xoadc_platform_data *pdata;
	struct pm8058_chip *pm_chip;
	struct adc_properties *adc_prop;
	struct xoadc_conv_state	conv[2];
	int adc_irq;
	struct linear_graph *adc_graph;
	struct xoadc_conv_state *conv_slot_request;
	struct xoadc_conv_state *conv_queue_list;
	struct adc_conv_slot conv_queue_elements[MAX_QUEUE_LENGTH];
	int xoadc_queue_count;
	int xoadc_num;
};

static struct pmic8058_adc *pmic_adc[XOADC_PMIC_0 + 1];

static bool xoadc_initialized;

int32_t pm8058_xoadc_registered(void)
{
	return xoadc_initialized;
}
EXPORT_SYMBOL(pm8058_xoadc_registered);

void pm8058_xoadc_restore_slot(uint32_t adc_instance,
					struct adc_conv_slot *slot)
{
	struct pmic8058_adc *adc_pmic = pmic_adc[adc_instance];
	struct xoadc_conv_state *slot_state = adc_pmic->conv_slot_request;

	mutex_lock(&slot_state->list_lock);
	list_add(&slot->list, &slot_state->slots);
	mutex_unlock(&slot_state->list_lock);
}
EXPORT_SYMBOL(pm8058_xoadc_restore_slot);

void pm8058_xoadc_slot_request(uint32_t adc_instance,
					struct adc_conv_slot **slot)
{
	struct pmic8058_adc *adc_pmic = pmic_adc[adc_instance];
	struct xoadc_conv_state *slot_state = adc_pmic->conv_slot_request;

	mutex_lock(&slot_state->list_lock);

	if (!list_empty(&slot_state->slots)) {
		*slot = list_first_entry(&slot_state->slots,
				struct adc_conv_slot, list);
		list_del(&(*slot)->list);
	} else
		*slot = NULL;

	mutex_unlock(&slot_state->list_lock);
}
EXPORT_SYMBOL(pm8058_xoadc_slot_request);

static int32_t pm8058_xoadc_configure(uint32_t adc_instance,
					struct adc_conv_slot *slot)
{

	struct pmic8058_adc *adc_pmic = pmic_adc[adc_instance];
	u8 data_arb_cnrtl, data_amux_chan, data_arb_rsv, data_ana_param;
	u8 data_dig_param, data_ana_param2;
	int rc;

	/* Write Twice to EN_ARB sig */
	data_arb_cnrtl = 0x71;
	rc = pm8058_write(adc_pmic->pm_chip, ADC_ARB_USRP_CNRTL,
					&data_arb_cnrtl, 1);
	if (rc < 0) {
		pr_debug("%s: PM8058 write failed\n", __func__);
		return rc;
	}

	rc = pm8058_write(adc_pmic->pm_chip, ADC_ARB_USRP_CNRTL,
					&data_arb_cnrtl, 1);
	if (rc < 0) {
		pr_debug("%s: PM8058 write failed\n", __func__);
		return rc;
	}

	switch (slot->chan_path) {

	case CHAN_PATH_TYPE1:
		data_amux_chan = CHANNEL_VCOIN << 4;
		data_arb_rsv = 0x20;
		slot->chan_properties.gain_numerator = 1;
		slot->chan_properties.gain_denominator = 2;
		slot->chan_properties.adc_graph = &adc_pmic->adc_graph[0];
		break;

	case CHAN_PATH_TYPE2:
		data_amux_chan = CHANNEL_VBAT << 4;
		data_arb_rsv = 0x20;
		slot->chan_properties.gain_numerator = 1;
		slot->chan_properties.gain_denominator = 3;
		slot->chan_properties.adc_graph = &adc_pmic->adc_graph[0];
		break;

	case CHAN_PATH_TYPE3:
		data_amux_chan = CHANNEL_VCHG << 4;
		data_arb_rsv = 0x20;
		slot->chan_properties.gain_numerator = 1;
		slot->chan_properties.gain_denominator = 10;
		slot->chan_properties.adc_graph = &adc_pmic->adc_graph[0];
		break;

	case CHAN_PATH_TYPE4:
		data_amux_chan = CHANNEL_CHG_MONITOR << 4;
		data_arb_rsv = 0x20;
		slot->chan_properties.gain_numerator = 1;
		slot->chan_properties.gain_denominator = 1;
		slot->chan_properties.adc_graph = &adc_pmic->adc_graph[0];
		break;

	case CHAN_PATH_TYPE5:
		data_amux_chan = CHANNEL_VPH_PWR << 4;
		data_arb_rsv = 0x20;
		slot->chan_properties.gain_numerator = 1;
		slot->chan_properties.gain_denominator = 3;
		slot->chan_properties.adc_graph = &adc_pmic->adc_graph[0];
		break;

	case CHAN_PATH_TYPE6:
		data_amux_chan = CHANNEL_MPP5 << 4;
		data_arb_rsv = 0x20;
		slot->chan_properties.gain_numerator = 1;
		slot->chan_properties.gain_denominator = 1;
		slot->chan_properties.adc_graph = &adc_pmic->adc_graph[0];
		break;

	case CHAN_PATH_TYPE7:
		data_amux_chan = CHANNEL_MPP6 << 4;
		data_arb_rsv = 0x20;
		slot->chan_properties.gain_numerator = 1;
		slot->chan_properties.gain_denominator = 1;
		slot->chan_properties.adc_graph = &adc_pmic->adc_graph[0];
		break;

	case CHAN_PATH_TYPE8:
		data_amux_chan = CHANNEL_MPP7 << 4;
		data_arb_rsv = 0x20;
		slot->chan_properties.gain_numerator = 1;
		slot->chan_properties.gain_denominator = 2;
		slot->chan_properties.adc_graph = &adc_pmic->adc_graph[0];
		break;

	case CHAN_PATH_TYPE9:
		data_amux_chan = CHANNEL_MPP8 << 4;
		data_arb_rsv = 0x20;
		slot->chan_properties.gain_numerator = 1;
		slot->chan_properties.gain_denominator = 2;
		slot->chan_properties.adc_graph = &adc_pmic->adc_graph[0];
		break;

	case CHAN_PATH_TYPE10:
		data_amux_chan = CHANNEL_MPP9 << 4;
		data_arb_rsv = 0x20;
		slot->chan_properties.gain_numerator = 1;
		slot->chan_properties.gain_denominator = 3;
		slot->chan_properties.adc_graph = &adc_pmic->adc_graph[0];
		break;

	case CHAN_PATH_TYPE11:
		data_amux_chan = CHANNEL_USB_VBUS << 4;
		data_arb_rsv = 0x20;
		slot->chan_properties.gain_numerator = 1;
		slot->chan_properties.gain_denominator = 3;
		slot->chan_properties.adc_graph = &adc_pmic->adc_graph[0];
		break;

	case CHAN_PATH_TYPE12:
		data_amux_chan = CHANNEL_DIE_TEMP << 4;
		data_arb_rsv = 0x20;
		slot->chan_properties.gain_numerator = 1;
		slot->chan_properties.gain_denominator = 1;
		slot->chan_properties.adc_graph = &adc_pmic->adc_graph[0];
		break;

	case CHAN_PATH_TYPE13:
		data_amux_chan = CHANNEL_125V << 4;
		data_arb_rsv = 0x20;
		slot->chan_properties.gain_numerator = 1;
		slot->chan_properties.gain_denominator = 1;
		slot->chan_properties.adc_graph = &adc_pmic->adc_graph[0];
		break;

	case CHAN_PATH_TYPE14:
		data_amux_chan = CHANNEL_INTERNAL_2 << 4;
		data_arb_rsv = 0x20;
		slot->chan_properties.gain_numerator = 1;
		slot->chan_properties.gain_denominator = 1;
		slot->chan_properties.adc_graph = &adc_pmic->adc_graph[0];
		break;

	case CHAN_PATH_TYPE_NONE:
		data_amux_chan = CHANNEL_MUXOFF << 4;
		data_arb_rsv = 0x10;
		slot->chan_properties.gain_numerator = 1;
		slot->chan_properties.gain_denominator = 1;
		slot->chan_properties.adc_graph = &adc_pmic->adc_graph[0];
		break;

	case CHAN_PATH_TYPE15:
		data_amux_chan = CHANNEL_INTERNAL << 4;
		data_arb_rsv = 0x10;
		slot->chan_properties.gain_numerator = 1;
		slot->chan_properties.gain_denominator = 1;
		slot->chan_properties.adc_graph = &adc_pmic->adc_graph[0];
		break;
	}

	rc = pm8058_write(adc_pmic->pm_chip,
			ADC_ARB_USRP_AMUX_CNTRL, &data_amux_chan, 1);
	if (rc < 0) {
		pr_debug("%s: PM8058 write failed\n", __func__);
		return rc;
	}

	rc = pm8058_write(adc_pmic->pm_chip,
			ADC_ARB_USRP_RSV, &data_arb_rsv, 1);
	if (rc < 0) {
		pr_debug("%s: PM8058 write failed\n", __func__);
		return rc;
	}

	/* Set default clock rate to 2.4 MHz XO ADC clock digital */
	switch (slot->chan_adc_config) {

	case ADC_CONFIG_TYPE1:
		data_ana_param = 0xFE;
		data_dig_param = 0x23;
		data_ana_param2 = 0xFF;
		/* AMUX register data to start the ADC conversion */
		data_arb_cnrtl = 0xF1;
		break;

	case ADC_CONFIG_TYPE2:
		data_ana_param = 0xFE;
		data_dig_param = 0x03;
		data_ana_param2 = 0xFF;
		/* AMUX register data to start the ADC conversion */
		data_arb_cnrtl = 0xF1;
		break;
	}

	rc = pm8058_write(adc_pmic->pm_chip,
				ADC_ARB_USRP_ANA_PARAM, &data_ana_param, 1);
	if (rc < 0) {
		pr_debug("%s: PM8058 write failed\n", __func__);
		return rc;
	}

	rc = pm8058_write(adc_pmic->pm_chip,
				ADC_ARB_USRP_DIG_PARAM, &data_dig_param, 1);
	if (rc < 0) {
		pr_debug("%s: PM8058 write failed\n", __func__);
		return rc;
	}

	rc = pm8058_write(adc_pmic->pm_chip,
				ADC_ARB_USRP_ANA_PARAM, &data_ana_param2, 1);
	if (rc < 0) {
		pr_debug("%s: PM8058 write failed\n", __func__);
		return rc;
	}

	rc = pm8058_write(adc_pmic->pm_chip,
				ADC_ARB_USRP_CNRTL, &data_arb_cnrtl, 1);
	if (rc < 0) {
		pr_debug("%s: PM8058 write failed\n", __func__);
		return rc;
	}

	return 0;
}

int32_t pm8058_xoadc_select_chan_and_start_conv(uint32_t adc_instance,
					struct adc_conv_slot *slot)
{
	struct pmic8058_adc *adc_pmic = pmic_adc[adc_instance];
	struct xoadc_conv_state *slot_state = adc_pmic->conv_queue_list;

	if (!xoadc_initialized)
		return -ENODEV;

	mutex_lock(&slot_state->list_lock);
	list_add_tail(&slot->list, &slot_state->slots);
	if (adc_pmic->xoadc_queue_count == 0)
		pm8058_xoadc_configure(adc_instance, slot);
	adc_pmic->xoadc_queue_count++;
	mutex_unlock(&slot_state->list_lock);

	return 0;
}
EXPORT_SYMBOL(pm8058_xoadc_select_chan_and_start_conv);

static int32_t pm8058_xoadc_dequeue_slot_request(uint32_t adc_instance,
				struct adc_conv_slot **slot)
{
	struct pmic8058_adc *adc_pmic = pmic_adc[adc_instance];
	struct xoadc_conv_state *slot_state = adc_pmic->conv_queue_list;

	mutex_lock(&slot_state->list_lock);
	*slot = list_first_entry(&slot_state->slots,
			struct adc_conv_slot, list);
	list_del(&(*slot)->list);
	mutex_unlock(&slot_state->list_lock);

	return 0;
}

int32_t pm8058_xoadc_read_adc_code(uint32_t adc_instance, int32_t *data)
{
	struct pmic8058_adc *adc_pmic = pmic_adc[adc_instance];
	struct xoadc_conv_state *slot_state = adc_pmic->conv_queue_list;
	uint8_t rslt_lsb, rslt_msb;
	struct adc_conv_slot *slot;
	int32_t rc, max_ideal_adc_code = 1 << adc_pmic->adc_prop->bitresolution;

	if (!xoadc_initialized)
		return -ENODEV;

	rc = pm8058_read(adc_pmic->pm_chip, ADC_ARB_USRP_DATA0, &rslt_lsb, 1);
	if (rc < 0) {
		pr_debug("%s: PM8058 read failed\n", __func__);
		return rc;
	}

	rc = pm8058_read(adc_pmic->pm_chip, ADC_ARB_USRP_DATA1, &rslt_msb, 1);
	if (rc < 0) {
		pr_debug("%s: PM8058 read failed\n", __func__);
		return rc;
	}

	*data = (rslt_msb << 8) | rslt_lsb;

	/* Use the midpoint to determine underflow or overflow */
	if (*data > max_ideal_adc_code + (max_ideal_adc_code >> 1))
		*data |= ((1 << (8 * sizeof(*data) -
			adc_pmic->adc_prop->bitresolution)) - 1) <<
			adc_pmic->adc_prop->bitresolution;

	mutex_lock(&slot_state->list_lock);
	adc_pmic->xoadc_queue_count--;
	if (adc_pmic->xoadc_queue_count) {
		slot = list_first_entry(&slot_state->slots,
				struct adc_conv_slot, list);
		pm8058_xoadc_configure(adc_instance, slot);
	}
	mutex_unlock(&slot_state->list_lock);

	return 0;
}
EXPORT_SYMBOL(pm8058_xoadc_read_adc_code);

static irqreturn_t pm8058_xoadc(int irq, void *dev_id)
{
	struct pmic8058_adc *xoadc_8058 = dev_id;
	struct adc_conv_slot *slot;

	pm8058_xoadc_dequeue_slot_request(xoadc_8058->xoadc_num, &slot);

	msm_adc_conv_cb(slot, 0, NULL, 0);

	return IRQ_HANDLED;
}

struct adc_properties *pm8058_xoadc_get_properties(uint32_t dev_instance)
{
	struct pmic8058_adc *xoadc_8058 = pmic_adc[dev_instance];

	return xoadc_8058->adc_prop;
}
EXPORT_SYMBOL(pm8058_xoadc_get_properties);

int32_t pm8058_xoadc_calibrate(uint32_t dev_instance,
				struct adc_conv_slot *slot, int *calib_status)
{
	*calib_status = CALIB_NOT_REQUIRED;
	return 0;
}
EXPORT_SYMBOL(pm8058_xoadc_calibrate);

static int __devexit pm8058_xoadc_teardown(struct platform_device *pdev)
{
	struct pmic8058_adc *adc_pmic = platform_get_drvdata(pdev);

	platform_set_drvdata(pdev, adc_pmic->pm_chip);
	device_init_wakeup(&pdev->dev, 0);
	kfree(adc_pmic);
	xoadc_initialized = false;

	return 0;
}

static int __devinit pm8058_xoadc_probe(struct platform_device *pdev)
{
	struct xoadc_platform_data *pdata = pdev->dev.platform_data;
	struct pm8058_chip *pm_chip;
	struct pmic8058_adc *adc_pmic;
	int i, rc = 0;

	pm_chip = platform_get_drvdata(pdev);
	if (pm_chip == NULL) {
		dev_err(&pdev->dev, "no parent data passed in\n");
		return -EFAULT;
	}

	if (!pdata) {
		dev_err(&pdev->dev, "no platform data?\n");
		return -EINVAL;
	}

	adc_pmic = kzalloc(sizeof(struct pmic8058_adc), GFP_KERNEL);
	if (!adc_pmic) {
		dev_err(&pdev->dev, "Unable to allocate memory\n");
		return -ENOMEM;
	}

	adc_pmic->pm_chip = pm_chip;
	adc_pmic->adc_prop = pdata->xoadc_prop;
	adc_pmic->xoadc_num = pdata->xoadc_num;

	platform_set_drvdata(pdev, adc_pmic);

	if (adc_pmic->xoadc_num > XOADC_PMIC_0) {
		dev_err(&pdev->dev, "ADC device not supported\n");
		rc = -EINVAL;
		goto err_cleanup;
	}

	adc_pmic->pdata = pdata;
	adc_pmic->adc_graph = kzalloc(sizeof(struct linear_graph)
			* MAX_CHANNEL_PROPERTIES_QUEUE, GFP_KERNEL);
	if (!adc_pmic->adc_graph) {
		dev_err(&pdev->dev, "Unable to allocate memory\n");
		rc = -ENOMEM;
		goto err_cleanup;
	}

	/* Will be replaced by individual channel calibration */
	for (i = 0; i < MAX_CHANNEL_PROPERTIES_QUEUE; i++) {
		adc_pmic->adc_graph[i].offset = 0 ;
		adc_pmic->adc_graph[i].dy = (1 << 15) - 1;
		adc_pmic->adc_graph[i].dx = 2200;
	}

	if (pdata->xoadc_mpp_config != NULL)
		pdata->xoadc_mpp_config();

	adc_pmic->conv_slot_request = &adc_pmic->conv[0];
	adc_pmic->conv_slot_request->context =
		&adc_pmic->conv_queue_elements[0];

	mutex_init(&adc_pmic->conv_slot_request->list_lock);
	INIT_LIST_HEAD(&adc_pmic->conv_slot_request->slots);

	/* tie each slot and initwork them */
	for (i = 0; i < MAX_QUEUE_LENGTH; i++) {
		list_add(&adc_pmic->conv_slot_request->context[i].list,
					&adc_pmic->conv_slot_request->slots);
		INIT_WORK(&adc_pmic->conv_slot_request->context[i].work,
							msm_adc_wq_work);
		init_completion(&adc_pmic->conv_slot_request->context[i].comp);
		adc_pmic->conv_slot_request->context[i].idx = i;
	}

	adc_pmic->conv_queue_list = &adc_pmic->conv[1];

	mutex_init(&adc_pmic->conv_queue_list->list_lock);
	INIT_LIST_HEAD(&adc_pmic->conv_queue_list->slots);

	adc_pmic->adc_irq = platform_get_irq(pdev, 0);
	if (adc_pmic->adc_irq < 0) {
		rc = -ENXIO;
		goto err_cleanup;
	}

	rc = request_threaded_irq(adc_pmic->adc_irq,
				NULL, pm8058_xoadc,
		IRQF_TRIGGER_RISING, "pm8058_adc_interrupt", adc_pmic);
	if (rc) {
		dev_err(&pdev->dev, "failed to request adc irq\n");
		goto err_cleanup;
	}

	device_init_wakeup(&pdev->dev, pdata->xoadc_wakeup);

	pmic_adc[adc_pmic->xoadc_num] = adc_pmic;

	pr_debug("pm8058 xoadc successfully registered\n");

	if (pdata->xoadc_setup != NULL)
		pdata->xoadc_setup();

	xoadc_initialized = true;

	return 0;

err_cleanup:
	pm8058_xoadc_teardown(pdev);

	return rc;
}

static struct platform_driver pm8058_xoadc_driver = {
	.probe = pm8058_xoadc_probe,
	.remove = __devexit_p(pm8058_xoadc_teardown),
	.driver = {
		.name = "pm8058-xoadc",
		.owner = THIS_MODULE,
	},
};

static int __init pm8058_xoadc_init(void)
{
	return platform_driver_register(&pm8058_xoadc_driver);
}
module_init(pm8058_xoadc_init);

static void __exit pm8058_xoadc_exit(void)
{
	platform_driver_unregister(&pm8058_xoadc_driver);
}
module_exit(pm8058_xoadc_exit);

MODULE_ALIAS("platform:pmic8058_xoadc");
MODULE_DESCRIPTION("PMIC8058 XOADC driver");
MODULE_LICENSE("GPL v2");
