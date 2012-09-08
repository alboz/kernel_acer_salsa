/* arch/arm/mach-msm/generic_gpio.c
 *
 * Copyright (C) 2007 Google, Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <asm/gpio.h>
#include "gpio_chip.h"

#define GPIO_NUM_TO_CHIP_INDEX(gpio) ((gpio)>>5)

struct gpio_state {
	unsigned long flags;
	int refcount;
};

static DEFINE_SPINLOCK(gpio_chips_lock);
static LIST_HEAD(gpio_chip_list);
static struct salsa_gpio_chip **gpio_chip_array;
static unsigned long gpio_chip_array_size;

int register_gpio_chip(struct salsa_gpio_chip *new_gpio_chip)
{
	int err = 0;
	struct salsa_gpio_chip *gpio_chip;
	int i;
	unsigned long irq_flags;
	unsigned int chip_array_start_index, chip_array_end_index;

	new_gpio_chip->state = kzalloc((new_gpio_chip->end + 1 - new_gpio_chip->start) * sizeof(new_gpio_chip->state[0]), GFP_KERNEL);
	if (new_gpio_chip->state == NULL) {
		printk(KERN_ERR "register_gpio_chip: failed to allocate state\n");
		return -ENOMEM;
	}

	spin_lock_irqsave(&gpio_chips_lock, irq_flags);
	chip_array_start_index = GPIO_NUM_TO_CHIP_INDEX(new_gpio_chip->start);
	chip_array_end_index = GPIO_NUM_TO_CHIP_INDEX(new_gpio_chip->end);
	if (chip_array_end_index >= gpio_chip_array_size) {
		struct salsa_gpio_chip **new_gpio_chip_array;
		unsigned long new_gpio_chip_array_size = chip_array_end_index + 1;

		new_gpio_chip_array = kmalloc(new_gpio_chip_array_size * sizeof(new_gpio_chip_array[0]), GFP_ATOMIC);
		if (new_gpio_chip_array == NULL) {
			printk(KERN_ERR "register_gpio_chip: failed to allocate array\n");
			err = -ENOMEM;
			goto failed;
		}
		for (i = 0; i < gpio_chip_array_size; i++)
			new_gpio_chip_array[i] = gpio_chip_array[i];
		for (i = gpio_chip_array_size; i < new_gpio_chip_array_size; i++)
			new_gpio_chip_array[i] = NULL;
		gpio_chip_array = new_gpio_chip_array;
		gpio_chip_array_size = new_gpio_chip_array_size;
	}
	list_for_each_entry(gpio_chip, &gpio_chip_list, list) {
		if (gpio_chip->start > new_gpio_chip->end) {
			list_add_tail(&new_gpio_chip->list, &gpio_chip->list);
			goto added;
		}
		if (gpio_chip->end >= new_gpio_chip->start) {
			printk(KERN_ERR "register_gpio_source %u-%u overlaps with %u-%u\n",
			       new_gpio_chip->start, new_gpio_chip->end,
			       gpio_chip->start, gpio_chip->end);
			err = -EBUSY;
			goto failed;
		}
	}
	list_add_tail(&new_gpio_chip->list, &gpio_chip_list);
added:
	for (i = chip_array_start_index; i <= chip_array_end_index; i++) {
		if (gpio_chip_array[i] == NULL || gpio_chip_array[i]->start > new_gpio_chip->start)
			gpio_chip_array[i] = new_gpio_chip;
	}
failed:
	spin_unlock_irqrestore(&gpio_chips_lock, irq_flags);
	if (err)
		kfree(new_gpio_chip->state);
	return err;
}

static struct salsa_gpio_chip *get_gpio_chip_locked(unsigned int gpio)
{
	unsigned long i;
	struct salsa_gpio_chip *chip;

	i = GPIO_NUM_TO_CHIP_INDEX(gpio);
	if (i >= gpio_chip_array_size)
		return NULL;
	chip = gpio_chip_array[i];
	if (chip == NULL)
		return NULL;
	list_for_each_entry_from(chip, &gpio_chip_list, list) {
		if (gpio < chip->start)
			return NULL;
		if (gpio <= chip->end)
			return chip;
	}
	return NULL;
}

int gpio_configure(unsigned int gpio, unsigned long flags)
{
	int ret = -ENOTSUPP;
	struct salsa_gpio_chip *chip;
	unsigned long irq_flags;

	spin_lock_irqsave(&gpio_chips_lock, irq_flags);
	chip = get_gpio_chip_locked(gpio);
	if (chip && chip->configure)
		ret = chip->configure(chip, gpio, flags);
	spin_unlock_irqrestore(&gpio_chips_lock, irq_flags);
	return ret;
}
EXPORT_SYMBOL(gpio_configure);

int gpio_read_detect_status(unsigned int gpio)
{
	int ret = -ENOTSUPP;
	struct salsa_gpio_chip *chip;
	unsigned long irq_flags;

	spin_lock_irqsave(&gpio_chips_lock, irq_flags);
	chip = get_gpio_chip_locked(gpio);
	if (chip && chip->read_detect_status)
		ret = chip->read_detect_status(chip, gpio);
	spin_unlock_irqrestore(&gpio_chips_lock, irq_flags);
	return ret;
}
EXPORT_SYMBOL(gpio_read_detect_status);

int gpio_clear_detect_status(unsigned int gpio)
{
	int ret = -ENOTSUPP;
	struct salsa_gpio_chip *chip;
	unsigned long irq_flags;

	spin_lock_irqsave(&gpio_chips_lock, irq_flags);
	chip = get_gpio_chip_locked(gpio);
	if (chip && chip->clear_detect_status)
		ret = chip->clear_detect_status(chip, gpio);
	spin_unlock_irqrestore(&gpio_chips_lock, irq_flags);
	return ret;
}
EXPORT_SYMBOL(gpio_clear_detect_status);
