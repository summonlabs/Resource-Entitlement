// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "commands.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "json.hpp"
#include "resource_entitlement/digest.hpp"
#include "resource_entitlement/store.hpp"
#include "resource_entitlement/text.hpp"
#include "resource_entitlement/token.hpp"
#include "resource_entitlement/version.hpp"

namespace entl::cli {
namespace {

const std::vector<std::string> kCommands = {
    "version", "selftest", "init",       "authority", "grant",  "show",     "list",
    "verify",  "token",    "verify-token", "suspend", "resume", "revoke",   "expire",
    "draw",    "release",  "transfer",   "merge",     "reissue", "lineage", "order",
    "compact", "inspect",  "stats",      "bench"};

int exit_code_for(ErrorCode code) {
  if (error_code_is_durability(code)) {
    return kExitDurability;
  }
  if (error_code_is_validation(code) || code == ErrorCode::kMalformedInput ||
      code == ErrorCode::kMissingField || code == ErrorCode::kUnexpectedField ||
      code == ErrorCode::kDuplicateField) {
    return kExitUsage;
  }
  return kExitRefused;
}

int emit_error(const Error& error) {
  JsonWriter json;
  json.begin_object();
  json.key("ok").value(false);
  json.key("code").value(error.code_name());
  json.key("message").value(error.message());
  if (!error.detail().empty()) {
    json.key("detail").value(error.detail());
  }
  json.end_object();
  std::cout << json.str() << "\n";
  return exit_code_for(error.code());
}

int emit_usage_error(const std::string& message, const std::string& detail = {}) {
  return emit_error(Error(ErrorCode::kMalformedInput, message, detail));
}

// ---------------------------------------------------------------------------
// Value parsing
// ---------------------------------------------------------------------------

template <class T>
[[nodiscard]] Result<T> parse_identifier(std::string_view text, const char* what) {
  auto parsed = T::parse(text);
  if (!parsed.has_value()) {
    return Error(ErrorCode::kInvalidIdentifier, std::string("value is not a valid ") + what,
                 std::string(entl::truncate_utf8(text, 32u)));
  }
  return parsed.value();
}

/// Compile-time-knowable identity built through the same validation path as
/// caller input, so a fixture literal can never bypass the identifier rules.
template <class T>
[[nodiscard]] T fixed_identifier(std::string_view text) {
  return parse_identifier<T>(text, "identity").value();
}

[[nodiscard]] Result<entl::Timestamp> parse_instant(std::string_view text, const char* what) {
  auto parsed = entl::Timestamp::parse(text);
  if (!parsed.has_value()) {
    return Error(ErrorCode::kInvalidTimestamp, std::string(what) + " is not a valid instant",
                 std::string(entl::truncate_utf8(text, 40u)));
  }
  return parsed.value();
}

[[nodiscard]] Result<entl::Unit> parse_unit(std::string_view text) {
  const auto unit = entl::unit_from_name(text);
  if (!unit.has_value()) {
    return Error(ErrorCode::kInvalidUnit, "unknown unit", std::string(text));
  }
  return unit.value();
}

[[nodiscard]] Result<entl::Quantity> parse_quantity(std::string_view unit_text, std::string_view amount_text) {
  auto unit = parse_unit(unit_text);
  if (!unit.has_value()) {
    return unit.error();
  }
  auto quantity = entl::Quantity::parse(unit.value(), amount_text);
  if (!quantity.has_value()) {
    return Error(ErrorCode::kInvalidQuantity, "quantity is not a valid amount in that unit",
                 std::string(amount_text));
  }
  return quantity.value();
}

[[nodiscard]] Result<entl::Sha256Digest> parse_digest(std::string_view text, const char* what) {
  auto parsed = entl::Sha256Digest::from_hex(text);
  if (!parsed.has_value()) {
    return Error(ErrorCode::kInvalidDigest, std::string(what) + " must be 64 hexadecimal characters");
  }
  if (parsed->is_zero()) {
    return Error(ErrorCode::kInvalidDigest, std::string(what) + " must not be the all-zero digest");
  }
  return parsed.value();
}

[[nodiscard]] Result<entl::RequestId> parse_request_id(std::string_view text) {
  auto parsed = entl::RequestId::from_hex(text);
  if (!parsed.has_value() || parsed->is_zero()) {
    return Error(ErrorCode::kInvalidArgument, "request identity must be 32 hexadecimal characters");
  }
  return parsed.value();
}

[[nodiscard]] Result<entl::EntitlementId> parse_entitlement_id(std::string_view text) {
  auto parsed = entl::EntitlementId::from_hex(text);
  if (!parsed.has_value() || parsed->is_zero()) {
    return Error(ErrorCode::kInvalidArgument, "entitlement identity must be 32 hexadecimal characters",
                 std::string(entl::truncate_utf8(text, 40u)));
  }
  return parsed.value();
}

[[nodiscard]] Result<entl::RequestId> derive_request_id(std::string_view command, std::string_view content) {
  entl::CanonicalWriter writer;
  writer.text(command);
  writer.text(content);
  const entl::RequestId derived = entl::RequestId::derive(writer.span());
  if (derived.is_zero()) {
    return Error(ErrorCode::kInternalError, "derived request identity is degenerate");
  }
  return derived;
}

/// Deterministic identity for a command that does not take a request document:
/// the caller's option set is canonicalised so a retry with identical options
/// resolves as a replay rather than as a second mutation.
[[nodiscard]] Result<entl::RequestId> derive_request_id_from_args(const Args& args,
                                                                 std::string_view command) {
  std::vector<std::pair<std::string, std::string>> sorted(args.entries().begin(), args.entries().end());
  std::sort(sorted.begin(), sorted.end());
  entl::CanonicalWriter writer;
  writer.text(command);
  for (const auto& entry : sorted) {
    if (entry.first == "request-id") {
      continue;
    }
    writer.text(entry.first);
    writer.text(entry.second);
  }
  const entl::RequestId derived = entl::RequestId::derive(writer.span());
  if (derived.is_zero()) {
    return Error(ErrorCode::kInternalError, "derived request identity is degenerate");
  }
  return derived;
}

[[nodiscard]] Result<entl::RequestId> resolve_request_id(const Args& args, std::string_view command) {
  const auto explicit_id = args.optional("request-id");
  if (explicit_id.has_value()) {
    return parse_request_id(explicit_id.value());
  }
  return derive_request_id_from_args(args, command);
}

[[nodiscard]] Result<entl::Timestamp> resolve_now(const Args& args) {
  const auto override_value = args.optional("now");
  if (override_value.has_value()) {
    return parse_instant(override_value.value(), "--now");
  }
  return entl::SystemClock{}.now();
}

[[nodiscard]] Result<bool> resolve_require_admission(const Args& args) {
  return args.optional_bool("require-admission-evidence", true);
}

[[nodiscard]] Result<std::unique_ptr<entl::Store>> open_writer(const Args& args) {
  auto directory = args.require("dir");
  if (!directory.has_value()) {
    return directory.error();
  }
  entl::StoreOpenOptions options;
  options.directory = std::filesystem::path(directory.value());
  auto fence = args.optional_bool("fence-on-open", false);
  if (!fence.has_value()) {
    return fence.error();
  }
  options.fence_live_authority_on_open = fence.value();
  auto capacity = args.optional_u64("idempotency-capacity", 8192u);
  if (!capacity.has_value()) {
    return capacity.error();
  }
  if (capacity.value() > 0xFFFFFFFFu) {
    return Error(ErrorCode::kInvalidArgument, "--idempotency-capacity is out of range");
  }
  options.max_idempotency_entries = static_cast<std::uint32_t>(capacity.value());
  auto require_evidence = resolve_require_admission(args);
  if (!require_evidence.has_value()) {
    return require_evidence.error();
  }
  options.require_admission_evidence = require_evidence.value();
  auto opened = entl::Store::open(options);
  if (!opened.has_value()) {
    return opened.error();
  }
  return std::move(opened.value());
}

[[nodiscard]] Result<std::unique_ptr<entl::Store>> open_reader(const Args& args) {
  auto directory = args.require("dir");
  if (!directory.has_value()) {
    return directory.error();
  }
  entl::StoreOpenOptions options;
  options.directory = std::filesystem::path(directory.value());
  options.read_only = true;
  auto opened = entl::Store::open(options);
  if (!opened.has_value()) {
    return opened.error();
  }
  return std::move(opened.value());
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

void write_quantity(JsonWriter& json, const entl::Quantity& quantity) {
  json.begin_object();
  json.key("unit").value(entl::unit_name(quantity.unit()));
  json.key("units").value(static_cast<unsigned long long>(quantity.units()));
  json.end_object();
}

void write_timestamp(JsonWriter& json, const entl::Timestamp& timestamp) {
  json.begin_object();
  json.key("unix_nanos").value(static_cast<long long>(timestamp.unix_nanos()));
  json.key("rfc3339").value(timestamp.to_rfc3339());
  json.end_object();
}

void write_priority(JsonWriter& json, const entl::Priority& priority) {
  json.begin_object();
  json.key("class").value(entl::priority_class_name(priority.klass));
  json.key("rank").value(static_cast<unsigned>(priority.rank));
  json.end_object();
}

void write_scope(JsonWriter& json, const entl::ResourceScope& scope) {
  json.begin_object();
  json.key("facility").value(scope.facility.str());
  json.key("resource_type").value(scope.resource_type.str());
  json.key("scope").value(scope.scope.str());
  json.end_object();
}

void write_binding(JsonWriter& json, const entl::AuthorityBinding& binding) {
  json.begin_object();
  json.key("authority_revision").value(static_cast<unsigned long long>(binding.authority_revision.value()));
  json.key("facility_capacity_generation")
      .value(static_cast<unsigned long long>(binding.facility_capacity_generation.value()));
  json.key("facility_policy_revision")
      .value(static_cast<unsigned long long>(binding.facility_policy_revision.value()));
  json.key("resource_envelope_revision")
      .value(static_cast<unsigned long long>(binding.resource_envelope_revision.value()));
  json.key("service_class_revision")
      .value(static_cast<unsigned long long>(binding.service_class_revision.value()));
  json.key("control_epoch").value(static_cast<unsigned long long>(binding.control_epoch.value()));
  json.key("policy_context_digest").value(binding.policy_context_digest.to_hex());
  json.key("envelope_binding_digest").value(binding.envelope_binding_digest.to_hex());
  json.key("admission_decision_digest").value(binding.admission_decision_digest.to_hex());
  json.key("binding_digest").value(binding.binding_digest().to_hex());
  json.end_object();
}

void write_entitlement(JsonWriter& json, const entl::Entitlement& record) {
  json.begin_object();
  json.key("id").value(record.id.to_hex());
  json.key("root").value(record.root.to_hex());
  json.key("relation").value(entl::lineage_relation_name(record.relation));
  json.key("sources").begin_array();
  for (const entl::EntitlementId& source : record.sources) {
    json.value(source.to_hex());
  }
  json.end_array();
  json.key("revision").value(static_cast<unsigned long long>(record.revision.value()));
  json.key("mint_ordinal").value(static_cast<unsigned long long>(record.mint_ordinal.value()));
  json.key("state").value(entl::entitlement_state_name(record.state));
  json.key("terminal_reason").value(entl::terminal_reason_name(record.terminal_reason));
  json.key("holder").value(record.holder.str());
  json.key("origin_holder").value(record.origin_holder.str());
  json.key("service").value(record.service.str());
  json.key("service_class").value(record.service_class.str());
  json.key("scope");
  write_scope(json, record.scope);
  json.key("unit").value(entl::unit_name(record.unit));
  json.key("granted");
  write_quantity(json, record.granted);
  json.key("remaining");
  write_quantity(json, record.remaining);
  json.key("delegated_out");
  write_quantity(json, record.delegated_out);
  json.key("priority");
  write_priority(json, record.priority);
  json.key("effective_from");
  write_timestamp(json, record.effective_from);
  json.key("expires_at");
  write_timestamp(json, record.expires_at);
  json.key("created_at");
  write_timestamp(json, record.created_at);
  json.key("updated_at");
  write_timestamp(json, record.updated_at);
  json.key("created_by").value(record.created_by.str());
  json.key("fence_bits").value(static_cast<unsigned>(record.fence.bits()));
  json.key("binding");
  write_binding(json, record.binding);
  json.key("request_digest").value(record.request_digest.to_hex());
  json.key("note").value(record.note);
  json.key("suspended_at");
  if (record.suspended_at.has_value()) {
    write_timestamp(json, record.suspended_at.value());
  } else {
    json.null_value();
  }
  json.key("revoked_at");
  if (record.revoked_at.has_value()) {
    write_timestamp(json, record.revoked_at.value());
  } else {
    json.null_value();
  }
  json.key("last_commit_seq").value(static_cast<unsigned long long>(record.last_commit_seq.value()));
  json.key("holder_history").begin_array();
  for (const entl::HolderChange& change : record.holder_history) {
    json.begin_object();
    json.key("from").value(change.from.str());
    json.key("to").value(change.to.str());
    json.key("at");
    write_timestamp(json, change.at);
    json.key("commit_seq").value(static_cast<unsigned long long>(change.commit_seq.value()));
    json.key("actor").value(change.actor.str());
    json.end_object();
  }
  json.end_array();
  json.end_object();
}

void write_outcome(JsonWriter& json, const entl::MutationOutcome& outcome) {
  json.begin_object();
  json.key("ok").value(outcome.accepted());
  json.key("replayed").value(outcome.replayed);
  json.key("request_id").value(outcome.request_id.to_hex());
  json.key("commit_seq").value(static_cast<unsigned long long>(outcome.commit_seq.value()));
  json.key("primary_id").value(outcome.primary_id.to_hex());
  json.key("primary_revision").value(static_cast<unsigned long long>(outcome.primary_revision.value()));
  json.key("affected").begin_array();
  for (const entl::EntitlementId& id : outcome.affected) {
    json.value(id.to_hex());
  }
  json.end_array();
  json.key("message").value(outcome.message);
  json.end_object();
}

void write_decision(JsonWriter& json, const entl::VerificationDecision& decision) {
  json.begin_object();
  json.key("ok").value(true);
  json.key("authorized").value(decision.authorized);
  json.key("code").value(entl::error_code_name(decision.primary));
  json.key("explanation").value(decision.explanation);
  json.key("id").value(decision.id.to_hex());
  json.key("revision").value(static_cast<unsigned long long>(decision.revision.value()));
  json.key("control_epoch").value(static_cast<unsigned long long>(decision.control_epoch.value()));
  json.key("observed_commit_seq")
      .value(static_cast<unsigned long long>(decision.observed_commit_seq.value()));
  json.key("decision_time");
  write_timestamp(json, decision.decision_time);
  json.key("stale").value(decision.stale);
  json.key("remaining");
  write_quantity(json, decision.remaining);
  json.key("binding_digest").value(decision.binding_digest.to_hex());
  if (!decision.token_digest.is_zero()) {
    json.key("token_digest").value(decision.token_digest.to_hex());
  }
  json.key("secondary_faults").begin_array();
  for (const ErrorCode code : decision.secondary_faults) {
    json.value(entl::error_code_name(code));
  }
  json.end_array();
  json.end_object();
}

// ---------------------------------------------------------------------------
// Command option sets
// ---------------------------------------------------------------------------

const std::vector<std::string> kOpenOptions = {"dir", "fence-on-open", "idempotency-capacity",
                                              "require-admission-evidence", "now", "request-id"};

Result<void> reject(const Args& args, std::vector<std::string> allowed) {
  return args.reject_unknown(allowed);
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

int cmd_version(const Args& args) {
  auto rejected = reject(args, {});
  if (!rejected) {
    return emit_error(rejected.error());
  }
  JsonWriter json;
  json.begin_object();
  json.key("ok").value(true);
  json.key("name").value("Resource Entitlement");
  json.key("version").value(entl::version_string());
  json.key("build_mode").value(entl::build_mode_string());
  json.key("store_format_version").value(static_cast<unsigned>(entl::kStoreFormatVersion));
  json.end_object();
  std::cout << json.str() << "\n";
  return kExitOk;
}

int cmd_selftest(const Args& args) {
  auto rejected = reject(args, {});
  if (!rejected) {
    return emit_error(rejected.error());
  }
  struct Vector {
    const char* input;
    const char* expected;
  };
  const Vector vectors[] = {
      {"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
      {"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
      {"The quick brown fox jumps over the lazy dog",
       "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592"}};
  bool ok = true;
  std::size_t digest_checks = 0;
  for (const Vector& vector : vectors) {
    ++digest_checks;
    if (entl::Sha256::hash(vector.input).to_hex() != vector.expected) {
      ok = false;
    }
  }
  const std::string crc_input = "123456789";
  const std::uint32_t crc = entl::crc32c(std::span<const std::uint8_t>(
      reinterpret_cast<const std::uint8_t*>(crc_input.data()), crc_input.size()));
  if (crc != 0xE3069283u) {
    ok = false;
  }
  const bool identifier_ok = entl::is_valid_identifier("tenant-a") && !entl::is_valid_identifier("CON") &&
                             !entl::is_valid_identifier("has space");
  if (!identifier_ok) {
    ok = false;
  }
  entl::CanonicalWriter writer;
  writer.u32(0xDEADBEEFu);
  writer.text("round-trip");
  entl::CanonicalReader reader(writer.data());
  const bool canonical_ok = reader.u32() == 0xDEADBEEFu && reader.text(32u) == "round-trip" &&
                            reader.finish().has_value();
  if (!canonical_ok) {
    ok = false;
  }

  JsonWriter json;
  json.begin_object();
  json.key("ok").value(ok);
  json.key("sha256_vectors").value(static_cast<unsigned long long>(digest_checks));
  json.key("crc32c_vector").value(crc == 0xE3069283u);
  json.key("identifier_rules").value(identifier_ok);
  json.key("canonical_round_trip").value(canonical_ok);
  json.key("version").value(entl::version_string());
  json.end_object();
  std::cout << json.str() << "\n";
  return ok ? kExitOk : kExitRefused;
}

int cmd_init(const Args& args) {
  auto rejected = reject(args, {"dir", "idempotency-capacity", "require-admission-evidence"});
  if (!rejected) {
    return emit_error(rejected.error());
  }
  auto directory = args.require("dir");
  if (!directory.has_value()) {
    return emit_error(directory.error());
  }
  entl::StoreOpenOptions options;
  options.directory = std::filesystem::path(directory.value());
  options.create_if_missing = true;
  auto capacity = args.optional_u64("idempotency-capacity", 8192u);
  if (!capacity.has_value()) {
    return emit_error(capacity.error());
  }
  if (capacity.value() > 0xFFFFFFFFu) {
    return emit_error(Error(ErrorCode::kInvalidArgument, "--idempotency-capacity is out of range"));
  }
  options.max_idempotency_entries = static_cast<std::uint32_t>(capacity.value());
  auto opened = entl::Store::open(options);
  if (!opened.has_value()) {
    return emit_error(opened.error());
  }
  const entl::StoreStats stats = opened.value()->stats();
  JsonWriter json;
  json.begin_object();
  json.key("ok").value(true);
  json.key("directory").value(std::filesystem::absolute(options.directory).string());
  json.key("authority_revision")
      .value(static_cast<unsigned long long>(opened.value()->authority().revision.value()));
  json.key("control_epoch")
      .value(static_cast<unsigned long long>(opened.value()->authority().control_epoch.value()));
  json.key("commit_seq").value(static_cast<unsigned long long>(stats.commit_seq.value()));
  json.key("note").value("bootstrap snapshot holds no published generations; publish authority before granting");
  json.end_object();
  std::cout << json.str() << "\n";
  return kExitOk;
}

int cmd_authority(const Args& args) {
  auto rejected = reject(args, {"dir", "now", "request-id", "publisher", "capacity-generation",
                                "policy-revision", "envelope-revision", "service-class-revision",
                                "policy-context-digest", "envelope-binding-digest", "advance-epoch",
                                "note", "require-admission-evidence", "idempotency-capacity"});
  if (!rejected) {
    return emit_error(rejected.error());
  }
  auto store = open_writer(args);
  if (!store.has_value()) {
    return emit_error(store.error());
  }
  auto now = resolve_now(args);
  if (!now.has_value()) {
    return emit_error(now.error());
  }
  auto request_id_value = resolve_request_id(args, "authority");
  if (!request_id_value.has_value()) {
    return emit_error(request_id_value.error());
  }
  auto publisher = args.require("publisher");
  if (!publisher.has_value()) {
    return emit_error(publisher.error());
  }
  auto publisher_id = parse_identifier<entl::ActorId>(publisher.value(), "actor identity");
  if (!publisher_id.has_value()) {
    return emit_error(publisher_id.error());
  }

  entl::AuthorityUpdateRequest request;
  request.request_id = request_id_value.value();
  request.now = now.value();
  request.expected_snapshot_revision = store.value()->authority().revision;
  request.publisher = publisher_id.value();
  auto capacity = args.optional_u64("capacity-generation", 0u);
  if (!capacity.has_value()) {
    return emit_error(capacity.error());
  }
  if (args.has("capacity-generation")) {
    request.facility_capacity_generation = entl::Generation::from_value(capacity.value());
  }
  auto policy = args.optional_u64("policy-revision", 0u);
  if (!policy.has_value()) {
    return emit_error(policy.error());
  }
  if (args.has("policy-revision")) {
    request.facility_policy_revision = entl::Revision::from_value(policy.value());
  }
  auto envelope = args.optional_u64("envelope-revision", 0u);
  if (!envelope.has_value()) {
    return emit_error(envelope.error());
  }
  if (args.has("envelope-revision")) {
    request.resource_envelope_revision = entl::Revision::from_value(envelope.value());
  }
  auto service_class = args.optional_u64("service-class-revision", 0u);
  if (!service_class.has_value()) {
    return emit_error(service_class.error());
  }
  if (args.has("service-class-revision")) {
    request.service_class_revision = entl::Revision::from_value(service_class.value());
  }
  const auto policy_digest = args.optional("policy-context-digest");
  if (policy_digest.has_value()) {
    auto parsed = parse_digest(policy_digest.value(), "--policy-context-digest");
    if (!parsed.has_value()) {
      return emit_error(parsed.error());
    }
    request.policy_context_digest = parsed.value();
  }
  const auto envelope_digest = args.optional("envelope-binding-digest");
  if (envelope_digest.has_value()) {
    auto parsed = parse_digest(envelope_digest.value(), "--envelope-binding-digest");
    if (!parsed.has_value()) {
      return emit_error(parsed.error());
    }
    request.envelope_binding_digest = parsed.value();
  }
  auto advance = args.optional_bool("advance-epoch", false);
  if (!advance.has_value()) {
    return emit_error(advance.error());
  }
  request.advance_epoch = advance.value();
  request.note = args.optional("note").value_or(std::string());

  auto outcome = store.value()->update_authority(request);
  if (!outcome.has_value()) {
    return emit_error(outcome.error());
  }
  JsonWriter json;
  json.begin_object();
  json.key("ok").value(true);
  json.key("replayed").value(outcome.value().replayed);
  json.key("commit_seq").value(static_cast<unsigned long long>(outcome.value().commit_seq.value()));
  json.key("authority_revision")
      .value(static_cast<unsigned long long>(store.value()->authority().revision.value()));
  json.key("control_epoch")
      .value(static_cast<unsigned long long>(store.value()->authority().control_epoch.value()));
  json.key("message").value(outcome.value().message);
  json.end_object();
  std::cout << json.str() << "\n";
  return kExitOk;
}

const std::vector<std::string> kGrantKeys = {
    "tenant",           "service",        "service-class",  "facility",
    "resource-type",    "scope",          "unit",           "quantity",
    "priority-class",   "priority-rank",  "effective-from", "expires-at",
    "admission-digest", "policy-context-digest", "fence",   "actor",
    "note"};

int cmd_grant(const Args& args) {
  auto rejected = reject(args, {"dir", "request", "now", "request-id", "require-admission-evidence",
                                "idempotency-capacity"});
  if (!rejected) {
    return emit_error(rejected.error());
  }
  auto store = open_writer(args);
  if (!store.has_value()) {
    return emit_error(store.error());
  }
  auto now = resolve_now(args);
  if (!now.has_value()) {
    return emit_error(now.error());
  }
  auto source = args.require("request");
  if (!source.has_value()) {
    return emit_error(source.error());
  }
  auto text = read_text_source(source.value());
  if (!text.has_value()) {
    return emit_error(text.error());
  }
  auto document = KvDocument::parse(text.value(), kGrantKeys,
                                    {"tenant", "service", "service-class", "facility", "resource-type",
                                     "scope", "unit", "quantity", "effective-from", "expires-at",
                                     "admission-digest", "actor"});
  if (!document.has_value()) {
    return emit_error(document.error());
  }
  const KvDocument& doc = document.value();

  entl::GrantRequest request;
  request.now = now.value();
  auto request_id_value = args.optional("request-id");
  if (request_id_value.has_value()) {
    auto parsed = parse_request_id(request_id_value.value());
    if (!parsed.has_value()) {
      return emit_error(parsed.error());
    }
    request.request_id = parsed.value();
  } else {
    auto derived = derive_request_id("grant", text.value());
    if (!derived.has_value()) {
      return emit_error(derived.error());
    }
    request.request_id = derived.value();
  }

  auto tenant = parse_identifier<entl::TenantId>(doc.require("tenant").value_or(""), "tenant identity");
  if (!tenant.has_value()) return emit_error(tenant.error());
  request.holder = tenant.value();
  auto service = parse_identifier<entl::ServiceId>(doc.require("service").value_or(""), "service identity");
  if (!service.has_value()) return emit_error(service.error());
  request.service = service.value();
  auto service_class =
      parse_identifier<entl::ServiceClassId>(doc.require("service-class").value_or(""), "service class identity");
  if (!service_class.has_value()) return emit_error(service_class.error());
  request.service_class = service_class.value();
  auto facility =
      parse_identifier<entl::FacilityId>(doc.require("facility").value_or(""), "facility identity");
  if (!facility.has_value()) return emit_error(facility.error());
  request.scope.facility = facility.value();
  auto resource_type = parse_identifier<entl::ResourceTypeId>(doc.require("resource-type").value_or(""),
                                                             "resource type identity");
  if (!resource_type.has_value()) return emit_error(resource_type.error());
  request.scope.resource_type = resource_type.value();
  auto scope = parse_identifier<entl::ResourceScopeId>(doc.require("scope").value_or(""), "scope identity");
  if (!scope.has_value()) return emit_error(scope.error());
  request.scope.scope = scope.value();

  auto unit = parse_unit(doc.require("unit").value_or(""));
  if (!unit.has_value()) return emit_error(unit.error());
  request.unit = unit.value();
  auto quantity = entl::Quantity::parse(unit.value(), doc.require("quantity").value_or(""));
  if (!quantity.has_value()) return emit_error(Error(ErrorCode::kInvalidQuantity, "quantity is not valid"));
  request.quantity = quantity.value();

  auto priority_class_text = doc.optional("priority-class");
  if (priority_class_text.has_value()) {
    const auto klass = entl::priority_class_from_name(priority_class_text.value());
    if (!klass.has_value()) {
      return emit_error(Error(ErrorCode::kInvalidPriority, "unknown priority class",
                              priority_class_text.value()));
    }
    request.priority.klass = klass.value();
  }
  const auto rank_text = doc.optional("priority-rank");
  if (rank_text.has_value()) {
    std::uint64_t rank = 0;
    for (const char c : rank_text.value()) {
      if (c < '0' || c > '9' || rank > 65535u) {
        return emit_error(Error(ErrorCode::kInvalidPriority, "priority rank must be 0..65535"));
      }
      rank = (rank * 10u) + static_cast<std::uint64_t>(c - '0');
    }
    if (rank > 65535u) {
      return emit_error(Error(ErrorCode::kInvalidPriority, "priority rank must be 0..65535"));
    }
    request.priority.rank = static_cast<std::uint16_t>(rank);
  }

  auto effective = parse_instant(doc.require("effective-from").value_or(""), "effective-from");
  if (!effective.has_value()) return emit_error(effective.error());
  request.effective_from = effective.value();
  auto expires = parse_instant(doc.require("expires-at").value_or(""), "expires-at");
  if (!expires.has_value()) return emit_error(expires.error());
  request.expires_at = expires.value();

  auto admission = parse_digest(doc.require("admission-digest").value_or(""), "admission-digest");
  if (!admission.has_value()) return emit_error(admission.error());
  request.admission_decision_digest = admission.value();

  const auto policy_digest = doc.optional("policy-context-digest");
  if (policy_digest.has_value() && !policy_digest->empty()) {
    auto parsed = parse_digest(policy_digest.value(), "policy-context-digest");
    if (!parsed.has_value()) return emit_error(parsed.error());
    request.policy_context_digest = parsed.value();
  }

  const auto fence_text = doc.optional("fence");
  if (fence_text.has_value() && !fence_text->empty()) {
    if (fence_text.value() == "standard") {
      request.fence = entl::FenceMask::standard();
    } else if (fence_text.value() == "all") {
      request.fence = entl::FenceMask::all();
    } else if (fence_text.value() == "epoch") {
      request.fence = entl::FenceMask::control_epoch_only();
    } else {
      return emit_error(Error(ErrorCode::kInvalidFenceMask, "fence must be standard, all, or epoch"));
    }
  } else {
    request.fence = entl::FenceMask::standard();
  }

  auto actor = parse_identifier<entl::ActorId>(doc.require("actor").value_or(""), "actor identity");
  if (!actor.has_value()) return emit_error(actor.error());
  request.actor = actor.value();
  request.note = doc.optional("note").value_or(std::string());

  auto outcome = store.value()->grant(request);
  if (!outcome.has_value()) {
    return emit_error(outcome.error());
  }
  JsonWriter json;
  json.begin_object();
  json.key("ok").value(true);
  json.key("replayed").value(outcome.value().replayed);
  json.key("id").value(outcome.value().primary_id.to_hex());
  json.key("revision").value(static_cast<unsigned long long>(outcome.value().primary_revision.value()));
  json.key("commit_seq").value(static_cast<unsigned long long>(outcome.value().commit_seq.value()));
  json.key("message").value(outcome.value().message);
  json.end_object();
  std::cout << json.str() << "\n";
  return kExitOk;
}
int cmd_show(const Args& args) {
  auto rejected = reject(args, {"dir", "id"});
  if (!rejected) return emit_error(rejected.error());
  auto store = open_reader(args);
  if (!store.has_value()) return emit_error(store.error());
  auto id = parse_entitlement_id(args.require("id").value_or(""));
  if (!id.has_value()) return emit_error(id.error());
  auto record = store.value()->get(id.value());
  if (!record.has_value()) return emit_error(record.error());
  JsonWriter json;
  json.begin_object();
  json.key("ok").value(true);
  json.key("entitlement");
  write_entitlement(json, record.value());
  json.end_object();
  std::cout << json.str() << "\n";
  return kExitOk;
}

int cmd_list(const Args& args) {
  auto rejected = reject(args, {"dir", "tenant", "service", "state", "relation", "root", "derived-from",
                                "live", "now", "limit", "facility", "resource-type", "scope"});
  if (!rejected) return emit_error(rejected.error());
  auto store = open_reader(args);
  if (!store.has_value()) return emit_error(store.error());
  entl::ListFilter filter;
  const auto tenant = args.optional("tenant");
  if (tenant.has_value()) {
    auto parsed = parse_identifier<entl::TenantId>(tenant.value(), "tenant identity");
    if (!parsed.has_value()) return emit_error(parsed.error());
    filter.holder = parsed.value();
  }
  const auto service = args.optional("service");
  if (service.has_value()) {
    auto parsed = parse_identifier<entl::ServiceId>(service.value(), "service identity");
    if (!parsed.has_value()) return emit_error(parsed.error());
    filter.service = parsed.value();
  }
  const auto state = args.optional("state");
  if (state.has_value()) {
    const auto parsed = entl::entitlement_state_from_name(state.value());
    if (!parsed.has_value()) return emit_error(Error(ErrorCode::kInvalidEnumValue, "unknown entitlement state"));
    filter.state = parsed.value();
  }
  const auto relation = args.optional("relation");
  if (relation.has_value()) {
    entl::LineageRelation parsed{};
    bool matched = true;
    if (relation.value() == "root") parsed = entl::LineageRelation::kRoot;
    else if (relation.value() == "split_from") parsed = entl::LineageRelation::kSplitFrom;
    else if (relation.value() == "delegated_from") parsed = entl::LineageRelation::kDelegatedFrom;
    else if (relation.value() == "moved_from") parsed = entl::LineageRelation::kMovedFrom;
    else if (relation.value() == "reissued_from") parsed = entl::LineageRelation::kReissuedFrom;
    else if (relation.value() == "merged_from") parsed = entl::LineageRelation::kMergedFrom;
    else matched = false;
    if (!matched) return emit_error(Error(ErrorCode::kInvalidEnumValue, "unknown lineage relation"));
    filter.relation = parsed;
  }
  const auto root = args.optional("root");
  if (root.has_value()) {
    auto parsed = parse_entitlement_id(root.value());
    if (!parsed.has_value()) return emit_error(parsed.error());
    filter.root = parsed.value();
  }
  const auto derived_from = args.optional("derived-from");
  if (derived_from.has_value()) {
    auto parsed = parse_entitlement_id(derived_from.value());
    if (!parsed.has_value()) return emit_error(parsed.error());
    filter.derived_from = parsed.value();
  }
  if (args.has("facility") || args.has("resource-type") || args.has("scope")) {
    if (!(args.has("facility") && args.has("resource-type") && args.has("scope"))) {
      return emit_error(Error(ErrorCode::kMissingField,
                              "a scope filter requires --facility, --resource-type, and --scope together"));
    }
    entl::ResourceScope target;
    auto facility = parse_identifier<entl::FacilityId>(args.optional("facility").value(), "facility identity");
    if (!facility.has_value()) return emit_error(facility.error());
    target.facility = facility.value();
    auto resource_type = parse_identifier<entl::ResourceTypeId>(args.optional("resource-type").value(),
                                                               "resource type identity");
    if (!resource_type.has_value()) return emit_error(resource_type.error());
    target.resource_type = resource_type.value();
    auto scope_id = parse_identifier<entl::ResourceScopeId>(args.optional("scope").value(), "scope identity");
    if (!scope_id.has_value()) return emit_error(scope_id.error());
    target.scope = scope_id.value();
    filter.scope = target;
  }
  auto live = args.optional_bool("live", false);
  if (!live.has_value()) return emit_error(live.error());
  filter.live_only = live.value();
  if (filter.live_only) {
    auto now = resolve_now(args);
    if (!now.has_value()) return emit_error(now.error());
    filter.now = now.value();
  }
  auto limit = args.optional_u64("limit", 1024u);
  if (!limit.has_value()) return emit_error(limit.error());
  if (limit.value() > 65536u) {
    return emit_error(Error(ErrorCode::kInvalidArgument, "--limit exceeds the supported bound"));
  }
  filter.limit = static_cast<std::size_t>(limit.value());

  auto listed = store.value()->list(filter);
  if (!listed.has_value()) return emit_error(listed.error());
  JsonWriter json;
  json.begin_object();
  json.key("ok").value(true);
  json.key("count").value(static_cast<unsigned long long>(listed->size()));
  json.key("commit_seq").value(static_cast<unsigned long long>(store.value()->commit_seq().value()));
  json.key("entitlements").begin_array();
  for (const entl::Entitlement& record : listed.value()) {
    write_entitlement(json, record);
  }
  json.end_array();
  json.end_object();
  std::cout << json.str() << "\n";
  return kExitOk;
}

int cmd_verify(const Args& args) {
  auto rejected = reject(args, {"dir", "id", "now", "quantity", "unit", "expect-tenant", "expect-service",
                                "expect-service-class", "expect-facility", "expect-resource-type",
                                "expect-scope", "expect-unit"});
  if (!rejected) return emit_error(rejected.error());
  auto store = open_reader(args);
  if (!store.has_value()) return emit_error(store.error());
  auto now = resolve_now(args);
  if (!now.has_value()) return emit_error(now.error());
  auto id = parse_entitlement_id(args.require("id").value_or(""));
  if (!id.has_value()) return emit_error(id.error());
  entl::VerifyRequest request;
  request.id = id.value();
  request.now = now.value();
  if (args.has("quantity") || args.has("unit")) {
    auto quantity = parse_quantity(args.optional("unit").value_or("count"),
                                   args.optional("quantity").value_or("0"));
    if (!quantity.has_value()) return emit_error(quantity.error());
    request.requested = quantity.value();
  }
  if (args.has("expect-tenant") || args.has("expect-service") || args.has("expect-service-class") ||
      args.has("expect-facility") || args.has("expect-resource-type") || args.has("expect-scope") ||
      args.has("expect-unit")) {
    entl::ExpectedContext context;
    auto tenant = parse_identifier<entl::TenantId>(args.optional("expect-tenant").value_or(""), "tenant identity");
    if (!tenant.has_value()) return emit_error(tenant.error());
    context.holder = tenant.value();
    auto service = parse_identifier<entl::ServiceId>(args.optional("expect-service").value_or(""),
                                                     "service identity");
    if (!service.has_value()) return emit_error(service.error());
    context.service = service.value();
    auto klass = parse_identifier<entl::ServiceClassId>(args.optional("expect-service-class").value_or(""),
                                                        "service class identity");
    if (!klass.has_value()) return emit_error(klass.error());
    context.service_class = klass.value();
    auto facility = parse_identifier<entl::FacilityId>(args.optional("expect-facility").value_or(""),
                                                       "facility identity");
    if (!facility.has_value()) return emit_error(facility.error());
    context.scope.facility = facility.value();
    auto resource_type = parse_identifier<entl::ResourceTypeId>(
        args.optional("expect-resource-type").value_or(""), "resource type identity");
    if (!resource_type.has_value()) return emit_error(resource_type.error());
    context.scope.resource_type = resource_type.value();
    auto scope = parse_identifier<entl::ResourceScopeId>(args.optional("expect-scope").value_or(""),
                                                         "scope identity");
    if (!scope.has_value()) return emit_error(scope.error());
    context.scope.scope = scope.value();
    auto unit = parse_unit(args.optional("expect-unit").value_or("count"));
    if (!unit.has_value()) return emit_error(unit.error());
    context.unit = unit.value();
    request.expected = context;
  }
  auto decision = store.value()->verify(request);
  if (!decision.has_value()) return emit_error(decision.error());
  JsonWriter json;
  json.begin_object();
  json.key("ok").value(true);
  json.key("authorized").value(decision->authorized);
  json.key("code").value(entl::error_code_name(decision->primary));
  json.key("explanation").value(decision->explanation);
  json.key("id").value(decision->id.to_hex());
  json.key("revision").value(static_cast<unsigned long long>(decision->revision.value()));
  json.key("control_epoch").value(static_cast<unsigned long long>(decision->control_epoch.value()));
  json.key("observed_commit_seq").value(static_cast<unsigned long long>(decision->observed_commit_seq.value()));
  json.key("decision_time");
  write_timestamp(json, decision->decision_time);
  json.key("stale").value(decision->stale);
  json.key("remaining");
  write_quantity(json, decision->remaining);
  json.key("binding_digest").value(decision->binding_digest.to_hex());
  json.key("secondary_faults").begin_array();
  for (const ErrorCode code : decision->secondary_faults) {
    json.value(entl::error_code_name(code));
  }
  json.end_array();
  json.end_object();
  std::cout << json.str() << "\n";
  return decision->authorized ? kExitOk : kExitRefused;
}

int cmd_token(const Args& args) {
  auto rejected = reject(args, {"dir", "id"});
  if (!rejected) return emit_error(rejected.error());
  auto store = open_reader(args);
  if (!store.has_value()) return emit_error(store.error());
  auto id = parse_entitlement_id(args.require("id").value_or(""));
  if (!id.has_value()) return emit_error(id.error());
  auto token = store.value()->issue_token(id.value());
  if (!token.has_value()) return emit_error(token.error());
  const std::vector<std::uint8_t> bytes = token.value().encode();
  JsonWriter json;
  json.begin_object();
  json.key("ok").value(true);
  json.key("id").value(token->id().to_hex());
  json.key("token_digest").value(token->token_digest().to_hex());
  json.key("token_hex").value(to_hex(bytes));
  json.end_object();
  std::cout << json.str() << "\n";
  return kExitOk;
}

int cmd_verify_token(const Args& args) {
  auto rejected = reject(args, {"dir", "token", "now", "quantity", "unit"});
  if (!rejected) return emit_error(rejected.error());
  auto store = open_reader(args);
  if (!store.has_value()) return emit_error(store.error());
  auto now = resolve_now(args);
  if (!now.has_value()) return emit_error(now.error());
  auto token_text = args.require("token");
  if (!token_text.has_value()) return emit_error(token_text.error());
  std::string hex = token_text.value();
  if (hex.rfind("file:", 0) == 0) {
    auto content = read_text_source(hex.substr(5u));
    if (!content.has_value()) return emit_error(content.error());
    hex = content.value();
    while (!hex.empty() && (hex.back() == '\n' || hex.back() == '\r' || hex.back() == ' ')) {
      hex.pop_back();
    }
  }
  auto bytes = from_hex(hex);
  if (!bytes.has_value()) return emit_error(bytes.error());
  entl::VerifyTokenRequest request;
  request.now = now.value();
  request.token = std::span<const std::uint8_t>(bytes->data(), bytes->size());
  if (args.has("quantity") || args.has("unit")) {
    auto quantity = parse_quantity(args.optional("unit").value_or("count"),
                                   args.optional("quantity").value_or("0"));
    if (!quantity.has_value()) return emit_error(quantity.error());
    request.requested = quantity.value();
  }
  auto decision = store.value()->verify_token(request);
  if (!decision.has_value()) return emit_error(decision.error());
  JsonWriter json;
  json.begin_object();
  json.key("ok").value(true);
  json.key("authorized").value(decision->authorized);
  json.key("code").value(entl::error_code_name(decision->primary));
  json.key("explanation").value(decision->explanation);
  json.key("id").value(decision->id.to_hex());
  json.key("stale").value(decision->stale);
  json.key("token_digest").value(decision->token_digest.to_hex());
  json.end_object();
  std::cout << json.str() << "\n";
  return decision->authorized ? kExitOk : kExitRefused;
}

int cmd_lifecycle(const Args& args, const std::string& command) {
  auto rejected = reject(args, {"dir", "id", "revision", "actor", "note", "now", "request-id", "reason",
                                "cascade", "require-admission-evidence", "idempotency-capacity"});
  if (!rejected) return emit_error(rejected.error());
  auto store = open_writer(args);
  if (!store.has_value()) return emit_error(store.error());
  auto now = resolve_now(args);
  if (!now.has_value()) return emit_error(now.error());
  auto id = parse_entitlement_id(args.require("id").value_or(""));
  if (!id.has_value()) return emit_error(id.error());
  auto revision = args.require_u64("revision");
  if (!revision.has_value()) return emit_error(revision.error());
  auto actor = parse_identifier<entl::ActorId>(args.require("actor").value_or(""), "actor identity");
  if (!actor.has_value()) return emit_error(actor.error());
  auto request_id_value = resolve_request_id(args, command);
  if (!request_id_value.has_value()) return emit_error(request_id_value.error());

  Result<entl::MutationOutcome> outcome = Error(ErrorCode::kInternalError, "unreachable");
  if (command == "suspend") {
    entl::SuspendRequest request;
    request.request_id = request_id_value.value();
    request.now = now.value();
    request.id = id.value();
    request.expected_revision = entl::Revision::from_value(revision.value());
    request.actor = actor.value();
    request.note = args.optional("note").value_or(std::string());
    outcome = store.value()->suspend(request);
  } else if (command == "resume") {
    entl::ResumeRequest request;
    request.request_id = request_id_value.value();
    request.now = now.value();
    request.id = id.value();
    request.expected_revision = entl::Revision::from_value(revision.value());
    request.actor = actor.value();
    request.note = args.optional("note").value_or(std::string());
    outcome = store.value()->resume(request);
  } else if (command == "revoke") {
    entl::RevokeRequest request;
    request.request_id = request_id_value.value();
    request.now = now.value();
    request.id = id.value();
    request.expected_revision = entl::Revision::from_value(revision.value());
    request.actor = actor.value();
    request.note = args.optional("note").value_or(std::string());
    auto cascade = args.optional_bool("cascade", true);
    if (!cascade.has_value()) return emit_error(cascade.error());
    request.cascade_delegated_children = cascade.value();
    const auto reason = args.optional("reason");
    if (reason.has_value()) {
      if (reason.value() == "operator") {
        request.reason = entl::TerminalReason::kOperatorRevocation;
      } else if (reason.value() == "policy") {
        request.reason = entl::TerminalReason::kPolicyRevocation;
      } else {
        return emit_error(Error(ErrorCode::kInvalidArgument, "reason must be operator or policy"));
      }
    }
    outcome = store.value()->revoke(request);
  } else {
    entl::ExpireRequest request;
    request.request_id = request_id_value.value();
    request.now = now.value();
    request.id = id.value();
    request.expected_revision = entl::Revision::from_value(revision.value());
    request.actor = actor.value();
    request.note = args.optional("note").value_or(std::string());
    outcome = store.value()->expire(request);
  }
  if (!outcome.has_value()) return emit_error(outcome.error());
  JsonWriter json;
  json.begin_object();
  json.key("ok").value(true);
  json.key("replayed").value(outcome->replayed);
  json.key("id").value(outcome->primary_id.to_hex());
  json.key("revision").value(static_cast<unsigned long long>(outcome->primary_revision.value()));
  json.key("commit_seq").value(static_cast<unsigned long long>(outcome->commit_seq.value()));
  json.key("affected").begin_array();
  for (const entl::EntitlementId& affected : outcome->affected) {
    json.value(affected.to_hex());
  }
  json.end_array();
  json.end_object();
  std::cout << json.str() << "\n";
  return kExitOk;
}

int cmd_suspend(const Args& args) { return cmd_lifecycle(args, "suspend"); }
int cmd_resume(const Args& args) { return cmd_lifecycle(args, "resume"); }
int cmd_revoke(const Args& args) { return cmd_lifecycle(args, "revoke"); }
int cmd_expire(const Args& args) { return cmd_lifecycle(args, "expire"); }

int cmd_accounting(const Args& args, bool drawing) {
  const std::string name = drawing ? "draw" : "release";
  auto rejected = reject(args, {"dir", "id", "revision", "actor", "note", "now", "request-id", "quantity",
                                "unit", "require-admission-evidence", "idempotency-capacity"});
  if (!rejected) return emit_error(rejected.error());
  auto store = open_writer(args);
  if (!store.has_value()) return emit_error(store.error());
  auto now = resolve_now(args);
  if (!now.has_value()) return emit_error(now.error());
  auto id = parse_entitlement_id(args.require("id").value_or(""));
  if (!id.has_value()) return emit_error(id.error());
  auto revision = args.require_u64("revision");
  if (!revision.has_value()) return emit_error(revision.error());
  auto actor = parse_identifier<entl::ActorId>(args.require("actor").value_or(""), "actor identity");
  if (!actor.has_value()) return emit_error(actor.error());
  auto quantity = parse_quantity(args.optional("unit").value_or("count"),
                                 args.require("quantity").value_or("0"));
  if (!quantity.has_value()) return emit_error(quantity.error());
  auto request_id_value = resolve_request_id(args, name);
  if (!request_id_value.has_value()) return emit_error(request_id_value.error());

  entl::MutationOutcome outcome;
  if (drawing) {
    entl::DrawRequest request;
    request.request_id = request_id_value.value();
    request.now = now.value();
    request.id = id.value();
    request.expected_revision = entl::Revision::from_value(revision.value());
    request.quantity = quantity.value();
    request.actor = actor.value();
    request.note = args.optional("note").value_or(std::string());
    auto result = store.value()->draw(request);
    if (!result.has_value()) return emit_error(result.error());
    outcome = result.value();
  } else {
    entl::ReleaseRequest request;
    request.request_id = request_id_value.value();
    request.now = now.value();
    request.id = id.value();
    request.expected_revision = entl::Revision::from_value(revision.value());
    request.quantity = quantity.value();
    request.actor = actor.value();
    request.note = args.optional("note").value_or(std::string());
    auto result = store.value()->release(request);
    if (!result.has_value()) return emit_error(result.error());
    outcome = result.value();
  }
  auto record = store.value()->get(id.value());
  JsonWriter json;
  json.begin_object();
  json.key("ok").value(true);
  json.key("replayed").value(outcome.replayed);
  json.key("id").value(outcome.primary_id.to_hex());
  json.key("revision").value(static_cast<unsigned long long>(outcome.primary_revision.value()));
  json.key("commit_seq").value(static_cast<unsigned long long>(outcome.commit_seq.value()));
  json.key("message").value(outcome.message);
  if (record.has_value()) {
    json.key("remaining");
    write_quantity(json, record->remaining);
  }
  json.end_object();
  std::cout << json.str() << "\n";
  return kExitOk;
}

int cmd_draw(const Args& args) { return cmd_accounting(args, true); }
int cmd_release(const Args& args) { return cmd_accounting(args, false); }
int cmd_transfer(const Args& args) {
  auto rejected = reject(args, {"dir", "id", "revision", "mode", "target", "quantity", "unit",
                                "priority-class", "priority-rank", "actor", "note", "now", "request-id",
                                "require-admission-evidence", "idempotency-capacity"});
  if (!rejected) return emit_error(rejected.error());
  auto store = open_writer(args);
  if (!store.has_value()) return emit_error(store.error());
  auto now = resolve_now(args);
  if (!now.has_value()) return emit_error(now.error());
  auto id = parse_entitlement_id(args.require("id").value_or(""));
  if (!id.has_value()) return emit_error(id.error());
  auto revision = args.require_u64("revision");
  if (!revision.has_value()) return emit_error(revision.error());
  auto actor = parse_identifier<entl::ActorId>(args.require("actor").value_or(""), "actor identity");
  if (!actor.has_value()) return emit_error(actor.error());
  auto target = parse_identifier<entl::TenantId>(args.require("target").value_or(""), "tenant identity");
  if (!target.has_value()) return emit_error(target.error());
  const auto mode = entl::transfer_mode_from_name(args.require("mode").value_or(""));
  if (!mode.has_value()) {
    return emit_error(Error(ErrorCode::kInvalidEnumValue, "mode must be move, split, or delegate"));
  }
  auto request_id_value = resolve_request_id(args, "transfer");
  if (!request_id_value.has_value()) return emit_error(request_id_value.error());

  entl::TransferRequest request;
  request.request_id = request_id_value.value();
  request.now = now.value();
  request.source = id.value();
  request.expected_revision = entl::Revision::from_value(revision.value());
  request.mode = mode.value();
  request.target_holder = target.value();
  request.actor = actor.value();
  request.note = args.optional("note").value_or(std::string());
  if (mode.value() != entl::TransferMode::kMove) {
    auto quantity = parse_quantity(args.optional("unit").value_or("count"),
                                   args.require("quantity").value_or(""));
    if (!quantity.has_value()) return emit_error(quantity.error());
    request.quantity = quantity.value();
  } else if (args.has("quantity")) {
    return emit_error(Error(ErrorCode::kUnexpectedField,
                            "a move transfers the whole remaining quantity; omit --quantity"));
  }
  const auto priority_class_text = args.optional("priority-class");
  if (priority_class_text.has_value()) {
    const auto klass = entl::priority_class_from_name(priority_class_text.value());
    if (!klass.has_value()) {
      return emit_error(Error(ErrorCode::kInvalidPriority, "unknown priority class"));
    }
    request.priority.klass = klass.value();
  }
  auto rank = args.optional_u64("priority-rank", 0u);
  if (!rank.has_value()) return emit_error(rank.error());
  if (rank.value() > 65535u) {
    return emit_error(Error(ErrorCode::kInvalidPriority, "priority rank must be 0..65535"));
  }
  request.priority.rank = static_cast<std::uint16_t>(rank.value());

  auto outcome = store.value()->transfer(request);
  if (!outcome.has_value()) return emit_error(outcome.error());
  JsonWriter json;
  json.begin_object();
  json.key("ok").value(true);
  json.key("replayed").value(outcome->replayed);
  json.key("mode").value(entl::transfer_mode_name(mode.value()));
  json.key("id").value(outcome->primary_id.to_hex());
  json.key("revision").value(static_cast<unsigned long long>(outcome->primary_revision.value()));
  json.key("commit_seq").value(static_cast<unsigned long long>(outcome->commit_seq.value()));
  json.key("affected").begin_array();
  for (const entl::EntitlementId& affected : outcome->affected) {
    json.value(affected.to_hex());
  }
  json.end_array();
  json.end_object();
  std::cout << json.str() << "\n";
  return kExitOk;
}

int cmd_merge(const Args& args) {
  auto rejected = reject(args, {"dir", "first", "first-revision", "second", "second-revision", "actor",
                                "note", "now", "request-id", "require-admission-evidence",
                                "idempotency-capacity"});
  if (!rejected) return emit_error(rejected.error());
  auto store = open_writer(args);
  if (!store.has_value()) return emit_error(store.error());
  auto now = resolve_now(args);
  if (!now.has_value()) return emit_error(now.error());
  auto first = parse_entitlement_id(args.require("first").value_or(""));
  if (!first.has_value()) return emit_error(first.error());
  auto second = parse_entitlement_id(args.require("second").value_or(""));
  if (!second.has_value()) return emit_error(second.error());
  auto first_revision = args.require_u64("first-revision");
  if (!first_revision.has_value()) return emit_error(first_revision.error());
  auto second_revision = args.require_u64("second-revision");
  if (!second_revision.has_value()) return emit_error(second_revision.error());
  auto actor = parse_identifier<entl::ActorId>(args.require("actor").value_or(""), "actor identity");
  if (!actor.has_value()) return emit_error(actor.error());
  auto request_id_value = resolve_request_id(args, "merge");
  if (!request_id_value.has_value()) return emit_error(request_id_value.error());

  entl::MergeRequest request;
  request.request_id = request_id_value.value();
  request.now = now.value();
  request.first = first.value();
  request.first_expected_revision = entl::Revision::from_value(first_revision.value());
  request.second = second.value();
  request.second_expected_revision = entl::Revision::from_value(second_revision.value());
  request.actor = actor.value();
  request.note = args.optional("note").value_or(std::string());
  auto outcome = store.value()->merge(request);
  if (!outcome.has_value()) return emit_error(outcome.error());
  JsonWriter json;
  json.begin_object();
  json.key("ok").value(true);
  json.key("replayed").value(outcome->replayed);
  json.key("id").value(outcome->primary_id.to_hex());
  json.key("revision").value(static_cast<unsigned long long>(outcome->primary_revision.value()));
  json.key("commit_seq").value(static_cast<unsigned long long>(outcome->commit_seq.value()));
  json.key("affected").begin_array();
  for (const entl::EntitlementId& affected : outcome->affected) {
    json.value(affected.to_hex());
  }
  json.end_array();
  json.end_object();
  std::cout << json.str() << "\n";
  return kExitOk;
}

int cmd_reissue(const Args& args) {
  auto rejected = reject(args, {"dir", "id", "revision", "actor", "note", "now", "request-id",
                                "keep-binding", "require-admission-evidence", "idempotency-capacity"});
  if (!rejected) return emit_error(rejected.error());
  auto store = open_writer(args);
  if (!store.has_value()) return emit_error(store.error());
  auto now = resolve_now(args);
  if (!now.has_value()) return emit_error(now.error());
  auto id = parse_entitlement_id(args.require("id").value_or(""));
  if (!id.has_value()) return emit_error(id.error());
  auto revision = args.require_u64("revision");
  if (!revision.has_value()) return emit_error(revision.error());
  auto actor = parse_identifier<entl::ActorId>(args.require("actor").value_or(""), "actor identity");
  if (!actor.has_value()) return emit_error(actor.error());
  auto request_id_value = resolve_request_id(args, "reissue");
  if (!request_id_value.has_value()) return emit_error(request_id_value.error());
  auto keep = args.optional_bool("keep-binding", false);
  if (!keep.has_value()) return emit_error(keep.error());

  entl::ReissueRequest request;
  request.request_id = request_id_value.value();
  request.now = now.value();
  request.id = id.value();
  request.expected_revision = entl::Revision::from_value(revision.value());
  request.rebind_to_current_authority = !keep.value();
  request.actor = actor.value();
  request.note = args.optional("note").value_or(std::string());
  auto outcome = store.value()->reissue(request);
  if (!outcome.has_value()) return emit_error(outcome.error());
  JsonWriter json;
  json.begin_object();
  json.key("ok").value(true);
  json.key("replayed").value(outcome->replayed);
  json.key("id").value(outcome->primary_id.to_hex());
  json.key("revision").value(static_cast<unsigned long long>(outcome->primary_revision.value()));
  json.key("commit_seq").value(static_cast<unsigned long long>(outcome->commit_seq.value()));
  json.key("affected").begin_array();
  for (const entl::EntitlementId& affected : outcome->affected) {
    json.value(affected.to_hex());
  }
  json.end_array();
  json.end_object();
  std::cout << json.str() << "\n";
  return kExitOk;
}

int cmd_lineage(const Args& args) {
  auto rejected = reject(args, {"dir", "id"});
  if (!rejected) return emit_error(rejected.error());
  auto store = open_reader(args);
  if (!store.has_value()) return emit_error(store.error());
  auto id = parse_entitlement_id(args.require("id").value_or(""));
  if (!id.has_value()) return emit_error(id.error());
  auto view = store.value()->lineage(id.value());
  if (!view.has_value()) return emit_error(view.error());
  JsonWriter json;
  json.begin_object();
  json.key("ok").value(true);
  json.key("root").value(view->root.to_hex());
  json.key("nodes").begin_array();
  for (const entl::LineageNode& node : view->nodes) {
    json.begin_object();
    json.key("id").value(node.id.to_hex());
    json.key("relation").value(entl::lineage_relation_name(node.relation));
    json.key("sources").begin_array();
    for (const entl::EntitlementId& source : node.sources) {
      json.value(source.to_hex());
    }
    json.end_array();
    json.key("holder").value(node.holder.str());
    json.key("state").value(entl::entitlement_state_name(node.state));
    json.key("terminal_reason").value(entl::terminal_reason_name(node.terminal_reason));
    json.key("revision").value(static_cast<unsigned long long>(node.revision.value()));
    json.key("created_at");
    write_timestamp(json, node.created_at);
    json.key("granted");
    write_quantity(json, node.granted);
    json.key("remaining");
    write_quantity(json, node.remaining);
    json.key("delegated_out");
    write_quantity(json, node.delegated_out);
    json.key("live").value(node.live);
    json.key("liveness_code").value(entl::error_code_name(node.liveness_code));
    json.end_object();
  }
  json.end_array();
  json.key("holder_history").begin_array();
  for (const entl::HolderChange& change : view->holder_history) {
    json.begin_object();
    json.key("from").value(change.from.str());
    json.key("to").value(change.to.str());
    json.key("at");
    write_timestamp(json, change.at);
    json.key("commit_seq").value(static_cast<unsigned long long>(change.commit_seq.value()));
    json.key("actor").value(change.actor.str());
    json.end_object();
  }
  json.end_array();
  json.end_object();
  std::cout << json.str() << "\n";
  return kExitOk;
}

int cmd_order(const Args& args) {
  auto rejected = reject(args, {"dir", "tenant", "facility", "resource-type", "scope", "unit", "now"});
  if (!rejected) return emit_error(rejected.error());
  auto store = open_reader(args);
  if (!store.has_value()) return emit_error(store.error());
  auto now = resolve_now(args);
  if (!now.has_value()) return emit_error(now.error());
  entl::OrderRequest request;
  request.now = now.value();
  const auto tenant = args.optional("tenant");
  if (tenant.has_value()) {
    auto parsed = parse_identifier<entl::TenantId>(tenant.value(), "tenant identity");
    if (!parsed.has_value()) return emit_error(parsed.error());
    request.holder = parsed.value();
  }
  if (args.has("facility") || args.has("resource-type") || args.has("scope")) {
    if (!(args.has("facility") && args.has("resource-type") && args.has("scope"))) {
      return emit_error(Error(ErrorCode::kMissingField,
                              "a scope filter requires --facility, --resource-type, and --scope together"));
    }
    entl::ResourceScope target;
    auto facility = parse_identifier<entl::FacilityId>(args.optional("facility").value(), "facility identity");
    if (!facility.has_value()) return emit_error(facility.error());
    target.facility = facility.value();
    auto resource_type = parse_identifier<entl::ResourceTypeId>(args.optional("resource-type").value(),
                                                               "resource type identity");
    if (!resource_type.has_value()) return emit_error(resource_type.error());
    target.resource_type = resource_type.value();
    auto scope = parse_identifier<entl::ResourceScopeId>(args.optional("scope").value(), "scope identity");
    if (!scope.has_value()) return emit_error(scope.error());
    target.scope = scope.value();
    request.scope = target;
  }
  const auto unit = args.optional("unit");
  if (unit.has_value()) {
    auto parsed = parse_unit(unit.value());
    if (!parsed.has_value()) return emit_error(parsed.error());
    request.unit = parsed.value();
  }
  auto result = store.value()->order_for_consumption(request);
  if (!result.has_value()) return emit_error(result.error());
  JsonWriter json;
  json.begin_object();
  json.key("ok").value(true);
  json.key("ordering_note")
      .value("advisory ordering only: it never preempts, suspends, or revokes any entitlement");
  json.key("entries").begin_array();
  for (const entl::OrderEntry& entry : result->entries) {
    json.begin_object();
    json.key("id").value(entry.id.to_hex());
    json.key("priority");
    write_priority(json, entry.priority);
    json.key("remaining");
    write_quantity(json, entry.remaining);
    json.key("effective_from");
    write_timestamp(json, entry.effective_from);
    json.key("expires_at");
    write_timestamp(json, entry.expires_at);
    json.end_object();
  }
  json.end_array();
  json.key("excluded").begin_array();
  for (const entl::ExcludedEntry& entry : result->excluded) {
    json.begin_object();
    json.key("id").value(entry.id.to_hex());
    json.key("code").value(entl::error_code_name(entry.code));
    json.key("explanation").value(entry.explanation);
    json.end_object();
  }
  json.end_array();
  json.end_object();
  std::cout << json.str() << "\n";
  return kExitOk;
}

int cmd_compact(const Args& args) {
  auto rejected = reject(args, {"dir", "fence-on-open", "idempotency-capacity",
                                "require-admission-evidence"});
  if (!rejected) return emit_error(rejected.error());
  auto store = open_writer(args);
  if (!store.has_value()) return emit_error(store.error());
  auto report = store.value()->compact();
  if (!report.has_value()) return emit_error(report.error());
  JsonWriter json;
  json.begin_object();
  json.key("ok").value(true);
  json.key("performed").value(report->performed);
  json.key("snapshot_seq").value(static_cast<unsigned long long>(report->snapshot_seq.value()));
  json.key("segments_removed").value(static_cast<unsigned long long>(report->segments_removed));
  json.key("bytes_reclaimed").value(static_cast<unsigned long long>(report->bytes_reclaimed));
  json.key("message").value(report->message);
  json.end_object();
  std::cout << json.str() << "\n";
  return kExitOk;
}

int cmd_inspect(const Args& args) {
  auto rejected = reject(args, {"dir"});
  if (!rejected) return emit_error(rejected.error());
  auto directory = args.require("dir");
  if (!directory.has_value()) return emit_error(directory.error());
  auto report = entl::Store::inspect(std::filesystem::path(directory.value()));
  if (!report.has_value()) return emit_error(report.error());
  JsonWriter json;
  json.begin_object();
  json.key("ok").value(true);
  json.key("opened").value(report->opened);
  json.key("format_version").value(static_cast<unsigned>(report->format_version));
  json.key("commit_seq").value(static_cast<unsigned long long>(report->commit_seq.value()));
  json.key("snapshot_seq").value(static_cast<unsigned long long>(report->snapshot_seq.value()));
  json.key("state_digest").value(report->state_digest.to_hex());
  json.key("manifest_digest").value(report->manifest_digest.to_hex());
  json.key("manifest_slots_consistent").value(report->manifest_slots_consistent);
  json.key("rollback_detected").value(report->rollback_detected);
  json.key("writer_lock_free").value(report->writer_lock_free);
  json.key("torn_tail_discarded").value(report->torn_tail_discarded);
  json.key("segments_scanned").value(static_cast<unsigned long long>(report->segments_scanned));
  json.key("records_replayed").value(static_cast<unsigned long long>(report->records_replayed));
  json.key("entitlements").value(static_cast<unsigned long long>(report->entitlements));
  json.key("orphan_records").value(static_cast<unsigned long long>(report->orphan_records));
  json.key("bytes_scanned").value(static_cast<unsigned long long>(report->bytes_scanned));
  json.key("findings").begin_array();
  for (const std::string& finding : report->findings) {
    json.value(finding);
  }
  json.end_array();
  json.end_object();
  std::cout << json.str() << "\n";
  return kExitOk;
}

int cmd_stats(const Args& args) {
  auto rejected = reject(args, {"dir"});
  if (!rejected) return emit_error(rejected.error());
  auto store = open_reader(args);
  if (!store.has_value()) return emit_error(store.error());
  const entl::StoreStats stats = store.value()->stats();
  JsonWriter json;
  json.begin_object();
  json.key("ok").value(true);
  json.key("commit_seq").value(static_cast<unsigned long long>(stats.commit_seq.value()));
  json.key("authority_revision").value(static_cast<unsigned long long>(stats.authority_revision.value()));
  json.key("control_epoch").value(static_cast<unsigned long long>(stats.control_epoch.value()));
  json.key("mint_ordinal").value(static_cast<unsigned long long>(stats.mint_ordinal.value()));
  json.key("entitlement_count").value(static_cast<unsigned long long>(stats.entitlement_count));
  json.key("live_count").value(static_cast<unsigned long long>(stats.live_count));
  json.key("terminal_count").value(static_cast<unsigned long long>(stats.terminal_count));
  json.key("idempotency_entries").value(static_cast<unsigned long long>(stats.idempotency_entries));
  json.key("log_segment_count").value(static_cast<unsigned long long>(stats.log_segment_count));
  json.key("log_bytes").value(static_cast<unsigned long long>(stats.log_bytes));
  json.key("fence_write_failures").value(static_cast<unsigned long long>(stats.fence_write_failures));
  json.end_object();
  std::cout << json.str() << "\n";
  return kExitOk;
}
int cmd_bench(const Args& args) {
  auto rejected = reject(args, {"dir", "grants", "verifies", "draws", "preload", "keep"});
  if (!rejected) return emit_error(rejected.error());
  auto grants = args.optional_u64("grants", 200u);
  if (!grants.has_value()) return emit_error(grants.error());
  auto verifies = args.optional_u64("verifies", 5000u);
  if (!verifies.has_value()) return emit_error(verifies.error());
  auto draws = args.optional_u64("draws", 200u);
  if (!draws.has_value()) return emit_error(draws.error());
  auto preload = args.optional_u64("preload", 0u);
  if (!preload.has_value()) return emit_error(preload.error());
  auto keep = args.optional_bool("keep", false);
  if (!keep.has_value()) return emit_error(keep.error());
  if (grants.value() == 0u || verifies.value() == 0u) {
    return emit_error(Error(ErrorCode::kInvalidArgument, "--grants and --verifies must be greater than zero"));
  }
  if (grants.value() > 100000u || verifies.value() > 1000000u || draws.value() > 100000u ||
      preload.value() > 100000u) {
    return emit_error(Error(ErrorCode::kInvalidArgument, "benchmark request exceeds the supported bound"));
  }

  std::filesystem::path directory;
  bool temporary = false;
  const auto explicit_dir = args.optional("dir");
  if (explicit_dir.has_value()) {
    directory = std::filesystem::path(explicit_dir.value());
  } else {
    std::error_code ec;
    const auto root = std::filesystem::temp_directory_path(ec);
    if (ec) {
      return emit_error(Error(ErrorCode::kIoError, "cannot locate the system temporary directory"));
    }
    directory = root / ("resource-entitlement-bench-" + std::to_string(static_cast<unsigned long long>(
                                                              std::chrono::steady_clock::now()
                                                                  .time_since_epoch()
                                                                  .count())));
    temporary = true;
  }

  entl::StoreOpenOptions options;
  options.directory = directory;
  options.create_if_missing = true;
  auto opened = entl::Store::open(options);
  if (!opened.has_value()) {
    return emit_error(opened.error());
  }
  std::unique_ptr<entl::Store> store_holder = std::move(opened.value());
  entl::Store& store = *store_holder;
  const entl::Timestamp base_instant = entl::SystemClock{}.now();
  const auto window_start = base_instant.checked_add_nanos(-3600LL * 1000000000LL).value_or(base_instant);
  const auto window_end = base_instant.checked_add_nanos(30LL * 24LL * 3600LL * 1000000000LL).value_or(base_instant);

  entl::AuthorityUpdateRequest authority;
  authority.request_id = derive_request_id("bench", "authority").value();
  authority.now = base_instant;
  authority.expected_snapshot_revision = store.authority().revision;
  authority.facility_capacity_generation = entl::Generation::from_value(1u);
  authority.facility_policy_revision = entl::Revision::from_value(1u);
  authority.resource_envelope_revision = entl::Revision::from_value(1u);
  authority.service_class_revision = entl::Revision::from_value(1u);
  authority.policy_context_digest = entl::Sha256::hash("bench-policy");
  authority.envelope_binding_digest = entl::Sha256::hash("bench-envelope");
  authority.publisher = fixed_identifier<entl::ActorId>("bench");
  auto authority_outcome = store.update_authority(authority);
  if (!authority_outcome.has_value()) {
    return emit_error(authority_outcome.error());
  }

  const auto make_grant = [&](std::uint64_t index) {
    entl::GrantRequest request;
    request.request_id = entl::RequestId::derive("bench-grant-" + std::to_string(index));
    request.now = base_instant;
    request.holder = fixed_identifier<entl::TenantId>("bench-tenant");
    request.service = fixed_identifier<entl::ServiceId>("inference");
    request.service_class = fixed_identifier<entl::ServiceClassId>("gold");
    request.scope.facility = fixed_identifier<entl::FacilityId>("dc-1");
    request.scope.resource_type = fixed_identifier<entl::ResourceTypeId>("accelerator");
    request.scope.scope = fixed_identifier<entl::ResourceScopeId>("pool-a");
    request.unit = entl::Unit::kCount;
    request.quantity = entl::Quantity::make(entl::Unit::kCount, 1000000u).value();
    request.priority.klass = entl::PriorityClass::kStandard;
    request.effective_from = window_start;
    request.expires_at = window_end;
    request.admission_decision_digest = entl::Sha256::hash("bench-admission-" + std::to_string(index));
    request.fence = entl::FenceMask::standard();
    request.actor = fixed_identifier<entl::ActorId>("bench");
    return request;
  };

  std::vector<entl::EntitlementId> preloaded;
  preloaded.reserve(static_cast<std::size_t>(preload.value()));
  const auto preload_begin = std::chrono::steady_clock::now();
  for (std::uint64_t index = 0; index < preload.value(); ++index) {
    auto outcome = store.grant(make_grant(index));
    if (!outcome.has_value()) {
      return emit_error(outcome.error());
    }
    preloaded.push_back(outcome->primary_id);
  }
  const auto preload_nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now() - preload_begin)
                                 .count();

  std::vector<entl::EntitlementId> minted;
  minted.reserve(static_cast<std::size_t>(grants.value()));
  const auto grant_begin = std::chrono::steady_clock::now();
  for (std::uint64_t index = 0; index < grants.value(); ++index) {
    auto outcome = store.grant(make_grant(preload.value() + index));
    if (!outcome.has_value()) {
      return emit_error(outcome.error());
    }
    minted.push_back(outcome->primary_id);
  }
  const auto grant_nanos =
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - grant_begin)
          .count();

  const auto verify_begin = std::chrono::steady_clock::now();
  std::size_t authorized = 0;
  for (std::uint64_t index = 0; index < verifies.value(); ++index) {
    const entl::EntitlementId& id = minted[static_cast<std::size_t>(index % minted.size())];
    entl::VerifyRequest request;
    request.id = id;
    request.now = base_instant;
    request.requested = entl::Quantity::make(entl::Unit::kCount, 1u).value();
    auto decision = store.verify(request);
    if (!decision.has_value()) {
      return emit_error(decision.error());
    }
    if (decision->authorized) {
      ++authorized;
    }
  }
  const auto verify_nanos =
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - verify_begin)
          .count();

  std::uint64_t draws_performed = 0;
  const auto draw_begin = std::chrono::steady_clock::now();
  for (std::uint64_t index = 0; index < draws.value(); ++index) {
    const entl::EntitlementId& id = minted[static_cast<std::size_t>(index % minted.size())];
    auto record = store.get(id);
    if (!record.has_value()) {
      return emit_error(record.error());
    }
    entl::DrawRequest request;
    request.request_id = entl::RequestId::derive("bench-draw-" + std::to_string(index));
    request.now = base_instant;
    request.id = id;
    request.expected_revision = record->revision;
    request.quantity = entl::Quantity::make(entl::Unit::kCount, 1u).value();
    request.actor = fixed_identifier<entl::ActorId>("bench");
    auto outcome = store.draw(request);
    if (!outcome.has_value()) {
      return emit_error(outcome.error());
    }
    ++draws_performed;
  }
  const auto draw_nanos =
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - draw_begin)
          .count();

  const entl::StoreStats before_compact = store.stats();
  const std::size_t segments_before = before_compact.log_segment_count;
  const auto compact_begin = std::chrono::steady_clock::now();
  auto compaction = store.compact();
  if (!compaction.has_value()) {
    return emit_error(compaction.error());
  }
  const auto compact_nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now() - compact_begin)
                                 .count();

