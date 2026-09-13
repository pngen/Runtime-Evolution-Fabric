// Runtime Evolution Fabric - status, provenance, bounded containers, codec.
//
// This header carries the non-domain plumbing: structured statuses, evidence
// provenance, bounded queues with explicit rejection, overflow-checked
// canonical encoding and a deterministic JSON writer used by the CLI and by
// deterministic explanations.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ref/ids.hpp"

namespace ref {

// ---------------------------------------------------------------------------
// Resource bounds. Every collection that can be driven from the network or from
// durable state has an explicit ceiling; exceeding it is an error, never growth.
// ---------------------------------------------------------------------------
namespace limits {
inline constexpr std::size_t kRuntimeComponents = 256;
inline constexpr std::size_t kComponentVersions = 1024;
inline constexpr std::size_t kCompatibilityEdges = 16384;
inline constexpr std::size_t kActivePlans = 256;
inline constexpr std::size_t kPlanHistory = 1024;
inline constexpr std::size_t kProtocolGenerations = 64;
inline constexpr std::size_t kSchemaGenerations = 64;
inline constexpr std::size_t kMessageTypes = 128;
inline constexpr std::size_t kMessageFields = 64;
inline constexpr std::size_t kFeatureGates = 128;
inline constexpr std::size_t kCohorts = 64;
inline constexpr std::size_t kMigrationRecords = 4096;
inline constexpr std::size_t kRollbackRecords = 1024;
inline constexpr std::size_t kStageHistory = 256;
inline constexpr std::size_t kWorkers = 2048;
inline constexpr std::size_t kFencedBoots = 4096;
inline constexpr std::size_t kConnections = 512;
inline constexpr std::size_t kEventQueueDepth = 1024;
inline constexpr std::size_t kSnapshots = 32;
inline constexpr std::size_t kReconcileFindings = 256;
inline constexpr std::size_t kFrameBytes = 1u << 20;        // 1 MiB
inline constexpr std::size_t kQueryBytes = 64u << 10;       // 64 KiB
inline constexpr std::size_t kStateFileBytes = 64u << 20;   // 64 MiB
inline constexpr std::size_t kMetadataBytes = 8u << 20;     // 8 MiB
inline constexpr std::size_t kTextBytes = 512;
inline constexpr std::size_t kExplanationLines = 64;
inline constexpr std::size_t kRetiredGenerations = 1024;
}  // namespace limits

// ---------------------------------------------------------------------------
// Status
// ---------------------------------------------------------------------------
enum class ErrorCode : std::uint8_t {
  Ok = 0,
  InvalidArgument,
  NotFound,
  AlreadyExists,
  LimitExceeded,
  StaleEpoch,         // evolution or coordinator epoch older than current
  StaleBoot,          // process boot identity no longer current
  StaleGeneration,    // runtime/protocol/schema generation no longer authorized
  StaleEvidence,      // compatibility evidence older than required
  StalePlan,          // superseded plan or late completion from a superseded stage
  Unauthorized,       // operation class not granted to this peer
  RetiredGeneration,  // runtime generation is terminal for this component
  Incompatible,
  Conflict,
  Corrupt,
  Truncated,
  IntegrityFailure,
  IoFailure,
  Unsupported,
  Busy,               // transient backpressure
  QueueFull,
  Retired,
  OutcomeUnknown,
  Internal,
};

[[nodiscard]] const char* to_string(ErrorCode code) noexcept;
[[nodiscard]] std::optional<ErrorCode> parse_error_code(std::string_view text) noexcept;

class Status {
 public:
  Status() noexcept = default;
  Status(ErrorCode code, std::string_view detail) noexcept : code_(code) {
    const auto parsed = DetailText::parse(detail);
    detail_ = parsed.has_value() ? *parsed : DetailText{};
  }

  [[nodiscard]] static Status ok() noexcept { return Status{}; }
  [[nodiscard]] static Status failure(ErrorCode code, std::string_view detail) noexcept {
    return Status(code, detail);
  }

  [[nodiscard]] bool is_ok() const noexcept { return code_ == ErrorCode::Ok; }
  [[nodiscard]] bool is_failure() const noexcept { return code_ != ErrorCode::Ok; }
  [[nodiscard]] ErrorCode code() const noexcept { return code_; }
  [[nodiscard]] std::string_view detail() const noexcept { return detail_.view(); }
  [[nodiscard]] std::string to_string() const;

