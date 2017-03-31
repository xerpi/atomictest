#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <signal.h>
#include <config.h>

#define ATOMICTEST_NUM_FBS 2

struct at_device {
	int fd;

	drmModeModeInfo mode;
	uint16_t width;
	uint16_t height;

	uint32_t connector;
	uint32_t crtc;

	drmModeCrtc *saved_crtc;
};

struct at_dumb_buffer {
	uint32_t width;
	uint32_t height;

	uint32_t handle;
	uint32_t pitch;
	uint64_t size;

	uint32_t fb;

	uint8_t *data;
};

struct at_instance {
	struct at_device device;
	struct at_dumb_buffer *fbs[ATOMICTEST_NUM_FBS];

	uint32_t cur_fb;
	bool run;
	bool flip_pending;
};

static bool run = true;

static void sigint_handler(int sig)
{
	run = false;
}

static int setup_device(struct at_device *device, drmModeRes *resources,
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
			device->crtc = encoder->crtc_id;
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

			device->crtc = resources->crtcs[j];
			drmModeFreeEncoder(encoder);
			return 0;
		}

		drmModeFreeEncoder(encoder);
	}

	return -1;
}

int at_device_open(struct at_device *device, const char *node)
{
	int i;
	int fd;
	int ret;
	uint64_t cap_dumb;
	drmModeRes *resources;

	fd = open(node, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		perror("Could not open input file");
		return -1;
	}

	ret = drmGetCap(fd, DRM_CAP_DUMB_BUFFER, &cap_dumb);
	if (ret < 0 || !cap_dumb) {
		fprintf(stderr, "drm: device doesn't support dumb buffers\n");
		close(fd);
		return -1;
	}

	resources = drmModeGetResources(fd);
	if (!resources) {
		fprintf(stderr, "drm: can't get mode resources\n");
		close(fd);
		return -1;
	}

	device->fd = fd;

	printf("Device fbs: %d\n", resources->count_fbs);
	printf("Device crtcs: %d\n", resources->count_crtcs);
	printf("Device encoders: %d\n", resources->count_encoders);
	printf("Device connectors: %d\n", resources->count_connectors);

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

		memcpy(&device->mode, &connector->modes[0], sizeof(device->mode));
		device->width = connector->modes[0].hdisplay;
		device->height = connector->modes[0].vdisplay;
		device->connector = connector->connector_id;
		device->saved_crtc = NULL;

		drmModeFreeConnector(connector);
		drmModeFreeResources(resources);

		return 0;
	}

	drmModeFreeResources(resources);
	close(fd);

	return -1;
}

int at_device_close(struct at_device *device)
{
	close(device->fd);
	return 0;
}

struct at_dumb_buffer *
at_dumb_buffer_create(struct at_device *device, uint16_t width,
		      uint16_t height)
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

	ret = drmModeAddFB(device->fd, width, height, 24, 32, dumb->pitch,
			   dumb->handle, &dumb->fb);
	if (ret)
		goto err_add;

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
	drmModeRmFB(device->fd, dumb->fb);
err_add:
	memset(&destroy_dumb, 0, sizeof(destroy_dumb));
	destroy_dumb.handle = create_dumb.handle;
	drmIoctl(device->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_dumb);
err_create:
	free(dumb);

	return NULL;
}

void at_dumb_buffer_free(struct at_device *device, struct at_dumb_buffer *dumb)
{
	struct drm_mode_destroy_dumb destroy_dumb;

	munmap(dumb->data, dumb->size);

	drmModeRmFB(device->fd, dumb->fb);

	memset(&destroy_dumb, 0, sizeof(destroy_dumb));
	destroy_dumb.handle = dumb->handle;
	drmIoctl(device->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_dumb);

	free(dumb);
}

int at_device_modeset_crtc(struct at_device *device, struct at_dumb_buffer *dumb)
{
	return drmModeSetCrtc(device->fd, device->crtc, dumb->fb, 0, 0,
			      &device->connector, 1, &device->mode);
}

int at_device_modeset_restore(struct at_device *device, bool set_crtc)
{
	int ret = 0;

	if (!device->saved_crtc)
		return -1;

	if (set_crtc)
		ret = drmModeSetCrtc(device->fd, device->saved_crtc->crtc_id,
				     device->saved_crtc->buffer_id,
				     device->saved_crtc->x, device->saved_crtc->y,
				     &device->connector, 1,
				     &device->saved_crtc->mode);

	drmModeFreeCrtc(device->saved_crtc);

	device->saved_crtc = NULL;

	return ret;
}

int at_device_modeset_backup(struct at_device *device)
{
	int ret;

	if (device->saved_crtc) {
		ret = at_device_modeset_restore(device, true);
		if (ret < 0)
			return ret;
	}

	device->saved_crtc = drmModeGetCrtc(device->fd, device->crtc);

	return 0;
}

