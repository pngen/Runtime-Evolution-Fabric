#include "ref/support.hpp"

#include <array>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

namespace ref {
namespace {

constexpr std::array<std::uint32_t, 256> build_crc_table() noexcept {
  std::array<std::uint32_t, 256> table{};
  for (std::uint32_t i = 0; i < 256; ++i) {
    std::uint32_t value = i;
    for (int bit = 0; bit < 8; ++bit) {
      value = (value & 1u) != 0u ? (value >> 1) ^ 0xEDB88320u : (value >> 1);
    }
    table[i] = value;
  }
  return table;
}

constexpr std::array<std::uint32_t, 256> kCrcTable = build_crc_table();

void write_le(std::vector<std::uint8_t>& out, std::uint64_t value, std::size_t width) {
  for (std::size_t i = 0; i < width; ++i) {
    out.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFu));
  }
}

}  // namespace

const char* to_string(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::Ok: return "OK";
    case ErrorCode::InvalidArgument: return "INVALID_ARGUMENT";
    case ErrorCode::NotFound: return "NOT_FOUND";
    case ErrorCode::AlreadyExists: return "ALREADY_EXISTS";
    case ErrorCode::LimitExceeded: return "LIMIT_EXCEEDED";
    case ErrorCode::StaleEpoch: return "STALE_EPOCH";
    case ErrorCode::StaleBoot: return "STALE_BOOT";
    case ErrorCode::StaleGeneration: return "STALE_GENERATION";
    case ErrorCode::StaleEvidence: return "STALE_EVIDENCE";
    case ErrorCode::StalePlan: return "STALE_PLAN";
    case ErrorCode::Unauthorized: return "UNAUTHORIZED";
    case ErrorCode::RetiredGeneration: return "RETIRED_GENERATION";
    case ErrorCode::Incompatible: return "INCOMPATIBLE";
    case ErrorCode::Conflict: return "CONFLICT";
    case ErrorCode::Corrupt: return "CORRUPT";
    case ErrorCode::Truncated: return "TRUNCATED";
    case ErrorCode::IntegrityFailure: return "INTEGRITY_FAILURE";
    case ErrorCode::IoFailure: return "IO_FAILURE";
    case ErrorCode::Unsupported: return "UNSUPPORTED";
    case ErrorCode::Busy: return "BUSY";
    case ErrorCode::QueueFull: return "QUEUE_FULL";
    case ErrorCode::Retired: return "RETIRED";
    case ErrorCode::OutcomeUnknown: return "OUTCOME_UNKNOWN";
    case ErrorCode::Internal: return "INTERNAL";
  }
  return "INTERNAL";
}

std::optional<ErrorCode> parse_error_code(std::string_view text) noexcept {
  for (std::uint8_t i = 0; i <= static_cast<std::uint8_t>(ErrorCode::Internal); ++i) {
    const auto code = static_cast<ErrorCode>(i);
    if (text == to_string(code)) return code;
  }
  return std::nullopt;
}

std::string Status::to_string() const {
  std::string out = ref::to_string(code_);
  if (!detail_.empty()) {
    out += ": ";
    out += detail_.view();
  }
  return out;
}

const char* to_string(EvidenceClass value) noexcept {
  switch (value) {
    case EvidenceClass::Unknown: return "UNKNOWN";
    case EvidenceClass::Real: return "REAL";
    case EvidenceClass::Synthetic: return "SYNTHETIC";
    case EvidenceClass::Unsupported: return "UNSUPPORTED";
  }
  return "UNKNOWN";
}

std::optional<EvidenceClass> parse_evidence_class(std::string_view text) noexcept {
  if (text == "REAL") return EvidenceClass::Real;
  if (text == "SYNTHETIC") return EvidenceClass::Synthetic;
  if (text == "UNSUPPORTED") return EvidenceClass::Unsupported;
  if (text == "UNKNOWN") return EvidenceClass::Unknown;
  return std::nullopt;
}

