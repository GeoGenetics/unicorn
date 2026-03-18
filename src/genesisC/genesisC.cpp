#include "genesis/genesis.hpp"
#include <cstdint>
#include <cstring>

extern "C" {

  typedef void* genesis_encoder_t;

  genesis_encoder_t genesis_encoderinit(int k)
  {
    return new genesis::sequence::MinimalCanonicalEncoding(k);
  }

  void genesis_encoderfree(genesis_encoder_t enc)
  {
    delete static_cast<genesis::sequence::MinimalCanonicalEncoding*>(enc);
  }

  uint64_t genesis_getcamexidx(genesis_encoder_t enc, const char *kmerstr, int k, uint8_t *ret)
  {
    *ret = 1;
    using namespace genesis::sequence;
    auto* encoder = static_cast<MinimalCanonicalEncoding*>(enc);
    Kmer kmer;
    try {
      kmer = kmer_from_string(std::string(kmerstr, k));
    } catch (const std::exception &e) {
      return 0;
    }
    set_reverse_complement(kmer);
    *ret = 0;
    return encoder->encode(kmer);
  }
}
