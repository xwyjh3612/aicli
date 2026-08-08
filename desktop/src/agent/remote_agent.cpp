#include "agent/remote_agent.h"

#include <civetweb.h>
#include <nlohmann/json.hpp>

#include <openssl/evp.h>
#include <openssl/ec.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <ws2tcpip.h>
#include <aclapi.h>
#include <sddl.h>
#include <objbase.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string_view>
#include <atomic>
#include <cstring>

namespace aicli::agent {
namespace {

using json = nlohmann::json;

std::int64_t Now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string Utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                                          static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) return {};
    std::string result(static_cast<size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                        result.data(), count, nullptr, nullptr);
    return result;
}

std::wstring Wide(const std::string& value) {
    if (value.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                          static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) return {};
    std::wstring result(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                        result.data(), count);
    return result;
}

std::string Base64UrlEncode(const unsigned char* bytes, size_t size) {
    if (!bytes || size == 0) return {};
    std::string result(((size + 2) / 3) * 4, '\0');
    const int count = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(result.data()), bytes,
                                      static_cast<int>(size));
    result.resize(static_cast<size_t>(std::max(0, count)));
    for (char& value : result) {
        if (value == '+') value = '-';
        else if (value == '/') value = '_';
    }
    while (!result.empty() && result.back() == '=') result.pop_back();
    return result;
}

std::string Base64UrlEncode(const std::string& value) {
    return Base64UrlEncode(reinterpret_cast<const unsigned char*>(value.data()), value.size());
}

std::vector<unsigned char> Base64UrlDecode(std::string value) {
    for (char& current : value) {
        if (current == '-') current = '+';
        else if (current == '_') current = '/';
    }
    while (value.size() % 4) value.push_back('=');
    std::vector<unsigned char> output((value.size() / 4) * 3 + 3);
    const int count = EVP_DecodeBlock(output.data(),
                                      reinterpret_cast<const unsigned char*>(value.data()),
                                      static_cast<int>(value.size()));
    if (count < 0) return {};
    size_t actual = static_cast<size_t>(count);
    if (!value.empty() && value.back() == '=') --actual;
    if (value.size() > 1 && value[value.size() - 2] == '=') --actual;
    output.resize(actual);
    return output;
}

std::wstring RandomString(size_t bytes) {
    std::vector<unsigned char> random(bytes);
    if (RAND_bytes(random.data(), static_cast<int>(random.size())) != 1) {
        for (auto& value : random) value = static_cast<unsigned char>(rand() & 0xff);
    }
    return Wide(Base64UrlEncode(random.data(), random.size()));
}

std::string Sha256Hex(const std::string& value) {
    std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
    SHA256(reinterpret_cast<const unsigned char*>(value.data()), value.size(), digest.data());
    std::ostringstream stream;
    stream << std::uppercase << std::hex << std::setfill('0');
    for (const auto byte : digest) stream << std::setw(2) << static_cast<unsigned>(byte);
    return stream.str();
}

std::string Hex(const unsigned char* bytes, size_t size) {
    std::ostringstream stream;
    stream << std::uppercase << std::hex << std::setfill('0');
    for (size_t i = 0; i < size; ++i) stream << std::setw(2) << static_cast<unsigned>(bytes[i]);
    return stream.str();
}

std::string ReadBody(struct mg_connection* connection) {
    const auto* info = mg_get_request_info(connection);
    if (!info) return {};
    const char* length_header = mg_get_header(connection, "Content-Length");
    const long long length = length_header ? _strtoi64(length_header, nullptr, 10) : 0;
    if (length < 0 || length > 1024 * 1024) return {};
    std::string body(static_cast<size_t>(length), '\0');
    size_t offset = 0;
    while (offset < body.size()) {
        const int read = mg_read(connection, body.data() + offset, body.size() - offset);
        if (read <= 0) return {};
        offset += static_cast<size_t>(read);
    }
    return body;
}

std::string QueryValue(const struct mg_request_info* info, const char* name) {
    if (!info || !info->query_string) return {};
    char value[2048]{};
    const int length = mg_get_var(info->query_string, strlen(info->query_string), name,
                                  value, sizeof(value));
    return length > 0 ? std::string{value, static_cast<size_t>(length)} : std::string{};
}

std::string Header(const struct mg_connection* connection, const char* name) {
    const char* value = mg_get_header(connection, name);
    return value ? std::string{value} : std::string{};
}

bool ParseInt64(const std::string& value, std::int64_t& output) {
    if (value.empty()) return false;
    char* end{};
    const auto parsed = _strtoi64(value.c_str(), &end, 10);
    if (!end || *end != '\0') return false;
    output = parsed;
    return true;
}

std::string JsonString(const json& value) {
    return value.dump(-1, ' ', false, json::error_handler_t::replace);
}