  friend bool operator==(const Status& a, const Status& b) noexcept {
    return a.code_ == b.code_;
  }
  friend bool operator!=(const Status& a, const Status& b) noexcept { return !(a == b); }

 private:
  ErrorCode code_{ErrorCode::Ok};
  DetailText detail_{};
};

// ---------------------------------------------------------------------------
// Evidence provenance
// ---------------------------------------------------------------------------
// REAL        : produced by an actual process, socket, filesystem or artifact in
//               this environment.
// SYNTHETIC   : produced by the deterministic synthetic runtime generator using
//               the production interfaces.
// UNSUPPORTED : a scenario this build cannot exercise locally; never claimed.
enum class EvidenceClass : std::uint8_t { Unknown = 0, Real, Synthetic, Unsupported };

[[nodiscard]] const char* to_string(EvidenceClass value) noexcept;
[[nodiscard]] std::optional<EvidenceClass> parse_evidence_class(std::string_view text) noexcept;

// A single piece of evidence with provenance and freshness generation.
struct EvidenceRef {
  EvidenceClass provenance{EvidenceClass::Unknown};
  EvidenceGeneration generation{};
  EvidenceSourceId source{};
  DetailText detail{};
};

// Describes what evidence an evolution step requires before it may advance.
struct RequiredEvidence {
  EvidenceClass minimum_provenance{EvidenceClass::Real};
  std::uint64_t max_age_generations{0};  // 0 == any age, otherwise max gap
  DetailText description{};
};

// ---------------------------------------------------------------------------
// Checked arithmetic
// ---------------------------------------------------------------------------
[[nodiscard]] bool checked_add(std::uint64_t a, std::uint64_t b, std::uint64_t& out) noexcept;
[[nodiscard]] bool checked_mul(std::uint64_t a, std::uint64_t b, std::uint64_t& out) noexcept;

// ---------------------------------------------------------------------------
// Integrity primitives
// ---------------------------------------------------------------------------
[[nodiscard]] std::uint32_t crc32(std::string_view bytes) noexcept;

// ---------------------------------------------------------------------------
// Canonical, overflow-checked byte encoding (little endian, length prefixed)
// ---------------------------------------------------------------------------
class ByteWriter {
 public:
  ByteWriter() = default;

  void u8(std::uint8_t value);
  void u16(std::uint16_t value);
  void u32(std::uint32_t value);
  void u64(std::uint64_t value);
  void boolean(bool value) { u8(value ? 1u : 0u); }
  void raw(std::string_view bytes);
  void text(std::string_view value, std::size_t max_length);
  void digest(IntegrityDigest value);
  template <class Tag, std::uint16_t Capacity>
  void ident(const Ident<Tag, Capacity>& value) {
    text(value.view(), Capacity);
  }
  template <class Tag>
  void generation(Generation<Tag> value) {
    u64(value.raw());
  }
  // Fix-up support for length-prefixed sections.
  [[nodiscard]] std::size_t mark() const noexcept { return bytes_.size(); }
  void patch_u32(std::size_t offset, std::uint32_t value);

  [[nodiscard]] const std::vector<std::uint8_t>& buffer() const noexcept { return bytes_; }
  [[nodiscard]] std::string_view view() const noexcept {
    return {reinterpret_cast<const char*>(bytes_.data()), bytes_.size()};
  }
  [[nodiscard]] std::size_t size() const noexcept { return bytes_.size(); }
  void clear() noexcept { bytes_.clear(); }

 private:
  std::vector<std::uint8_t> bytes_{};
};

// Reader that fails closed: any short read or bound violation sets a sticky
// failure flag and yields zero values afterwards.
class ByteReader {
 public:
  explicit ByteReader(std::string_view bytes) noexcept
      : begin_(bytes.data()), cursor_(bytes.data()), end_(bytes.data() + bytes.size()) {}

