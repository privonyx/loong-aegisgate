#pragma once

// Phase 9.3 Epic 7 — aegisctl config subcommand suite.
//
// Responsibility split:
//   * `ConfigServiceClient` is a thin abstract seam over the 9 control-plane
//     RPCs. Production code uses `GrpcConfigServiceClient` which wraps a
//     `ControlPlaneClient` + stub; unit tests substitute a fake.
//   * Pure `parse*Args()` helpers convert argv slices to typed structs and
//     throw `std::invalid_argument` on bad input. No I/O.
//   * `run*()` functions execute one subcommand against an already-built
//     `ConfigServiceClient`, writing human output to out/err. They return
//     the process exit code (0 = success).
//   * `runConfigCommand()` is the top-level dispatcher invoked from
//     aegisctl.cpp behind `#ifdef AEGISGATE_ENABLE_CONTROL_PLANE`.
//
// Security hygiene (SR8): runners must never echo the operator's API key
// to stdout/stderr. The only user-visible strings are gRPC status code +
// server-supplied message, both already operator-safe by the server's SR8
// rules.

#include "cli/grpc_client.h"
#include "control_plane/v1/control_plane.grpc.pb.h"

#include <grpcpp/grpcpp.h>

#include <functional>
#include <iosfwd>
#include <memory>
#include <string>
#include <vector>

namespace aegisgate::cli {

// --------------------------------------------------------------------------
// Output format (used by runners + --output flag in Task 7.6).
// --------------------------------------------------------------------------
enum class OutputFormat { Table, Json, Yaml };

// --------------------------------------------------------------------------
// Abstract seam over the gRPC surface.
// --------------------------------------------------------------------------

class ConfigServiceClient {
public:
    virtual ~ConfigServiceClient() = default;

    virtual grpc::Status Submit(
        const aegisgate::controlplane::v1::SubmitVersionRequest& req,
        aegisgate::controlplane::v1::ConfigVersion* out) = 0;

    virtual grpc::Status Approve(
        const aegisgate::controlplane::v1::ApproveVersionRequest& req,
        aegisgate::controlplane::v1::ConfigVersion* out) = 0;

    virtual grpc::Status Reject(
        const aegisgate::controlplane::v1::RejectVersionRequest& req,
        aegisgate::controlplane::v1::ConfigVersion* out) = 0;

    virtual grpc::Status Activate(
        const aegisgate::controlplane::v1::ActivateVersionRequest& req,
        aegisgate::controlplane::v1::ConfigVersion* out) = 0;

    virtual grpc::Status Rollback(
        const aegisgate::controlplane::v1::RollbackVersionRequest& req,
        aegisgate::controlplane::v1::ConfigVersion* out) = 0;

    virtual grpc::Status List(
        const aegisgate::controlplane::v1::ListVersionsRequest& req,
        aegisgate::controlplane::v1::ListVersionsResponse* out) = 0;

    virtual grpc::Status GetVersion(
        const aegisgate::controlplane::v1::GetVersionRequest& req,
        aegisgate::controlplane::v1::ConfigVersion* out) = 0;

    virtual grpc::Status GetActive(
        const aegisgate::controlplane::v1::GetActiveRequest& req,
        aegisgate::controlplane::v1::ConfigVersion* out) = 0;

