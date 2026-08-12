#pragma once

#include <cstdint>
#include <string>

namespace aicli {

struct Project {
    std::wstring id;
    std::wstring name;
    std::wstring note;
    std::wstring type;
    std::wstring group;
    std::wstring folder;
    std::wstring command;
    std::wstring status{L"created"};
    std::int64_t sort_order{};
};

struct ProjectType {
    std::int64_t id{};
    std::wstring name;
    std::wstring default_command;
    bool builtin{};
};

struct ProjectGroup {
    std::int64_t id{};
    std::wstring name;
    bool builtin{};
};

struct DeviceRecord {
    std::wstring phone_id;
    std::wstring name;
    std::wstring model;
    std::wstring public_key_base64;
    bool active{};
    std::int64_t paired_at{};
    std::int64_t last_seen{};
    std::int64_t last_sequence{};
};

struct EventRecord {
    std::int64_t id{};
    std::wstring project_id;
    std::wstring event_type;
    std::string payload_json;
    std::int64_t created_at{};
};

}
