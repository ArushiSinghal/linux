/*
 * Copyright (C) STMicroelectronics 2016
 *
 * Author: Gerald Baeza <gerald.baeza@st.com>
 * License terms: GNU General Public License (GPL), version 2
 *
 * Inspired by timer-stm32.c from Maxime Coquelin
 *             pwm-atmel.c from Bo Shen
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pwm.h>
#include <linux/of.h>

#include <linux/mfd/stm32-gptimer.h>

#define CCMR_CHANNEL_SHIFT 8
#define CCMR_CHANNEL_MASK  0xFF

struct stm32_pwm {
	struct pwm_chip chip;
	struct device *dev;
	struct clk *clk;
	struct regmap *regmap;
	unsigned int caps;
	unsigned int npwm;
	u32 breakinput_polarity;
	u32 max_arr;
	bool have_complementary_output;
	bool have_breakinput;
	bool use_breakinput;
};

#define to_stm32_pwm_dev(x) container_of(chip, struct stm32_pwm, chip)

static u32 active_channels(struct stm32_pwm *dev)
{
	u32 ccer;

	regmap_read(dev->regmap, TIM_CCER, &ccer);

	return ccer & TIM_CCER_CCXE;
}

static int write_ccrx(struct stm32_pwm *dev, struct pwm_device *pwm,
		      u32 value)
{
	switch (pwm->hwpwm) {
	case 0:
		return regmap_write(dev->regmap, TIM_CCR1, value);
	case 1:
		return regmap_write(dev->regmap, TIM_CCR2, value);
	case 2:
		return regmap_write(dev->regmap, TIM_CCR3, value);
	case 3:
		return regmap_write(dev->regmap, TIM_CCR4, value);
	}
	return -EINVAL;
}

static int stm32_pwm_config(struct pwm_chip *chip, struct pwm_device *pwm,
			    int duty_ns, int period_ns)
{
	struct stm32_pwm *priv = to_stm32_pwm_dev(chip);
	unsigned long long prd, div, dty;
	unsigned int prescaler = 0;
	u32 ccmr, mask, shift, bdtr;

	/* Period and prescaler values depends on clock rate */
	div = (unsigned long long)clk_get_rate(priv->clk) * period_ns;

	do_div(div, NSEC_PER_SEC);
	prd = div;

	while (div > priv->max_arr) {
		prescaler++;
		div = prd;
		do_div(div, (prescaler + 1));
	}

	prd = div;

	if (prescaler > MAX_TIM_PSC) {
		dev_err(chip->dev, "prescaler exceeds the maximum value\n");
		return -EINVAL;
	}

	/*
	 * All channels share the same prescaler and counter so when two
	 * channels are active at the same we can't change them
	 */
	if (active_channels(priv) & ~(1 << pwm->hwpwm * 4)) {
		u32 psc, arr;

		regmap_read(priv->regmap, TIM_PSC, &psc);
		regmap_read(priv->regmap, TIM_ARR, &arr);

		if ((psc != prescaler) || (arr != prd - 1))
			return -EBUSY;
	}

	regmap_write(priv->regmap, TIM_PSC, prescaler);
	regmap_write(priv->regmap, TIM_ARR, prd - 1);
	regmap_update_bits(priv->regmap, TIM_CR1, TIM_CR1_ARPE, TIM_CR1_ARPE);

	/* Calculate the duty cycles */
	dty = prd * duty_ns;
	do_div(dty, period_ns);

	write_ccrx(priv, pwm, dty);

	/* Configure output mode */
	shift = (pwm->hwpwm & 0x1) * CCMR_CHANNEL_SHIFT;
	ccmr = (TIM_CCMR_PE | TIM_CCMR_M1) << shift;
	mask = CCMR_CHANNEL_MASK << shift;

	if (pwm->hwpwm < 2)
		regmap_update_bits(priv->regmap, TIM_CCMR1, mask, ccmr);
	else
		regmap_update_bits(priv->regmap, TIM_CCMR2, mask, ccmr);

	if (!priv->have_breakinput)
		return 0;

	bdtr = TIM_BDTR_MOE | TIM_BDTR_AOE;

	if (priv->use_breakinput)
		bdtr |= TIM_BDTR_BKE;

	if (priv->breakinput_polarity)
		bdtr |= TIM_BDTR_BKP;

	regmap_update_bits(priv->regmap, TIM_BDTR,
			   TIM_BDTR_MOE | TIM_BDTR_AOE |
			   TIM_BDTR_BKP | TIM_BDTR_BKE,
			   bdtr);

	return 0;
}

