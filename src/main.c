#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/mman.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#include <libudev.h>
#include <libinput.h>
#include <linux/input.h>
#include <config.h>

#define ATOMICTEST_NUM_FBS 2

struct at_device {
	int fd;

	drmModeModeInfo mode;
	uint16_t width;
	uint16_t height;

	uint32_t connector;
	uint32_t crtc_id;
	uint32_t crtc_idx;

	/* possible overlay planes for crtc_id */
	uint32_t overlays_count;
	uint32_t *overlay_ids;

	drmModeCrtc *saved_crtc;
};

struct at_dumb_buffer {
	uint32_t width;
	uint32_t height;

	uint32_t handle;
	uint32_t pitch;
	uint64_t size;

	uint8_t *data;
};

struct at_dumb_fb {
	struct at_dumb_buffer *dumb;
	uint32_t fb_id;
};

struct at_instance {
	struct at_device device;
	struct at_dumb_fb *fbs[ATOMICTEST_NUM_FBS];
	struct at_dumb_buffer *cursor_buf;
	struct at_dumb_fb **overlay_fbs;

	uint32_t cur_fb;
	bool run;
	bool flip_pending;
	bool crtc_changed;

	struct libinput *li;

	int cursor_x;
	int cursor_y;
};

static bool run = true;

static void
sigint_handler(int sig)
{
	run = false;
}

static int
setup_device(struct at_device *device, drmModeRes *resources,
	     drmModeConnector *connector)
{
	int i;
	drmModeEncoder *encoder;

	if (connector->encoder_id) {
		printf("  there's a connected encoder (id %d)\n",
			connector->encoder_id);
		encoder = drmModeGetEncoder(device->fd, connector->encoder_id);
	} else {
		encoder = NULL;
	}

	if (encoder) {
		if (encoder->crtc_id) {
			printf("  the encoder is connected to the CRTC %d\n",
				encoder->crtc_id);
			device->crtc_id = encoder->crtc_id;

			for (i = 0; i < resources->count_crtcs; i++) {
				if (resources->crtcs[i] == device->crtc_id) {
					device->crtc_idx = i;
					break;
				}
			}

			drmModeFreeEncoder(encoder);
			return 0;
		}
		drmModeFreeEncoder(encoder);
	}

	for (i = 0; i < connector->count_encoders; i++) {
		int j;

		encoder = drmModeGetEncoder(device->fd, connector->encoders[i]);
		if (!encoder)
			continue;

		for (j = 0; j < resources->count_crtcs; j++) {
			if (!(encoder->possible_crtcs & (1 << j)))
				continue;

			printf("  crtc %d is available to this encoder\n", j);

			device->crtc_id = resources->crtcs[j];
			device->crtc_idx = j;
			drmModeFreeEncoder(encoder);
			return 0;
		}

		drmModeFreeEncoder(encoder);
	}

	return -1;
}

static bool
is_overlay(int fd, drmModePlanePtr plane)
{
	int i;
	drmModeObjectPropertiesPtr props;
	bool overlay = false;

	props = drmModeObjectGetProperties(fd, plane->plane_id, DRM_MODE_OBJECT_PLANE);
	if (!props)
		return false;

	for (i = 0; i < props->count_props; i++) {
		drmModePropertyPtr prop = drmModeGetProperty(fd, props->props[i]);
		if (!prop)
			continue;

		if (!strcmp(prop->name, "type") && props->prop_values[i] == DRM_PLANE_TYPE_OVERLAY) {
			drmModeFreeProperty(prop);
			overlay = true;
			break;
		}

		drmModeFreeProperty(prop);
	}

	drmModeFreeObjectProperties(props);

	return overlay;
}

static void
setup_planes(struct at_device *device, drmModePlaneRes *plane_res)
{
	int i;

	device->overlays_count = 0;
	device->overlay_ids = NULL;

	for (i = 0; i < plane_res->count_planes; i++) {
		drmModePlanePtr plane = drmModeGetPlane(device->fd, plane_res->planes[i]);
		if (!plane)
			continue;

		if ((plane->possible_crtcs & (1 << device->crtc_idx)) &&
		    is_overlay(device->fd, plane)) {
			device->overlay_ids = realloc(device->overlay_ids, sizeof(*device->overlay_ids) *
						      (device->overlays_count + 1));
			device->overlay_ids[device->overlays_count] = plane->plane_id;
			device->overlays_count++;
		}

		drmModeFreePlane(plane);
	}
}

