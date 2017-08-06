/*
 * Copyright (C) 2015 Balázs Triszka <balika011@protonmail.ch>
 * Copyright (C) 2016 Litew <litew9@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/platform_device.h>
#include <linux/input.h>
#include <linux/slab.h>
#include <linux/pwm.h>
#include <linux/time.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>

#define PWM_HAPTIC_PERIOD		44640
#define PWM_HAPTIC_DEFAULT_LEVEL	2
#define GPIO_ISA1000_EN		33
#define GPIO_HAPTIC_EN			50

struct isa1000_vib {
	struct input_dev *vib_input_dev;
	struct work_struct work;
	struct pwm_device *pwm;
	int gpio_isa1000_en;
	int gpio_haptic_en;
	int level;
	int speed;
	int pwm_channel;
	int pwm_duty_percent;
	unsigned int pwm_frequency;
	bool active;
};

static struct isa1000_vib vib_dev = {
	.pwm_frequency = 25000,
	.pwm_duty_percent = 80,
};


static int isa1000_vib_set(struct isa1000_vib *vib, bool on)
{
	if (on) {
		int rc;
		unsigned int pwm_period_ns = NSEC_PER_SEC / vib->pwm_frequency;

		rc = pwm_config(vib->pwm, (pwm_period_ns * vib->pwm_duty_percent) / 100, pwm_period_ns);
		if (rc < 0) {
			pr_err("Unable to config pwm\n");
			return rc;
		}

		rc = pwm_enable(vib->pwm);
		if (rc < 0) {
			pr_err("Unable to enable pwm\n");
			return rc;
		}

		gpio_set_value_cansleep(vib->gpio_isa1000_en, 1);
	} else {
		gpio_set_value_cansleep(vib->gpio_isa1000_en, 0);
		pwm_disable(vib->pwm);
	}

	return 0;
}

static void isa1000_work_handler(struct work_struct *work)
{
	struct isa1000_vib *vib = container_of(work, struct isa1000_vib, work);

	if (vib->speed) {
		vib->active = true;
		if (vib->speed > 100)
			vib->pwm_duty_percent = 100;
		else if (vib->speed < 50)
			vib->pwm_duty_percent = 50;
	} else {
		vib->active = false;
		vib->pwm_duty_percent = 50;
	}
	isa1000_vib_set(vib, vib->active);
}

static int isa1000_parse_dt(struct platform_device *pdev, struct isa1000_vib *vib)
{
	int ret;

	ret = of_get_named_gpio_flags(pdev->dev.of_node, "gpio-isa1000-en", 0, NULL);
	if (ret < 0) {
		dev_err(&pdev->dev, "please check enable gpio");
		return ret;
	}
	vib->gpio_isa1000_en = ret;

	ret = of_get_named_gpio_flags(pdev->dev.of_node, "gpio-haptic-en", 0, NULL);
	if (ret < 0) {
		dev_err(&pdev->dev, "please check enable gpio");
		return ret;
	}
	vib->gpio_haptic_en = ret;

	ret = of_property_read_u32(pdev->dev.of_node, "pwm-channel", &vib->pwm_channel);
	if (ret < 0)
		dev_err(&pdev->dev, "please check pwm output channel");

	dev_info(&pdev->dev, "gpio-isa1000-en: %i, gpio-haptic-en: %i, pwm-channel: %i", 
				vib->gpio_isa1000_en, vib->gpio_haptic_en, vib->pwm_channel);

	return 0;
}

static void isa1000_vib_close(struct input_dev *dev)
{
	struct isa1000_vib *vib = input_get_drvdata(dev);

	cancel_work_sync(&vib->work);

	if (vib->active)
		isa1000_vib_set(vib, false);
}


static int isa1000_vib_play_effect(struct input_dev *dev, void *data,
				  struct ff_effect *effect)
{
	struct isa1000_vib *vib = input_get_drvdata(dev);

	vib->speed = effect->u.rumble.strong_magnitude >> 8;
	if (!vib->speed)
		vib->speed = effect->u.rumble.weak_magnitude >> 9;

	schedule_work(&vib->work);

	return 0;
}

static int isa1000_probe(struct platform_device *pdev)
{
	struct isa1000_vib *vib;
	struct input_dev *input_dev;
	int ret;

	platform_set_drvdata(pdev, &vib_dev);
	vib = (struct isa1000_vib *) platform_get_drvdata(pdev);

	ret = isa1000_parse_dt(pdev, vib);
	if (ret < 0) {
		dev_err(&pdev->dev, "error occured while parsing dt\n");
	}

	ret = gpio_is_valid(vib->gpio_isa1000_en);
	if (ret) {
		ret = gpio_request(vib->gpio_isa1000_en, "gpio_isa1000_en");
		if (ret) {
			dev_err(&pdev->dev, "gpio %d request failed",vib->gpio_isa1000_en);
			return ret;
		}
	} else {
		dev_err(&pdev->dev, "invalid gpio %d\n", vib->gpio_isa1000_en);
		return ret;
	}

	ret = gpio_is_valid(vib->gpio_haptic_en);
	if (ret) {
		ret = gpio_request(vib->gpio_haptic_en, "gpio_haptic_en");
		if (ret) {
			dev_err(&pdev->dev, "gpio %d request failed\n", vib->gpio_haptic_en);
			return ret;
		}
	} else {
		dev_err(&pdev->dev, "invalid gpio %d\n", vib->gpio_haptic_en);
		return ret;
	}

	gpio_direction_output(vib->gpio_isa1000_en, 0);
	gpio_direction_output(vib->gpio_haptic_en, 1);

	vib->pwm = pwm_request(vib->pwm_channel, "isa1000");
	if (IS_ERR(vib->pwm)) {
		dev_err(&pdev->dev,"pwm request failed");
		return PTR_ERR(vib->pwm);
	}

	input_dev = input_allocate_device();
	if (!input_dev) {
		dev_err(&pdev->dev, "input device alloc failed\n");
		return -ENOMEM;
	}
	INIT_WORK(&vib->work, isa1000_work_handler);

	vib->vib_input_dev = input_dev;

	input_dev->name = "isa1000-ffmemless";
	input_dev->id.version = 1;
	input_dev->close = isa1000_vib_close;
	input_set_drvdata(input_dev, vib);
	input_set_capability(input_dev, EV_FF, FF_RUMBLE);

	ret = input_ff_create_memless(input_dev, NULL,
					isa1000_vib_play_effect);
	if (ret) {
		dev_err(&pdev->dev,
			"couldn't register vibrator as FF device\n");
		return ret;
	}

	ret = input_register_device(input_dev);
	if (ret) {
		dev_err(&pdev->dev, "couldn't register input device\n");
		return ret;
	}

	platform_set_drvdata(pdev, vib);
	return 0;
}

static int __maybe_unused isa1000_vib_suspend(struct device *dev)
{
	struct isa1000_vib *vib = dev_get_drvdata(dev);

	/* Turn off the vibrator */
	isa1000_vib_set(vib, false);

	return 0;
}

static SIMPLE_DEV_PM_OPS(isa1000_vib_pm_ops, isa1000_vib_suspend, NULL);

static struct of_device_id isa1000_vib_id_table[] = {
	{ .compatible = "imagis,isa1000", },
	{ },
};

static struct platform_driver isa1000_driver = {
	.probe		= isa1000_probe,
	.driver		= {
		.name	= "isa1000-ffmemless",
		.owner = THIS_MODULE,
		.pm		= &isa1000_vib_pm_ops,
		.of_match_table = isa1000_vib_id_table,
	},
};

static int __init isa1000_init(void)
{
	return platform_driver_register(&isa1000_driver);
}
module_init(isa1000_init);

static void __exit isa1000_exit(void)
{
	return platform_driver_unregister(&isa1000_driver);
}
module_exit(isa1000_exit);

MODULE_ALIAS("platform:isa1000_vib");
MODULE_DESCRIPTION("ISA1000 driver based on ffmemless framework.");
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Balázs Triszka <balika011@protonmail.ch>");