static int stm32_pwm_set_polarity(struct pwm_chip *chip, struct pwm_device *pwm,
				  enum pwm_polarity polarity)
{
	u32 mask;
	struct stm32_pwm *priv = to_stm32_pwm_dev(chip);

	mask = TIM_CCER_CC1P << (pwm->hwpwm * 4);
	if (priv->have_complementary_output)
		mask |= TIM_CCER_CC1NP << (pwm->hwpwm * 4);

	regmap_update_bits(priv->regmap, TIM_CCER, mask,
			   polarity == PWM_POLARITY_NORMAL ? 0 : mask);

	return 0;
}

static int stm32_pwm_enable(struct pwm_chip *chip, struct pwm_device *pwm)
{
	u32 mask;
	struct stm32_pwm *priv = to_stm32_pwm_dev(chip);

	clk_enable(priv->clk);

	/* Enable channel */
	mask = TIM_CCER_CC1E << (pwm->hwpwm * 4);
	if (priv->have_complementary_output)
		mask |= TIM_CCER_CC1NE << (pwm->hwpwm * 4);

	regmap_update_bits(priv->regmap, TIM_CCER, mask, mask);

	/* Make sure that registers are updated */
	regmap_update_bits(priv->regmap, TIM_EGR, TIM_EGR_UG, TIM_EGR_UG);

	/* Enable controller */
	regmap_update_bits(priv->regmap, TIM_CR1, TIM_CR1_CEN, TIM_CR1_CEN);

	return 0;
}

static void stm32_pwm_disable(struct pwm_chip *chip, struct pwm_device *pwm)
{
	u32 mask;
	struct stm32_pwm *priv = to_stm32_pwm_dev(chip);

	/* Disable channel */
	mask = TIM_CCER_CC1E << (pwm->hwpwm * 4);
	if (priv->have_complementary_output)
		mask |= TIM_CCER_CC1NE << (pwm->hwpwm * 4);

	regmap_update_bits(priv->regmap, TIM_CCER, mask, 0);

	/* When all channels are disabled, we can disable the controller */
	if (!active_channels(priv))
		regmap_update_bits(priv->regmap, TIM_CR1, TIM_CR1_CEN, 0);

	clk_disable(priv->clk);
}

static int stm32_pwm_apply(struct pwm_chip *chip, struct pwm_device *pwm,
			   struct pwm_state *state)
{
	struct pwm_state curstate;
	bool enabled;
	int ret;

	pwm_get_state(pwm, &curstate);
	enabled = curstate.enabled;

	if (enabled && !state->enabled) {
		stm32_pwm_disable(chip, pwm);
		return 0;
	}

	if (state->polarity != curstate.polarity && enabled)
		stm32_pwm_set_polarity(chip, pwm, state->polarity);

	ret = stm32_pwm_config(chip, pwm, state->duty_cycle, state->period);
	if (ret)
		return ret;

	if (!enabled && state->enabled)
		ret = stm32_pwm_enable(chip, pwm);

	return ret;
}

static const struct pwm_ops stm32pwm_ops = {
	.owner = THIS_MODULE,
	.apply = stm32_pwm_apply,
};

