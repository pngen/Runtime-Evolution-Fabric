// Runtime Evolution Fabric - strong identities and generation boundaries.
//
// Every generation type in this header corresponds to a real compatibility,
// authority, lifecycle or stale-state boundary. Nothing here is decorative:
// a generation family exists only when accepting a stale value of it as
// current would be unsafe.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace ref {

// ---------------------------------------------------------------------------
// Text primitives
// ---------------------------------------------------------------------------

// Subset of ASCII accepted in free-form text that reaches durable state or the
// wire. Control characters are rejected so canonical encodings stay stable.
[[nodiscard]] bool is_valid_text_char(char c) noexcept;
// Subset of ASCII accepted in identity fields: [A-Za-z0-9._:+-]
[[nodiscard]] bool is_valid_ident_char(char c) noexcept;

// Free-form, bounded, printable text.
template <std::uint16_t Capacity>
class FixedText {
 public:
  static constexpr std::uint16_t capacity = Capacity;

  constexpr FixedText() noexcept = default;

  [[nodiscard]] static std::optional<FixedText> parse(std::string_view text) noexcept {
    if (text.size() > Capacity) return std::nullopt;
    FixedText out;
    for (std::size_t i = 0; i < text.size(); ++i) {
      if (!is_valid_text_char(text[i])) return std::nullopt;
      out.buf_[i] = text[i];
    }
    out.len_ = static_cast<std::uint16_t>(text.size());
    return out;
  }

  [[nodiscard]] static FixedText from_valid(std::string_view text) noexcept {
    const auto parsed = parse(text);
    return parsed.has_value() ? *parsed : FixedText{};
  }

  [[nodiscard]] std::string_view view() const noexcept { return {buf_.data(), len_}; }
  [[nodiscard]] std::string str() const { return std::string(view()); }
  [[nodiscard]] bool empty() const noexcept { return len_ == 0; }
  [[nodiscard]] std::uint16_t size() const noexcept { return len_; }

  // Narrowing and widening between text bounds is allowed for free text; the
  // value is clipped to this capacity.
  template <std::uint16_t Other>
  FixedText(const FixedText<Other>& other) noexcept {
    assign_from(other.view());
  }
  template <std::uint16_t Other>
  FixedText& operator=(const FixedText<Other>& other) noexcept {
    assign_from(other.view());
    return *this;
  }

  friend bool operator==(const FixedText& a, const FixedText& b) noexcept { return a.view() == b.view(); }
  friend bool operator!=(const FixedText& a, const FixedText& b) noexcept { return !(a == b); }
  friend bool operator<(const FixedText& a, const FixedText& b) noexcept { return a.view() < b.view(); }

 private:
  void assign_from(std::string_view text) noexcept {
    const std::size_t length = text.size() > Capacity ? Capacity : text.size();
    for (std::size_t i = 0; i < length; ++i) buf_[i] = text[i];
    len_ = static_cast<std::uint16_t>(length);
  }

  std::array<char, Capacity> buf_{};
  std::uint16_t len_{0};
};

// Strong identifier tagged by a tag type, so unrelated identities cannot mix.
template <class Tag, std::uint16_t Capacity>
class Ident {
 public:
  static constexpr std::uint16_t capacity = Capacity;
  using tag_type = Tag;

  constexpr Ident() noexcept = default;

  [[nodiscard]] static std::optional<Ident> parse(std::string_view text) noexcept {
    if (text.empty() || text.size() > Capacity) return std::nullopt;
    for (const char c : text) {
      if (!is_valid_ident_char(c)) return std::nullopt;
    }
    Ident out;
    for (std::size_t i = 0; i < text.size(); ++i) out.buf_[i] = text[i];
    out.len_ = static_cast<std::uint16_t>(text.size());
    return out;
  }

  [[nodiscard]] static Ident from_valid(std::string_view text) noexcept {
    const auto parsed = parse(text);
    return parsed.has_value() ? *parsed : Ident{};
  }

  [[nodiscard]] std::string_view view() const noexcept { return {buf_.data(), len_}; }
  [[nodiscard]] std::string str() const { return std::string(view()); }
  [[nodiscard]] bool empty() const noexcept { return len_ == 0; }
  [[nodiscard]] std::uint16_t size() const noexcept { return len_; }

