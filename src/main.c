#include <stdio.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <config.h>

int main(int argc, char *argv[])
{
	int i;
	int fd;
	int ret;
	uint64_t cap_dumb;
	drmModeRes *mode_res;

	printf("Hello from " PACKAGE_NAME ".\n");

	fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
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

	mode_res = drmModeGetResources(fd);
	if (!mode_res) {
		fprintf(stderr, "drm: can't get mode resources\n");
		close(fd);
		return -1;
	}

	printf("Device fbs: %d\n", mode_res->count_fbs);
	printf("Device crtcs: %d\n", mode_res->count_crtcs);
	printf("Device encoders: %d\n", mode_res->count_encoders);
	printf("Device connectors: %d\n", mode_res->count_connectors);

	for (i = 0; i < mode_res->count_connectors; i++) {
		drmModeConnector *mode_conn;

		printf("Connector %d\n", i);

		mode_conn = drmModeGetConnector(fd, mode_res->connectors[i]);
		if (!mode_conn) {
			fprintf(stderr, "drm: can't get mode connector %d\n",
				mode_res->connectors[i]);
			continue;
		}

		drmModeFreeConnector(mode_conn);

	}

	drmModeFreeResources(mode_res);

	close(fd);

	return 0;
}
