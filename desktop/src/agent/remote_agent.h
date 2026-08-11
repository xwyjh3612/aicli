#pragma once

#include "core/models.h"
#include "data/data_store.h"
#include "agent/terminal_stream_protocol.h"

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

struct mg_connection;
struct mg_context;

namespace aicli::agent {

struct PairingInfo {
    std::wstring pairing_id;
    std::wstring challenge;
    std::wstring qr_payload;
    std::wstring certificate_fingerprint;
    std::int64_t expires_at{};
};

struct PendingPairing {
    PairingInfo info;
    DeviceRecord device;
    bool submitted{};
    bool authorized{};
    bool rejected{};
};

struct AgentConfig {
    std::wstring computer_name;
    std::wstring bind_address{L"0.0.0.0"};
    std::wstring port{L"4280"};
    std::wstring local_address;
    std::wstring remote_address;
    std::wstring data_directory;
};

class RemoteAgent {
public:
    using PairingCallback = std::function<void(const PendingPairing&)>;
    using InputCallback = std::function<bool(const std::wstring& project_id,
                                             const std::wstring& phone_id,
                                             const std::wstring& input,
                                             std::wstring* error)>;
    using KeyCallback = std::function<bool(const std::wstring& project_id,
                                           const std::wstring& phone_id,
                                           const std::wstring& key,
                                           std::wstring* error)>;
    using ControlCallback = std::function<bool(const std::wstring& project_id,
                                               const std::wstring& phone_id,
                                               const std::wstring& action,
                                               std::wstring* error)>;
    using ConversationsCallback = std::function<std::string()>;
    using EventsCallback = std::function<std::string(const std::wstring& project_id,
                                                     std::int64_t after_event_id)>;
    using TerminalReplayCallback =
        std::function<TerminalReplayBatch(const std::wstring& project_id,
                                          const std::wstring& terminal_session_id,
                                          std::uint64_t after_sequence)>;
    using TerminalHistoryCallback =
        std::function<TerminalHistoryPage(const std::wstring& project_id,
                                          const std::wstring& history_token,
                                          std::size_t before_line,
                                          std::size_t limit)>;
    using StatusCallback = std::function<void(bool online, const std::wstring& message)>;

    RemoteAgent() = default;
    ~RemoteAgent();
    RemoteAgent(const RemoteAgent&) = delete;
    RemoteAgent& operator=(const RemoteAgent&) = delete;

    bool Start(DataStore* store, const AgentConfig& config, std::wstring* error = nullptr);
    void Stop();
    bool Online() const noexcept;

    void SetPairingCallback(PairingCallback callback);
    void SetInputCallback(InputCallback callback);
    void SetKeyCallback(KeyCallback callback);
    void SetControlCallback(ControlCallback callback);
    void SetConversationsCallback(ConversationsCallback callback);
    void SetEventsCallback(EventsCallback callback);
    void SetTerminalReplayCallback(TerminalReplayCallback callback);
    void SetTerminalHistoryCallback(TerminalHistoryCallback callback);
    void SetStatusCallback(StatusCallback callback);

    PairingInfo CreatePairing(std::int64_t lifetime_seconds = 300);
    bool ConfirmPairing(const std::wstring& pairing_id, bool approve,
                       std::wstring* error = nullptr);
    std::vector<PendingPairing> PendingPairings() const;
    std::vector<DeviceRecord> Devices() const;
    bool RevokeDevice(const std::wstring& phone_id);

    void BroadcastEvent(const std::wstring& project_id, const std::string& event_json);
    const std::wstring& ComputerId() const noexcept { return computer_id_; }
    const std::wstring& CertificateFingerprint() const noexcept { return certificate_fingerprint_; }
    PairingInfo LastPairing() const;

private:
    struct Token {
        std::wstring phone_id;
        std::int64_t expires_at{};
    };
    struct AuthChallenge {
        std::wstring phone_id;
        std::wstring nonce;
        std::int64_t expires_at{};
    };
    struct InputLease {
        std::wstring phone_id;
        std::int64_t expires_at{};
    };
    struct WsClient;

    static int HandleHttp(struct mg_connection* connection, void* callback_data);
    static int HandleWsConnect(const struct mg_connection* connection, void* callback_data);
    static void HandleWsReady(struct mg_connection* connection, void* callback_data);
    static int HandleWsData(struct mg_connection* connection, int bits, char* data,
                            size_t data_length, void* callback_data);
    static void HandleWsClose(const struct mg_connection* connection, void* callback_data);

    int OnHttp(struct mg_connection* connection);
    int OnWsConnect(const struct mg_connection* connection);
    void OnWsReady(struct mg_connection* connection);
    int OnWsData(struct mg_connection* connection, int bits, char* data, size_t data_length);
    void OnWsClose(const struct mg_connection* connection);

    bool EnsureIdentity(const std::wstring& data_directory, std::wstring* error);
    bool EnsureCertificate(const std::wstring& data_directory, std::wstring* error);
    bool VerifySignature(const std::wstring& public_key_base64,
                         const std::string& message,
                         const std::wstring& signature_base64) const;
    bool AuthenticateRequest(const struct mg_connection* connection,
                             const char* method, const std::string& path,
                             const std::string& body, std::wstring& phone_id,
                             std::wstring* error);
    bool AuthenticateToken(const struct mg_connection* connection,
                           std::wstring& phone_id, std::wstring* error) const;
    bool AcquireInputLease(const std::wstring& project_id, const std::wstring& phone_id,
                           bool release, std::wstring* error);
    void WriteJson(struct mg_connection* connection, int status, const std::string& body) const;
    void WriteError(struct mg_connection* connection, int status, const std::wstring& message) const;
    std::optional<PendingPairing> FindPending(const std::wstring& pairing_id) const;
    std::wstring NewRandomToken(size_t bytes = 32) const;
    std::wstring NewComputerId() const;
    std::string LocalAddress() const;
    void NotifyPairing(const PendingPairing& pairing);
    void NotifyStatus(bool online, const std::wstring& message);

    mutable std::recursive_mutex mutex_;
    DataStore* store_{};
    AgentConfig config_;
    mg_context* context_{};
    bool online_{};
    std::wstring computer_id_;
    std::wstring certificate_path_;
    std::wstring certificate_fingerprint_;
    std::unordered_map<std::wstring, PendingPairing> pending_pairings_;
    std::unordered_map<std::wstring, Token> tokens_;
    std::unordered_map<std::wstring, AuthChallenge> auth_challenges_;
    std::unordered_map<std::wstring, InputLease> input_leases_;
    std::vector<std::shared_ptr<WsClient>> websocket_clients_;
    PairingInfo last_pairing_;
    PairingCallback pairing_callback_;
    InputCallback input_callback_;
    KeyCallback key_callback_;
    ControlCallback control_callback_;
    ConversationsCallback conversations_callback_;
    EventsCallback events_callback_;
    TerminalReplayCallback terminal_replay_callback_;
    TerminalHistoryCallback terminal_history_callback_;
    StatusCallback status_callback_;
};

}