  friend bool operator==(const Ident& a, const Ident& b) noexcept { return a.view() == b.view(); }
  friend bool operator!=(const Ident& a, const Ident& b) noexcept { return !(a == b); }
  friend bool operator<(const Ident& a, const Ident& b) noexcept { return a.view() < b.view(); }

 private:
  std::array<char, Capacity> buf_{};
  std::uint16_t len_{0};
};

// ---------------------------------------------------------------------------
// Identity tags
// ---------------------------------------------------------------------------
struct RuntimeComponentIdTag {};
struct RuntimeVersionIdTag {};
struct ProtocolIdTag {};
struct SchemaIdTag {};
struct EvolutionPlanIdTag {};
struct RolloutStageIdTag {};
struct CohortIdTag {};
struct FeatureGateIdTag {};
struct MigrationIdTag {};
struct RollbackIdTag {};
struct WorkerIdTag {};
struct BuildIdTag {};
struct MessageTypeIdTag {};
struct FieldIdTag {};
struct EvidenceSourceIdTag {};
struct SessionIdTag {};

using RuntimeComponentId = Ident<RuntimeComponentIdTag, 63>;
using ProtocolId = Ident<ProtocolIdTag, 63>;
using SchemaId = Ident<SchemaIdTag, 63>;
using EvolutionPlanId = Ident<EvolutionPlanIdTag, 63>;
using RolloutStageId = Ident<RolloutStageIdTag, 63>;
using CohortId = Ident<CohortIdTag, 63>;
using FeatureGateId = Ident<FeatureGateIdTag, 63>;
using MigrationId = Ident<MigrationIdTag, 63>;
using RollbackId = Ident<RollbackIdTag, 63>;
using WorkerId = Ident<WorkerIdTag, 63>;
using BuildId = Ident<BuildIdTag, 63>;
using MessageTypeId = Ident<MessageTypeIdTag, 63>;
using FieldId = Ident<FieldIdTag, 63>;
using EvidenceSourceId = Ident<EvidenceSourceIdTag, 63>;
using SessionId = Ident<SessionIdTag, 47>;

// Bounded descriptive text attached to outcomes and explanations.
using DetailText = FixedText<192>;
using NoteText = FixedText<96>;
using DigestText = FixedText<40>;

// ---------------------------------------------------------------------------
// Generation counters
// ---------------------------------------------------------------------------

// Saturated-successor bound: a generation never wraps into a stale-looking value.
inline constexpr std::uint64_t kGenerationCeiling = 0xFFFFFFFFFFFFFFFFull - 1ull;

// A monotonic generation. Generation 0 always means "unset"; the first real
// generation of every family is 1.
template <class Tag>
class Generation {
 public:
  using tag_type = Tag;

  constexpr Generation() noexcept = default;

  [[nodiscard]] static constexpr Generation from_raw(std::uint64_t raw) noexcept { return Generation(raw); }
  [[nodiscard]] static constexpr Generation first() noexcept { return Generation(1); }
  [[nodiscard]] static constexpr Generation unset() noexcept { return Generation(0); }

  [[nodiscard]] constexpr std::uint64_t raw() const noexcept { return value_; }
  [[nodiscard]] constexpr bool is_set() const noexcept { return value_ != 0; }

  [[nodiscard]] constexpr Generation next() const noexcept {
    return value_ >= kGenerationCeiling ? Generation(kGenerationCeiling) : Generation(value_ + 1);
  }

  // True when this value is older than the supplied current generation.
  [[nodiscard]] constexpr bool is_stale_relative_to(Generation current) const noexcept {
    return value_ < current.value_;
  }

  friend constexpr bool operator==(Generation a, Generation b) noexcept { return a.value_ == b.value_; }
  friend constexpr bool operator!=(Generation a, Generation b) noexcept { return a.value_ != b.value_; }
  friend constexpr bool operator<(Generation a, Generation b) noexcept { return a.value_ < b.value_; }
  friend constexpr bool operator<=(Generation a, Generation b) noexcept { return a.value_ <= b.value_; }
  friend constexpr bool operator>(Generation a, Generation b) noexcept { return a.value_ > b.value_; }
  friend constexpr bool operator>=(Generation a, Generation b) noexcept { return a.value_ >= b.value_; }

