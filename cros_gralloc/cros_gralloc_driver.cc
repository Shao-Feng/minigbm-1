/*
 * Copyright 2017 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "cros_gralloc_driver.h"

#include <cstdlib>
#include <fcntl.h>
#include <sys/mman.h>
#include <xf86drm.h>

#include "../drv_priv.h"
#include "../helpers.h"
#include "../util.h"

#ifdef USE_GRALLOC1
#include "i915_private_android.h"
#endif

// drv_render_ aim to open the render node
cros_gralloc_driver::cros_gralloc_driver() : drv_render_(nullptr)
{
}

cros_gralloc_driver::~cros_gralloc_driver()
{
	buffers_.clear();
	handles_.clear();

	if (drv_render_) {
		int fd = drv_get_fd(drv_render_);
		drv_destroy(drv_render_);
		drv_render_ = nullptr;
		close(fd);
	}
}

int32_t cros_gralloc_driver::init()
{
	/*
	 * Create a driver from display node or/and render node while filtering out
	 * the specified undesired driver.
	 *
	 * TODO(gsingh): Enable render nodes on udl/evdi.
	 */

	int fd;
	drmVersionPtr version;
	char const *str = "%s/renderD%d";
	const char *undesired[2] = { "vgem", nullptr };
	uint32_t num_nodes = 63;
	uint32_t min_node = 128;
	uint32_t max_node = (min_node + num_nodes);
	uint32_t j;
	char *node;

	const int render_num = 10;
	const int name_length = 50;
	int node_fd[render_num];
	char node_name[render_num][name_length] = { "" };
	int availabe_node = 0;
	int virtio_node_idx = -1;
	uint32_t gpu_grp_type = 0;

	// destroy drivers if exist before re-initializing them
	if (drv_render_) {
		int fd = drv_get_fd(drv_render_);
		drv_destroy(drv_render_);
		drv_render_ = nullptr;
		close(fd);
	}

	for (uint32_t i = min_node; i < max_node; i++) {
		if (asprintf(&node, str, DRM_DIR_NAME, i) < 0)
			continue;

		fd = open(node, O_RDWR, 0);
		free(node);
		if (fd < 0)
			continue;

		version = drmGetVersion(fd);
		if (!version) {
			close(fd);
			continue;
		}

		for (j = 0; j < ARRAY_SIZE(undesired); j++) {
			if (undesired[j] && !strcmp(version->name, undesired[j])) {
				drmFreeVersion(version);
				close(fd);
				break;
			}
		}

		// hit any of undesired render node
		if (j < ARRAY_SIZE(undesired))
			continue;

		if (!strcmp(version->name, "virtio_gpu")) {
			virtio_node_idx = availabe_node;
		}

		node_fd[availabe_node] = fd;
		strcpy(node_name[availabe_node], version->name);
		availabe_node++;

		drmFreeVersion(version);
	}

	// open the first render node
	if (availabe_node > 0) {
		drv_render_ = drv_create(node_fd[0]);
		if (!drv_render_) {
			drv_log("Failed to create driver for the 1st device\n");
			close(node_fd[0]);
			goto fail;
		}

		switch (availabe_node) {
		// only have one render node, is GVT-d/BM/VirtIO
		case 1:
			if (!drv_render_)
				goto fail;
			gpu_grp_type = (virtio_node_idx != -1)? ONE_GPU_VIRTIO: ONE_GPU_INTEL;
			break;
		// is SR-IOV or iGPU + dGPU
		case 2:
			close(node_fd[1]);
			if (virtio_node_idx != -1) {
				gpu_grp_type = TWO_GPU_IGPU_VIRTIO;
			} else {
				gpu_grp_type = TWO_GPU_IGPU_DGPU;
			}
			break;
		// is SR-IOV + dGPU
		case 3:
			if (!strcmp(node_name[1], "i915")) {
				close(node_fd[1]);
			}
			if (virtio_node_idx != -1) {
				close(node_fd[virtio_node_idx]);
			}
			gpu_grp_type = THREE_GPU_IGPU_VIRTIO_DGPU;
			// TO-DO: the 3rd node is i915 or others.
			break;
		}

		if (drv_render_) {
			if (drv_init(drv_render_, gpu_grp_type)) {
				drv_log("Failed to init render driver\n");
				goto fail;
			}
		}
	}

	if (!drv_render_)
		return -ENODEV;

	return 0;