int
at_device_open(struct at_device *device, const char *node)
{
	int i;
	int fd;
	int ret;
	uint64_t cap_dumb;
	drmModeRes *resources;
	drmModePlaneRes *plane_res;

	fd = open(node, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		perror("Could not open input file");
		return -1;
	}

	ret = drmGetCap(fd, DRM_CAP_DUMB_BUFFER, &cap_dumb);
	if (ret < 0 || !cap_dumb) {
		fprintf(stderr, "Error: device doesn't support dumb buffers.\n");
		close(fd);
		return -1;
	}

	resources = drmModeGetResources(fd);
	if (!resources) {
		fprintf(stderr, "Error: can't get mode resources.\n");
		close(fd);
		return -1;
	}

	device->fd = fd;

	ret = drmSetClientCap(device->fd, DRM_CLIENT_CAP_ATOMIC, 1);
	if (ret < 0) {
		fprintf(stderr, "Error: the device doesn't support atomic.\n");
		drmModeFreeResources(resources);
		close(fd);
		return -1;
	}

	printf("Device fbs: %d\n", resources->count_fbs);
	printf("Device crtcs: %d\n", resources->count_crtcs);
	printf("Device encoders: %d\n", resources->count_encoders);
	printf("Device connectors: %d\n", resources->count_connectors);

	plane_res = drmModeGetPlaneResources(device->fd);

	/*
	 * Get the first connected connector.
	 */
	for (i = 0; i < resources->count_connectors; i++) {
		drmModeConnector *connector;

		printf("\nTrying connector %d...\n", i);

		connector = drmModeGetConnector(fd, resources->connectors[i]);
		if (!connector) {
			printf("  can't get connector info %d\n",
			       resources->connectors[i]);
			continue;
		}

		printf("  connector type: %d\n", connector->connector_type);

		if (connector->connection != DRM_MODE_CONNECTED) {
			printf("  not connected, skipping...\n");
			drmModeFreeConnector(connector);
			continue;
		}

		if (connector->count_modes == 0) {
			printf("  this connector doesn't have any valid modes\n");
			continue;
		}

		drmModeModeInfo *mode_info = &connector->modes[i];
		printf("    Mode %d\n", i);
		printf("      clock: %d\n", mode_info->clock);
		printf("      hdisplay: %d\n", mode_info->hdisplay);
		printf("      vdisplay: %d\n", mode_info->vdisplay);
		printf("      vrefresh: %d\n", mode_info->vrefresh);
		printf("      flags: 0x%08X\n", mode_info->flags);
		printf("      type: 0x%08X\n", mode_info->type);
		printf("      name: %s\n", mode_info->name);

		if (setup_device(device, resources, connector) < 0) {
			drmModeFreeConnector(connector);
			continue;
		}

		setup_planes(device, plane_res);

		memcpy(&device->mode, &connector->modes[0], sizeof(device->mode));
		device->width = connector->modes[0].hdisplay;
		device->height = connector->modes[0].vdisplay;
		device->connector = connector->connector_id;
		device->saved_crtc = NULL;

		drmModeFreePlaneResources(plane_res);
		drmModeFreeConnector(connector);
		drmModeFreeResources(resources);

		return 0;
	}

	drmModeFreePlaneResources(plane_res);
	drmModeFreeResources(resources);
	close(fd);

	return -1;
}

int
at_device_close(struct at_device *device)
{
	free(device->overlay_ids);
	close(device->fd);
	return 0;
}

