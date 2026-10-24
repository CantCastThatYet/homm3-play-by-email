#include "world_resume.hpp"
#include <cassert>
#include <iostream>
int main() {
    using namespace hota_resume;
    std::uint32_t hero_begin=0;
    assert(ResolveHeroArrayAddress(0x0c0f0000,0x03900000,215,hero_begin)&&hero_begin==0x0f9f0000);
    // A relocation below world is encoded as a negative disp32; LEA wraps the
    // addition, but the actual resulting hero array must remain contiguous.
    assert(ResolveHeroArrayAddress(0x20000000,0xf0000000,215,hero_begin)&&hero_begin==0x10000000);
    assert(!ResolveHeroArrayAddress(0x20000000,0xe0000000,215,hero_begin));
    assert(!ResolveHeroArrayAddress(0xf0000000,0x0fffff00,215,hero_begin));
    assert(!ResolveHeroArrayAddress(0x20000000,0xf0000000,1025,hero_begin));
    Audit source;source.width=source.height=source.map_size=144;source.underground=1;
    source.local=source.current=1;source.date={3,2,4};source.world=0x12345678;source.adventure=0x87654321;
    source.map=0x10203040;source.hero_offset=0x21620;
    source.players[0].transport_id=0xabcd;source.players[0].human=1;
    source.players[1].transport_id=0xbcde;source.players[1].human=source.players[1].local=1;
    source.players[1].resources={1,2,3,4,5,6,12000};
    source.heroes={{7,1900,475,11,27,1,1},{-1,0,0,-1,-1,255,255}};
    const auto bytes=EncodeAudit(source);assert(bytes.size()==36+288+40);
    Audit restored;assert(DecodeAudit(bytes,restored));assert(SameWorldContent(source,restored));
    assert(!restored.world&&!restored.adventure&&!restored.map&&!restored.hero_offset);
    assert(EncodeAudit(restored)==bytes);
    restored.heroes[0].movement=1900;assert(!SameWorldContent(source,restored));
    restored=source;restored.players[1].resources[6]++;assert(!SameWorldContent(source,restored));
    restored=source;restored.players[1].human=0;assert(!SameWorldContent(source,restored));
    restored=source;restored.players[0].transport_id=0;restored.players[1].transport_id=0;restored.players[1].human=0;
    assert(SameGameplayBeforeMembership(source,restored));assert(!SameWorldContent(source,restored));
    restored.players[1].local=0;restored.players[0].local=1;
    assert(SameGameplayBeforeMembership(source,restored));assert(!SameWorldContent(source,restored));
    assert(ValidSourceMembership(source,1));assert(!ValidSourceMembership(source,0));
    auto bad_membership=source;bad_membership.players[0].local=1;assert(!ValidSourceMembership(bad_membership,1));
    bad_membership=source;bad_membership.players[1].local=0;assert(!ValidSourceMembership(bad_membership,1));
    bad_membership=source;bad_membership.players[1].human=0;assert(!ValidSourceMembership(bad_membership,1));
    bad_membership=source;bad_membership.players[1].transport_id=0;assert(!ValidSourceMembership(bad_membership,1));
    restored=source;restored.heroes[0].movement++;assert(!SameGameplayBeforeMembership(source,restored));
    restored=source;restored.players[1].resources[6]++;assert(!SameGameplayBeforeMembership(source,restored));
    restored=source;restored.current=0;assert(!SameGameplayBeforeMembership(source,restored));
    auto corrupt=bytes;corrupt.push_back(0);assert(!DecodeAudit(corrupt,restored));
    corrupt=bytes;corrupt[4]=2;assert(!DecodeAudit(corrupt,restored));
    corrupt=bytes;corrupt[32]=0xff;corrupt[33]=0xff;assert(!DecodeAudit(corrupt,restored));
    corrupt=bytes;corrupt[23]=1;assert(!DecodeAudit(corrupt,restored));
    for(std::size_t i=0;i<bytes.size();++i) {corrupt.assign(bytes.begin(),bytes.begin()+i);assert(!DecodeAudit(corrupt,restored));}
    assert(DecodeAuditHex(EncodeAuditHex(source),restored));assert(SameWorldContent(source,restored));
    assert(!DecodeAuditHex("zz",restored));assert(SerializeAudit(source).find("\"movement\":475")!=std::string::npos);
    assert(!Queue(Request{}));assert(!IsGameplayBlocked());assert(!TryAtAdventureBoundary());
    std::cout<<"World audit roundtrip, pointer exclusion, semantic mismatch, malformed/truncated input and uninitialized gates passed\n";
}