 private:
  explicit constexpr Generation(std::uint64_t raw) noexcept : value_(raw) {}
  std::uint64_t value_{0};
};

// ---------------------------------------------------------------------------
// Named generation families (one per real stale-state boundary)
// ---------------------------------------------------------------------------
struct RuntimeGenerationTag {};
struct ProtocolGenerationTag {};
struct SchemaGenerationTag {};
struct StateFormatGenerationTag {};
struct CompatibilityGenerationTag {};
struct EvolutionPlanGenerationTag {};
struct EvolutionEpochTag {};
struct CoordinatorEpochTag {};
struct CapabilityGenerationTag {};
struct FeatureGateGenerationTag {};
struct MigrationGenerationTag {};
struct RollbackGenerationTag {};
struct EvidenceGenerationTag {};
struct SnapshotGenerationTag {};
struct WorkerBootIdTag {};
struct StageGenerationTag {};
struct PolicyGenerationTag {};

using RuntimeGeneration = Generation<RuntimeGenerationTag>;              // runtime binary/behaviour identity
using ProtocolGeneration = Generation<ProtocolGenerationTag>;            // wire contract identity
using SchemaGeneration = Generation<SchemaGenerationTag>;                // state schema identity
using StateFormatGeneration = Generation<StateFormatGenerationTag>;      // persisted state format on disk/wire
using CompatibilityGeneration = Generation<CompatibilityGenerationTag>;  // compatibility evidence revision
using EvolutionPlanGeneration = Generation<EvolutionPlanGenerationTag>;  // plan revision (rebind => stale)
using EvolutionEpoch = Generation<EvolutionEpochTag>;                    // authority epoch of an evolution
using CoordinatorEpoch = Generation<CoordinatorEpochTag>;                // coordinator incarnation
using CapabilityGeneration = Generation<CapabilityGenerationTag>;        // advertised capability set revision
using FeatureGateGeneration = Generation<FeatureGateGenerationTag>;      // feature-gate table revision
using MigrationGeneration = Generation<MigrationGenerationTag>;          // migration execution attempt
using RollbackGeneration = Generation<RollbackGenerationTag>;            // rollback attempt revision
using EvidenceGeneration = Generation<EvidenceGenerationTag>;            // evidence freshness counter
using SnapshotGeneration = Generation<SnapshotGenerationTag>;            // immutable snapshot revision
using WorkerBootId = Generation<WorkerBootIdTag>;                        // process incarnation of a worker
using StageGeneration = Generation<StageGenerationTag>;                  // rollout stage advance counter
using PolicyGeneration = Generation<PolicyGenerationTag>;                // evolution policy revision

// ---------------------------------------------------------------------------
// Generation sets and ranges
// ---------------------------------------------------------------------------
inline constexpr std::uint8_t kMaxGenerationsPerSet = 32;

// Sorted, bounded set of generations. Deterministic iteration order, bounded
// memory, explicit failure when the bound is exceeded.
template <class Tag>
class GenerationSet {
 public:
  using generation_type = Generation<Tag>;

  GenerationSet() noexcept = default;

  [[nodiscard]] static GenerationSet all_of(std::initializer_list<generation_type> values) noexcept {
    GenerationSet set;
    for (const auto v : values) (void)set.add(v);
    return set;
  }

  // Returns false when the set is full and the value is new.
  bool add(generation_type value) noexcept {
    if (!value.is_set()) return false;
    std::uint8_t pos = 0;
    while (pos < count_ && raw_[pos] < value.raw()) ++pos;
    if (pos < count_ && raw_[pos] == value.raw()) return true;
    if (count_ >= kMaxGenerationsPerSet) return false;
    for (std::uint8_t i = count_; i > pos; --i) raw_[i] = raw_[i - 1];
    raw_[pos] = value.raw();
    ++count_;
    return true;
  }

  [[nodiscard]] bool contains(generation_type value) const noexcept {
    for (std::uint8_t i = 0; i < count_; ++i) {
      if (raw_[i] == value.raw()) return true;
    }
    return false;
  }