struct at_dumb_buffer *
at_dumb_buffer_create(struct at_device *device, uint16_t width,
		      uint16_t height, uint32_t format)
{
	int ret;
	struct at_dumb_buffer *dumb;
	struct drm_mode_create_dumb create_dumb;
	struct drm_mode_destroy_dumb destroy_dumb;
	struct drm_mode_map_dumb map_dumb;

	dumb = malloc(sizeof(*dumb));
	if (!dumb)
		return NULL;

	dumb->width = width;
	dumb->height = height;

	memset(&create_dumb, 0, sizeof(create_dumb));
	create_dumb.width = width;
	create_dumb.height = height;
	create_dumb.bpp = 32;

	ret = drmIoctl(device->fd, DRM_IOCTL_MODE_CREATE_DUMB, &create_dumb);
	if (ret < 0)
		goto err_create;

	dumb->handle = create_dumb.handle;
	dumb->pitch = create_dumb.pitch;
	dumb->size = create_dumb.size;

	memset(&map_dumb, 0, sizeof(map_dumb));
	map_dumb.handle = dumb->handle;

	ret = drmIoctl(device->fd, DRM_IOCTL_MODE_MAP_DUMB, &map_dumb);
	if (ret < 0)
		goto err_map;

	dumb->data = mmap(NULL, dumb->size, PROT_READ | PROT_WRITE, MAP_SHARED,
			  device->fd, map_dumb.offset);

	if (dumb->data == MAP_FAILED)
		goto err_map;

	memset(dumb->data, 0, dumb->size);

	return dumb;

err_map:
	memset(&destroy_dumb, 0, sizeof(destroy_dumb));
	destroy_dumb.handle = create_dumb.handle;
	drmIoctl(device->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_dumb);
err_create:
	free(dumb);

	return NULL;
}

void
at_dumb_buffer_free(struct at_device *device, struct at_dumb_buffer *dumb)
{
	struct drm_mode_destroy_dumb destroy_dumb;

	munmap(dumb->data, dumb->size);

	memset(&destroy_dumb, 0, sizeof(destroy_dumb));
	destroy_dumb.handle = dumb->handle;
	drmIoctl(device->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_dumb);

	free(dumb);
}

static void
at_dumb_buffer_fill(struct at_dumb_buffer *dumb, uint32_t color)
{
	int i, j;

	for (i = 0; i < dumb->height; i++) {
		uint32_t *pixel = (uint32_t *)(dumb->data + i * dumb->pitch);
		for (j = 0; j < dumb->width; j++)
			pixel[j] = color;
	}
}

struct at_dumb_fb *
at_dumb_fb_create(struct at_device *device, uint16_t width,
		      uint16_t height, uint32_t format)
{
	int ret;
	struct at_dumb_fb *fb;
	uint32_t handles[4] = { 0 }, pitches[4] = { 0 }, offsets[4] = { 0 };

	fb = malloc(sizeof(*fb));
	if (!fb)
		return NULL;

	fb->dumb = at_dumb_buffer_create(device, width, height, format);
	if (!fb->dumb) {
		free(fb);
		return NULL;
	}

	handles[0] = fb->dumb->handle;
	pitches[0] = fb->dumb->pitch;
	offsets[0] = 0;
	ret = drmModeAddFB2(device->fd, width, height, format,
			    handles, pitches, offsets, &fb->fb_id, 0);
	if (ret) {
		at_dumb_buffer_free(device, fb->dumb);
		free(fb);
		return NULL;
	}

	return fb;
}

void
at_dumb_fb_free(struct at_device *device, struct at_dumb_fb *fb)
{
	drmModeRmFB(device->fd, fb->fb_id);
	at_dumb_buffer_free(device, fb->dumb);
	free(fb);
}

int
at_device_mode_set_crtc(struct at_device *device, struct at_dumb_fb *fb)
{
	return drmModeSetCrtc(device->fd, device->crtc_id, fb->fb_id, 0, 0,
			      &device->connector, 1, &device->mode);
}

int
at_device_mode_set_cursor(struct at_device *device, struct at_dumb_buffer *dumb)
{
	return drmModeSetCursor(device->fd, device->crtc_id, dumb->handle,
				dumb->width, dumb->height);
}

int
at_device_mode_move_cursor(struct at_device *device, int x, int y)
{
	return drmModeMoveCursor(device->fd, device->crtc_id, x, y);
}

int
at_device_modeset_restore(struct at_device *device, bool restore_crtc)
{
	int i;
	int ret = 0;

	if (!device->saved_crtc)
		return -1;

	drmModeSetCursor(device->fd, device->crtc_id, 0, 0, 0);

	for (i = 0; i < device->overlays_count; i++)
		drmModeSetPlane(device->fd, device->overlay_ids[i],
				device->crtc_id, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);

	if (restore_crtc)
		ret = drmModeSetCrtc(device->fd, device->saved_crtc->crtc_id,
				     device->saved_crtc->buffer_id,
				     device->saved_crtc->x, device->saved_crtc->y,
				     &device->connector, 1,
				     &device->saved_crtc->mode);

	drmModeFreeCrtc(device->saved_crtc);

	device->saved_crtc = NULL;

	return ret;
}

