#include "session_request.hpp"
#include <iostream>
#include <stdexcept>
int main() {
    using namespace hota_request;
    const std::string valid = "[Session]\r\nVersion=1\r\nAction=Host\r\nPlayerName=Red\r\nSessionName=HP test\r\nAddress=127.0.0.1\r\nMapFile=Small.h3m\r\n";
    Settings output; std::string error;
    try {
        if (!Parse(valid, output, &error) || output.player_name != "Red") throw std::runtime_error("Valid session request rejected");
        if (!Parse(valid + "Port=24777\n", output, &error) || output.port != 24777) throw std::runtime_error("Explicit port rejected");
        const auto reject = [&](std::string text) {
            Settings result = output;
            if (Parse(text, result, &error) || result.player_name != output.player_name) throw std::runtime_error("Invalid request accepted or output changed");
        };
        reject(valid + "Address=1.2.3.4\n"); reject(valid + "Unknown=1\n");
        reject(valid + "Port=65536\n"); reject(valid + "Port=-1\n"); reject(valid + "Port=24x77\n");
        reject(valid + "Port=\n"); reject(valid + "Port=1\nPort=2\n");
        auto traversal = valid; traversal.replace(traversal.find("Small.h3m"), 9, "../Small.h3m"); reject(traversal);
        auto ip = valid; ip.replace(ip.find("127.0.0.1"), 9, "999.0.0.1"); reject(ip);
        auto name = valid; name.replace(name.find("PlayerName=Red"), 14, "PlayerName=" + std::string(21, 'A')); reject(name);
        reject(valid + std::string(1, '\0')); reject(std::string(2049, 'A'));
        const std::string start="[Start]\nVersion=1\nAction=HostStart\nPeerId1=1\nPeerId2=4294967295\nMapFile=[HotA] Test.h3m\nSimTurnThrough=211\n";
        StartSettings started;
        if(!ParseStart(start,started,&error)||started.peer_ids[1]!=UINT32_MAX||started.date_mwd!=211)throw std::runtime_error("Valid start request rejected");
        const auto bad_start=[&](std::string text){StartSettings value=started;if(ParseStart(text,value,&error)||value.peer_ids[1]!=started.peer_ids[1])throw std::runtime_error("Invalid start accepted or output changed");};
        bad_start(start+"PeerId1=2\n");bad_start(start+"Unknown=1\n");
        auto overflow=start;overflow.replace(overflow.find("4294967295"),10,"4294967296");bad_start(overflow);
        auto same=start;same.replace(same.find("4294967295"),10,"1");bad_start(same);
        auto date=start;date.replace(date.find("211"),3,"218");bad_start(date);
        std::cout << "Session request validation tests passed; no game invoked.\n"; return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
