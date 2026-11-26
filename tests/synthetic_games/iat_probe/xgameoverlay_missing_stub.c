#include <stdint.h>

uint64_t XOverlayShowNotice(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    return a ^ b ^ c ^ d;
}