int
at_device_modeset_save(struct at_device *device)
{
	int ret;

	if (device->saved_crtc) {
		ret = at_device_modeset_restore(device, true);
		if (ret < 0)
			return ret;
	}

	device->saved_crtc = drmModeGetCrtc(device->fd, device->crtc_id);

	return 0;
}

static int
at_libinput_if_open_restricted(const char *path, int flags, void *user_data)
{
	return open(path, flags);
}

static void
at_libinput_if_close_restricted(int fd, void *user_data)
{
	close(fd);
}

static const struct libinput_interface at_libinput_if = {
	.open_restricted = at_libinput_if_open_restricted,
	.close_restricted = at_libinput_if_close_restricted
};

static void
at_instance_li_handle_key_event(struct at_instance *instance, struct libinput_event *ev)
{
	struct libinput_event_keyboard *kev =
		libinput_event_get_keyboard_event(ev);
	if (!kev)
		return;

	switch (libinput_event_keyboard_get_key(kev)) {
	case KEY_Q:
		run = 0;
		break;
	}
}

static void
at_instance_li_handle_pointer_motion(struct at_instance *instance, struct libinput_event *ev)
{
	double dx, dy;
	struct libinput_event_pointer *pev =
		libinput_event_get_pointer_event(ev);
	if (!pev)
		return;

	dx = libinput_event_pointer_get_dx(pev);
	dy = libinput_event_pointer_get_dy(pev);

	instance->cursor_x += dx;
	instance->cursor_y += dy;

	if (instance->cursor_x > instance->device.width - 1)
		instance->cursor_x = instance->device.width - 1;
	else if (instance->cursor_x < 0)
		instance->cursor_x = 0;

	if (instance->cursor_y > instance->device.height - 1)
		instance->cursor_y = instance->device.height - 1;
	else if (instance->cursor_y < 0)
		instance->cursor_y = 0;

	at_device_mode_move_cursor(&instance->device, instance->cursor_x,
				   instance->cursor_y);
}

static int
at_instance_libinput_handle_events(struct at_instance *instance)
{
	struct libinput_event *ev;

	libinput_dispatch(instance->li);

	while ((ev = libinput_get_event(instance->li))) {
		switch (libinput_event_get_type(ev)) {
		case LIBINPUT_EVENT_KEYBOARD_KEY:
			at_instance_li_handle_key_event(instance, ev);
			break;
		case LIBINPUT_EVENT_POINTER_MOTION:
			at_instance_li_handle_pointer_motion(instance, ev);
			break;
		}

		libinput_event_destroy(ev);
	}

	return 0;
}

static int
at_instance_libinput_init(struct at_instance *instance)
{
	struct libinput *li;
	struct udev *udev = udev_new();

	if (!udev) {
		fprintf(stderr, "Failed to initialize udev\n");
		return -1;
	}

	li = libinput_udev_create_context(&at_libinput_if, instance, udev);
	if (!li) {
		fprintf(stderr, "Failed to initialize context from udev\n");
		goto err_li_udev;
	}

	if (libinput_udev_assign_seat(li, "seat0")) {
		fprintf(stderr, "Failed to set seat\n");
		goto err_li_seat;
	}

	instance->li = li;

	at_instance_libinput_handle_events(instance);

	udev_unref(udev);
	return 0;

err_li_seat:
	libinput_unref(li);
err_li_udev:
	udev_unref(udev);
	return -1;
}

static int
at_instance_libinput_close(struct at_instance *instance)
{
	return libinput_unref(instance->li) == NULL;
}

