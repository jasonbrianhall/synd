/* sha1.h - SHA-1 (FIPS 180-1), used only to check that an original executable is the one the code was
 * recompiled from. */
#pragma once
#include <stddef.h>
#include <stdint.h>

void sha1_hex(const void *data, size_t n, char out[41]);
