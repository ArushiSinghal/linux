/*
 * Copyright © 2015 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Authors:
 * Shashank Sharma <shashank.sharma@intel.com>
 * Kausal Malladi <Kausal.Malladi@intel.com>
 */

#include "intel_color_manager.h"

static void bdw_write_8bit_gamma_legacy(struct drm_device *dev,
	struct drm_r32g32b32 *correction_values, u32 palette)
{
	u16 blue_fract, green_fract, red_fract;
	u32 blue, green, red;
	u32 count = 0;
	u32 word = 0;
	struct drm_i915_private *dev_priv = dev->dev_private;

	while (count < BDW_8BIT_GAMMA_MAX_VALS) {
		blue = correction_values[count].b32;
		green = correction_values[count].g32;
		red = correction_values[count].r32;

		/*
		* Maximum possible gamma correction value supported
		* for BDW is 0xFFFFFFFF, so clamp the values accordingly
		*/
		if (blue >= BDW_MAX_GAMMA)
			blue = BDW_MAX_GAMMA;
		if (green >= BDW_MAX_GAMMA)
			green = BDW_MAX_GAMMA;
		if (red >= BDW_MAX_GAMMA)
			red = BDW_MAX_GAMMA;

		blue_fract = GET_BITS(blue, 16, 8);
		green_fract = GET_BITS(green, 16, 8);
		red_fract = GET_BITS(red, 16, 8);

		/* Blue (7:0) Green (15:8) and Red (23:16) */
		SET_BITS(word, blue_fract, 0, 8);
		SET_BITS(word, green_fract, 8, 8);
		SET_BITS(word, blue_fract, 16, 8);
		I915_WRITE(palette, word);
		palette += 4;
		count++;
	}
}

static void bdw_write_10bit_gamma_precision(struct drm_device *dev,
	struct drm_r32g32b32 *correction_values, u32 pal_prec_data,
			u32 no_of_coeff)
{
	u16 blue_fract, green_fract, red_fract;
	u32 word = 0;
	u32 count = 0;
	u32 blue, green, red;
	struct drm_i915_private *dev_priv = dev->dev_private;

	while (count < no_of_coeff) {

		blue = correction_values[count].b32;
		green = correction_values[count].g32;
		red = correction_values[count].r32;

		/*
		* Maximum possible gamma correction value supported
		* for BDW is 0xFFFFFFFF, so clamp the values accordingly
		*/
		if (blue >= BDW_MAX_GAMMA)
			blue = BDW_MAX_GAMMA;
		if (green >= BDW_MAX_GAMMA)
			green = BDW_MAX_GAMMA;
		if (red >= BDW_MAX_GAMMA)
			red = BDW_MAX_GAMMA;

		/*
		* Gamma correction values are sent in 8.24 format
		* with 8 int and 24 fraction bits. BDW 10 bit gamma
		* unit expects correction registers to be programmed in
		* 0.10 format, with 0 int and 16 fraction bits. So take
		* MSB 10 bit values(bits 23-14) from the fraction part and
		* prepare the correction registers.
		*/
		blue_fract = GET_BITS(blue, 14, 10);
		green_fract = GET_BITS(green, 14, 10);
		red_fract = GET_BITS(red, 14, 10);

		/* Arrange: Red (29:20) Green (19:10) and Blue (9:0) */
		SET_BITS(word, red_fract, 20, 10);
		SET_BITS(word, green_fract, 10, 10);
		SET_BITS(word, blue_fract, 0, 10);
		I915_WRITE(pal_prec_data, word);
		count++;
	}
	DRM_DEBUG_DRIVER("Gamma correction programmed\n");
}

static void bdw_write_12bit_gamma_precision(struct drm_device *dev,
	struct drm_r32g32b32 *correction_values, u32 pal_prec_data,
		enum pipe pipe)
{
	uint16_t blue_fract, green_fract, red_fract;
	uint32_t gcmax;
	uint32_t word = 0;
	uint32_t count = 0;
	uint32_t gcmax_reg;
	u32 blue, green, red;
	struct drm_i915_private *dev_priv = dev->dev_private;