static bool stm32_pwm_detect_breakinput(struct regmap *regmap)
{
	u32 bdtr;

	/*
	 * If breakinput enable bit doesn't exist writing 1 will have no
	 * effect so we can detect it.
	 */
	regmap_update_bits(regmap, TIM_BDTR, TIM_BDTR_BKE, TIM_BDTR_BKE);
	regmap_read(regmap, TIM_BDTR, &bdtr);
	regmap_update_bits(regmap, TIM_BDTR, TIM_BDTR_BKE, 0);

	return (bdtr != 0);
}

static bool stm32_pwm_detect_complementary(struct regmap *regmap)
{
	u32 ccer;

	/*
	 * If complementary bit doesn't exist writing 1 will have no
	 * effect so we can detect it.
	 */
	regmap_update_bits(regmap, TIM_CCER, TIM_CCER_CC1NE, TIM_CCER_CC1NE);
	regmap_read(regmap, TIM_CCER, &ccer);
	regmap_update_bits(regmap, TIM_CCER, TIM_CCER_CCXE, 0);

	return (ccer != 0);
}

static int stm32_pwm_detect_channels(struct regmap *regmap)
{
	int channels = 0;
	u32 ccer;

	/*
	 * If channels enable bits don't exist writing 1 will have no
	 * effect so we can detect and count them.
	 */
	regmap_update_bits(regmap, TIM_CCER, TIM_CCER_CCXE, TIM_CCER_CCXE);
	regmap_read(regmap, TIM_CCER, &ccer);
	regmap_update_bits(regmap, TIM_CCER, TIM_CCER_CCXE, 0);

	if (ccer & TIM_CCER_CC1E)
		channels++;

	if (ccer & TIM_CCER_CC2E)
		channels++;

	if (ccer & TIM_CCER_CC3E)
		channels++;

	if (ccer & TIM_CCER_CC4E)
		channels++;

	return channels;
}

static int stm32_pwm_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct stm32_gptimer *ddata = dev_get_drvdata(pdev->dev.parent);
	struct stm32_pwm *priv;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->regmap = ddata->regmap;
	priv->clk = ddata->clk;
	priv->max_arr = ddata->max_arr;

	if (!priv->regmap || !priv->clk)
		return -EINVAL;

	priv->have_breakinput = stm32_pwm_detect_breakinput(priv->regmap);
	priv->have_complementary_output =
		stm32_pwm_detect_complementary(priv->regmap);
	priv->npwm = stm32_pwm_detect_channels(priv->regmap);

	if (!of_property_read_u32(np, "st,breakinput-polarity",
				  &priv->breakinput_polarity))
		priv->use_breakinput = true;

	priv->chip.base = -1;
	priv->chip.dev = dev;
	priv->chip.ops = &stm32pwm_ops;
	priv->chip.npwm = priv->npwm;

	ret = pwmchip_add(&priv->chip);
	if (ret < 0)
		return ret;

	platform_set_drvdata(pdev, priv);

	return 0;
}

static int stm32_pwm_remove(struct platform_device *pdev)
{
	struct stm32_pwm *priv = platform_get_drvdata(pdev);
	unsigned int i;

	for (i = 0; i < priv->npwm; i++)
		pwm_disable(&priv->chip.pwms[i]);

	pwmchip_remove(&priv->chip);

	return 0;
}

static const struct of_device_id stm32_pwm_of_match[] = {
	{ .compatible = "st,stm32-pwm",	}
};
MODULE_DEVICE_TABLE(of, stm32_pwm_of_match);

static struct platform_driver stm32_pwm_driver = {
	.probe	= stm32_pwm_probe,
	.remove	= stm32_pwm_remove,
	.driver	= {
		.name = "stm32-pwm",
		.of_match_table = stm32_pwm_of_match,
	},
};
module_platform_driver(stm32_pwm_driver);

MODULE_ALIAS("platform: stm32-pwm");
MODULE_DESCRIPTION("STMicroelectronics STM32 PWM driver");
MODULE_LICENSE("GPL v2");