void SetCertificatePermissions(const std::wstring& path) {
    // The PEM contains the Agent's private TLS key.  Keep it readable only by
    // the owning Windows account and LocalSystem; the phone never receives the
    // key, only the certificate's SHA-256 pin.
    PSECURITY_DESCRIPTOR descriptor{};
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;;FA;;;SY)(A;;FA;;;OW)", SDDL_REVISION_1, &descriptor, nullptr)) {
        BOOL present = FALSE, defaulted = FALSE;
        PACL dacl = nullptr;
        if (GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted) && present) {
            SetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT,
                                  DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                  nullptr, nullptr, dacl, nullptr);
        }
        LocalFree(descriptor);
    }
}

}

struct RemoteAgent::WsClient {
    mg_connection* connection{};
    std::wstring phone_id;
    std::wstring project_id;
    std::wstring terminal_session_id;
    std::uint64_t after_sequence{};
    std::atomic_bool closed{false};
    std::atomic_bool ready{false};
    std::mutex write_mutex;
    std::shared_ptr<WsClient>* connection_handle{};
};

RemoteAgent::~RemoteAgent() {
    Stop();
}

bool RemoteAgent::Start(DataStore* store, const AgentConfig& config, std::wstring* error) {
    std::scoped_lock lock(mutex_);
    if (online_) return true;
    store_ = store;
    config_ = config;
    if (!store_) {
        if (error) *error = L"Agent 未连接 SQLite";
        return false;
    }
    computer_id_ = store_->GetSetting(L"computer_id");
    if (computer_id_.empty()) {
        computer_id_ = NewComputerId();
        store_->SetSetting(L"computer_id", computer_id_);
    }
    if (!EnsureCertificate(config.data_directory.empty() ? aicli::DefaultDataDirectory()
                                                          : config.data_directory, error)) {
        return false;
    }
    std::string bind = Utf8(config.bind_address.empty() ? L"0.0.0.0" : config.bind_address);
    std::string port = Utf8(config.port.empty() ? L"4280" : config.port);
    std::string listen = bind + ":" + port + "s";
    std::string certificate = Utf8(certificate_path_);
    const char* options[] = {
        "listening_ports", listen.c_str(),
        "ssl_certificate", certificate.c_str(),
        "num_threads", "8",
        "enable_directory_listing", "no",
        "request_timeout_ms", "15000",
        "keep_alive_timeout_ms", "30",
        nullptr
    };
    mg_callbacks callbacks{};
    context_ = mg_start(&callbacks, this, options);
    if (!context_) {
        if (error) *error = L"HTTPS 服务启动失败，请检查端口或证书";
        return false;
    }
    mg_set_request_handler(context_, "/api", &RemoteAgent::HandleHttp, this);
    mg_set_websocket_handler(context_, "/api/events", &RemoteAgent::HandleWsConnect,
                             &RemoteAgent::HandleWsReady, &RemoteAgent::HandleWsData,
                             &RemoteAgent::HandleWsClose, this);
    online_ = true;
    NotifyStatus(true, L"HTTPS/WSS Agent 在线");
    return true;
}

void RemoteAgent::Stop() {
    mg_context* context = nullptr;
    {
        std::scoped_lock lock(mutex_);
        context = context_;
        context_ = nullptr;
        online_ = false;
        tokens_.clear();
        auth_challenges_.clear();
        input_leases_.clear();
        for (const auto& client : websocket_clients_) if (client) client->closed = true;
    }
    if (context) mg_stop(context);
    {
        std::scoped_lock lock(mutex_);
        for (const auto& client : websocket_clients_) {
            if (client && client->connection_handle) {
                delete client->connection_handle;
                client->connection_handle = nullptr;
            }
        }
        websocket_clients_.clear();
    }
    NotifyStatus(false, L"Agent 已停止");
}

bool RemoteAgent::Online() const noexcept {
    std::scoped_lock lock(mutex_);
    return online_;
}

void RemoteAgent::SetPairingCallback(PairingCallback callback) { std::scoped_lock l(mutex_); pairing_callback_=std::move(callback); }
void RemoteAgent::SetInputCallback(InputCallback callback) { std::scoped_lock l(mutex_); input_callback_=std::move(callback); }
void RemoteAgent::SetControlCallback(ControlCallback callback) { std::scoped_lock l(mutex_); control_callback_=std::move(callback); }
void RemoteAgent::SetConversationsCallback(ConversationsCallback callback) { std::scoped_lock l(mutex_); conversations_callback_=std::move(callback); }
void RemoteAgent::SetEventsCallback(EventsCallback callback) { std::scoped_lock l(mutex_); events_callback_=std::move(callback); }
void RemoteAgent::SetTerminalReplayCallback(TerminalReplayCallback callback) { std::scoped_lock l(mutex_); terminal_replay_callback_=std::move(callback); }
void RemoteAgent::SetStatusCallback(StatusCallback callback) { std::scoped_lock l(mutex_); status_callback_=std::move(callback); }

