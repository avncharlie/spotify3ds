#include "art_scale.h"
#include <string.h>

bool art_fit_dimensions(unsigned width, unsigned height, unsigned cap,
                        unsigned *out_width, unsigned *out_height)
{
	if (!width || !height || !cap || cap > 256 || width > 65535 || height > 65535)
		return false;
	unsigned largest = width > height ? width : height;
	*out_width = width;
	*out_height = height;
	if (largest > cap) {
		*out_width = (unsigned)((uint64_t)width * cap / largest);
		*out_height = (unsigned)((uint64_t)height * cap / largest);
		if (!*out_width)
			*out_width = 1;
		if (!*out_height)
			*out_height = 1;
	}
	return true;
}

void art_scale_row_rgba(const uint8_t *source, unsigned source_width,
                        uint8_t *target, unsigned target_width)
{
	for (unsigned x = 0; x < target_width; x++) {
		unsigned src_x = (unsigned)((uint64_t)x * source_width / target_width);
		memcpy(target + (size_t)x * 4, source + (size_t)src_x * 4, 4);
	}
}
