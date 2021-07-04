#pragma once

#include <boost/utility/string_view.hpp>
#include <optional>
#include <string>
#include <vector>

namespace binance {

namespace utilities {

std::string base64_encode(std::basic_string<unsigned char> const &binary_data);
std::string base64_encode(std::string const &binary_data);
std::string base64_decode(std::string const &asc_data);
std::string get_random_string(std::size_t const length);
std::size_t get_random_integer();
std::string decode_url(boost::string_view const &encoded_string);
std::vector<boost::string_view> split_string_view(boost::string_view const &str,
                                                  char const *delim);
std::basic_string<unsigned char> hmac256_encode(std::string const &data,
                                                std::string const &key);
} // namespace utilities

} // namespace binance