PairingInfo RemoteAgent::CreatePairing(std::int64_t lifetime_seconds) {
    std::scoped_lock lock(mutex_);
    PairingInfo info;
    info.pairing_id = RandomString(18);
    info.challenge = RandomString(32);
    info.expires_at = Now() + std::clamp<std::int64_t>(lifetime_seconds, 60, 900);
    info.certificate_fingerprint = certificate_fingerprint_;
    json payload{
        {"v", 1}, {"computerId", Utf8(computer_id_)},
        {"pairingId", Utf8(info.pairing_id)}, {"challenge", Utf8(info.challenge)},
        {"expiresAt", info.expires_at}, {"certificateSha256", Utf8(info.certificate_fingerprint)},
        {"localAddress", Utf8(config_.local_address.empty() ? Wide(LocalAddress()) : config_.local_address)},
        {"remoteAddress", Utf8(config_.remote_address)}
    };
    info.qr_payload = Wide(JsonString(payload));
    PendingPairing pending;pending.info=info;
    pending_pairings_[info.pairing_id]=pending;
    last_pairing_=info;
    return info;
}

PairingInfo RemoteAgent::LastPairing() const { std::scoped_lock l(mutex_); return last_pairing_; }

std::vector<PendingPairing> RemoteAgent::PendingPairings() const {
    std::scoped_lock l(mutex_);std::vector<PendingPairing> result;
    for(const auto& [id,pending]:pending_pairings_)if(!pending.authorized&&!pending.rejected&&pending.info.expires_at>=Now())result.push_back(pending);
    return result;
}

std::vector<DeviceRecord> RemoteAgent::Devices() const { std::scoped_lock l(mutex_);return store_?store_->LoadDevices(true):std::vector<DeviceRecord>{}; }

bool RemoteAgent::ConfirmPairing(const std::wstring& pairing_id, bool approve, std::wstring* error) {
    PendingPairing pairing;
    {
        std::scoped_lock lock(mutex_);
        auto it=pending_pairings_.find(pairing_id);
        if(it==pending_pairings_.end()){if(error)*error=L"配对请求不存在或已过期";return false;}
        if(it->second.info.expires_at<Now()){if(error)*error=L"二维码已过期";return false;}
        if(approve&&!it->second.submitted){if(error)*error=L"尚未收到手机配对请求";return false;}
        if(!approve){it->second.rejected=true;NotifyPairing(it->second);return true;}
        if(!store_->UpsertDevice(it->second.device,error))return false;
        it->second.authorized=true;pairing=it->second;
    }
    NotifyPairing(pairing);return true;
}

bool RemoteAgent::RevokeDevice(const std::wstring& phone_id) {
    const bool result=store_&&store_->RevokeDevice(phone_id);
    if(result){std::vector<std::shared_ptr<WsClient>> revoked;{std::scoped_lock l(mutex_);for(auto it=tokens_.begin();it!=tokens_.end();){if(it->second.phone_id==phone_id)it=tokens_.erase(it);else ++it;}for(auto it=input_leases_.begin();it!=input_leases_.end();){if(it->second.phone_id==phone_id)it=input_leases_.erase(it);else ++it;}for(auto& client:websocket_clients_)if(client&&client->phone_id==phone_id){client->closed=true;revoked.push_back(client);}}for(const auto& client:revoked){std::scoped_lock write(client->write_mutex);if(client->connection){const char reason[]="device revoked";mg_websocket_write(client->connection,MG_WEBSOCKET_OPCODE_CONNECTION_CLOSE,reason,sizeof(reason)-1);}}}
    return result;
}

void RemoteAgent::BroadcastEvent(const std::wstring& project_id, const std::string& event_json) {
    std::vector<std::shared_ptr<WsClient>> clients;
    {std::scoped_lock l(mutex_);clients=websocket_clients_;}
    for(const auto& client:clients){if(!client||client->closed||!client->ready||(!client->project_id.empty()&&client->project_id!=project_id))continue;std::scoped_lock write(client->write_mutex);if(client->closed||!client->ready||!client->connection)continue;if(mg_websocket_write(client->connection,MG_WEBSOCKET_OPCODE_TEXT,event_json.data(),event_json.size())<=0)client->closed=true;}
    std::scoped_lock l(mutex_);websocket_clients_.erase(std::remove_if(websocket_clients_.begin(),websocket_clients_.end(),[](const auto& c){return !c||c->closed.load();}),websocket_clients_.end());
}

int RemoteAgent::HandleHttp(struct mg_connection* connection, void* callback_data) { return static_cast<RemoteAgent*>(callback_data)->OnHttp(connection); }
int RemoteAgent::HandleWsConnect(const struct mg_connection* connection, void* callback_data) { return static_cast<RemoteAgent*>(callback_data)->OnWsConnect(connection); }
void RemoteAgent::HandleWsReady(struct mg_connection* connection, void* callback_data) { static_cast<RemoteAgent*>(callback_data)->OnWsReady(connection); }
int RemoteAgent::HandleWsData(struct mg_connection* connection,int bits,char* data,size_t data_length,void* callback_data){return static_cast<RemoteAgent*>(callback_data)->OnWsData(connection,bits,data,data_length);}
void RemoteAgent::HandleWsClose(const struct mg_connection* connection,void* callback_data){static_cast<RemoteAgent*>(callback_data)->OnWsClose(connection);}

