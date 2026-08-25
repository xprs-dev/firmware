/* TweetNaCl's one external need: randombytes(). Hardware RNG on ESP-IDF.
 * The host test harness compiles its own (test_rns_host.c). */
#include <stddef.h>
#include "esp_random.h"

void randombytes(unsigned char *p, unsigned long long n)
{
    esp_fill_random(p, (size_t)n);
}