bool checked_add(std::uint64_t a, std::uint64_t b, std::uint64_t& out) noexcept {
  if (a > kGenerationCeiling - b) return false;
  out = a + b;
  return true;
}

bool checked_mul(std::uint64_t a, std::uint64_t b, std::uint64_t& out) noexcept {
  if (a == 0 || b == 0) {
    out = 0;
    return true;
  }
  if (a > kGenerationCeiling / b) return false;
  out = a * b;
  return true;
}

std::uint32_t crc32(std::string_view bytes) noexcept {
  std::uint32_t crc = 0xFFFFFFFFu;
  for (const char c : bytes) {
    crc = kCrcTable[(crc ^ static_cast<std::uint8_t>(c)) & 0xFFu] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFFu;
}

// ---------------------------------------------------------------------------
// ByteWriter
// ---------------------------------------------------------------------------
void ByteWriter::u8(std::uint8_t value) { bytes_.push_back(value); }
void ByteWriter::u16(std::uint16_t value) { write_le(bytes_, value, 2); }
void ByteWriter::u32(std::uint32_t value) { write_le(bytes_, value, 4); }
void ByteWriter::u64(std::uint64_t value) { write_le(bytes_, value, 8); }

void ByteWriter::raw(std::string_view bytes) {
  bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
}

void ByteWriter::text(std::string_view value, std::size_t max_length) {
  const std::size_t length = value.size() > max_length ? max_length : value.size();
  u32(static_cast<std::uint32_t>(length));
  raw(value.substr(0, length));
}

void ByteWriter::digest(IntegrityDigest value) {
  u64(value.a);
  u64(value.b);
}

void ByteWriter::patch_u32(std::size_t offset, std::uint32_t value) {
  if (offset + 4 > bytes_.size()) return;
  for (std::size_t i = 0; i < 4; ++i) {
    bytes_[offset + i] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFu);
  }
}

// ---------------------------------------------------------------------------
// ByteReader
// ---------------------------------------------------------------------------
bool ByteReader::need(std::size_t count) noexcept {
  if (failed_) return false;
  if (static_cast<std::size_t>(end_ - cursor_) < count) {
    failed_ = true;
    return false;
  }
  return true;
}

std::uint8_t ByteReader::u8() noexcept {
  if (!need(1)) return 0;
  return static_cast<std::uint8_t>(*cursor_++);
}

std::uint16_t ByteReader::u16() noexcept {
  if (!need(2)) return 0;
  std::uint16_t value = 0;
  for (int i = 0; i < 2; ++i) {
    value = static_cast<std::uint16_t>(value | (static_cast<std::uint16_t>(static_cast<std::uint8_t>(*cursor_++)) << (8 * i)));
  }
  return value;
}

std::uint32_t ByteReader::u32() noexcept {
  if (!need(4)) return 0;
  std::uint32_t value = 0;
  for (int i = 0; i < 4; ++i) {
    value |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(*cursor_++)) << (8 * i);
  }
  return value;
}

std::uint64_t ByteReader::u64() noexcept {
  if (!need(8)) return 0;
  std::uint64_t value = 0;
  for (int i = 0; i < 8; ++i) {
    value |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(*cursor_++)) << (8 * i);
  }
  return value;
}

bool ByteReader::skip(std::size_t count) noexcept {
  if (!need(count)) return false;
  cursor_ += count;
  return true;
}

std::string_view ByteReader::bytes(std::size_t max_length) noexcept {
  const std::uint32_t length = u32();
  if (failed_) return {};
  if (length > max_length || !need(length)) {
    failed_ = true;
    return {};
  }
  const char* start = cursor_;
  cursor_ += length;
  return {start, length};
}

IntegrityDigest ByteReader::digest() noexcept {
  IntegrityDigest value;
  value.a = u64();
  value.b = u64();
  return value;
}

