// SPDX-License-Identifier: GPL-2.0
/*
 * camss-isp-m2m: interactive ISP parameter control
 *
 * When camss-config.h adds new blocks, add a new else-if section in
 * parse_command() and corresponding fields to params_config / params_build.
 */
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "params.h"
#include "params_ctrl.h"

static void print_help(void)
{
	fprintf(stderr,
		"ISP params commands:\n"
		"  wb_gain=enable|disable    Enable/disable white balance block\n"
		"  chroma_enhan=enable|disable\n"
		"  color_correct=enable|disable\n"
		"  wb_gain.g_gain=N          WB green gain (Q5.10, 1024=1.0)\n"
		"  wb_gain.b_gain=N          WB blue gain\n"
		"  wb_gain.r_gain=N          WB red gain\n"
		"  chroma_enhan.luma_v0=N    R->Y coefficient (12sQ8)\n"
		"  chroma_enhan.luma_v1=N    G->Y coefficient\n"
		"  chroma_enhan.luma_v2=N    B->Y coefficient\n"
		"  chroma_enhan.luma_k=N     Y offset\n"
		"  chroma_enhan.coeff_ap=N   Cb positive coefficient\n"
		"  chroma_enhan.coeff_am=N   Cb negative coefficient\n"
		"  chroma_enhan.coeff_cp=N   Cr positive coefficient\n"
		"  chroma_enhan.coeff_cm=N   Cr negative coefficient\n"
		"  chroma_enhan.coeff_dp=N   Cr positive coefficient 2\n"
		"  chroma_enhan.coeff_dm=N   Cr negative coefficient 2\n"
		"  chroma_enhan.kcb=N        Cb output offset\n"
		"  chroma_enhan.kcr=N        Cr output offset\n"
		"  color_correct.a[0..2]=N   CC matrix row A\n"
		"  color_correct.b[0..2]=N   CC matrix row B\n"
		"  color_correct.c[0..2]=N   CC matrix row C\n"
		"  color_correct.k[0..2]=N   CC matrix offsets\n"
		"  color_correct.m=N         CC Q mode (0-3)\n"
		"  reset                     Reload default values\n"
		"  help                      Show this help\n");
}

/* Parse "block.field[idx]=value" or "block.field=value".
 * Returns 1 if cfg was modified, 0 otherwise. */