fail:
	if (drv_render_) {
		fd = drv_get_fd(drv_render_);
		drv_destroy(drv_render_);
		close(fd);
		drv_render_ = nullptr;
	}

	return -ENODEV;
}

bool cros_gralloc_driver::is_supported(struct cros_gralloc_buffer_descriptor *descriptor)
{
	struct combination *combo;
	uint32_t resolved_format;
	bool supported;
	struct driver *drv = drv_render_;

	resolved_format = drv_resolve_format(drv, descriptor->drm_format, descriptor->use_flags);
	combo = drv_get_combination(drv, resolved_format, descriptor->use_flags);

	supported = (combo != nullptr);

	if (!supported && (descriptor->use_flags & BO_USE_SCANOUT)) {
		descriptor->use_flags &= ~BO_USE_SCANOUT;
		combo = drv_get_combination(drv, resolved_format, descriptor->use_flags);
				supported = (combo != nullptr);
	}
	return supported;
}

int32_t create_reserved_region(const std::string &buffer_name, uint64_t reserved_region_size)
{
	int32_t reserved_region_fd;
	std::string reserved_region_name = buffer_name + " reserved region";

	reserved_region_fd = memfd_create(reserved_region_name.c_str(), FD_CLOEXEC);
	if (reserved_region_fd == -1) {
		drv_log("Failed to create reserved region fd: %s.\n", strerror(errno));
		return -errno;
	}

	if (ftruncate(reserved_region_fd, reserved_region_size)) {
		drv_log("Failed to set reserved region size: %s.\n", strerror(errno));
		return -errno;
	}

	return reserved_region_fd;
}