  [[nodiscard]] std::uint8_t u8() noexcept;
  [[nodiscard]] std::uint16_t u16() noexcept;
  [[nodiscard]] std::uint32_t u32() noexcept;
  [[nodiscard]] std::uint64_t u64() noexcept;
  [[nodiscard]] bool boolean() noexcept { return u8() != 0; }
  // Reads a length-prefixed byte string; fails when longer than max_length.
  [[nodiscard]] std::string_view bytes(std::size_t max_length) noexcept;
  [[nodiscard]] IntegrityDigest digest() noexcept;
  template <class Tag>
  [[nodiscard]] Generation<Tag> generation() noexcept {
    return Generation<Tag>::from_raw(u64());
  }
  template <class Tag, std::uint16_t Capacity>
  [[nodiscard]] Ident<Tag, Capacity> ident() noexcept {
    const auto raw = bytes(Capacity);
    if (failed_) return Ident<Tag, Capacity>{};
    // An empty identity field means "unset": optional identities such as a
    // feature gate or a migration reference are legitimately absent.
    if (raw.empty()) return Ident<Tag, Capacity>{};
    const auto parsed = Ident<Tag, Capacity>::parse(raw);
    if (!parsed.has_value()) {
      failed_ = true;
      return Ident<Tag, Capacity>{};
    }
    return *parsed;
  }
  template <std::uint16_t Capacity>
  [[nodiscard]] FixedText<Capacity> text() noexcept {
    const auto raw = bytes(Capacity);
    if (failed_) return FixedText<Capacity>{};
    const auto parsed = FixedText<Capacity>::parse(raw);
    if (!parsed.has_value()) {
      failed_ = true;
      return FixedText<Capacity>{};
    }
    return *parsed;
  }

  // Skips a fixed number of raw bytes; fails closed when they are not present.
  bool skip(std::size_t count) noexcept;

  [[nodiscard]] bool failed() const noexcept { return failed_; }
  [[nodiscard]] bool at_end() const noexcept { return cursor_ == end_; }
  [[nodiscard]] std::size_t remaining() const noexcept {
    return static_cast<std::size_t>(end_ - cursor_);
  }
  [[nodiscard]] std::size_t consumed() const noexcept {
    return static_cast<std::size_t>(cursor_ - begin_);
  }

 private:
  [[nodiscard]] bool need(std::size_t count) noexcept;

  const char* begin_;
  const char* cursor_;
  const char* end_;
  bool failed_{false};
};

// ---------------------------------------------------------------------------
// Bounded work queue with explicit rejection and observable sequence gaps
// ---------------------------------------------------------------------------
template <class T>
class BoundedQueue {
 public:
  explicit BoundedQueue(std::size_t capacity) : capacity_(capacity == 0 ? 1 : capacity) {}

  BoundedQueue(const BoundedQueue&) = delete;
  BoundedQueue& operator=(const BoundedQueue&) = delete;

  // Every offered item consumes a sequence number, whether or not it is
  // accepted, so a rejected item leaves a visible gap instead of vanishing.
  std::optional<std::uint64_t> offer(T value) {
    std::lock_guard<std::mutex> guard(mutex_);
    const std::uint64_t sequence = next_sequence_++;
    if (closed_) {
      ++rejected_;
      return std::nullopt;
    }
    if (items_.size() >= capacity_) {
      ++rejected_;
      return std::nullopt;
    }
    items_.push_back(Entry{sequence, std::move(value)});
    ++accepted_;
    ready_.notify_one();
    return sequence;
  }

  // Blocks until an item is available or the queue is closed.
  std::optional<std::pair<std::uint64_t, T>> wait_pop() {
    std::unique_lock<std::mutex> lock(mutex_);
    ready_.wait(lock, [this] { return !items_.empty() || closed_; });
    if (items_.empty()) return std::nullopt;
    Entry entry = std::move(items_.front());
    items_.pop_front();
    return std::make_pair(entry.sequence, std::move(entry.value));
  }

  void close() {
    std::lock_guard<std::mutex> guard(mutex_);
    closed_ = true;
    ready_.notify_all();
  }

