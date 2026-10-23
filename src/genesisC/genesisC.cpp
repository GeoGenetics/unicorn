#include "genesis/genesis.hpp"
#include <cstdint>
#include <cstring>

extern "C" {

  typedef void* genesis_encoder_t;

  genesis_encoder_t genesis_encoderinit(int k)
  {
    try { return new genesis::sequence::MinimalCanonicalEncoding(k); }
    catch (const std::exception &) { return nullptr; }
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

extern "C" void genesis_getcamexidx_bulk(genesis_encoder_t enc,
    const char *sequence, int length, int k, uint64_t *indices)
{
  if (!enc || !sequence || !indices || k < 1 || k > 32 || length < k) return;
  using namespace genesis::sequence;
  auto *encoder = static_cast<MinimalCanonicalEncoding *>(enc);
  uint64_t forward = 0, reverse = 0;
  uint64_t mask = k == 32 ? UINT64_MAX : ((uint64_t(1) << (2*k)) - 1);
  int valid = 0;
  for (int i = 0; i < length; ++i) {
    int rank;
    switch (sequence[i]) {
      case 'A': case 'a': rank = 0; break;
      case 'C': case 'c': rank = 1; break;
      case 'G': case 'g': rank = 2; break;
      case 'T': case 't': rank = 3; break;
      default: rank = -1;
    }
    if (rank < 0) { valid = 0; forward = reverse = 0; }
    else {
      forward = ((forward << 2) | rank) & mask;
      reverse = (reverse >> 2) | (uint64_t(3-rank) << (2*(k-1)));
      ++valid;
    }
    if (i < k-1) continue;
    if (valid < k) { indices[i-k+1] = 0; continue; }
    Kmer value(k, forward);
    value.rev_comp = reverse;
    indices[i-k+1] = encoder->encode(value);
  }
}