int32_t cros_gralloc_driver::allocate(const struct cros_gralloc_buffer_descriptor *descriptor,
				      buffer_handle_t *out_handle)
{
#ifdef USE_GRALLOC1
	uint64_t mod;
#endif
	uint32_t id;
	size_t num_planes;
	size_t num_fds;
	size_t num_ints;
	size_t num_bytes;
	uint32_t resolved_format;
	uint32_t bytes_per_pixel;
	uint64_t use_flags;
	int32_t reserved_region_fd;
	char *name;

	struct bo *bo;
	struct cros_gralloc_handle *hnd;

	struct driver *drv;
	drv = drv_render_;

	resolved_format = drv_resolve_format(drv, descriptor->drm_format, descriptor->use_flags);
	use_flags = descriptor->use_flags;
	/*
	 * TODO(b/79682290): ARC++ assumes NV12 is always linear and doesn't
	 * send modifiers across Wayland protocol, so we or in the
	 * BO_USE_LINEAR flag here. We need to fix ARC++ to allocate and work
	 * with tiled buffers.
	 */
	if (resolved_format == DRM_FORMAT_NV12)
		use_flags |= BO_USE_LINEAR;

	/*
	 * This unmask is a backup in the case DRM_FORMAT_FLEX_IMPLEMENTATION_DEFINED is resolved
	 * to non-YUV formats.
	 */
	if (descriptor->drm_format == DRM_FORMAT_FLEX_IMPLEMENTATION_DEFINED &&
	    (resolved_format == DRM_FORMAT_XBGR8888 || resolved_format == DRM_FORMAT_ABGR8888)) {
		use_flags &= ~BO_USE_HW_VIDEO_ENCODER;
	}

#ifdef USE_GRALLOC1
	if (descriptor->modifier == 0) {
		bo = drv_bo_create(drv, descriptor->width, descriptor->height, resolved_format,
				   use_flags);
	} else {
		bo = drv_bo_create_with_modifiers(drv, descriptor->width, descriptor->height,
						  resolved_format, &descriptor->modifier, 1);
	}
#else
	bo = drv_bo_create(drv, descriptor->width, descriptor->height, resolved_format, use_flags);
#endif
	if (!bo) {
		drv_log("Failed to create bo.\n");
		return -ENOMEM;
	}

	/*
	 * If there is a desire for more than one kernel buffer, this can be
	 * removed once the ArcCodec and Wayland service have the ability to
	 * send more than one fd. GL/Vulkan drivers may also have to modified.
	 */
	if (drv_num_buffers_per_bo(bo) != 1) {
		drv_bo_destroy(bo);
		drv_log("Can only support one buffer per bo.\n");
		return -EINVAL;
	}

	num_planes = drv_bo_get_num_planes(bo);
	num_fds = num_planes;

	if (descriptor->reserved_region_size > 0) {
		reserved_region_fd =
		    create_reserved_region(descriptor->name, descriptor->reserved_region_size);
		if (reserved_region_fd < 0) {
			drv_bo_destroy(bo);
			return reserved_region_fd;
		}
		num_fds += 1;
	} else {
		reserved_region_fd = -1;
	}

	num_bytes = sizeof(struct cros_gralloc_handle);
	num_bytes += (descriptor->name.size() + 1);
	/*
	 * Ensure that the total number of bytes is a multiple of sizeof(int) as
	 * native_handle_clone() copies data based on hnd->base.numInts.
	 */
	num_bytes = ALIGN(num_bytes, sizeof(int));
	num_ints = num_bytes - sizeof(native_handle_t) - num_fds;
	/*
	 * Malloc is used as handles are ultimetly destroyed via free in
	 * native_handle_delete().
	 */
	hnd = static_cast<struct cros_gralloc_handle *>(malloc(num_bytes));
	hnd->from_kms = false; // not used, just set a default value. keep this member to be backward compatible.
	hnd->base.version = sizeof(hnd->base);
	hnd->base.numFds = num_fds;
	hnd->base.numInts = num_ints;
	hnd->num_planes = num_planes;
	for (size_t plane = 0; plane < num_planes; plane++) {
		hnd->fds[plane] = drv_bo_get_plane_fd(bo, plane);
		hnd->strides[plane] = drv_bo_get_plane_stride(bo, plane);
		hnd->offsets[plane] = drv_bo_get_plane_offset(bo, plane);
		hnd->sizes[plane] = drv_bo_get_plane_size(bo, plane);
#ifdef USE_GRALLOC1
		mod = drv_bo_get_plane_format_modifier(bo, plane);
		hnd->format_modifiers[2 * plane] = static_cast<uint32_t>(mod >> 32);
		hnd->format_modifiers[2 * plane + 1] = static_cast<uint32_t>(mod);
#endif
	}
	hnd->fds[hnd->num_planes] = reserved_region_fd;
	hnd->reserved_region_size = descriptor->reserved_region_size;
	static std::atomic<uint32_t> next_buffer_id{ 1 };
	hnd->id = next_buffer_id++;
	hnd->width = drv_bo_get_width(bo);
	hnd->height = drv_bo_get_height(bo);
	hnd->format = drv_bo_get_format(bo);
	hnd->format_modifier = drv_bo_get_plane_format_modifier(bo, 0);
	hnd->use_flags = descriptor->use_flags;
	bytes_per_pixel = drv_bytes_per_pixel_from_format(hnd->format, 0);
	hnd->pixel_stride = DIV_ROUND_UP(hnd->strides[0], bytes_per_pixel);
	hnd->magic = cros_gralloc_magic;
	hnd->usage = descriptor->droid_usage;
#ifdef USE_GRALLOC1
	hnd->producer_usage = descriptor->producer_usage;
	hnd->consumer_usage = descriptor->consumer_usage;
	hnd->tiling_mode = drv_bo_get_stride_or_tiling(bo);
	int32_t format = i915_private_invert_format(hnd->format);
	if (format == 0) {
		format =  descriptor->droid_format;
	}
	hnd->droid_format = format;
#else
	hnd->droid_format = descriptor->droid_format;
#endif
	hnd->total_size = descriptor->reserved_region_size + bo->meta.total_size;
	hnd->name_offset = handle_data_size;

	name = (char *)(&hnd->base.data[hnd->name_offset]);
	snprintf(name, descriptor->name.size() + 1, "%s", descriptor->name.c_str());

	id = drv_bo_get_plane_handle(bo, 0).u32;
	auto buffer = new cros_gralloc_buffer(id, bo, hnd, hnd->fds[hnd->num_planes],
					      hnd->reserved_region_size);

	std::lock_guard<std::mutex> lock(mutex_);
	buffers_.emplace(id, buffer);
	handles_.emplace(hnd, std::make_pair(buffer, 1));
	*out_handle = reinterpret_cast<buffer_handle_t>(hnd);
	return 0;
}

