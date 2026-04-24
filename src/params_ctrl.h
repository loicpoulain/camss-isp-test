/* SPDX-License-Identifier: GPL-2.0 */
/*
 * camss-isp-m2m: interactive ISP parameter control
 *
 * Reads "block.field=value" commands from stdin in a background thread
 * and updates the active params_config between frames.
 *
 * Command syntax mirrors UAPI struct names from camss-config.h:
 *
 *   wb_gain.g_gain=1200
 *   wb_gain.b_gain=1500
 *   wb_gain.r_gain=1500
 *   chroma_enhan.luma_v0=0x4d
 *   chroma_enhan.kcb=128
 *   color_correct.a[0]=256
 *   color_correct.m=1
 *   reset                    (reload defaults)
 *   help                     (print available commands)
 *
 * Values are decimal or hex (0x...).
 * When camss-config.h adds new blocks, update params_ctrl.c only.
 */
#ifndef CAMSS_TEST_PARAMS_CTRL_H
#define CAMSS_TEST_PARAMS_CTRL_H

#include <pthread.h>
#include "params.h"

struct params_ctrl {
	pthread_mutex_t  mutex;
	struct params_config cfg;
	int              dirty;
	int              running;
	pthread_t        thread;
};

/**
 * params_ctrl_start - copy initial config and start the reader thread
 */
void params_ctrl_start(struct params_ctrl *ctrl,
		       const struct params_config *initial);

/**
 * params_ctrl_stop - signal the reader thread to exit and join it
 */
void params_ctrl_stop(struct params_ctrl *ctrl);

/**
 * params_ctrl_get - retrieve updated config if available
 *
 * Returns 1 and copies the new config to @out if a command was received
 * since the last call. Returns 0 if nothing changed.
 */
int params_ctrl_get(struct params_ctrl *ctrl, struct params_config *out);

#endif /* CAMSS_TEST_PARAMS_CTRL_H */