void RemoteAgent::WriteJson(struct mg_connection* connection, int status, const std::string& body) const {
    const char* phrase=status==200?"OK":status==201?"Created":status==202?"Accepted":status==400?"Bad Request":status==401?"Unauthorized":status==403?"Forbidden":status==404?"Not Found":status==409?"Conflict":"Error";
    mg_printf(connection,"HTTP/1.1 %d %s\r\nContent-Type: application/json; charset=utf-8\r\nCache-Control: no-store\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s",status,phrase,body.size(),body.data());
}

void RemoteAgent::WriteError(struct mg_connection* connection,int status,const std::wstring& message) const { WriteJson(connection,status,JsonString(json{{"ok",false},{"error",Utf8(message)}})); }

int RemoteAgent::OnHttp(struct mg_connection* connection) {
    const auto* info=mg_get_request_info(connection);if(!info||!info->local_uri)return 0;
    const std::string path=info->local_uri;const std::string method=info->request_method?info->request_method:"GET";
    const std::string body=(method=="POST"||method=="PUT")?ReadBody(connection):std::string{};
    try {
        if(path=="/api/health"){WriteJson(connection,200,JsonString(json{{"ok",true},{"computerId",Utf8(computer_id_)},{"certificateSha256",Utf8(certificate_fingerprint_)},{"online",online_}}));return 200;}
        if(path=="/api/pair/request"&&method=="POST"){
            const auto value=json::parse(body);const auto pairing_id=Wide(value.value("pairingId",std::string{}));const auto phone_id=Wide(value.value("phoneId",std::string{}));const auto name=Wide(value.value("deviceName",std::string{}));const auto model=Wide(value.value("model",std::string{}));const auto pub=value.value("publicKey",std::string{});const auto sig=Wide(value.value("signature",std::string{}));
            if(pairing_id.empty()||phone_id.empty()||name.empty()||model.empty()||pub.empty()||sig.empty()){WriteError(connection,400,L"配对请求字段不完整");return 400;}
            std::scoped_lock lock(mutex_);auto it=pending_pairings_.find(pairing_id);if(it==pending_pairings_.end()||it->second.info.expires_at<Now()){WriteError(connection,410,L"配对二维码已过期");return 410;}
            const std::string canonical=std::string{"AICLI-PAIR|"}+Utf8(computer_id_)+"|"+Utf8(pairing_id)+"|"+Utf8(it->second.info.challenge)+"|"+Utf8(phone_id)+"|"+Utf8(name)+"|"+Utf8(model)+"|"+pub;
            if(!VerifySignature(Wide(pub),canonical,sig)){WriteError(connection,401,L"手机签名验证失败");return 401;}
            it->second.device={phone_id,name,model,Wide(pub),true,Now(),Now(),0};it->second.submitted=true;const auto pending=it->second;NotifyPairing(pending);WriteJson(connection,202,JsonString(json{{"ok",true},{"status","pending"},{"pairingId",pairing_id}}));return 202;
        }
        if(path=="/api/pair/status"&&method=="GET"){
            const auto pairing_id=Wide(QueryValue(info,"pairingId"));std::scoped_lock lock(mutex_);auto it=pending_pairings_.find(pairing_id);if(it==pending_pairings_.end()){WriteError(connection,404,L"配对请求不存在");return 404;}const auto& p=it->second;WriteJson(connection,200,JsonString(json{{"ok",true},{"status",p.authorized?"authorized":p.rejected?"rejected":p.submitted?"pending":"waiting"},{"computerId",Utf8(computer_id_)},{"certificateSha256",Utf8(certificate_fingerprint_)}}));return 200;
        }
        if(path=="/api/auth/challenge"&&method=="GET"){
            const auto phone_id=Wide(QueryValue(info,"phoneId"));if(!store_||!store_->FindDevice(phone_id).has_value()){WriteError(connection,401,L"设备未绑定");return 401;}const auto id=RandomString(18),nonce=RandomString(32);{std::scoped_lock l(mutex_);auth_challenges_[id]={phone_id,nonce,Now()+120};}WriteJson(connection,200,JsonString(json{{"ok",true},{"computerId",Utf8(computer_id_)},{"challengeId",Utf8(id)},{"nonce",Utf8(nonce)},{"expiresAt",Now()+120},{"certificateSha256",Utf8(certificate_fingerprint_)}}));return 200;
        }
        if(path=="/api/auth/token"&&method=="POST"){
            const auto value=json::parse(body);const auto phone_id=Wide(value.value("phoneId",std::string{}));const auto challenge_id=Wide(value.value("challengeId",std::string{}));const auto sig=Wide(value.value("signature",std::string{}));std::scoped_lock l(mutex_);auto ch=auth_challenges_.find(challenge_id);auto device=store_?store_->FindDevice(phone_id):std::nullopt;if(ch==auth_challenges_.end()||ch->second.phone_id!=phone_id||ch->second.expires_at<Now()||!device.has_value()||!device->active){WriteError(connection,401,L"认证挑战无效");return 401;}const std::string canonical=std::string{"AICLI-AUTH|"}+Utf8(computer_id_)+"|"+Utf8(phone_id)+"|"+Utf8(challenge_id)+"|"+Utf8(ch->second.nonce);if(!VerifySignature(device->public_key_base64,canonical,sig)){WriteError(connection,401,L"设备签名验证失败");return 401;}const auto token=RandomString(32);tokens_[token]={phone_id,Now()+900};auth_challenges_.erase(ch);WriteJson(connection,200,JsonString(json{{"ok",true},{"token",Utf8(token)},{"expiresAt",Now()+900},{"computerId",Utf8(computer_id_)},{"certificateSha256",Utf8(certificate_fingerprint_)}}));return 200;
        }
        std::wstring phone_id,error;
        if(!AuthenticateRequest(connection,method.c_str(),path,body,phone_id,&error)){WriteError(connection,error==L"未授权"?401:403,error);return error==L"未授权"?401:403;}
        if(path=="/api/conversations"&&method=="GET"){const auto callback=conversations_callback_;WriteJson(connection,200,callback?callback():"{\"items\":[]}");return 200;}
        constexpr std::string_view prefix="/api/conversations/";
        if(path.rfind(prefix,0)==0){
            const auto rest=path.substr(prefix.size());
            const auto slash=rest.find('/');
            const auto project_id=Wide(slash==std::string::npos?rest:rest.substr(0,slash));
            const auto action=slash==std::string::npos?std::string{}:rest.substr(slash+1);
            if(action=="events"&&method=="GET"){
                std::int64_t after=0; ParseInt64(QueryValue(info,"after"),after);
                auto callback=events_callback_;
                WriteJson(connection,200,callback?callback(project_id,after):"{\"items\":[]}");
                return 200;
            }
            const auto value=body.empty()?json::object():json::parse(body);
            if(action=="input"||action=="chat"){
                auto input=Wide(value.value(action=="chat"?"message":"input",std::string{}));
                if(input.empty()){WriteError(connection,400,L"输入内容不能为空");return 400;}
                if(!AcquireInputLease(project_id,phone_id,false,&error)){WriteError(connection,409,error);return 409;}
                if(action=="chat")input.push_back(L'\r');
                auto callback=input_callback_;
                if(!callback||!callback(project_id,phone_id,input,&error)){WriteError(connection,409,error.empty()?L"输入未被接受":error);return 409;}
                WriteJson(connection,200,"{\"ok\":true}");return 200;
            }
            if(action=="control"){
                const auto command=Wide(value.value("action",std::string{}));
                const bool release=command==L"release";
                if(!release&&!AcquireInputLease(project_id,phone_id,false,&error)){WriteError(connection,409,error);return 409;}
                if(release){AcquireInputLease(project_id,phone_id,true,nullptr);WriteJson(connection,200,"{\"ok\":true}");return 200;}
                auto callback=control_callback_;
                if(!callback||!callback(project_id,phone_id,command,&error)){WriteError(connection,409,error.empty()?L"控制操作未被接受":error);return 409;}
                WriteJson(connection,200,"{\"ok\":true}");return 200;
            }
        }
        WriteError(connection,404,L"接口不存在");return 404;
    } catch(const std::exception& exception){WriteError(connection,400,Wide(exception.what()));return 400;}catch(...){WriteError(connection,400,L"请求格式错误");return 400;}
}