  [[nodiscard]] bool empty() const noexcept { return count_ == 0; }
  [[nodiscard]] std::uint8_t size() const noexcept { return count_; }
  [[nodiscard]] generation_type at(std::uint8_t index) const noexcept {
    return index < count_ ? generation_type::from_raw(raw_[index]) : generation_type::unset();
  }
  [[nodiscard]] generation_type lowest() const noexcept {
    return count_ == 0 ? generation_type::unset() : generation_type::from_raw(raw_[0]);
  }
  [[nodiscard]] generation_type highest() const noexcept {
    return count_ == 0 ? generation_type::unset() : generation_type::from_raw(raw_[count_ - 1]);
  }
  // Highest member that is <= limit, or unset when none qualifies.
  [[nodiscard]] generation_type highest_at_most(generation_type limit) const noexcept {
    generation_type best = generation_type::unset();
    for (std::uint8_t i = 0; i < count_; ++i) {
      if (raw_[i] <= limit.raw()) best = generation_type::from_raw(raw_[i]);
    }
    return best;
  }

  friend bool operator==(const GenerationSet& a, const GenerationSet& b) noexcept {
    if (a.count_ != b.count_) return false;
    for (std::uint8_t i = 0; i < a.count_; ++i) {
      if (a.raw_[i] != b.raw_[i]) return false;
    }
    return true;
  }
  friend bool operator!=(const GenerationSet& a, const GenerationSet& b) noexcept { return !(a == b); }

 private:
  std::array<std::uint64_t, kMaxGenerationsPerSet> raw_{};
  std::uint8_t count_{0};
};

// Inclusive generation range.
template <class Tag>
struct GenerationRange {
  Generation<Tag> min{};
  Generation<Tag> max{};

  [[nodiscard]] bool is_set() const noexcept { return min.is_set() && max.is_set(); }
  [[nodiscard]] bool is_valid() const noexcept { return is_set() && min <= max; }
  [[nodiscard]] bool contains(Generation<Tag> value) const noexcept {
    return is_valid() && value >= min && value <= max;
  }
  [[nodiscard]] static GenerationRange closed(Generation<Tag> lo, Generation<Tag> hi) noexcept {
    return GenerationRange{lo, hi};
  }
  [[nodiscard]] static GenerationRange single(Generation<Tag> value) noexcept {
    return GenerationRange{value, value};
  }
  friend bool operator==(const GenerationRange& a, const GenerationRange& b) noexcept {
    return a.min == b.min && a.max == b.max;
  }
};

using ProtocolGenerationSet = GenerationSet<ProtocolGenerationTag>;
using StateFormatGenerationSet = GenerationSet<StateFormatGenerationTag>;
using SchemaGenerationSet = GenerationSet<SchemaGenerationTag>;
using RuntimeGenerationSet = GenerationSet<RuntimeGenerationTag>;
using ProtocolGenerationRange = GenerationRange<ProtocolGenerationTag>;
using SchemaGenerationRange = GenerationRange<SchemaGenerationTag>;
using RuntimeGenerationRange = GenerationRange<RuntimeGenerationTag>;

// ---------------------------------------------------------------------------
// Structured version identity
// ---------------------------------------------------------------------------

struct VersionNumber {
  std::uint16_t major{0};
  std::uint16_t minor{0};
  std::uint16_t patch{0};

  [[nodiscard]] static std::optional<VersionNumber> parse(std::string_view text) noexcept;
  [[nodiscard]] std::string to_string() const;

  friend bool operator==(const VersionNumber& a, const VersionNumber& b) noexcept {
    return a.major == b.major && a.minor == b.minor && a.patch == b.patch;
  }
  friend bool operator!=(const VersionNumber& a, const VersionNumber& b) noexcept { return !(a == b); }
  friend bool operator<(const VersionNumber& a, const VersionNumber& b) noexcept {
    if (a.major != b.major) return a.major < b.major;
    if (a.minor != b.minor) return a.minor < b.minor;
    return a.patch < b.patch;
  }
  friend bool operator>(const VersionNumber& a, const VersionNumber& b) noexcept { return b < a; }
  friend bool operator<=(const VersionNumber& a, const VersionNumber& b) noexcept { return !(b < a); }
  friend bool operator>=(const VersionNumber& a, const VersionNumber& b) noexcept { return !(a < b); }
};

