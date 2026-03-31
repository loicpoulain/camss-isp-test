/* SPDX-License-Identifier: GPL-2.0 */
/*
 * camss-isp-test: media topology helpers
 */
#ifndef CAMSS_TEST_MEDIA_H
#define CAMSS_TEST_MEDIA_H

#include <linux/media.h>
#include <stdbool.h>
#include <stdint.h>

#define MAX_ENTITIES  32
#define MAX_PADS      16
#define MAX_LINKS     64
#define MAX_VNODES    8

/**
 * struct isp_vnode - one video device endpoint found in the pipeline
 * @entity_id: media entity id
 * @name:      entity name
 * @devnode:   /dev/videoN path
 * @is_output: true = V4L2 output (we write to it), false = capture
 * @is_meta:   true = meta format (params/stats), false = video frame
 */
struct isp_vnode {
	uint32_t entity_id;
	char     name[64];
	char     devnode[32];
	bool     is_output;
	bool     is_meta;
};

/**
 * struct isp_pipeline - discovered OPE pipeline
 * @media_fd:   open fd for /dev/mediaX
 * @vnodes:     discovered video device endpoints
 * @num_vnodes: number of entries in vnodes
 */
struct isp_pipeline {
	int            media_fd;
	struct isp_vnode vnodes[MAX_VNODES];
	int            num_vnodes;
};

/**
 * media_find_ope_pipeline - scan /dev/media* for the OPE pipeline
 *
 * Looks for a media device whose model matches "qcom-camss-ope", enumerates
 * its entities and resolves video device nodes.
 *
 * Returns 0 on success and fills @pipe, -1 on failure.
 */
int media_find_ope_pipeline(struct isp_pipeline *pipe);

/**
 * media_pipeline_print - print the discovered topology to stdout
 */
void media_pipeline_print(const struct isp_pipeline *pipe);

/**
 * media_pipeline_close - close the media fd
 */
void media_pipeline_close(struct isp_pipeline *pipe);

/**
 * media_find_vnode - find a vnode by name substring
 */
struct isp_vnode *media_find_vnode(struct isp_pipeline *pipe,
				   const char *name_substr);

#endif /* CAMSS_TEST_MEDIA_H */