int RemoteAgent::OnWsConnect(const struct mg_connection* connection) {
    const auto* info=mg_get_request_info(connection);if(!info)return 1;std::wstring phone,error;if(!AuthenticateRequest(connection,"GET",info->local_uri?info->local_uri:"",{},phone,&error))return 1;auto client=std::make_shared<WsClient>();client->phone_id=phone;client->project_id=Wide(QueryValue(info,"conversationId"));client->terminal_session_id=Wide(QueryValue(info,"terminalSessionId"));std::int64_t after=0;if(ParseInt64(QueryValue(info,"afterSequence"),after)&&after>0)client->after_sequence=static_cast<std::uint64_t>(after);client->connection=const_cast<mg_connection*>(connection);auto* handle=new(std::nothrow) std::shared_ptr<WsClient>(client);if(!handle)return 1;client->connection_handle=handle;mg_set_user_connection_data(connection,handle);{std::scoped_lock l(mutex_);websocket_clients_.push_back(client);}return 0;
}

void RemoteAgent::OnWsReady(struct mg_connection* connection){
    auto* handle=static_cast<std::shared_ptr<WsClient>*>(mg_get_user_connection_data(connection));if(!handle||!*handle)return;const auto client=*handle;
    TerminalReplayCallback replay_callback;{std::scoped_lock l(mutex_);replay_callback=terminal_replay_callback_;}
    const std::string hello=JsonString(json{{"type","hello"},{"computerId",Utf8(computer_id_)},{"certificateSha256",Utf8(certificate_fingerprint_)},{"terminalProtocol",1}});
    auto send=[&](const std::string& value){if(client->closed||!client->connection)return false;if(mg_websocket_write(connection,MG_WEBSOCKET_OPCODE_TEXT,value.data(),value.size())<=0){client->closed=true;return false;}return true;};
    std::scoped_lock write(client->write_mutex);
    if(!send(hello))return;
    TerminalReplayBatch first;
    if(replay_callback&&!client->project_id.empty())first=replay_callback(client->project_id,client->terminal_session_id,client->after_sequence);
    for(const auto& event:first.events)if(!send(event))return;
    client->ready=true;
    // Output produced while the first replay was being serialized was retained
    // by TerminalStreamPublisher while this client was still hidden from live
    // broadcasts. A second replay closes that small handshake gap. Events that
    // race with it can be duplicated, but sequence numbers make them harmless.
    if(replay_callback&&!client->project_id.empty()){
        const auto second=replay_callback(client->project_id,first.session_id,first.last_sequence);
        for(const auto& event:second.events)if(!send(event))return;
    }
}

