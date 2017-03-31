#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <config.h>

struct at_device {
	int fd;

	uint16_t width;
	uint16_t height;

	uint32_t connector;
	uint32_t crtc;
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

		device->connector = connector->connector_id;
		device->width = connector->modes[0].hdisplay;
		device->height = connector->modes[0].vdisplay;

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

int main(int argc, char *argv[])
{
	struct at_device dev;
	struct at_dumb_buffer *dumb;

	printf("Hello from " PACKAGE_NAME ".\n");

	if (at_device_open(&dev, "/dev/dri/card0") < 0) {
		fprintf(stderr, "Couldn't initialize card0\n");
		return -1;
	}

	printf("Size: (%d, %d)\n", dev.width, dev.height);

	dumb = at_dumb_buffer_create(&dev, 128, 128);
	if (!dumb) {
		fprintf(stderr, "Couldn't create dumb buffer\n");
		at_device_close(&dev);
		return -1;
	}

	at_dumb_buffer_free(&dev, dumb);

	at_device_close(&dev);

	return 0;
}
