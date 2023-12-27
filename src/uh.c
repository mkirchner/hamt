#include "uh.h"

/* Sedgewick universal hash from Sedgewick R, "Algorithms in C" Third
 * Edition, 1998, p. 579. Works on null-terminated C strings.
 *
 * Best hash function for 32-ary trees according to Bagwell P, "Ideal
 * Hash Trees" (in comparison with Elf and PJW hash).
 *
 * And indeed, comparative experiments w/ murmur3 show more consistent and
 * smaller max tree depths. For use in HAMT, it is importatnt to choose M
 * large enough since the probability of two nonequal keys to collide is
 * approximately 1/M.
 */

uint32_t sedgewick_universal_hash(const char *str, uint32_t M)
{
    uint32_t h;
    uint32_t a = 31415, b = 27183;
    for (h = 0; *str != '\0'; ++str, a = a * b % (M - 1))
        h = (a * h + *str) % M;
    return h;
}