  const entl::StoreStats after_compact = store.stats();
  const std::uint64_t committed_seq = store.commit_seq().value();
  const std::size_t entitlement_count = after_compact.entitlement_count;
  const std::size_t segments_after = after_compact.log_segment_count;
  store_holder.reset();

  const auto reopen_begin = std::chrono::steady_clock::now();
  auto reopened = entl::Store::open(options);
  if (!reopened.has_value()) {
    return emit_error(reopened.error());
  }
  const auto reopen_nanos =
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - reopen_begin)
          .count();
  const std::uint64_t reopened_seq = reopened.value()->commit_seq().value();
  reopened.value().reset();
  (void)segments_before;

  const auto per_op = [](std::int64_t total_nanos, std::uint64_t count) {
    if (count == 0u) {
      return static_cast<double>(0);
    }
    return static_cast<double>(total_nanos) / static_cast<double>(count) / 1000.0;
  };

  JsonWriter json;
  json.begin_object();
  json.key("ok").value(true);
  json.key("provenance").value("SYNTHETIC");
  json.key("methodology")
      .value("single host, local NTFS directory, warm file cache; every mutating operation includes the real "
             "durable path (journal append, flush, read-back verification, dual-slot manifest publication, "
             "rollback fence advance); verification is an in-memory read of the committed state");
  json.key("directory").value(directory.string());
  json.key("temporary_directory").value(temporary);
  json.key("entitlements_after_run").value(static_cast<unsigned long long>(entitlement_count));
  json.key("commit_seq").value(static_cast<unsigned long long>(committed_seq));
  json.key("commit_seq_after_reopen").value(static_cast<unsigned long long>(reopened_seq));
  json.key("log_bytes_before_compaction").value(static_cast<unsigned long long>(before_compact.log_bytes));
  json.key("log_segments_before_compaction").value(static_cast<unsigned long long>(segments_before));
  json.key("log_segments_after_compaction").value(static_cast<unsigned long long>(segments_after));
  json.key("measurements").begin_object();
  json.key("preload").begin_object();
  json.key("operations").value(static_cast<unsigned long long>(preload.value()));
  json.key("total_nanos").value(static_cast<long long>(preload_nanos));
  json.key("mean_microseconds_per_completed_operation").value(per_op(preload_nanos, preload.value()));
  json.end_object();
  json.key("grant_commit").begin_object();
  json.key("operations").value(static_cast<unsigned long long>(grants.value()));
  json.key("total_nanos").value(static_cast<long long>(grant_nanos));
  json.key("mean_microseconds_per_completed_operation").value(per_op(grant_nanos, grants.value()));
  json.end_object();
  json.key("verify").begin_object();
  json.key("operations").value(static_cast<unsigned long long>(verifies.value()));
  json.key("authorized").value(static_cast<unsigned long long>(authorized));
  json.key("total_nanos").value(static_cast<long long>(verify_nanos));
  json.key("mean_microseconds_per_completed_operation").value(per_op(verify_nanos, verifies.value()));
  json.end_object();
  json.key("draw_commit").begin_object();
  json.key("operations").value(static_cast<unsigned long long>(draws_performed));
  json.key("total_nanos").value(static_cast<long long>(draw_nanos));
  json.key("mean_microseconds_per_completed_operation").value(per_op(draw_nanos, draws_performed));
  json.end_object();
  json.key("compaction").begin_object();
  json.key("operations").value(1);
  json.key("total_nanos").value(static_cast<long long>(compact_nanos));
  json.key("mean_microseconds_per_completed_operation").value(per_op(compact_nanos, 1u));
  json.key("bytes_reclaimed").value(static_cast<unsigned long long>(compaction->bytes_reclaimed));
  json.end_object();
  json.key("reopen_recovery").begin_object();
  json.key("operations").value(1);
  json.key("total_nanos").value(static_cast<long long>(reopen_nanos));
  json.key("mean_microseconds_per_completed_operation").value(per_op(reopen_nanos, 1u));
  json.end_object();
  json.end_object();
  json.end_object();
  std::cout << json.str() << "\n";