	/* Program first 512 values in precision palette */
	while (count < BDW_12BIT_GAMMA_MAX_VALS - 1) {

		blue = correction_values[count].b32;
		green = correction_values[count].g32;
		red = correction_values[count].r32;

		/*
		* Maximum possible gamma correction value supported
		* for BDW is 0xFFFFFFFF, so clamp the values accordingly
		*/
		if (blue >= BDW_MAX_GAMMA)
			blue = BDW_MAX_GAMMA;
		if (green >= BDW_MAX_GAMMA)
			green = BDW_MAX_GAMMA;
		if (red >= BDW_MAX_GAMMA)
			red = BDW_MAX_GAMMA;

		/*
		* Framework's general gamma format is 8.24 (8 int 16 fraction)
		* BDW Platform's supported gamma format is 16 bit correction
		* values in 0.16 format. So extract higher 16 fraction bits
		* from 8.24 gamma correction values.
		*/
		red_fract = GET_BITS(red, 8, 16);
		green_fract = GET_BITS(green, 8, 16);
		blue_fract = GET_BITS(blue, 8, 16);

		/*
		* From the bspec:
		* For 12 bit gamma correction, program precision palette
		* with 16 bits per color in a 0.16 format with 0 integer and
		* 16 fractional bits (upper 10 bits in odd indexes, lower 6
		* bits in even indexes)
		*/

		/* Even index: Lower 6 bits from correction should go as MSB */
		SET_BITS(word, GET_BITS(red_fract, 0, 6), 24, 6);
		SET_BITS(word, GET_BITS(green_fract, 0, 6), 14, 6);
		SET_BITS(word, GET_BITS(blue_fract, 0, 6), 4, 6);
		I915_WRITE(pal_prec_data, word);

		word = 0x0;
		/* Odd index: Upper 10 bits of correction should go as MSB */
		SET_BITS(word, GET_BITS(red_fract, 6, 10), 20, 10);
		SET_BITS(word, GET_BITS(green_fract, 6, 10), 10, 10);
		SET_BITS(word, GET_BITS(blue_fract, 6, 10), 0, 10);

		I915_WRITE(pal_prec_data, word);
		count++;
	}

	/* Now program the 513th value in GCMAX regs */
	word = 0;
	gcmax_reg = _PREC_PAL_GCMAX(pipe);
	gcmax = min_t(u32, GET_BITS(correction_values[count].r32, 8, 17),
				BDW_MAX_GAMMA);
	SET_BITS(word, gcmax, 0, 17);
	I915_WRITE(gcmax_reg, word);
	gcmax_reg += 4;

	word = 0;
	gcmax = min_t(u32, GET_BITS(correction_values[count].g32, 8, 17),
				BDW_MAX_GAMMA);
	SET_BITS(word, gcmax, 0, 17);
	I915_WRITE(gcmax_reg, word);
	gcmax_reg += 4;

	word = 0;
	gcmax = min_t(u32, GET_BITS(correction_values[count].b32, 8, 17),
				BDW_MAX_GAMMA);
	SET_BITS(word, gcmax, 0, 17);
	I915_WRITE(gcmax_reg, word);
}

/* Apply unity gamma for gamma reset */
static void bdw_reset_gamma(struct drm_i915_private *dev_priv,
			enum pipe pipe)
{
	u16 count = 0;
	u32 val;
	u32 pal_prec_data = LGC_PALETTE(pipe, 0);

	DRM_DEBUG_DRIVER("\n");

	/* Reset the palette for unit gamma */
	while (count < BDW_8BIT_GAMMA_MAX_VALS) {
		/* Red (23:16) Green (15:8) and Blue (7:0) */
		val = (count << 16) | (count << 8) | count;
		I915_WRITE(pal_prec_data, val);
		pal_prec_data += 4;
		count++;
	}
}

