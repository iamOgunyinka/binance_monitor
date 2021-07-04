#include "crypto.hpp"
#include <boost/algorithm/string.hpp>
#include <limits>
#include <openssl/hmac.h>

namespace binance {

namespace utilities {

std::string base64_encode(std::basic_string<unsigned char> const &bindata) {
  static const char b64_table[65] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

  if (bindata.size() >
      (std::numeric_limits<std::string::size_type>::max() / 4u) * 3u) {
    throw std::length_error("Converting too large a string to base64.");
  }

  std::size_t const binlen = bindata.size();
  // Use = signs so the end is properly padded.
  std::string retval((((binlen + 2) / 3) * 4), '=');
  std::size_t outpos{};
  int bits_collected{};
  unsigned int accumulator{};

  for (auto const &i : bindata) {
    accumulator = (accumulator << 8) | (i & 0xffu);
    bits_collected += 8;
    while (bits_collected >= 6) {
      bits_collected -= 6;
      retval[outpos++] = b64_table[(accumulator >> bits_collected) & 0x3fu];
    }
  }

  if (bits_collected > 0) { // Any trailing bits that are missing.
    assert(bits_collected < 6);
    accumulator <<= 6 - bits_collected;
    retval[outpos++] = b64_table[accumulator & 0x3fu];
  }
  assert(outpos >= (retval.size() - 2));
  assert(outpos <= retval.size());
  return retval;
}

std::string base64_encode(std::string const &bindata) {
  std::basic_string<unsigned char> temp{};
  temp.resize(bindata.size());
  for (int i = 0; i < bindata.size(); ++i) {
    temp[i] = static_cast<unsigned char>(bindata[i]);
  }
  return base64_encode(temp);
}

std::string base64_decode(std::string const &asc_data) {
  static char const reverse_table[128]{
      64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
      64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
      64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 62, 64, 64, 64, 63,
      52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 64, 64, 64, 64, 64, 64,
      64, 0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14,
      15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 64, 64, 64, 64, 64,
      64, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
      41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 64, 64, 64, 64, 64};
  std::string ret_val{};
  int bits_collected{};
  unsigned int accumulator{};

  for (auto const &c : asc_data) {
    if (::std::isspace(c) || c == '=') {
      // Skip whitespace and padding. Be liberal in what you accept.
      continue;
    }
    if ((c > 127) || (c < 0) || (reverse_table[c] > 63)) {
      throw ::std::invalid_argument(
          "This contains characters not legal in a base64 encoded string.");
    }
    accumulator = (accumulator << 6) | reverse_table[c];
    bits_collected += 6;
    if (bits_collected >= 8) {
      bits_collected -= 8;
      ret_val += static_cast<char>((accumulator >> bits_collected) & 0xFFu);
    }
  }
  return ret_val;
}

std::basic_string<unsigned char> hmac256_encode(std::string const &data,
                                                std::string const &key) {
  HMAC_CTX *ctx = HMAC_CTX_new();
  HMAC_Init_ex(ctx, key.c_str(), key.size(), EVP_sha256(), nullptr);
  unsigned int len{};
  unsigned char out[EVP_MAX_MD_SIZE];
  HMAC_Init(ctx, key.c_str(), key.length(), EVP_sha256());
  HMAC_Update(ctx, (unsigned char *)data.c_str(), data.length());
  HMAC_Final(ctx, out, &len);
  HMAC_CTX_free(ctx);

  return std::basic_string<unsigned char>(out, len);
}

} // namespace utilities
} // namespace binance