struct at_instance *
at_instance_create(const char *node)
{
	int i, j;
	struct at_instance *instance;

	instance = malloc(sizeof(*instance));
	if (!instance)
		return NULL;

	memset(instance, 0, sizeof(*instance));

	if (at_device_open(&instance->device, node) < 0) {
		fprintf(stderr, "Couldn't initialize %s.\n", node);
		goto err_open;
	}

	for (i = 0; i < ATOMICTEST_NUM_FBS; i++) {
		instance->fbs[i] = at_dumb_buffer_create(&instance->device,
							 instance->device.width,
							 instance->device.height);
		if (!instance->fbs[i]) {
			fprintf(stderr, "Couldn't create dumb buffer.\n");
			goto err_dumb_create;
		}
	}

	instance->cur_fb = 0;
	instance->run = true;
	instance->flip_pending = false;

	return instance;

err_dumb_create:
	for (j = 0; j < i; j++)
		at_dumb_buffer_free(&instance->device, instance->fbs[j]);

	at_device_close(&instance->device);
err_open:
	free(instance);

	return NULL;
}

int at_instance_destroy(struct at_instance *instance)
{
	int i;

	for (i = 0; i < ATOMICTEST_NUM_FBS; i++)
		at_dumb_buffer_free(&instance->device, instance->fbs[i]);

	at_device_close(&instance->device);
}

static void at_page_flip_handler(int fd, unsigned int sequence,
				 unsigned int tv_sec, unsigned int tv_usec,
				 void *user_data);

int at_instance_process_events(struct at_instance *instance)
{
	int ret;
	drmEventContext evctx;
	struct pollfd pfd;

	memset(&evctx, 0, sizeof(evctx));
	evctx.version = DRM_EVENT_CONTEXT_VERSION;
	evctx.page_flip_handler = at_page_flip_handler;

	pfd.fd = instance->device.fd;
	pfd.events = POLLIN;

	ret = poll(&pfd, 1, -1);
	if (ret < 0)
		return ret;

	if (pfd.revents & POLLIN) {
		ret = drmHandleEvent(instance->device.fd, &evctx);
		if (ret < 0)
			return ret;
	}

	return 0;
}

int at_instance_stop(struct at_instance *instance)
{
	instance->run = false;
	while (instance->flip_pending) {
		if (at_instance_process_events(instance) < 0)
			break;
	}
}

int at_instance_modeset_apply(struct at_instance *instance)
{
	return at_device_modeset_crtc(&instance->device,
				      instance->fbs[instance->cur_fb]);
}

int at_instance_modeset_backup(struct at_instance *instance)
{
	return at_device_modeset_backup(&instance->device);
}

int at_instance_modeset_restore(struct at_instance *instance, bool set_crtc)
{
	return at_device_modeset_restore(&instance->device, set_crtc);
}

static void fill_dumb(struct at_dumb_buffer *dumb, uint32_t color)
{
	int i, j;

	for (i = 0; i < dumb->height; i++) {
		uint32_t *pixel = (uint32_t *)(dumb->data + i * dumb->pitch);
		for (j = 0; j < dumb->width; j++)
			pixel[j] = color;
	}
}

static void at_draw_frame(struct at_instance *instance)
{
	static const uint32_t colors[] = {
		0xFF0000, 0x00FF00, 0x0000FF
	};

	static int c = 0;
	int ret;
	uint32_t next_fb = (instance->cur_fb + 1) % ATOMICTEST_NUM_FBS;

	fill_dumb(instance->fbs[next_fb], colors[c]);

	c = (c + 1) % (sizeof(colors) / sizeof(*colors));

	ret = drmModePageFlip(instance->device.fd, instance->device.crtc,
			instance->fbs[next_fb]->fb,
			DRM_MODE_PAGE_FLIP_EVENT, instance);
	if (!ret) {
		instance->cur_fb = next_fb;
		instance->flip_pending = true;
	}
}

static void at_page_flip_handler(int fd, unsigned int sequence,
				 unsigned int tv_sec, unsigned int tv_usec,
				 void *user_data)
{
	struct at_instance *instance = user_data;

	instance->flip_pending = false;

	if (instance->run)
		at_draw_frame(instance);
}

int main(int argc, char *argv[])
{
	struct at_instance *instance;

	signal(SIGINT, sigint_handler);

	printf("Hello from " PACKAGE_NAME ".\n");

	instance = at_instance_create("/dev/dri/card0");
	if (!instance)
		return -1;

	if (at_instance_modeset_backup(instance) < 0)
		goto err_modeset_backup;

	if (at_instance_modeset_apply(instance) < 0)
		goto err_modeset_apply;

	at_draw_frame(instance);

	while (run) {
		if (at_instance_process_events(instance) < 0)
			break;
	}

	at_instance_stop(instance);
	at_instance_modeset_restore(instance, true);
	at_instance_destroy(instance);

	return 0;

err_modeset_apply:
	at_instance_modeset_restore(instance, false);
err_modeset_backup:
	at_instance_destroy(instance);

	return -1;
}