static int bdw_set_gamma(struct drm_device *dev, struct drm_property_blob *blob,
			struct drm_crtc *crtc)
{
	enum pipe pipe;
	int num_samples;
	u32 mode, pal_prec_index, pal_prec_data, index;
	u32 word = 0;
	struct drm_palette *gamma_data;
	struct drm_crtc_state *state = crtc->state;
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct drm_r32g32b32 *correction_values = NULL;

	if (WARN_ON(!blob))
		return -EINVAL;

	gamma_data = (struct drm_palette *)blob->data;
	pipe = to_intel_crtc(crtc)->pipe;
	num_samples = gamma_data->num_samples;

	pal_prec_index = _PREC_PAL_INDEX(pipe);
	pal_prec_data = _PREC_PAL_DATA(pipe);

	correction_values = (struct drm_r32g32b32 *)&gamma_data->lut;
	index = I915_READ(pal_prec_index);

	switch (num_samples) {
	case GAMMA_DISABLE_VALS:

		/* Disable Gamma functionality on Pipe */
		DRM_DEBUG_DRIVER("Disabling gamma on Pipe %c\n",
			pipe_name(pipe));
		mode = I915_READ(GAMMA_MODE(pipe));
		if ((mode & GAMMA_MODE_MODE_MASK) == GAMMA_MODE_MODE_12BIT)
			bdw_reset_gamma(dev_priv, pipe);
		state->palette_after_ctm_blob = NULL;
		word = GAMMA_MODE_MODE_8BIT;
		break;

	case BDW_8BIT_GAMMA_MAX_VALS:

		/* Legacy palette */
		bdw_write_8bit_gamma_legacy(dev, correction_values,
				LGC_PALETTE(pipe, 0));
		word = GAMMA_MODE_MODE_8BIT;
		break;

	case BDW_SPLITGAMMA_MAX_VALS:

		index |= BDW_INDEX_AUTO_INCREMENT | BDW_INDEX_SPLIT_MODE;
		I915_WRITE(pal_prec_index, index);
		bdw_write_10bit_gamma_precision(dev, correction_values,
			pal_prec_data, BDW_SPLITGAMMA_MAX_VALS);
		word = GAMMA_MODE_MODE_SPLIT;
		break;

	case BDW_12BIT_GAMMA_MAX_VALS:

		index |= BDW_INDEX_AUTO_INCREMENT;
		index &= ~BDW_INDEX_SPLIT_MODE;
		I915_WRITE(pal_prec_index, index);
		bdw_write_12bit_gamma_precision(dev, correction_values,
			pal_prec_data, pipe);
		word = GAMMA_MODE_MODE_12BIT;
		break;

	case BDW_10BIT_GAMMA_MAX_VALS:
		index |= BDW_INDEX_AUTO_INCREMENT;
		index &= ~BDW_INDEX_SPLIT_MODE;
		I915_WRITE(pal_prec_index, index);
		bdw_write_10bit_gamma_precision(dev, correction_values,
			pal_prec_data, BDW_10BIT_GAMMA_MAX_VALS);
		word = GAMMA_MODE_MODE_10BIT;
		break;

	default:
		DRM_ERROR("Invalid number of samples\n");
		return -EINVAL;
	}

	/* Set gamma mode on pipe control reg */
	mode = I915_READ(GAMMA_MODE(pipe));
	mode &= ~GAMMA_MODE_MODE_MASK;
	I915_WRITE(GAMMA_MODE(pipe), mode | word);
	DRM_DEBUG_DRIVER("Gamma applied on pipe %c\n",
		pipe_name(pipe));
	return 0;
}

static int bdw_set_degamma(struct drm_device *dev,
	struct drm_property_blob *blob, struct drm_crtc *crtc)
{
	enum pipe pipe;
	int num_samples;
	u32 index, mode;
	u32 pal_prec_index, pal_prec_data;
	struct drm_palette *degamma_data;
	struct drm_crtc_state *state = crtc->state;
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct drm_r32g32b32 *correction_values = NULL;