// ---------------------------------------------------------------------------
// JsonWriter
// ---------------------------------------------------------------------------
void JsonWriter::separate() {
  if (!stack_.empty()) {
    if (pending_key_) {
      pending_key_ = false;
      return;
    }
    if (!first_.back()) out_ += ',';
    first_.back() = false;
    if (stack_.back() == 0) {
      if (pretty_) out_ += '\n';
      indent();
    }
  }
}

void JsonWriter::indent() {
  if (!pretty_) return;
  for (std::size_t i = 0; i < depth_; ++i) out_ += "  ";
}

JsonWriter& JsonWriter::begin_object() {
  separate();
  out_ += '{';
  stack_.push_back(0);
  first_.push_back(true);
  ++depth_;
  return *this;
}

JsonWriter& JsonWriter::end_object() {
  if (stack_.empty() || stack_.back() != 0) {
    well_formed_ = false;
    return *this;
  }
  const bool was_empty = first_.back();
  stack_.pop_back();
  first_.pop_back();
  if (depth_ > 0) --depth_;
  if (!was_empty) {
    if (pretty_) out_ += '\n';
    indent();
  }
  out_ += '}';
  return *this;
}

JsonWriter& JsonWriter::begin_array() {
  separate();
  out_ += '[';
  stack_.push_back(1);
  first_.push_back(true);
  ++depth_;
  return *this;
}

JsonWriter& JsonWriter::end_array() {
  if (stack_.empty() || stack_.back() != 1) {
    well_formed_ = false;
    return *this;
  }
  const bool was_empty = first_.back();
  stack_.pop_back();
  first_.pop_back();
  if (depth_ > 0) --depth_;
  if (!was_empty) {
    if (pretty_) out_ += '\n';
    indent();
  }
  out_ += ']';
  return *this;
}

JsonWriter& JsonWriter::key(std::string_view name) {
  separate();
  out_ += json_escape(name);
  out_ += ": ";
  pending_key_ = true;
  return *this;
}

JsonWriter& JsonWriter::string(std::string_view value) {
  separate();
  out_ += json_escape(value);
  return *this;
}

JsonWriter& JsonWriter::number(std::uint64_t value) {
  separate();
  out_ += std::to_string(value);
  return *this;
}

JsonWriter& JsonWriter::number(std::int64_t value) {
  separate();
  out_ += std::to_string(value);
  return *this;
}

JsonWriter& JsonWriter::boolean(bool value) {
  separate();
  out_ += value ? "true" : "false";
  return *this;
}

JsonWriter& JsonWriter::null() {
  separate();
  out_ += "null";
  return *this;
}

JsonWriter& JsonWriter::field(std::string_view name, std::string_view value) {
  key(name);
  return string(value);
}

JsonWriter& JsonWriter::field(std::string_view name, std::uint64_t value) {
  key(name);
  return number(value);
}

JsonWriter& JsonWriter::field(std::string_view name, std::int64_t value) {
  key(name);
  return number(value);
}

JsonWriter& JsonWriter::field(std::string_view name, bool value) {
  key(name);
  return boolean(value);
}

JsonWriter& JsonWriter::field_json(std::string_view name, const std::string& raw_json) {
  key(name);
  separate();
  out_ += raw_json;
  return *this;
}

JsonWriter& JsonWriter::close_all() {
  while (!stack_.empty()) {
    if (stack_.back() == 0) {
      end_object();
    } else {
      end_array();
    }
  }
  return *this;
}

std::string json_escape(std::string_view value) {
  std::string out;
  out.reserve(value.size() + 2);
  out += '"';
  for (const char c : value) {
    const auto byte = static_cast<unsigned char>(c);
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (byte < 0x20u) {
          char buffer[7];
          std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned>(byte));
          out += buffer;
        } else {
          out += c;
        }
        break;
    }
  }
  out += '"';
  return out;
}

std::uint64_t parse_u64(std::string_view text, bool& ok) noexcept {
  ok = false;
  if (text.empty() || text.size() > 20) return 0;
  std::uint64_t value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') return 0;
    const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
    if (value > (kGenerationCeiling / 10)) return 0;
    value = value * 10 + digit;
    if (value > kGenerationCeiling) return 0;
  }
  ok = true;
  return value;
}