int32_t cros_gralloc_driver::retain(buffer_handle_t handle)
{
	uint32_t id;
	std::lock_guard<std::mutex> lock(mutex_);
	struct driver *drv;

	auto hnd = cros_gralloc_convert_handle(handle);
	if (!hnd) {
		drv_log("Invalid handle.\n");
		return -EINVAL;
	}

	drv = drv_render_;

	auto buffer = get_buffer(hnd);
	if (buffer) {
		handles_[hnd].second++;
		buffer->increase_refcount();
		return 0;
	}

	if (drmPrimeFDToHandle(drv_get_fd(drv), hnd->fds[0], &id)) {
		drv_log("drmPrimeFDToHandle failed.\n");
		return -errno;
	}

	if (buffers_.count(id)) {
		buffer = buffers_[id];
		buffer->increase_refcount();
	} else {
		struct bo *bo;
		struct drv_import_fd_data data;
		data.format = hnd->format;

		data.width = hnd->width;
		data.height = hnd->height;
		data.use_flags = hnd->use_flags;

		memcpy(data.fds, hnd->fds, sizeof(data.fds));
		memcpy(data.strides, hnd->strides, sizeof(data.strides));
		memcpy(data.offsets, hnd->offsets, sizeof(data.offsets));
		for (uint32_t plane = 0; plane < DRV_MAX_PLANES; plane++) {
			data.format_modifiers[plane] = hnd->format_modifier;
		}

		bo = drv_bo_import(drv, &data);
		if (!bo)
			return -EFAULT;

		id = drv_bo_get_plane_handle(bo, 0).u32;

		buffer = new cros_gralloc_buffer(id, bo, nullptr, hnd->fds[hnd->num_planes],
						 hnd->reserved_region_size);
		buffers_.emplace(id, buffer);
	}

	handles_.emplace(hnd, std::make_pair(buffer, 1));
	return 0;
}

int32_t cros_gralloc_driver::release(buffer_handle_t handle)
{
	std::lock_guard<std::mutex> lock(mutex_);

	auto hnd = cros_gralloc_convert_handle(handle);
	if (!hnd) {
		drv_log("Invalid handle.\n");
		return -EINVAL;
	}

	auto buffer = get_buffer(hnd);
	if (!buffer) {
		drv_log("Invalid Reference.\n");
		return -EINVAL;
	}

	if (!--handles_[hnd].second)
		handles_.erase(hnd);

	if (buffer->decrease_refcount() == 0) {
		buffers_.erase(buffer->get_id());
		delete buffer;
	}

	return 0;
}

int32_t cros_gralloc_driver::lock(buffer_handle_t handle, int32_t acquire_fence,
				  bool close_acquire_fence, const struct rectangle *rect,
				  uint32_t map_flags, uint8_t *addr[DRV_MAX_PLANES])
{
	int32_t ret = cros_gralloc_sync_wait(acquire_fence, close_acquire_fence);
	if (ret)
		return ret;

	std::lock_guard<std::mutex> lock(mutex_);
	auto hnd = cros_gralloc_convert_handle(handle);
	if (!hnd) {
		drv_log("Invalid handle.\n");
		return -EINVAL;
	}

	auto buffer = get_buffer(hnd);
	if (!buffer) {
		drv_log("Invalid Reference.\n");
		return -EINVAL;
	}

	return buffer->lock(rect, map_flags, addr);
}

#ifdef USE_GRALLOC1
int32_t cros_gralloc_driver::lock(buffer_handle_t handle, int32_t acquire_fence, uint32_t map_flags,
                                  uint8_t *addr[DRV_MAX_PLANES])
{
        int32_t ret = cros_gralloc_sync_wait(acquire_fence);
        if (ret)
                return ret;

        std::lock_guard<std::mutex> lock(mutex_);
        auto hnd = cros_gralloc_convert_handle(handle);
        if (!hnd) {
                drv_log("Invalid handle.");
                return -EINVAL;
        }

        auto buffer = get_buffer(hnd);
        if (!buffer) {
                drv_log("Invalid Reference.");
                return -EINVAL;
        }

        return buffer->lock(map_flags, addr);
}
#endif

int32_t cros_gralloc_driver::unlock(buffer_handle_t handle, int32_t *release_fence)
{
	std::lock_guard<std::mutex> lock(mutex_);

	auto hnd = cros_gralloc_convert_handle(handle);
	if (!hnd) {
		drv_log("Invalid handle.\n");
		return -EINVAL;
	}

	auto buffer = get_buffer(hnd);
	if (!buffer) {
		drv_log("Invalid Reference.\n");
		return -EINVAL;
	}

	/*
	 * From the ANativeWindow::dequeueBuffer documentation:
	 *
	 * "A value of -1 indicates that the caller may access the buffer immediately without
	 * waiting on a fence."
	 */
	*release_fence = -1;
	return buffer->unlock();
}