	if (WARN_ON(!blob))
		return -EINVAL;

	degamma_data = (struct drm_palette *)blob->data;
	pipe = to_intel_crtc(crtc)->pipe;
	num_samples = degamma_data->num_samples;

	if (num_samples == GAMMA_DISABLE_VALS) {
		/* Disable degamma on Pipe */
		mode = I915_READ(GAMMA_MODE(pipe)) & ~GAMMA_MODE_MODE_MASK;
		I915_WRITE(GAMMA_MODE(pipe), mode | GAMMA_MODE_MODE_8BIT);

		state->palette_before_ctm_blob = NULL;
		DRM_DEBUG_DRIVER("Disabling degamma on Pipe %c\n",
			pipe_name(pipe));
		return 0;
	}

	if (num_samples != BDW_SPLITGAMMA_MAX_VALS) {
		DRM_ERROR("Invalid number of samples\n");
		return -EINVAL;
	}

	pal_prec_index = _PREC_PAL_INDEX(pipe);
	pal_prec_data = _PREC_PAL_DATA(pipe);
	correction_values = degamma_data->lut;

	index = I915_READ(pal_prec_index);
	index |= BDW_INDEX_AUTO_INCREMENT | BDW_INDEX_SPLIT_MODE;
	I915_WRITE(pal_prec_index, index);

	bdw_write_10bit_gamma_precision(dev, correction_values,
	pal_prec_data, BDW_SPLITGAMMA_MAX_VALS);

	/* Enable degamma on Pipe */
	mode = I915_READ(GAMMA_MODE(pipe));
	mode &= ~GAMMA_MODE_MODE_MASK;
	I915_WRITE(GAMMA_MODE(pipe), mode | GAMMA_MODE_MODE_SPLIT);
	DRM_DEBUG_DRIVER("degamma correction enabled on Pipe %c\n",
		pipe_name(pipe));

	return 0;
}

static s32 chv_prepare_csc_coeff(s64 csc_value)
{
	s32 csc_int_value;
	u32 csc_fract_value;
	s32 csc_s3_12_format;

	if (csc_value >= 0) {
		csc_value += CHV_CSC_FRACT_ROUNDOFF;
		if (csc_value > CHV_CSC_COEFF_MAX)
			csc_value = CHV_CSC_COEFF_MAX;
	} else {
		csc_value = -csc_value;
		csc_value += CHV_CSC_FRACT_ROUNDOFF;
		if (csc_value > CHV_CSC_COEFF_MAX + 1)
			csc_value = CHV_CSC_COEFF_MAX + 1;
		csc_value = -csc_value;
	}

	csc_int_value = csc_value >> CHV_CSC_COEFF_SHIFT;
	csc_int_value <<= CHV_CSC_COEFF_INT_SHIFT;
	if (csc_value < 0)
		csc_int_value |= CSC_COEFF_SIGN;

	csc_fract_value = csc_value;
	csc_fract_value >>= CHV_CSC_COEFF_FRACT_SHIFT;
	csc_s3_12_format = csc_int_value | csc_fract_value;

	return csc_s3_12_format;
}

static int chv_set_csc(struct drm_device *dev, struct drm_property_blob *blob,
		struct drm_crtc *crtc)
{
	struct drm_ctm *csc_data;
	struct drm_i915_private *dev_priv = dev->dev_private;
	u32 reg;
	enum pipe pipe;
	s32 word = 0, temp;
	int count = 0;

	if (WARN_ON(!blob))
		return -EINVAL;

	if (blob->length != sizeof(struct drm_ctm)) {
		DRM_ERROR("Invalid length of data received\n");
		return -EINVAL;
	}

	csc_data = (struct drm_ctm *)blob->data;
	pipe = to_intel_crtc(crtc)->pipe;

	/* Disable CSC functionality */
	reg = _PIPE_CGM_CONTROL(pipe);
	I915_WRITE(reg, I915_READ(reg) & (~CGM_CSC_EN));

