/* userspace wrapper: pull in our type shim before the kernel header */
#ifndef _CAMSS_TEST_VIDEODEV2_WRAPPER_H
#define _CAMSS_TEST_VIDEODEV2_WRAPPER_H
#include "types.h"
#include_next <linux/videodev2.h>
#endif
