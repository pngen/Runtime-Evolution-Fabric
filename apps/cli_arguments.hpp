// Shared command-line parsing for the Runtime Evolution Fabric tools.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <map>
#include <vector>

#include "ref/ids.hpp"
#include "ref/support.hpp"
#include "ref/wire.hpp"

namespace ref::cli {

// Minimal, dependency-free option parser: "--name value" and "--flag".
class Arguments {
 public:
  Arguments(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
      std::string token = argv[i];
      if (token.size() > 2 && token[0] == '-' && token[1] == '-') {
        std::string name = token.substr(2);
        const std::size_t equals = name.find('=');
        if (equals != std::string::npos) {
          values_[name.substr(0, equals)] = name.substr(equals + 1);
          continue;
        }
        if (i + 1 < argc && !(argv[i + 1][0] == '-' && argv[i + 1][1] == '-' && argv[i + 1][2] != '\0')) {
          values_[name] = argv[++i];
        } else {
          flags_.push_back(name);
          values_[name] = "1";
        }
      } else {
        positional_.push_back(std::move(token));
      }
    }
  }

  [[nodiscard]] bool has(std::string_view name) const { return values_.find(std::string(name)) != values_.end(); }

  [[nodiscard]] std::string get(std::string_view name, std::string fallback = {}) const {
    const auto it = values_.find(std::string(name));
    return it == values_.end() ? std::move(fallback) : it->second;
  }

  [[nodiscard]] std::uint64_t number(std::string_view name, std::uint64_t fallback = 0) const {
    const auto it = values_.find(std::string(name));
    if (it == values_.end()) return fallback;
    bool ok = false;
    const std::uint64_t value = parse_u64(it->second, ok);
    return ok ? value : fallback;
  }

  [[nodiscard]] const std::vector<std::string>& positional() const { return positional_; }
  [[nodiscard]] const std::vector<std::string>& flags() const { return flags_; }

 private:
  std::map<std::string, std::string> values_{};
  std::vector<std::string> flags_{};
  std::vector<std::string> positional_{};
};

template <class Tag, std::uint16_t Capacity>
[[nodiscard]] Ident<Tag, Capacity> ident_arg(const std::string& value) {
  return Ident<Tag, Capacity>::from_valid(value);
}

[[nodiscard]] inline GenerationSet<ProtocolGenerationTag> protocol_set(const std::string& csv) {
  GenerationSet<ProtocolGenerationTag> set;
  (void)decode_generation_set(csv, set);
  return set;
}

[[nodiscard]] inline GenerationSet<SchemaGenerationTag> schema_set(const std::string& csv) {
  GenerationSet<SchemaGenerationTag> set;
  (void)decode_generation_set(csv, set);
  return set;
}

}  // namespace ref::cli