  [[nodiscard]] bool closed() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return closed_;
  }
  [[nodiscard]] std::size_t size() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return items_.size();
  }
  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
  [[nodiscard]] std::uint64_t accepted() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return accepted_;
  }
  [[nodiscard]] std::uint64_t rejected() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return rejected_;
  }
  // Number of sequence numbers handed out so far, including rejected items.
  [[nodiscard]] std::uint64_t sequences_issued() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return next_sequence_ - 1;
  }

 private:
  struct Entry {
    std::uint64_t sequence;
    T value;
  };

  mutable std::mutex mutex_;
  std::condition_variable ready_;
  std::deque<Entry> items_;
  std::size_t capacity_;
  std::uint64_t next_sequence_{1};
  std::uint64_t accepted_{0};
  std::uint64_t rejected_{0};
  bool closed_{false};
};

// ---------------------------------------------------------------------------
// Deterministic JSON writer (insertion ordered, escaped, bounded)
// ---------------------------------------------------------------------------
class JsonWriter {
 public:
  // Compact (single line) by default: JSON documents travel inside canonical
  // text fields, which do not carry control characters.
  explicit JsonWriter(bool pretty = false) : pretty_(pretty) {}

  JsonWriter& begin_object();
  JsonWriter& end_object();
  JsonWriter& begin_array();
  JsonWriter& end_array();
  JsonWriter& key(std::string_view name);
  JsonWriter& string(std::string_view value);
  JsonWriter& number(std::uint64_t value);
  JsonWriter& number(std::int64_t value);
  JsonWriter& boolean(bool value);
  JsonWriter& null();
  JsonWriter& field(std::string_view name, std::string_view value);
  // Exact-match overload: a string literal must never bind to the bool overload.
  JsonWriter& field(std::string_view name, const char* value) {
    return field(name, std::string_view(value));
  }
  JsonWriter& field(std::string_view name, const std::string& value) {
    return field(name, std::string_view(value));
  }
  JsonWriter& field(std::string_view name, std::uint64_t value);
  JsonWriter& field(std::string_view name, std::int64_t value);
  JsonWriter& field(std::string_view name, bool value);
  JsonWriter& field_json(std::string_view name, const std::string& raw_json);

  template <std::uint16_t Capacity>
  JsonWriter& field_text(std::string_view name, const FixedText<Capacity>& value) {
    return field(name, value.view());
  }
  template <class Tag, std::uint16_t Capacity>
  JsonWriter& field_ident(std::string_view name, const Ident<Tag, Capacity>& value) {
    return field(name, value.view());
  }
  template <class Tag>
  JsonWriter& field_generation(std::string_view name, Generation<Tag> value) {
    return field(name, value.raw());
  }

  // Closes every container still open. Used as a defensive net where a document
  // is assembled along several branches: a partially written document must never
  // reach a peer.
  JsonWriter& close_all();

  [[nodiscard]] const std::string& str() const noexcept { return out_; }
  [[nodiscard]] bool well_formed() const noexcept { return well_formed_; }
  [[nodiscard]] std::size_t depth() const noexcept { return stack_.size(); }

 private:
  void separate();
  void indent();

  std::string out_{};
  std::vector<int> stack_{};  // 0 = object, 1 = array
  std::vector<bool> first_{};
  bool pending_key_{false};
  bool well_formed_{true};
  bool pretty_{false};
  std::size_t depth_{0};
};

[[nodiscard]] std::string json_escape(std::string_view value);
[[nodiscard]] std::uint64_t parse_u64(std::string_view text, bool& ok) noexcept;

// ---------------------------------------------------------------------------
// Bounded filesystem helpers.
// Reads are size-capped, writes are atomic (temp file in the destination
// directory followed by an atomic replacement), and no helper ever grows state
// without a limit.
// ---------------------------------------------------------------------------
[[nodiscard]] Status read_file_bounded(const std::string& path, std::size_t max_bytes,
                                       std::string& out);
[[nodiscard]] bool file_exists(const std::string& path) noexcept;
[[nodiscard]] std::uint64_t file_size_or_zero(const std::string& path) noexcept;
[[nodiscard]] Status write_file_atomic(const std::string& path, std::string_view bytes);
[[nodiscard]] Status remove_file_if_exists(const std::string& path) noexcept;
[[nodiscard]] Status rename_file(const std::string& from, const std::string& to);

// Temporary path used for atomic replacement and for migration staging.
[[nodiscard]] std::string temp_sibling_path(const std::string& path, std::string_view suffix);

}  // namespace ref
