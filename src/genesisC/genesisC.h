#include <stdint.h>

typedef void* genesis_encoder_t;

genesis_encoder_t  genesis_encoderinit(int);
void               genesis_encoderfree(genesis_encoder_t enc);
uint64_t           genesis_getcamexidx(genesis_encoder_t enc,
                                       const char *kmerstr,
                                       int k,
                                       uint8_t *ret);