	DRM_DEBUG_DRIVER("Disabled CSC Functionality on Pipe %c\n",
			pipe_name(pipe));

	reg = _PIPE_CSC_BASE(pipe);

	/*
	* First 8 of 9 CSC correction values go in pair, to first
	* 4 CSC register (bit 0:15 and 16:31)
	*/
	while (count < CSC_MAX_VALS - 1) {
		temp = chv_prepare_csc_coeff(
					csc_data->ctm_coeff[count]);
		SET_BITS(word, GET_BITS(temp, 16, 16), 0, 16);
		count++;

		temp = chv_prepare_csc_coeff(
				csc_data->ctm_coeff[count]);
		SET_BITS(word, GET_BITS(temp, 16, 16), 16, 16);
		count++;

		I915_WRITE(reg, word);
		reg += 4;
	}

	/* 9th coeff goes to 5th register, bit 0:16 */
	temp = chv_prepare_csc_coeff(
			csc_data->ctm_coeff[count]);
	SET_BITS(word, GET_BITS(temp, 16, 16), 0, 16);
	I915_WRITE(reg, word);

	/* Enable CSC functionality */
	reg = _PIPE_CGM_CONTROL(pipe);
	I915_WRITE(reg, I915_READ(reg) | CGM_CSC_EN);
	DRM_DEBUG_DRIVER("CSC enabled on Pipe %c\n", pipe_name(pipe));
	return 0;
}

static int chv_set_degamma(struct drm_device *dev,
	struct drm_property_blob *blob, struct drm_crtc *crtc)
{
	u16 red_fract, green_fract, blue_fract;
	u32 red, green, blue;
	u32 num_samples;
	u32 word = 0;
	u32 count, cgm_control_reg, cgm_degamma_reg;
	u64 length;
	enum pipe pipe;
	struct drm_palette *degamma_data;
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct drm_r32g32b32 *correction_values = NULL;
	struct drm_crtc_state *state = crtc->state;

	if (WARN_ON(!blob))
		return -EINVAL;

	degamma_data = (struct drm_palette *)blob->data;
	pipe = to_intel_crtc(crtc)->pipe;
	num_samples = degamma_data->num_samples;
	length = num_samples * sizeof(struct drm_r32g32b32);

	if (num_samples == GAMMA_DISABLE_VALS) {
		/* Disable DeGamma functionality on Pipe - CGM Block */
		cgm_control_reg = I915_READ(_PIPE_CGM_CONTROL(pipe));
		cgm_control_reg &= ~CGM_DEGAMMA_EN;
		state->palette_before_ctm_blob = NULL;

		I915_WRITE(_PIPE_CGM_CONTROL(pipe), cgm_control_reg);
		DRM_DEBUG_DRIVER("DeGamma disabled on Pipe %c\n",
				pipe_name(pipe));
		return 0;
	} else if (num_samples == CHV_DEGAMMA_MAX_VALS) {
		cgm_degamma_reg = _PIPE_DEGAMMA_BASE(pipe);

		count = 0;
		correction_values = (struct drm_r32g32b32 *)&degamma_data->lut;
		while (count < CHV_DEGAMMA_MAX_VALS) {
			blue = correction_values[count].b32;
			green = correction_values[count].g32;
			red = correction_values[count].r32;

			if (blue > CHV_MAX_GAMMA)
				blue = CHV_MAX_GAMMA;

			if (green > CHV_MAX_GAMMA)
				green = CHV_MAX_GAMMA;

			if (red > CHV_MAX_GAMMA)
				red = CHV_MAX_GAMMA;

			blue_fract = GET_BITS(blue, 8, 14);
			green_fract = GET_BITS(green, 8, 14);
			red_fract = GET_BITS(red, 8, 14);

			/* Green (29:16) and Blue (13:0) in DWORD1 */
			SET_BITS(word, green_fract, 16, 14);
			SET_BITS(word, green_fract, 0, 14);
			I915_WRITE(cgm_degamma_reg, word);
			cgm_degamma_reg += 4;

			/* Red (13:0) to be written to DWORD2 */
			word = red_fract;
			I915_WRITE(cgm_degamma_reg, word);
			cgm_degamma_reg += 4;
			count++;
		}

		DRM_DEBUG_DRIVER("DeGamma LUT loaded for Pipe %c\n",
				pipe_name(pipe));

		/* Enable DeGamma on Pipe */
		I915_WRITE(_PIPE_CGM_CONTROL(pipe),
			I915_READ(_PIPE_CGM_CONTROL(pipe)) | CGM_DEGAMMA_EN);

		DRM_DEBUG_DRIVER("DeGamma correction enabled on Pipe %c\n",
				pipe_name(pipe));
		return 0;
	} else {
		DRM_ERROR("Invalid number of samples for DeGamma LUT\n");
		return -EINVAL;
	}
}

