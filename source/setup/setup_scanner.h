#pragma once

#include <stdbool.h>

#include "setup_qr.h"

/* Blocking setup scanner. Owns the camera and top framebuffer until it returns.
 * B cancels. */
bool setup_scanner_run(setup_credentials *out, char *err, int errlen);