int RemoteAgent::OnWsData(struct mg_connection* connection,int bits,char*,size_t){if((bits&0x0f)==MG_WEBSOCKET_OPCODE_PING)return 1;auto* handle=static_cast<std::shared_ptr<WsClient>*>(mg_get_user_connection_data(connection));if(!handle||!*handle)return 0;const auto client=*handle;const auto device=store_?store_->FindDevice(client->phone_id):std::nullopt;if(!device.has_value()||!device->active){client->closed=true;return 0;}/* WebSocket is event-only. Remote input/control uses individually signed HTTP requests. */return 1;}

void RemoteAgent::OnWsClose(const struct mg_connection* connection){auto* handle=static_cast<std::shared_ptr<WsClient>*>(mg_get_user_connection_data(connection));if(!handle)return;const auto client=*handle;if(client){client->closed=true;client->connection=nullptr;std::scoped_lock l(mutex_);websocket_clients_.erase(std::remove_if(websocket_clients_.begin(),websocket_clients_.end(),[&](const auto& value){return value==client;}),websocket_clients_.end());client->connection_handle=nullptr;}mg_set_user_connection_data(connection,nullptr);delete handle;}

bool RemoteAgent::EnsureIdentity(const std::wstring&, std::wstring*) {
    if (!store_) return false;
    if (computer_id_.empty()) {
        computer_id_ = store_->GetSetting(L"computer_id");
        if (computer_id_.empty()) {
            computer_id_ = NewComputerId();
            store_->SetSetting(L"computer_id", computer_id_);
        }
    }
    return !computer_id_.empty();
}