static int chv_set_gamma(struct drm_device *dev, struct drm_property_blob *blob,
		struct drm_crtc *crtc)
{
	enum pipe pipe;
	u16 red_fract, green_fract, blue_fract;
	u32 red, green, blue, num_samples;
	u32 word = 0;
	u32 count, cgm_gamma_reg, cgm_control_reg;
	u64 length;
	struct drm_r32g32b32 *correction_values;
	struct drm_palette *gamma_data;
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct drm_crtc_state *state = crtc->state;

	if (WARN_ON(!blob))
		return -EINVAL;

	gamma_data = (struct drm_palette *)blob->data;
	pipe = to_intel_crtc(crtc)->pipe;
	num_samples = gamma_data->num_samples;
	length = num_samples * sizeof(struct drm_r32g32b32);

	switch (num_samples) {
	case GAMMA_DISABLE_VALS:

		/* Disable Gamma functionality on Pipe - CGM Block */
		cgm_control_reg = I915_READ(_PIPE_CGM_CONTROL(pipe));
		cgm_control_reg &= ~CGM_GAMMA_EN;
		I915_WRITE(_PIPE_CGM_CONTROL(pipe), cgm_control_reg);
		state->palette_after_ctm_blob = NULL;
		DRM_DEBUG_DRIVER("Gamma disabled on Pipe %c\n",
			pipe_name(pipe));
		return 0;

	case CHV_8BIT_GAMMA_MAX_VALS:
	case CHV_10BIT_GAMMA_MAX_VALS:

		count = 0;
		cgm_gamma_reg = _PIPE_GAMMA_BASE(pipe);
		correction_values = gamma_data->lut;

		while (count < num_samples) {
			blue = correction_values[count].b32;
			green = correction_values[count].g32;
			red = correction_values[count].r32;

			if (blue > CHV_MAX_GAMMA)
				blue = CHV_MAX_GAMMA;

			if (green > CHV_MAX_GAMMA)
				green = CHV_MAX_GAMMA;

			if (red > CHV_MAX_GAMMA)
				red = CHV_MAX_GAMMA;

			/* get MSB 10 bits from fraction part (14:23) */
			blue_fract = GET_BITS(blue, 14, 10);
			green_fract = GET_BITS(green, 14, 10);
			red_fract = GET_BITS(red, 14, 10);

			/* Green (25:16) and Blue (9:0) to be written */
			SET_BITS(word, green_fract, 16, 10);
			SET_BITS(word, blue_fract, 0, 10);
			I915_WRITE(cgm_gamma_reg, word);
			cgm_gamma_reg += 4;

			/* Red (9:0) to be written */
			word = red_fract;
			I915_WRITE(cgm_gamma_reg, word);

			cgm_gamma_reg += 4;
			count++;
		}

		/* Enable (CGM) Gamma on Pipe */
		I915_WRITE(_PIPE_CGM_CONTROL(pipe),
		I915_READ(_PIPE_CGM_CONTROL(pipe)) | CGM_GAMMA_EN);
		DRM_DEBUG_DRIVER("CGM Gamma enabled on Pipe %c\n",
			pipe_name(pipe));
		return 0;

	default:
		DRM_ERROR("Invalid number of samples (%u) for Gamma LUT\n",
				num_samples);
		return -EINVAL;
	}
}

