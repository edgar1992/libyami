/*
 *  v4l2_wrapper.h
 *
 *  Copyright (C) 2014 Intel Corporation
 *    Author: Zhao, Halley<halley.zhao@intel.com>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public License
 *  as published by the Free Software Foundation; either version 2.1
 *  of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free
 *  Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 *  Boston, MA 02110-1301 USA
 */

#include <stdint.h>
#include <stddef.h>

#ifndef v4l2_wrapper_h
#define v4l2_wrapper_h
extern "C" {
int32_t YamiV4L2_Open(const char* name, int32_t flags);
int32_t YamiV4L2_Close(int32_t fd);
int32_t YamiV4L2_Ioctl(int32_t fd, int request, void* arg);
int32_t YamiV4L2_Poll(int32_t fd, bool poll_device, bool* event_pending);
int32_t YamiV4L2_SetDevicePollInterrupt(int32_t fd);
int32_t YamiV4L2_ClearDevicePollInterrupt(int32_t fd);
void* YamiV4L2_Mmap(void* addr, size_t length,
                     int prot, int flags, int fd, unsigned int offset);
int32_t YamiV4L2_Munmap(void* addr, size_t length);
int32_t YamiV4L2_UseEglImage(int fd, unsigned int buffer_index, void* egl_image);
} // extern "C"
#endif