    virtual grpc::Status Diff(
        const aegisgate::controlplane::v1::DiffVersionsRequest& req,
        aegisgate::controlplane::v1::DiffVersionsResponse* out) = 0;
};

// Production implementation that wraps a ControlPlaneClient + stub.
std::unique_ptr<ConfigServiceClient> makeGrpcConfigServiceClient(
    ControlPlaneClient& base);

// --------------------------------------------------------------------------
// Task 7.2 — apply / approve / reject
// --------------------------------------------------------------------------

struct ApplyArgs {
    std::string file_path;
    std::string comment;
    bool        dry_run = false;
};

struct ReviewArgs {
    std::string version_id;
    std::string comment;
};

ApplyArgs  parseApplyArgs  (const std::vector<std::string>& argv);
ReviewArgs parseApproveArgs(const std::vector<std::string>& argv);
ReviewArgs parseRejectArgs (const std::vector<std::string>& argv);

int runApply  (ConfigServiceClient& client, const ApplyArgs&  args,
               std::ostream& out, std::ostream& err,
               OutputFormat fmt = OutputFormat::Table);
int runApprove(ConfigServiceClient& client, const ReviewArgs& args,
               std::ostream& out, std::ostream& err,
               OutputFormat fmt = OutputFormat::Table);
int runReject (ConfigServiceClient& client, const ReviewArgs& args,
               std::ostream& out, std::ostream& err,
               OutputFormat fmt = OutputFormat::Table);

// --------------------------------------------------------------------------
// Task 7.3 — activate (RPC + optional data-plane file write + optional SIGHUP)
// --------------------------------------------------------------------------

struct ActivateArgs {
    std::string version_id;
    std::string comment;                 // activation_comment (required)
    std::string data_plane_config_path;  // empty = do not write file
    int         signal_pid = 0;          // 0 = do not signal
};

ActivateArgs parseActivateArgs(const std::vector<std::string>& argv);

// Injected so tests can verify SIGHUP dispatch without touching real PIDs.
// Return 0 on success, -1 on failure; errno-style contract mirrors ::kill.
using SignalSenderFn = std::function<int(int pid, int sig)>;
int defaultSignalSender(int pid, int sig);

// Atomic file write: materializes `content` at `path` via "<path>.tmp.<pid>"
// + rename. Throws std::runtime_error on any I/O failure. Exposed for
// direct unit testing.
void writeYamlAtomic(const std::string& path, const std::string& content);

int runActivate(ConfigServiceClient& client, const ActivateArgs& args,
                std::ostream& out, std::ostream& err,
                SignalSenderFn send_signal = &defaultSignalSender,
                OutputFormat fmt = OutputFormat::Table);

// --------------------------------------------------------------------------
// Task 7.4 — rollback (SR9/T15: --emergency is always rejected by the server)
// --------------------------------------------------------------------------

struct RollbackArgs {
    std::string target_version_id;
    std::string comment;                 // rollback_comment (required)
    std::string data_plane_config_path;  // empty = do not write file
    int         signal_pid = 0;          // 0 = do not signal
    bool        emergency = false;       // SR9: server returns UNIMPLEMENTED
};

RollbackArgs parseRollbackArgs(const std::vector<std::string>& argv);

int runRollback(ConfigServiceClient& client, const RollbackArgs& args,
                std::ostream& out, std::ostream& err,
                SignalSenderFn send_signal = &defaultSignalSender,
                OutputFormat fmt = OutputFormat::Table);

// --------------------------------------------------------------------------
// Task 7.5 — read subcommands: list / get / show / current / diff
// --------------------------------------------------------------------------

struct ListArgs {
    std::vector<aegisgate::controlplane::v1::ConfigStatus> statuses;  // empty = all
    int64_t     since_millis = 0;
    int32_t     page_size    = 50;
    std::string page_token;
};

struct GetArgs  { std::string version_id; };
struct ShowArgs { std::string version_id; bool redact = false; };
struct DiffArgs {
    std::string from_version_id;
    std::string to_version_id;   // empty => server resolves to current ACTIVE
};

ListArgs parseListArgs(const std::vector<std::string>& argv);
GetArgs  parseGetArgs (const std::vector<std::string>& argv);
ShowArgs parseShowArgs(const std::vector<std::string>& argv);
DiffArgs parseDiffArgs(const std::vector<std::string>& argv);

// Parse "PENDING" | "APPROVED" | ... (case-insensitive). Unknown throws.
aegisgate::controlplane::v1::ConfigStatus statusFromString(const std::string& s);

// Replace the value of every SR4-sensitive field in `yaml_text` with
// "<redacted>". Idempotent. Non-matching lines pass through unchanged.
std::string redactYaml(const std::string& yaml_text);

// SR-NEW1 (TASK-20260515-01 C1) — test helper.
// Parses `json_text`, redacts every string value whose key matches one of
// the SENSITIVE_KEY substrings (password / secret / token / credential /
// api_key / apikey / auth / _key / comment, case-insensitive), and writes
// the pretty-printed result back to `json_text`. Returns the number of
// redactions performed.  This is the same logic that --output json runs
// unconditionally; exposed here so unit tests can pin the contract
// without spinning up the gRPC stub.
size_t redactSensitiveJsonForTest(std::string& json_text);


int runList   (ConfigServiceClient& client, const ListArgs& args,
               std::ostream& out, std::ostream& err,
               OutputFormat fmt = OutputFormat::Table);
int runGet    (ConfigServiceClient& client, const GetArgs& args,
               std::ostream& out, std::ostream& err,
               OutputFormat fmt = OutputFormat::Table);
int runShow   (ConfigServiceClient& client, const ShowArgs& args,
               std::ostream& out, std::ostream& err);
int runCurrent(ConfigServiceClient& client,
               std::ostream& out, std::ostream& err,
               OutputFormat fmt = OutputFormat::Table);
int runDiff   (ConfigServiceClient& client, const DiffArgs& args,
               std::ostream& out, std::ostream& err);

// --------------------------------------------------------------------------
// Shared helpers (exposed for tests)
// --------------------------------------------------------------------------

// Returns one of "PENDING" | "APPROVED" | "REJECTED" | "ACTIVE" |
// "SUPERSEDED" | "UNKNOWN" — stable strings shown to operators.
std::string statusToString(aegisgate::controlplane::v1::ConfigStatus s);

// --------------------------------------------------------------------------
// Task 7.6 — common flags + top-level dispatcher
// --------------------------------------------------------------------------

// Environment-variable defaults. The api_key MUST come from env (never from
// argv) so it never appears in `ps`/syslog snapshots (SR8 hygiene).
//   AEGISGATE_CP_ENDPOINT   — host:port (default 127.0.0.1:9443)
//   AEGISGATE_CP_TLS_CA     — PEM path (default "")
//   AEGISGATE_CP_TLS_CERT   — PEM path (default "")
//   AEGISGATE_CP_TLS_KEY    — PEM path (default "")
//   AEGISGATE_CP_API_KEY    — required Bearer token
//   AEGISGATE_CP_TIMEOUT    — integer seconds (default 30)
using EnvLookupFn = std::function<std::string(const std::string&)>;

struct GlobalFlags {
    ControlPlaneClient::ConnectOptions connect;
    OutputFormat                       output = OutputFormat::Table;
    std::string                        subcommand;
    std::vector<std::string>           subcommand_args;
};

// Parses the full post-`config` argv (i.e. argv[0] is the subcommand such
// as "apply"). Global flags (--endpoint/--tls-*/--timeout/--output) are
// consumed in any position; unknown flags pass through to
// subcommand_args so each subcommand's parser decides what to do with
// them.
// `lookup` is injected so unit tests can provide a deterministic
// environment view. Passing nullptr defaults to ::getenv.
GlobalFlags parseGlobalFlags(const std::vector<std::string>& argv,
                              EnvLookupFn lookup = nullptr);

// Parses "table" | "json" | "yaml" (case-insensitive). Unknown throws.
OutputFormat outputFormatFromString(const std::string& s);

// Top-level dispatcher invoked from aegisctl.cpp behind
// `#ifdef AEGISGATE_ENABLE_CONTROL_PLANE`. `argv` excludes the outer
// `aegisctl` and `config` tokens: the first element is the subcommand
// (e.g. "apply") or a global flag like "--endpoint".
int runConfigCommand(const std::vector<std::string>& argv,
                     std::ostream& out, std::ostream& err);

}  // namespace aegisgate::cli