bool RemoteAgent::EnsureCertificate(const std::wstring& data_directory, std::wstring* error) {
    try {
        std::filesystem::path directory{data_directory};
        std::filesystem::create_directories(directory);
        const auto certificate_path = directory / L"agent-server.pem";
        certificate_path_ = certificate_path.wstring();

        auto load_fingerprint = [&]() -> bool {
            std::ifstream input(certificate_path, std::ios::binary);
            if (!input) return false;
            const std::string bytes{std::istreambuf_iterator<char>(input), {}};
            BIO* bio = BIO_new_mem_buf(bytes.data(), static_cast<int>(bytes.size()));
            if (!bio) return false;
            X509* certificate = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
            BIO_free(bio);
            if (!certificate) return false;
            unsigned char digest[EVP_MAX_MD_SIZE]{};
            unsigned int length{};
            const bool ok = X509_digest(certificate, EVP_sha256(), digest, &length) == 1;
            X509_free(certificate);
            if (!ok) return false;
            certificate_fingerprint_ = Wide(Hex(digest, length));
            return true;
        };
        if (std::filesystem::exists(certificate_path) && load_fingerprint()) {
            // The certificate file also contains the private key.  Reapply
            // the restrictive ACL on every start, because an installer,
            // restore operation, or a manual copy can have broadened the
            // inherited permissions since the previous run.
            SetCertificatePermissions(certificate_path_);
            return true;
        }

        EVP_PKEY_CTX* key_context = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
        if (!key_context) throw std::runtime_error("EVP_PKEY_CTX_new_id failed");
        EVP_PKEY* key{};
        if (EVP_PKEY_keygen_init(key_context) <= 0 ||
            EVP_PKEY_CTX_set_ec_paramgen_curve_nid(key_context, NID_X9_62_prime256v1) <= 0 ||
            EVP_PKEY_keygen(key_context, &key) <= 0) {
            EVP_PKEY_CTX_free(key_context);
            throw std::runtime_error("EC key generation failed");
        }
        EVP_PKEY_CTX_free(key_context);

        X509* certificate = X509_new();
        if (!certificate) {
            EVP_PKEY_free(key);
            throw std::runtime_error("X509_new failed");
        }
        X509_set_version(certificate, 2);
        ASN1_INTEGER_set(X509_get_serialNumber(certificate), static_cast<long>(Now() & 0x7fffffff));
        X509_gmtime_adj(X509_get_notBefore(certificate), -60);
        X509_gmtime_adj(X509_get_notAfter(certificate), 60L * 60L * 24L * 365L * 10L);
        X509_set_pubkey(certificate, key);
        X509_NAME* subject = const_cast<X509_NAME*>(X509_get_subject_name(certificate));
        const auto common_name = Utf8(L"AI CLI " + computer_id_);
        X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_UTF8,
                                   reinterpret_cast<const unsigned char*>(common_name.c_str()),
                                   -1, -1, 0);
        X509_NAME_add_entry_by_txt(subject, "O", MBSTRING_ASC,
                                   reinterpret_cast<const unsigned char*>("AI CLI"), -1, -1, 0);
        X509_set_issuer_name(certificate, subject);
        X509V3_CTX extension_context{};
        X509V3_set_ctx_nodb(&extension_context);
        X509V3_set_ctx(&extension_context, certificate, certificate, nullptr, nullptr, 0);
        if (X509_EXTENSION* extension = X509V3_EXT_conf_nid(
                nullptr, &extension_context, NID_subject_alt_name,
                const_cast<char*>("DNS:localhost,IP:127.0.0.1"))) {
            X509_add_ext(certificate, extension, -1);
            X509_EXTENSION_free(extension);
        }
        if (X509_sign(certificate, key, EVP_sha256()) <= 0) {
            X509_free(certificate);
            EVP_PKEY_free(key);
            throw std::runtime_error("certificate signing failed");
        }

        BIO* output = BIO_new(BIO_s_mem());
        if (!output || PEM_write_bio_X509(output, certificate) != 1 ||
            PEM_write_bio_PrivateKey(output, key, nullptr, nullptr, 0, nullptr, nullptr) != 1) {
            if (output) BIO_free(output);
            X509_free(certificate);
            EVP_PKEY_free(key);
            throw std::runtime_error("certificate serialization failed");
        }
        BUF_MEM* memory{};
        BIO_get_mem_ptr(output, &memory);
        std::ofstream file(certificate_path, std::ios::binary | std::ios::trunc);
        file.write(memory->data, static_cast<std::streamsize>(memory->length));
        file.close();
        BIO_free(output);
        X509_free(certificate);
        EVP_PKEY_free(key);
        SetCertificatePermissions(certificate_path_);
        if (!load_fingerprint()) throw std::runtime_error("certificate fingerprint failed");
        return true;
    } catch (const std::exception& exception) {
        if (error) *error = L"TLS 证书创建失败：" + Wide(exception.what());
        return false;
    } catch (...) {
        if (error) *error = L"TLS 证书创建失败";
        return false;
    }
}

bool RemoteAgent::VerifySignature(const std::wstring& public_key_base64,
                                  const std::string& message,
                                  const std::wstring& signature_base64) const {
    const auto key_bytes = Base64UrlDecode(Utf8(public_key_base64));
    const auto signature = Base64UrlDecode(Utf8(signature_base64));
    if (key_bytes.empty() || signature.empty()) return false;
    const unsigned char* key_cursor = key_bytes.data();
    EVP_PKEY* public_key = d2i_PUBKEY(nullptr, &key_cursor,
                                     static_cast<long>(key_bytes.size()));
    if (!public_key) return false;
    EVP_MD_CTX* context = EVP_MD_CTX_new();
    bool valid = false;
    if (context && EVP_DigestVerifyInit(context, nullptr, EVP_sha256(), nullptr, public_key) == 1 &&
        EVP_DigestVerifyUpdate(context, message.data(), message.size()) == 1 &&
        EVP_DigestVerifyFinal(context, signature.data(), signature.size()) == 1) {
        valid = true;
    }
    if (context) EVP_MD_CTX_free(context);
    EVP_PKEY_free(public_key);
    return valid;
}

bool RemoteAgent::AuthenticateToken(const struct mg_connection* connection,
                                    std::wstring& phone_id, std::wstring* error) const {
    const auto authorization = Header(connection, "Authorization");
    constexpr std::string_view prefix = "Bearer ";
    if (authorization.rfind(prefix, 0) != 0) {
        if (error) *error = L"未授权";
        return false;
    }
    const auto token = Wide(authorization.substr(prefix.size()));
    std::scoped_lock lock(mutex_);
    const auto found = tokens_.find(token);
    if (found == tokens_.end() || found->second.expires_at < Now()) {
        if (error) *error = L"未授权";
        return false;
    }
    const auto device = store_ ? store_->FindDevice(found->second.phone_id) : std::nullopt;
    if (!device.has_value() || !device->active) {
        if (error) *error = L"设备已解绑";
        return false;
    }
    phone_id = found->second.phone_id;
    return true;
}