// ---------------------------------------------------------------------------
// Filesystem helpers
// ---------------------------------------------------------------------------
bool file_exists(const std::string& path) noexcept {
  std::error_code error;
  return std::filesystem::is_regular_file(std::filesystem::path(path), error);
}

std::uint64_t file_size_or_zero(const std::string& path) noexcept {
  std::error_code error;
  const auto size = std::filesystem::file_size(std::filesystem::path(path), error);
  if (error) return 0;
  return static_cast<std::uint64_t>(size);
}

std::string temp_sibling_path(const std::string& path, std::string_view suffix) {
  std::string out = path;
  out += '.';
  out.append(suffix.data(), suffix.size());
  return out;
}

Status read_file_bounded(const std::string& path, std::size_t max_bytes, std::string& out) {
  std::error_code error;
  const auto size = std::filesystem::file_size(std::filesystem::path(path), error);
  if (error) return Status::failure(ErrorCode::NotFound, "state file is not readable: " + path);
  if (size > max_bytes) {
    return Status::failure(ErrorCode::LimitExceeded, "state file exceeds the configured size bound");
  }
  std::ifstream stream(path, std::ios::binary);
  if (!stream.is_open()) return Status::failure(ErrorCode::IoFailure, "state file could not be opened");
  out.resize(static_cast<std::size_t>(size));
  if (size > 0) {
    stream.read(out.data(), static_cast<std::streamsize>(size));
    if (stream.gcount() != static_cast<std::streamsize>(size)) {
      out.clear();
      return Status::failure(ErrorCode::Truncated, "state file shrank while reading");
    }
  }
  return Status::ok();
}

Status write_file_atomic(const std::string& path, std::string_view bytes) {
  if (bytes.size() > limits::kStateFileBytes) {
    return Status::failure(ErrorCode::LimitExceeded, "refusing to write an oversized state file");
  }
  const std::string temp = temp_sibling_path(path, "tmp");
  {
    std::ofstream stream(temp, std::ios::binary | std::ios::trunc);
    if (!stream.is_open()) {
      std::string detail = "temporary state file could not be created: ";
      detail += temp;
      return Status::failure(ErrorCode::IoFailure, detail);
    }
    if (!bytes.empty()) stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    stream.flush();
    if (!stream.good()) {
      stream.close();
      (void)remove_file_if_exists(temp);
      return Status::failure(ErrorCode::IoFailure, "temporary state file could not be written");
    }
  }
  // Atomic replacement. On Windows a freshly written file can be transiently
  // locked by the filesystem or a scanner, so the replacement is retried a
  // bounded number of times before it is reported as a failure.
  std::error_code error;
  for (int attempt = 0; attempt < 8; ++attempt) {
    std::filesystem::rename(std::filesystem::path(temp), std::filesystem::path(path), error);
    if (!error) return Status::ok();
    std::this_thread::sleep_for(std::chrono::milliseconds(10 * (attempt + 1)));
  }
  (void)remove_file_if_exists(temp);
  std::string detail = "atomic replacement of the state file failed after retries: ";
  detail += error.message();
  detail += " (";
  detail += path;
  detail += ")";
  return Status::failure(ErrorCode::IoFailure, detail);
}

Status remove_file_if_exists(const std::string& path) noexcept {
  std::error_code error;
  std::filesystem::remove(std::filesystem::path(path), error);
  if (error) return Status::failure(ErrorCode::IoFailure, "state file could not be removed");
  return Status::ok();
}

Status rename_file(const std::string& from, const std::string& to) {
  std::error_code error;
  std::filesystem::rename(std::filesystem::path(from), std::filesystem::path(to), error);
  if (error) return Status::failure(ErrorCode::IoFailure, "state file rename failed");
  return Status::ok();
}

}  // namespace ref