// Integrity digest used for state digests, compatibility evidence and frame
// integrity. It is a deterministic non-cryptographic 128-bit digest and is
// documented as such: it detects corruption and accidental mismatch, and is not
// a security primitive.
struct IntegrityDigest {
  std::uint64_t a{0};
  std::uint64_t b{0};

  [[nodiscard]] static IntegrityDigest of(std::string_view bytes) noexcept;
  [[nodiscard]] static IntegrityDigest combine(IntegrityDigest left, std::string_view bytes) noexcept;
  [[nodiscard]] std::string to_hex() const;
  [[nodiscard]] static std::optional<IntegrityDigest> from_hex(std::string_view text) noexcept;
  [[nodiscard]] bool is_set() const noexcept { return a != 0 || b != 0; }

  friend bool operator==(const IntegrityDigest& x, const IntegrityDigest& y) noexcept {
    return x.a == y.a && x.b == y.b;
  }
  friend bool operator!=(const IntegrityDigest& x, const IntegrityDigest& y) noexcept { return !(x == y); }
};

// Artifact identity: version numbers alone never identify a binary.
struct ArtifactIdentity {
  BuildId build{};                  // build label, e.g. "refworker-g1-msvc-x64"
  IntegrityDigest content_hash{};   // digest of the built artifact's declared content set

  [[nodiscard]] bool is_set() const noexcept { return !build.empty(); }
  friend bool operator==(const ArtifactIdentity& a, const ArtifactIdentity& b) noexcept {
    return a.build == b.build && a.content_hash == b.content_hash;
  }
  friend bool operator!=(const ArtifactIdentity& a, const ArtifactIdentity& b) noexcept { return !(a == b); }
};

// Full runtime version identity: structured version + artifact identity.
struct RuntimeVersionId {
  VersionNumber number{};
  ArtifactIdentity artifact{};

  [[nodiscard]] std::string to_string() const;
  [[nodiscard]] bool is_set() const noexcept { return number.major != 0 || artifact.is_set(); }
  friend bool operator==(const RuntimeVersionId& a, const RuntimeVersionId& b) noexcept {
    return a.number == b.number && a.artifact == b.artifact;
  }
  friend bool operator!=(const RuntimeVersionId& a, const RuntimeVersionId& b) noexcept { return !(a == b); }
  friend bool operator<(const RuntimeVersionId& a, const RuntimeVersionId& b) noexcept {
    if (a.number != b.number) return a.number < b.number;
    if (a.artifact.build != b.artifact.build) return a.artifact.build < b.artifact.build;
    return a.artifact.content_hash.to_hex() < b.artifact.content_hash.to_hex();
  }
};

// ---------------------------------------------------------------------------
// Hashing support for unordered containers
// ---------------------------------------------------------------------------
[[nodiscard]] std::uint64_t hash_bytes(std::string_view bytes) noexcept;
[[nodiscard]] std::uint64_t hash_mix(std::uint64_t seed, std::uint64_t value) noexcept;

template <class Tag, std::uint16_t Capacity>
[[nodiscard]] std::uint64_t ident_hash(const Ident<Tag, Capacity>& ident) noexcept {
  return hash_bytes(ident.view());
}

template <class Tag>
[[nodiscard]] std::uint64_t generation_hash(Generation<Tag> generation) noexcept {
  return hash_mix(0x9E3779B97F4A7C15ull, generation.raw());
}

}  // namespace ref

namespace std {
template <class Tag, std::uint16_t Capacity>
struct hash<ref::Ident<Tag, Capacity>> {
  [[nodiscard]] size_t operator()(const ref::Ident<Tag, Capacity>& ident) const noexcept {
    return static_cast<size_t>(ref::hash_bytes(ident.view()));
  }
};

template <class Tag>
struct hash<ref::Generation<Tag>> {
  [[nodiscard]] size_t operator()(const ref::Generation<Tag>& generation) const noexcept {
    return static_cast<size_t>(ref::hash_mix(0x9E3779B97F4A7C15ull, generation.raw()));
  }
};

template <std::uint16_t Capacity>
struct hash<ref::FixedText<Capacity>> {
  [[nodiscard]] size_t operator()(const ref::FixedText<Capacity>& text) const noexcept {
    return static_cast<size_t>(ref::hash_bytes(text.view()));
  }
};
}  // namespace std
