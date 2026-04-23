// SPDX-License-Identifier: GPL-2.0
/*
 * camss-isp-test: media topology discovery
 */
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include <linux/media.h>
#include <linux/v4l2-subdev.h>

#include "media.h"

/* Use MEDIA_IOC_G_TOPOLOGY to get entities + interface links */
static int topology_get(int fd, struct isp_pipeline *pipe)
{
	struct media_v2_topology topo = {};
	struct media_v2_entity  *entities = NULL;
	struct media_v2_interface *ifaces = NULL;
	struct media_v2_link     *links = NULL;
	struct media_v2_pad      *pads = NULL;
	int ret;

	/* First call: get counts */
	ret = ioctl(fd, MEDIA_IOC_G_TOPOLOGY, &topo);
	if (ret < 0) {
		perror("MEDIA_IOC_G_TOPOLOGY (count)");
		return -1;
	}

	entities = calloc(topo.num_entities, sizeof(*entities));
	ifaces   = calloc(topo.num_interfaces, sizeof(*ifaces));
	links    = calloc(topo.num_links, sizeof(*links));
	pads     = calloc(topo.num_pads, sizeof(*pads));
	if (!entities || !ifaces || !links || !pads) {
		ret = -1;
		goto out;
	}

	topo.ptr_entities   = (uintptr_t)entities;
	topo.ptr_interfaces = (uintptr_t)ifaces;
	topo.ptr_links      = (uintptr_t)links;
	topo.ptr_pads       = (uintptr_t)pads;

	ret = ioctl(fd, MEDIA_IOC_G_TOPOLOGY, &topo);
	if (ret < 0) {
		perror("MEDIA_IOC_G_TOPOLOGY (data)");
		goto out;
	}

	pipe->num_vnodes = 0;

	for (uint32_t i = 0; i < topo.num_entities; i++) {
		struct media_v2_entity *e = &entities[i];

		/* Only interested in video device entities */
		if ((e->function & MEDIA_ENT_F_IO_V4L) != MEDIA_ENT_F_IO_V4L)
			continue;

		if (pipe->num_vnodes >= MAX_VNODES)
			break;

		struct isp_vnode *vn = &pipe->vnodes[pipe->num_vnodes];
		vn->entity_id = e->id;
		strncpy(vn->name, e->name, sizeof(vn->name) - 1);

		/* Determine direction from name: params/stats are meta,
		 * output/source pad = V4L2 output device (we write to it) */
		vn->is_meta   = strstr(e->name, "params") != NULL ||
				strstr(e->name, "stats")  != NULL;
		/* Default: capture. Override below if entity has a SOURCE pad. */
		vn->is_output = false;
		for (uint32_t p = 0; p < topo.num_pads; p++) {
			struct media_v2_pad *pad = &pads[p];
			if (pad->entity_id == e->id &&
			    (pad->flags & MEDIA_PAD_FL_SOURCE)) {
				vn->is_output = true;
				break;
			}
		}

		/* Find the interface link for this entity to get /dev/videoN */
		for (uint32_t j = 0; j < topo.num_links; j++) {
			struct media_v2_link *lnk = &links[j];

			if (!(lnk->flags & MEDIA_LNK_FL_INTERFACE_LINK))
				continue;

			/* Interface link: source_id = interface, sink_id = entity */
			if (lnk->sink_id != e->id)
				continue;

			/* Find the interface */
			for (uint32_t k = 0; k < topo.num_interfaces; k++) {
				struct media_v2_interface *iface = &ifaces[k];

				if (iface->id != lnk->source_id)
					continue;

				/* Resolve major:minor -> /dev/videoN via sysfs */
				char sysfs_path[128];
				char dev_str[16];
				FILE *fp;
				glob_t vg;

				if (glob("/sys/class/video4linux/*/dev", 0, NULL, &vg) == 0) {
					for (size_t vi = 0; vi < vg.gl_pathc; vi++) {
						fp = fopen(vg.gl_pathv[vi], "r");
						if (!fp)
							continue;
						if (fgets(dev_str, sizeof(dev_str), fp)) {
							unsigned int maj, min;
							if (sscanf(dev_str, "%u:%u", &maj, &min) == 2 &&
							    maj == iface->devnode.major &&
							    min == iface->devnode.minor) {
								/* Extract videoN from path */
								const char *vname = strrchr(vg.gl_pathv[vi], '/');
								if (vname) {
									/* go up one dir: .../videoN/dev -> videoN */
									strncpy(sysfs_path, vg.gl_pathv[vi],
										sizeof(sysfs_path) - 1);
									char *slash = strrchr(sysfs_path, '/');
									if (slash) *slash = '\0';
									const char *vn_name = strrchr(sysfs_path, '/');
									if (vn_name)
										snprintf(vn->devnode,
											sizeof(vn->devnode),
											"/dev/%s", vn_name + 1);
								}
								fclose(fp);
								break;
							}
						}
						fclose(fp);
					}
					globfree(&vg);
				}
				break;
			}
			break;
		}

		pipe->num_vnodes++;
	}

	ret = 0;
out:
	free(entities);
	free(ifaces);
	free(links);
	free(pads);
	return ret;
}

int media_find_ope_pipeline(struct isp_pipeline *pipe)
{
	glob_t g;
	int found = -1;

	memset(pipe, 0, sizeof(*pipe));
	pipe->media_fd = -1;

	if (glob("/dev/media*", 0, NULL, &g) != 0) {
		fprintf(stderr, "No /dev/media* devices found\n");
		return -1;
	}

	for (size_t i = 0; i < g.gl_pathc; i++) {
		int fd = open(g.gl_pathv[i], O_RDWR | O_CLOEXEC);
		if (fd < 0)
			continue;

		struct media_device_info info = {};
		if (ioctl(fd, MEDIA_IOC_DEVICE_INFO, &info) < 0) {
			close(fd);
			continue;
		}

		if (strstr(info.model, "qcom-camss-ope") ||
		    strstr(info.driver, "qcom-camss-ope")) {
			pipe->media_fd = fd;
			found = 0;
			printf("Found OPE media device: %s (model=%s driver=%s)\n",
			       g.gl_pathv[i], info.model, info.driver);
			break;
		}

		close(fd);
	}

	globfree(&g);

	if (found < 0) {
		fprintf(stderr, "OPE media device not found\n");
		return -1;
	}

	return topology_get(pipe->media_fd, pipe);
}

void media_pipeline_print(const struct isp_pipeline *pipe)
{
	printf("\nPipeline topology (%d video nodes):\n", pipe->num_vnodes);
	printf("  %-30s  %-16s  %-8s  %s\n",
	       "Name", "Device", "Dir", "Type");
	printf("  %s\n", "--------------------------------------------------------------");

	for (int i = 0; i < pipe->num_vnodes; i++) {
		const struct isp_vnode *vn = &pipe->vnodes[i];
		printf("  %-30s  %-16s  %-8s  %s\n",
		       vn->name,
		       vn->devnode[0] ? vn->devnode : "(unknown)",
		       vn->is_output ? "output" : "capture",
		       vn->is_meta   ? "meta"   : "video");
	}
	printf("\n");
}

void media_pipeline_close(struct isp_pipeline *pipe)
{
	if (pipe->media_fd >= 0) {
		close(pipe->media_fd);
		pipe->media_fd = -1;
	}
}

struct isp_vnode *media_find_vnode(struct isp_pipeline *pipe,
				   const char *name_substr)
{
	for (int i = 0; i < pipe->num_vnodes; i++) {
		if (strstr(pipe->vnodes[i].name, name_substr))
			return &pipe->vnodes[i];
	}
	return NULL;
}