void intel_color_manager_crtc_commit(struct drm_device *dev,
		struct drm_crtc_state *crtc_state)
{
	struct drm_property_blob *blob;
	struct drm_crtc *crtc = crtc_state->crtc;
	int ret = -EINVAL;

	blob = crtc_state->palette_after_ctm_blob;
	if (blob) {
		/* Gamma correction is platform specific */
		if (IS_CHERRYVIEW(dev))
			ret = chv_set_gamma(dev, blob, crtc);
		else if (IS_BROADWELL(dev) || IS_GEN9(dev))
			ret = bdw_set_gamma(dev, blob, crtc);

		if (ret)
			DRM_ERROR("set Gamma correction failed\n");
		else
			DRM_DEBUG_DRIVER("Gamma correction success\n");
	}

	blob = crtc_state->palette_before_ctm_blob;
	if (blob) {
		/* Degamma correction */
		if (IS_CHERRYVIEW(dev))
			ret = chv_set_degamma(dev, blob, crtc);
		else if (IS_BROADWELL(dev) || IS_GEN9(dev))
			ret = bdw_set_degamma(dev, blob, crtc);

		if (ret)
			DRM_ERROR("set degamma correction failed\n");
		else
			DRM_DEBUG_DRIVER("degamma correction success\n");
	}

	blob = crtc_state->ctm_blob;
	if (blob) {
		/* CSC correction */
		if (IS_CHERRYVIEW(dev))
			ret = chv_set_csc(dev, blob, crtc);

		if (ret)
			DRM_ERROR("set CSC correction failed\n");
		else
			DRM_DEBUG_DRIVER("CSC correction success\n");
	}
}

void intel_attach_color_properties_to_crtc(struct drm_device *dev,
		struct drm_crtc *crtc)
{
	struct drm_mode_config *config = &dev->mode_config;
	struct drm_mode_object *mode_obj = &crtc->base;

	/*
	 * Register:
	 * =========
	 * Gamma correction as palette_after_ctm property
	 * Degamma correction as palette_before_ctm property
	 *
	 * Load:
	 * =====
	 * no. of coefficients supported on this platform for gamma
	 * and degamma with the query properties. A user
	 * space agent should read these query property, and prepare
	 * the color correction values accordingly. Its expected from the
	 * driver to load the right number of coefficients during the init
	 * phase.
	 */
	if (config->cm_coeff_after_ctm_property) {
		drm_object_attach_property(mode_obj,
			config->cm_coeff_after_ctm_property,
				INTEL_INFO(dev)->num_samples_after_ctm);
		DRM_DEBUG_DRIVER("Gamma query property initialized\n");
	}

	if (config->cm_coeff_before_ctm_property) {
		drm_object_attach_property(mode_obj,
			config->cm_coeff_before_ctm_property,
				INTEL_INFO(dev)->num_samples_before_ctm);
		DRM_DEBUG_DRIVER("Degamma query property initialized\n");
	}

	/* Gamma correction */
	if (config->cm_palette_after_ctm_property) {
		drm_object_attach_property(mode_obj,
			config->cm_palette_after_ctm_property, 0);
		DRM_DEBUG_DRIVER("gamma property attached to CRTC\n");
	}

	/* Degamma correction */
	if (config->cm_palette_before_ctm_property) {
		drm_object_attach_property(mode_obj,
			config->cm_palette_before_ctm_property, 0);
		DRM_DEBUG_DRIVER("degamma property attached to CRTC\n");
	}

	/* CSC */
	if (config->cm_ctm_property) {
		drm_object_attach_property(mode_obj,
			config->cm_ctm_property, 0);
		DRM_DEBUG_DRIVER("CSC property attached to CRTC\n");
	}
}
