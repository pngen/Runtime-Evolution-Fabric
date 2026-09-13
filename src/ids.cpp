#include "ref/ids.hpp"

#include <cstdio>

namespace ref {
namespace {

constexpr std::uint64_t kFnvOffsetA = 0xCBF29CE484222325ull;
constexpr std::uint64_t kFnvPrimeA = 0x100000001B3ull;
constexpr std::uint64_t kFnvOffsetB = 0x9E3779B97F4A7C15ull;
constexpr std::uint64_t kFnvPrimeB = 0x100000001B3ull;

}  // namespace

bool is_valid_text_char(char c) noexcept {
  const auto value = static_cast<unsigned char>(c);
  return value >= 0x20u && value <= 0x7Eu;
}

bool is_valid_ident_char(char c) noexcept {
  const bool alpha = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
  const bool digit = c >= '0' && c <= '9';
  const bool punct = c == '.' || c == '_' || c == ':' || c == '-' || c == '+';
  return alpha || digit || punct;
}

std::uint64_t hash_bytes(std::string_view bytes) noexcept {
  std::uint64_t h = kFnvOffsetA;
  for (const char c : bytes) {
    h ^= static_cast<unsigned char>(c);
    h *= kFnvPrimeA;
  }
  return h;
}

std::uint64_t hash_mix(std::uint64_t seed, std::uint64_t value) noexcept {
  std::uint64_t h = seed ^ (value + 0x9E3779B97F4A7C15ull + (seed << 6) + (seed >> 2));
  h ^= h >> 33;
  h *= 0xFF51AFD7ED558CCDull;
  h ^= h >> 33;
  h *= 0xC4CEB9FE1A85EC53ull;
  h ^= h >> 33;
  return h;
}

std::optional<VersionNumber> VersionNumber::parse(std::string_view text) noexcept {
  VersionNumber out;
  std::uint16_t* fields[3] = {&out.major, &out.minor, &out.patch};
  std::size_t start = 0;
  for (std::size_t index = 0; index < 3; ++index) {
    const std::size_t dot = text.find('.', start);
    const std::size_t end = (index == 2) ? text.size() : dot;
    if (index < 2 && dot == std::string_view::npos) return std::nullopt;
    if (end <= start || end - start > 5) return std::nullopt;
    std::uint32_t value = 0;
    for (std::size_t i = start; i < end; ++i) {
      const char c = text[i];
      if (c < '0' || c > '9') return std::nullopt;
      value = value * 10u + static_cast<std::uint32_t>(c - '0');
    }
    if (value > 0xFFFFu) return std::nullopt;
    *fields[index] = static_cast<std::uint16_t>(value);
    start = dot == std::string_view::npos ? text.size() : dot + 1;
  }
  return out;
}

std::string VersionNumber::to_string() const {
  std::string out;
  out.reserve(18);
  out += std::to_string(major);
  out += '.';
  out += std::to_string(minor);
  out += '.';
  out += std::to_string(patch);
  return out;
}

IntegrityDigest IntegrityDigest::of(std::string_view bytes) noexcept {
  std::uint64_t a = kFnvOffsetA;
  std::uint64_t b = kFnvOffsetB;
  for (const char c : bytes) {
    const auto value = static_cast<unsigned char>(c);
    a ^= value;
    a *= kFnvPrimeA;
    b ^= value;
    b *= kFnvPrimeB;
    b = (b << 7) | (b >> 57);
  }
  IntegrityDigest digest;
  digest.a = hash_mix(a, bytes.size());
  digest.b = hash_mix(b, bytes.size() ^ 0x5DEECE66Dull);
  return digest;
}

IntegrityDigest IntegrityDigest::combine(IntegrityDigest left, std::string_view bytes) noexcept {
  const IntegrityDigest right = of(bytes);
  IntegrityDigest out;
  out.a = hash_mix(left.a, right.a);
  out.b = hash_mix(left.b, right.b);
  return out;
}

std::string IntegrityDigest::to_hex() const {
  char buffer[33];
  std::snprintf(buffer, sizeof(buffer), "%016llx%016llx", static_cast<unsigned long long>(a),
                static_cast<unsigned long long>(b));
  return std::string(buffer);
}

std::optional<IntegrityDigest> IntegrityDigest::from_hex(std::string_view text) noexcept {
  if (text.size() != 32) return std::nullopt;
  auto parse_part = [](std::string_view part) -> std::optional<std::uint64_t> {
    std::uint64_t value = 0;
    for (const char c : part) {
      std::uint32_t digit = 0;
      if (c >= '0' && c <= '9') {
        digit = static_cast<std::uint32_t>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        digit = static_cast<std::uint32_t>(c - 'a') + 10u;
      } else {
        return std::nullopt;
      }
      value = (value << 4) | digit;
    }
    return value;
  };
  const auto high = parse_part(text.substr(0, 16));
  const auto low = parse_part(text.substr(16, 16));
  if (!high.has_value() || !low.has_value()) return std::nullopt;
  IntegrityDigest out;
  out.a = *high;
  out.b = *low;
  return out;
}

std::string RuntimeVersionId::to_string() const {
  std::string out = number.to_string();
  if (!artifact.build.empty()) {
    out += '+';
    out += artifact.build.str();
  }
  return out;
}

}  // namespace ref