int32_t cros_gralloc_driver::invalidate(buffer_handle_t handle)
{
	std::lock_guard<std::mutex> lock(mutex_);

	auto hnd = cros_gralloc_convert_handle(handle);
	if (!hnd) {
		drv_log("Invalid handle.\n");
		return -EINVAL;
	}

	auto buffer = get_buffer(hnd);
	if (!buffer) {
		drv_log("Invalid Reference.\n");
		return -EINVAL;
	}

	return buffer->invalidate();
}

int32_t cros_gralloc_driver::flush(buffer_handle_t handle, int32_t *release_fence)
{
	std::lock_guard<std::mutex> lock(mutex_);

	auto hnd = cros_gralloc_convert_handle(handle);
	if (!hnd) {
		drv_log("Invalid handle.\n");
		return -EINVAL;
	}

	auto buffer = get_buffer(hnd);
	if (!buffer) {
		drv_log("Invalid Reference.\n");
		return -EINVAL;
	}

	/*
	 * From the ANativeWindow::dequeueBuffer documentation:
	 *
	 * "A value of -1 indicates that the caller may access the buffer immediately without
	 * waiting on a fence."
	 */
	*release_fence = -1;
	return buffer->flush();
}

int32_t cros_gralloc_driver::get_backing_store(buffer_handle_t handle, uint64_t *out_store)
{
	std::lock_guard<std::mutex> lock(mutex_);

	auto hnd = cros_gralloc_convert_handle(handle);
	if (!hnd) {
		drv_log("Invalid handle.\n");
		return -EINVAL;
	}

#ifdef USE_GRALLOC1
	*out_store = static_cast<uint64_t>(hnd->id);
#else
	auto buffer = get_buffer(hnd);
	if (!buffer) {
		drv_log("Invalid Reference.\n");
		return -EINVAL;
	}

	*out_store = static_cast<uint64_t>(buffer->get_id());
#endif
	return 0;
}

int32_t cros_gralloc_driver::resource_info(buffer_handle_t handle, uint32_t strides[DRV_MAX_PLANES],
					   uint32_t offsets[DRV_MAX_PLANES])
{
	std::lock_guard<std::mutex> lock(mutex_);

	auto hnd = cros_gralloc_convert_handle(handle);
	if (!hnd) {
		drv_log("Invalid handle.\n");
		return -EINVAL;
	}

	auto buffer = get_buffer(hnd);
	if (!buffer) {
		drv_log("Invalid Reference.\n");
		return -EINVAL;
	}

	return buffer->resource_info(strides, offsets);
}

int32_t cros_gralloc_driver::get_reserved_region(buffer_handle_t handle,
						 void **reserved_region_addr,
						 uint64_t *reserved_region_size)
{
	std::lock_guard<std::mutex> lock(mutex_);

	auto hnd = cros_gralloc_convert_handle(handle);
	if (!hnd) {
		drv_log("Invalid handle.\n");
		return -EINVAL;
	}

	auto buffer = get_buffer(hnd);
	if (!buffer) {
		drv_log("Invalid Reference.\n");
		return -EINVAL;
	}

	return buffer->get_reserved_region(reserved_region_addr, reserved_region_size);
}

uint32_t cros_gralloc_driver::get_resolved_drm_format(uint32_t drm_format, uint64_t usage)
{
	struct driver *drv = drv_render_;

	return drv_resolve_format(drv, drm_format, usage);
}

cros_gralloc_buffer *cros_gralloc_driver::get_buffer(cros_gralloc_handle_t hnd)
{
	/* Assumes driver mutex is held. */
	if (handles_.count(hnd))
		return handles_[hnd].first;

	return nullptr;
}

void cros_gralloc_driver::for_each_handle(
    const std::function<void(cros_gralloc_handle_t)> &function)
{
	std::lock_guard<std::mutex> lock(mutex_);

	for (const auto &pair : handles_) {
		function(pair.first);
	}
}

bool cros_gralloc_driver::IsSupportedYUVFormat(uint32_t droid_format)
{
	switch (droid_format) {
	case HAL_PIXEL_FORMAT_YCbCr_420_888:
	case HAL_PIXEL_FORMAT_YV12:
	case HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED:
		return true;
	default:
		return i915_private_supported_yuv_format(droid_format);
	}
	return false;
}