  if (temporary && !keep.value()) {
    std::error_code ec;
    std::filesystem::remove_all(directory, ec);
  }
  return kExitOk;
}

}  // namespace

int report_error(const Error& error) { return emit_error(error); }

const std::vector<std::string>& command_names() { return kCommands; }

bool is_known_command(const std::string& name) {
  return std::find(kCommands.begin(), kCommands.end(), name) != kCommands.end();
}

void print_usage() {
  std::cout <<
      "entl -- Resource Entitlement command line\n"
      "\n"
      "Usage: entl <command> [options]\n"
      "\n"
      "Commands:\n"
      "  version                     Print version and build mode.\n"
      "  selftest                    Run built-in digest, identifier, and codec checks.\n"
      "  init                        Create a store and its bootstrap snapshot.\n"
      "  authority                   Publish a new authoritative snapshot.\n"
      "  grant --request <file|->    Grant a new generation-bound entitlement.\n"
      "  show --id <hex>             Print one entitlement record.\n"
      "  list                        List entitlements with optional filters.\n"
      "  verify --id <hex>           Verify authority at an explicit instant.\n"
      "  token --id <hex>            Encode one entitlement as a canonical token.\n"
      "  verify-token --token <hex>  Verify a token against the ledger.\n"
      "  suspend|resume|revoke|expire  Lifecycle transitions.\n"
      "  draw|release                Durable accounting against remaining authority.\n"
      "  transfer --mode move|split|delegate  Move, split, or delegate authority.\n"
      "  merge                       Merge two compatible entitlements.\n"
      "  reissue                     Mint a successor bound to current authority.\n"
      "  lineage --id <hex>          Print historical lineage and holder changes.\n"
      "  order                       Advisory priority ordering of live entitlements.\n"
      "  compact                     Publish a snapshot and reclaim superseded segments.\n"
      "  inspect                     Read-only integrity and rollback inspection.\n"
      "  stats                       Print store counters.\n"
      "  bench                       Measure completed durable operations (SYNTHETIC).\n"
      "\n"
      "Common options: --dir <path>, --now <instant>, --request-id <hex32>.\n"
      "Every command prints one JSON object on stdout.\n"
      "Exit codes: 0 success, 1 refused, 2 usage or input error, 3 storage failure.\n";
}

