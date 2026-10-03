#pragma once
#include <stdbool.h>
#include <stdint.h>

bool art_fit_dimensions(unsigned width, unsigned height, unsigned cap,
                        unsigned *out_width, unsigned *out_height);
void art_scale_row_rgba(const uint8_t *source, unsigned source_width,
                        uint8_t *target, unsigned target_width);
