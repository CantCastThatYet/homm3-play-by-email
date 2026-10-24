#include "session_request.hpp"
#include "common.hpp"
#include <map>
#include <sstream>
#include <vector>

namespace hota_request {
namespace {
bool Fail(std::string* error, const char* message) noexcept {
    if (error) { try { *error = message; } catch (...) {} } return false;
}
bool Field(const std::string& value, std::size_t maximum, bool empty = false) {
    if ((!empty && value.empty()) || value.size() > maximum ||
        (!value.empty() && (value.front() == ' ' || value.back() == ' '))) return false;
    for (const unsigned char c : value)
        if (c < 32 || c > 126 || c == '=' || c == ';' || c == '#' || c == '"') return false;
    return true;
}
bool Ipv4(const std::string& value) {
    unsigned octets = 0, number = 0, digits = 0;
    for (std::size_t i = 0; i <= value.size(); ++i) {
        const char c = i == value.size() ? '.' : value[i];
        if (c == '.') { if (!digits || number > 255 || ++octets > 4) return false; number = digits = 0; }
        else { if (c < '0' || c > '9' || ++digits > 3) return false; number = number * 10 + c - '0'; }
    }
    return octets == 4;
}
}
bool Parse(const std::string& text, Settings& output, std::string* error) noexcept {
    try {
        if (text.empty() || text.size() > 2048 || text.find('\0') != text.npos) return Fail(error, "invalid_request_size");
        std::istringstream lines(text); std::string line; bool section = false;
        std::map<std::string, std::string> values;
        while (std::getline(lines, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            if (!section) { if (line != "[Session]") return Fail(error, "missing_session_section"); section = true; continue; }
            const auto split = line.find('=');
            if (split == line.npos || split == 0 || !values.emplace(line.substr(0, split), line.substr(split + 1)).second)
                return Fail(error, "invalid_or_duplicate_session_field");
        }
        const char* keys[] = {"Version", "Action", "PlayerName", "SessionName", "Address", "MapFile"};
        if (values.size() != 6 && !(values.size() == 7 && values.count("Port"))) return Fail(error, "unexpected_session_fields");
        for (const auto* key : keys) if (values.find(key) == values.end()) return Fail(error, "missing_session_field");
        if (values["Version"] != "1" || (values["Action"] != "Host" && values["Action"] != "Join" && values["Action"] != "SoloScaffold"))
            return Fail(error, "unsupported_session_request");
        Settings candidate{values["Action"],values["PlayerName"],values["SessionName"],values["Address"],values["MapFile"]};
        if (values.count("Port")) {
            const auto& text_port = values["Port"];
            if (text_port.empty() || text_port.size() > 5) return Fail(error, "invalid_session_port");
            unsigned port = 0;
            for (char c : text_port) { if (c < '0' || c > '9') return Fail(error, "invalid_session_port"); port = port * 10 + c - '0'; }
            if (port > 65535 || (port && port < 1024)) return Fail(error, "invalid_session_port");
            candidate.port = static_cast<std::uint16_t>(port);
        }
        if (!Field(candidate.player_name, 20) || !Field(candidate.session_name, 63) || !Ipv4(candidate.address) ||
            !Field(candidate.map_basename, 200, true)) return Fail(error, "invalid_session_field_value");
        if(candidate.action=="SoloScaffold"&&candidate.map_basename.empty())return Fail(error,"solo_scaffold_requires_checkpoint_map");
        if (!candidate.map_basename.empty() && (candidate.map_basename.find_first_of("/\\:") != std::string::npos ||
            candidate.map_basename.size() < 5 || _stricmp(candidate.map_basename.c_str() + candidate.map_basename.size() - 4, ".h3m") != 0))
            return Fail(error, "invalid_map_basename");
        output = std::move(candidate); if (error) error->clear(); return true;
    } catch (...) { return Fail(error, "session_request_parse_failed"); }
}
static bool ReadAscii(const std::wstring& path, std::string& output, std::string* error) noexcept {
    try {
        hp::Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        BY_HANDLE_FILE_INFORMATION info{}; LARGE_INTEGER length{};
        if (!file || !GetFileInformationByHandle(file.value, &info) ||
            (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
            !GetFileSizeEx(file.value, &length) || length.QuadPart < 2 || length.QuadPart > 4098 || length.QuadPart % 2)
            return Fail(error, "session_request_file_unavailable_or_invalid");
        std::vector<unsigned char> bytes(static_cast<std::size_t>(length.QuadPart)); DWORD count = 0;
        if (!ReadFile(file.value, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr) || count != bytes.size() ||
            bytes[0] != 0xff || bytes[1] != 0xfe) return Fail(error, "session_request_must_be_utf16le");
        std::string text; text.reserve(bytes.size()/2);
        for (std::size_t i = 2; i < bytes.size(); i += 2) {
            if (bytes[i+1] || bytes[i] > 127) return Fail(error, "session_request_requires_ascii_fields");
            text += static_cast<char>(bytes[i]);
        }
        output = std::move(text); return true;
    } catch (...) { return Fail(error, "session_request_read_failed"); }
}
bool Read(const std::wstring& path, Settings& output, std::string* error) noexcept {
    std::string text; return ReadAscii(path,text,error) && Parse(text,output,error);
}
bool ParseStart(const std::string& text, StartSettings& output, std::string* error) noexcept {
    try {
        if(text.empty()||text.size()>2048||text.find('\0')!=text.npos)return Fail(error,"invalid_start_request_size");
        std::istringstream lines(text);std::string line;bool section=false;
        std::map<std::string,std::string> values;
        while(std::getline(lines,line)) {
            if(!line.empty()&&line.back()=='\r')line.pop_back();if(line.empty())continue;
            if(!section) { if(line!="[Start]")return Fail(error,"missing_start_section");section=true;continue; }
            const auto split=line.find('=');
            if(split==line.npos||!split||!values.emplace(line.substr(0,split),line.substr(split+1)).second)return Fail(error,"invalid_or_duplicate_start_field");
        }
        const char* keys[]={"Version","Action","PeerId1","PeerId2","MapFile","SimTurnThrough"};
        if(values.size()!=6)return Fail(error,"unexpected_start_fields");
        for(const auto* key:keys)if(!values.count(key))return Fail(error,"missing_start_field");
        if(values["Version"]!="1"||(values["Action"]!="HostStart"&&values["Action"]!="PeerAccept"))return Fail(error,"unsupported_start_request");
        auto number=[](const std::string& s,std::uint32_t& result) {
            if(s.empty()||s.size()>10)return false;std::uint64_t value=0;
            for(char c:s){if(c<'0'||c>'9')return false;value=value*10+c-'0';if(value>UINT32_MAX)return false;}
            result=static_cast<std::uint32_t>(value);return true;
        };
        StartSettings candidate;candidate.action=values["Action"];candidate.map_basename=values["MapFile"];
        if(!number(values["PeerId1"],candidate.peer_ids[0])||!number(values["PeerId2"],candidate.peer_ids[1])||
           !candidate.peer_ids[0]||!candidate.peer_ids[1]||candidate.peer_ids[0]==candidate.peer_ids[1]||
           !number(values["SimTurnThrough"],candidate.date_mwd))return Fail(error,"invalid_start_identity_or_date");
        const auto date=candidate.date_mwd;
        if(date/100<1||date/100>12||(date/10)%10<1||(date/10)%10>4||date%10<1||date%10>7)return Fail(error,"invalid_start_date");
        const auto& map=candidate.map_basename;
        if(!Field(map,120)||map.find_first_of("/\\:")!=std::string::npos||map.find("..")!=std::string::npos||
           map.size()<5||_stricmp(map.c_str()+map.size()-4,".h3m"))return Fail(error,"invalid_start_map");
        output=std::move(candidate);if(error)error->clear();return true;
    }catch(...){return Fail(error,"start_request_parse_failed");}
}
bool ReadStart(const std::wstring& path, StartSettings& output, std::string* error) noexcept {
    std::string text;return ReadAscii(path,text,error)&&ParseStart(text,output,error);
}
}