int dispatch(const std::string& command, const Args& args) {
  if (command == "version") return cmd_version(args);
  if (command == "selftest") return cmd_selftest(args);
  if (command == "init") return cmd_init(args);
  if (command == "authority") return cmd_authority(args);
  if (command == "grant") return cmd_grant(args);
  if (command == "show") return cmd_show(args);
  if (command == "list") return cmd_list(args);
  if (command == "verify") return cmd_verify(args);
  if (command == "token") return cmd_token(args);
  if (command == "verify-token") return cmd_verify_token(args);
  if (command == "suspend") return cmd_suspend(args);
  if (command == "resume") return cmd_resume(args);
  if (command == "revoke") return cmd_revoke(args);
  if (command == "expire") return cmd_expire(args);
  if (command == "draw") return cmd_draw(args);
  if (command == "release") return cmd_release(args);
  if (command == "transfer") return cmd_transfer(args);
  if (command == "merge") return cmd_merge(args);
  if (command == "reissue") return cmd_reissue(args);
  if (command == "lineage") return cmd_lineage(args);
  if (command == "order") return cmd_order(args);
  if (command == "compact") return cmd_compact(args);
  if (command == "inspect") return cmd_inspect(args);
  if (command == "stats") return cmd_stats(args);
  if (command == "bench") return cmd_bench(args);
  return emit_usage_error("unknown command", command);
}

}  // namespace entl::cli