struct at_instance *
at_instance_create(const char *node)
{
	int i, j, k;
	struct at_instance *instance;
	uint64_t cursor_width, cursor_height;

	instance = malloc(sizeof(*instance));
	if (!instance)
		return NULL;

	memset(instance, 0, sizeof(*instance));

	if (at_device_open(&instance->device, node) < 0) {
		fprintf(stderr, "Couldn't initialize %s.\n", node);
		goto err_open;
	}

	drmGetCap(instance->device.fd, DRM_CAP_CURSOR_WIDTH, &cursor_width);
	drmGetCap(instance->device.fd, DRM_CAP_CURSOR_HEIGHT, &cursor_height);

	instance->cursor_buf = at_dumb_buffer_create(&instance->device, cursor_width,
						     cursor_height, DRM_FORMAT_ARGB8888);
	if (!instance->cursor_buf) {
		fprintf(stderr, "Couldn't create the cursor buffer.\n");
		goto err_cursor_buf_create;
	}

	at_dumb_buffer_fill(instance->cursor_buf, 0xFFFF0000);

	for (i = 0; i < ATOMICTEST_NUM_FBS; i++) {
		instance->fbs[i] = at_dumb_fb_create(&instance->device,
						     instance->device.width,
						     instance->device.height,
						     DRM_FORMAT_XRGB8888);
		if (!instance->fbs[i]) {
			fprintf(stderr, "Couldn't create dumb buffer.\n");
			goto err_free_fbs;
		}
	}

	instance->overlay_fbs = calloc(instance->device.overlays_count,
				       sizeof(*instance->overlay_fbs));
	if (!instance->overlay_fbs)
		goto err_free_fbs;

	for (j = 0; j < instance->device.overlays_count; j++) {
		instance->overlay_fbs[j] = at_dumb_fb_create(&instance->device,
							     128,
							     128,
							     DRM_FORMAT_XRGB8888);
		if (!instance->overlay_fbs[j]) {
			fprintf(stderr, "Couldn't create dumb buffer.\n");
			goto err_free_overlays;
		}
	}

	if (at_instance_libinput_init(instance) < 0)
		goto err_free_overlays;

	instance->cur_fb = 0;
	instance->run = true;
	instance->flip_pending = false;
	instance->crtc_changed = false;
	instance->cursor_x = instance->device.width / 2;
	instance->cursor_y = instance->device.height / 2;

	return instance;

err_free_overlays:
	for (k = 0; k < j; k++)
		at_dumb_fb_free(&instance->device, instance->overlay_fbs[k]);
err_free_fbs:
	for (j = 0; j < i; j++)
		at_dumb_fb_free(&instance->device, instance->fbs[j]);
	at_dumb_buffer_free(&instance->device, instance->cursor_buf);
err_cursor_buf_create:
	at_device_close(&instance->device);
err_open:
	free(instance);

	return NULL;
}

int
at_instance_destroy(struct at_instance *instance)
{
	int i;

	at_instance_libinput_close(instance);

	for (i = 0; i < instance->device.overlays_count; i++)
		at_dumb_fb_free(&instance->device, instance->overlay_fbs[i]);

	for (i = 0; i < ATOMICTEST_NUM_FBS; i++)
		at_dumb_fb_free(&instance->device, instance->fbs[i]);

	at_dumb_buffer_free(&instance->device, instance->cursor_buf);

	at_device_close(&instance->device);
}

static void
at_page_flip_handler(int fd, unsigned int sequence, unsigned int tv_sec,
		     unsigned int tv_usec, void *user_data);

int
at_instance_process_events(struct at_instance *instance)
{
	int ret;
	drmEventContext evctx;
	struct pollfd pfds[2];

	memset(&evctx, 0, sizeof(evctx));
	evctx.version = 2;
	evctx.page_flip_handler = at_page_flip_handler;

	memset(pfds, 0, sizeof(pfds));
	pfds[0].fd = instance->device.fd;
	pfds[0].events = POLLIN;

	pfds[1].fd = libinput_get_fd(instance->li);
	pfds[1].events = POLLIN;

	ret = poll(pfds, 2, -1);
	if (ret < 0)
		return ret;

	if (pfds[0].revents & POLLIN) {
		ret = drmHandleEvent(instance->device.fd, &evctx);
		if (ret < 0)
			return ret;
	}

	if (pfds[1].revents & POLLIN)
		at_instance_libinput_handle_events(instance);

	return 0;
}

int
at_instance_stop(struct at_instance *instance)
{
	instance->run = false;
	while (instance->flip_pending) {
		if (at_instance_process_events(instance) < 0)
			break;
	}
}