bool RemoteAgent::AcquireInputLease(const std::wstring& project_id,
                                    const std::wstring& phone_id,
                                    bool release,
                                    std::wstring* error) {
    if (project_id.empty() || phone_id.empty()) {
        if (error) *error = L"项目或设备标识为空";
        return false;
    }
    std::scoped_lock lock(mutex_);
    const auto now = Now();
    auto found = input_leases_.find(project_id);
    if (release) {
        if (found != input_leases_.end() && found->second.phone_id == phone_id) {
            input_leases_.erase(found);
        }
        return true;
    }
    if (found != input_leases_.end() && found->second.expires_at > now &&
        found->second.phone_id != phone_id) {
        if (error) *error = L"该会话正在被另一台手机控制，请稍后重试";
        return false;
    }
    input_leases_[project_id] = InputLease{phone_id, now + 30};
    return true;
}

bool RemoteAgent::AuthenticateRequest(const struct mg_connection* connection,
                                      const char* method, const std::string& path,
                                      const std::string& body, std::wstring& phone_id,
                                      std::wstring* error) {
    if (!AuthenticateToken(connection, phone_id, error)) return false;
    const auto header_phone = Wide(Header(connection, "X-Aicli-Phone"));
    const auto request_id = Wide(Header(connection, "X-Aicli-Request-Id"));
    const auto signature = Wide(Header(connection, "X-Aicli-Signature"));
    std::int64_t timestamp{}, sequence{};
    if (header_phone != phone_id || !ParseInt64(Header(connection, "X-Aicli-Timestamp"), timestamp) ||
        !ParseInt64(Header(connection, "X-Aicli-Sequence"), sequence) ||
        request_id.empty() || signature.empty()) {
        if (error) *error = L"签名请求头不完整";
        return false;
    }
    const auto device = store_->FindDevice(phone_id);
    if (!device.has_value() || !device->active) {
        if (error) *error = L"设备已解绑";
        return false;
    }
    const std::string canonical = std::string{method ? method : "GET"} + "\n" + path + "\n" +
        Utf8(request_id) + "\n" + std::to_string(timestamp) + "\n" +
        std::to_string(sequence) + "\n" + Sha256Hex(body);
    if (!VerifySignature(device->public_key_base64, canonical, signature)) {
        if (error) *error = L"请求签名无效";
        return false;
    }
    return store_->AcceptSignedRequest(phone_id, request_id, sequence, timestamp, error);
}

std::optional<PendingPairing> RemoteAgent::FindPending(const std::wstring& pairing_id) const {
    std::scoped_lock lock(mutex_);
    const auto found = pending_pairings_.find(pairing_id);
    if (found == pending_pairings_.end()) return std::nullopt;
    return found->second;
}

std::wstring RemoteAgent::NewRandomToken(size_t bytes) const { return RandomString(bytes); }

std::wstring RemoteAgent::NewComputerId() const {
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid))) return L"computer-" + RandomString(12);
    wchar_t text[40]{};
    StringFromGUID2(guid, text, static_cast<int>(std::size(text)));
    std::wstring value{text};
    if (value.size() > 2) value = value.substr(1, value.size() - 2);
    return L"computer-" + value;
}

std::string RemoteAgent::LocalAddress() const {
    std::string address = "127.0.0.1";
    WSADATA winsock{};
    if (WSAStartup(MAKEWORD(2, 2), &winsock) == 0) {
        char hostname[256]{};
        if (gethostname(hostname, sizeof(hostname)) == 0) {
            addrinfo hints{};hints.ai_family=AF_INET;hints.ai_socktype=SOCK_STREAM;
            addrinfo* results{};
            if (getaddrinfo(hostname, nullptr, &hints, &results) == 0) {
                for (auto* current=results;current;current=current->ai_next) {
                    char numeric[NI_MAXHOST]{};
                    if (getnameinfo(current->ai_addr, static_cast<socklen_t>(current->ai_addrlen),
                                    numeric, sizeof(numeric), nullptr, 0, NI_NUMERICHOST) == 0 &&
                        std::string_view{numeric} != "127.0.0.1") {
                        address=numeric;break;
                    }
                }
                freeaddrinfo(results);
            }
        }
        WSACleanup();
    }
    return "https://" + address + ":" + Utf8(config_.port.empty() ? L"4280" : config_.port);
}

void RemoteAgent::NotifyPairing(const PendingPairing& pairing) {
    PairingCallback callback;
    { std::scoped_lock lock(mutex_); callback = pairing_callback_; }
    if (callback) callback(pairing);
}

void RemoteAgent::NotifyStatus(bool online, const std::wstring& message) {
    StatusCallback callback;
    { std::scoped_lock lock(mutex_); callback = status_callback_; }
    if (callback) callback(online, message);
}

}