static int parse_command(const char *line, struct params_config *cfg)
{
	char block[64], field[64];
	int  idx = -1;
	long value = 0;
	int  has_value = 0;

	/* Trim trailing whitespace */
	char buf[256];
	strncpy(buf, line, sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = '\0';
	char *end = buf + strlen(buf) - 1;
	while (end >= buf && isspace((unsigned char)*end))
		*end-- = '\0';

	if (buf[0] == '\0' || buf[0] == '#')
		return 0;

	if (strcmp(buf, "reset") == 0) {
		params_config_default(cfg);
		fprintf(stderr, "params: reset to defaults\n");
		return 1;
	}

	if (strcmp(buf, "help") == 0 || strcmp(buf, "list") == 0) {
		print_help();
		return 0;
	}

	/* Try "block=enable|disable|0|1" */
	{
		char bname[64], bval[16];
		if (sscanf(buf, "%63[^=]=%15s", bname, bval) == 2 && !strchr(bname, '.')) {
			char *be = bname + strlen(bname) - 1;
			while (be >= bname && isspace((unsigned char)*be)) *be-- = '\0';
			int en = (strcmp(bval, "enable") == 0 || strcmp(bval, "1") == 0) ? 1
			       : (strcmp(bval, "disable") == 0 || strcmp(bval, "0") == 0) ? 0
			       : -1;
			if (en >= 0) {
				if (strcmp(bname, "wb_gain") == 0) {
					cfg->wb_enabled = en;
					fprintf(stderr, "params: wb_gain %s\n", en ? "enabled" : "disabled");
					return 1;
				} else if (strcmp(bname, "chroma_enhan") == 0) {
					cfg->ce_enabled = en;
					fprintf(stderr, "params: chroma_enhan %s\n", en ? "enabled" : "disabled");
					return 1;
				} else if (strcmp(bname, "color_correct") == 0) {
					cfg->cc_enabled = en;
					fprintf(stderr, "params: color_correct %s\n", en ? "enabled" : "disabled");
					return 1;
				} else {
					fprintf(stderr, "params: unknown block '%s'\n", bname);
					return 0;
				}
			}
		}
	}

	/* Try "block.field[idx]=value" */
	if (sscanf(buf, "%63[^.].%63[^[=[][%d]=%li", block, field, &idx, &value) == 4) {
		has_value = 1;
	} else if (sscanf(buf, "%63[^.].%63[^=]=%li", block, field, &value) == 3) {
		has_value = 1;
		idx = -1;
	} else {
		fprintf(stderr, "params: unrecognised command: %s\n", buf);
		return 0;
	}

	/* Trim trailing whitespace from field name (e.g. "field " from "field = value") */
	char *fe = field + strlen(field) - 1;
	while (fe >= field && isspace((unsigned char)*fe))
		*fe-- = 0;

	if (!has_value)
		return 0;

	/* ---- wb_gain ---- */
	if (strcmp(block, "wb_gain") == 0) {
		if      (strcmp(field, "g_gain") == 0) cfg->wb_g_gain = (uint16_t)value;
		else if (strcmp(field, "b_gain") == 0) cfg->wb_b_gain = (uint16_t)value;
		else if (strcmp(field, "r_gain") == 0) cfg->wb_r_gain = (uint16_t)value;
		else goto unknown_field;
		fprintf(stderr, "params: wb_gain.%s = %ld\n", field, value);
		return 1;
	}

	/* ---- chroma_enhan ---- */
	if (strcmp(block, "chroma_enhan") == 0) {
		if      (strcmp(field, "luma_v0")   == 0) cfg->ce_luma_v0   = (int16_t)value;
		else if (strcmp(field, "luma_v1")   == 0) cfg->ce_luma_v1   = (int16_t)value;
		else if (strcmp(field, "luma_v2")   == 0) cfg->ce_luma_v2   = (int16_t)value;
		else if (strcmp(field, "luma_k")    == 0) cfg->ce_luma_k    = (int16_t)value;
		else if (strcmp(field, "coeff_ap")  == 0) cfg->ce_coeff_ap  = (int16_t)value;
		else if (strcmp(field, "coeff_am")  == 0) cfg->ce_coeff_am  = (int16_t)value;
		else if (strcmp(field, "coeff_cp")  == 0) cfg->ce_coeff_cp  = (int16_t)value;
		else if (strcmp(field, "coeff_cm")  == 0) cfg->ce_coeff_cm  = (int16_t)value;
		else if (strcmp(field, "coeff_dp")  == 0) cfg->ce_coeff_dp  = (int16_t)value;
		else if (strcmp(field, "coeff_dm")  == 0) cfg->ce_coeff_dm  = (int16_t)value;
		else if (strcmp(field, "kcb")       == 0) cfg->ce_kcb       = (int16_t)value;
		else if (strcmp(field, "kcr")       == 0) cfg->ce_kcr       = (int16_t)value;
		else goto unknown_field;
		fprintf(stderr, "params: chroma_enhan.%s = %ld\n", field, value);
		return 1;
	}

	/* ---- color_correct ---- */
	if (strcmp(block, "color_correct") == 0) {
		if (strcmp(field, "m") == 0) {
			cfg->cc_m = (int16_t)value;
			fprintf(stderr, "params: color_correct.m = %ld\n", value);
			return 1;
		}
		if (idx < 0 || idx > 2) {
			fprintf(stderr, "params: color_correct.%s requires index [0..2]\n", field);
			return 0;
		}
		if      (strcmp(field, "a") == 0) cfg->cc_a[idx] = (int16_t)value;
		else if (strcmp(field, "b") == 0) cfg->cc_b[idx] = (int16_t)value;
		else if (strcmp(field, "c") == 0) cfg->cc_c[idx] = (int16_t)value;
		else if (strcmp(field, "k") == 0) cfg->cc_k[idx] = (int16_t)value;
		else goto unknown_field;
		fprintf(stderr, "params: color_correct.%s[%d] = %ld\n", field, idx, value);
		return 1;
	}

	fprintf(stderr, "params: unknown block '%s'\n", block);
	return 0;

unknown_field:
	fprintf(stderr, "params: unknown field '%s.%s'\n", block, field);
	return 0;
}

static void *reader_thread(void *arg)
{
	struct params_ctrl *ctrl = arg;
	char line[256];

	while (ctrl->running) {
		if (!fgets(line, sizeof(line), stdin))
			break;

		struct params_config tmp;
		pthread_mutex_lock(&ctrl->mutex);
		tmp = ctrl->cfg;
		pthread_mutex_unlock(&ctrl->mutex);

		if (parse_command(line, &tmp)) {
			pthread_mutex_lock(&ctrl->mutex);
			ctrl->cfg   = tmp;
			ctrl->dirty = 1;
			pthread_mutex_unlock(&ctrl->mutex);
		}
	}

	return NULL;
}

void params_ctrl_start(struct params_ctrl *ctrl,
		       const struct params_config *initial)
{
	pthread_mutex_init(&ctrl->mutex, NULL);
	ctrl->cfg     = *initial;
	ctrl->dirty   = 0;
	ctrl->running = 1;

	pthread_create(&ctrl->thread, NULL, reader_thread, ctrl);

	fprintf(stderr,
		"ISP params control active — type block.field=value "
		"(e.g. wb_gain.g_gain=1200), 'reset', or 'help'\n");
}

void params_ctrl_stop(struct params_ctrl *ctrl)
{
	if (!ctrl->running)
		return;
	ctrl->running = 0;
	/* Reader thread will exit on next EOF or line read */
	pthread_join(ctrl->thread, NULL);
	pthread_mutex_destroy(&ctrl->mutex);
}

int params_ctrl_get(struct params_ctrl *ctrl, struct params_config *out)
{
	int got = 0;

	pthread_mutex_lock(&ctrl->mutex);
	if (ctrl->dirty) {
		*out        = ctrl->cfg;
		ctrl->dirty = 0;
		got         = 1;
	}
	pthread_mutex_unlock(&ctrl->mutex);

	return got;
}