int
at_instance_modeset_apply(struct at_instance *instance)
{
	int ret = at_device_mode_set_crtc(&instance->device,
					 instance->fbs[instance->cur_fb]);
	if (ret < 0) {
		fprintf(stderr, "Error setting CRTC.\n");
		return ret;
	}

	instance->crtc_changed = true;

	ret = at_device_mode_set_cursor(&instance->device, instance->cursor_buf);
	if (ret < 0)
		fprintf(stderr, "Error setting the cursor.\n");

	ret = at_device_mode_move_cursor(&instance->device, instance->cursor_x,
					 instance->cursor_y);

	return ret;
}

int
at_instance_modeset_save(struct at_instance *instance)
{
	return at_device_modeset_save(&instance->device);
}

int
at_instance_modeset_restore(struct at_instance *instance)
{
	return at_device_modeset_restore(&instance->device, instance->crtc_changed);
}

static void
at_instance_set_overlays(struct at_instance *instance)
{
	static float angle = 0.0f;
	int i, ret;

	for (i = 0; i <  instance->device.overlays_count; i++) {
		struct at_dumb_buffer *dumb = instance->overlay_fbs[i]->dumb;
		uint32_t width = dumb->width;
		uint32_t height = dumb->height;
		float angle_offset = ((M_PI * 2) / instance->device.overlays_count) * i;
		int32_t x = cosf(angle + angle_offset) * 256.0f;
		int32_t y = sinf(angle + angle_offset) * 256.0f;

		at_dumb_buffer_fill(dumb, 0xFF000000 | (0xFF0000 >> (i % 3) * 8));

		/* Under-spec'ed, might block until the vblank occurs! */
		ret = drmModeSetPlane(instance->device.fd,
				instance->device.overlay_ids[i],
				instance->device.crtc_id,
				instance->overlay_fbs[i]->fb_id, 0,
				instance->device.width / 2 + x - width / 2,
				instance->device.height / 2 + y - height / 2,
				width, height,
				0 << 16, 0 << 16,
				width << 16, height << 16);
	}

	angle += 0.1f;
}

static void
at_instance_draw_frame(struct at_instance *instance)
{
	static uint32_t color = 0;
	int ret;
	uint32_t component;
	uint32_t primary_rgb;
	uint32_t cursor_rgb;
	uint32_t next_fb = (instance->cur_fb + 1) % ATOMICTEST_NUM_FBS;

	component = (0xFFlu - abs(color++ % (2 * 0xFFlu) - 0xFFlu));
	primary_rgb = component | component << 16;
	cursor_rgb = ~component;

	at_dumb_buffer_fill(instance->fbs[next_fb]->dumb, 0xFF000000 | primary_rgb);
	at_dumb_buffer_fill(instance->cursor_buf, 0xFF000000 | cursor_rgb);

	at_instance_set_overlays(instance);

	ret = drmModePageFlip(instance->device.fd, instance->device.crtc_id,
			instance->fbs[next_fb]->fb_id,
			DRM_MODE_PAGE_FLIP_EVENT, instance);
	if (!ret) {
		instance->cur_fb = next_fb;
		instance->flip_pending = true;
	}
}

static void
at_page_flip_handler(int fd, unsigned int sequence, unsigned int tv_sec,
		     unsigned int tv_usec, void *user_data)
{
	struct at_instance *instance = user_data;

	instance->flip_pending = false;

	if (instance->run)
		at_instance_draw_frame(instance);
}

int
main(int argc, char *argv[])
{
	struct at_instance *instance;

	signal(SIGINT, sigint_handler);

	printf("Hello from " PACKAGE_NAME ".\n");

	instance = at_instance_create("/dev/dri/card0");
	if (!instance)
		return -1;

	if (at_instance_modeset_save(instance) < 0)
		goto err_modeset_save;

	if (at_instance_modeset_apply(instance) < 0)
		goto err_modeset_apply;

	at_instance_draw_frame(instance);

	while (run) {
		if (at_instance_process_events(instance) < 0)
			break;
	}

	at_instance_stop(instance);
	at_instance_modeset_restore(instance);
	at_instance_destroy(instance);

	return 0;

err_modeset_apply:
	at_instance_modeset_restore(instance);
err_modeset_save:
	at_instance_destroy(instance);

	return -1;
}
