#include "navi33_smu_mailbox_roundtrip.h"
#include <cassert>
#include <cstdint>
#include <initializer_list>
using namespace rn_navi33;
namespace smu = smu_mailbox_admission;
namespace rt = smu_mailbox_roundtrip;

struct Fake {
    rt::RegisterPlan r;
    uint32_t response = smu::kResponseOk, parameter = 99, message = 0;
    uint32_t reply = smu::kResponseOk, version = 0x00526000u;
    uint32_t writes = 0, messageWrites = 0, pauses = 0, pollsAfterSend = 0;
    uint32_t replyAt = 3;
    bool ignoreResponseClear = false, ignoreParameterClear = false;
    explicit Fake(rt::RegisterPlan p) : r(p) {}
    uint32_t read(uint32_t offset) {
        if (offset == r.responseByteOffset) {
            if (message == smu::kGetSmuVersionMessage && ++pollsAfterSend >= replyAt) {
                response = reply;
                parameter = version;
            }
            return response;
        }
        assert(offset == r.parameterByteOffset);
        return parameter;
    }
    void write(uint32_t offset, uint32_t v) {
        ++writes;
        if (offset == r.responseByteOffset) {
            if (!ignoreResponseClear) response = v;
        } else if (offset == r.parameterByteOffset) {
            if (!ignoreParameterClear) parameter = v;
        } else {
            assert(offset == r.messageByteOffset && v == smu::kGetSmuVersionMessage);
            message = v;
            ++messageWrites;
        }
    }
    void pause() { ++pauses; }
};

smu::Report gate() { smu::Report g{};g.error=smu::Error::Ok;g.maySubmit=true;return g; }
rt::Result run(Fake &f,bool &used,smu::Report g=gate(),bool leased=true) {
    return rt::execute(g,f.r,leased,used,
        [&](uint32_t o){return f.read(o);},
        [&](uint32_t o,uint32_t v){f.write(o,v);},
        [&](){f.pause();});
}
int main() {
    const auto p=rt::plan(0x16000u, 1024u*1024u);
    assert(p.valid && p.messageByteOffset==363016u &&
           p.parameterByteOffset==363080u && p.responseByteOffset==363112u);
    assert(!rt::plan(0u,1024u*1024u).valid);
    assert(!rt::plan(0x16000u,363112u).valid);
    assert(!rt::plan(0xffffffffu,1024u*1024u).valid);
    {
        Fake f(p);bool used=false;
        const auto x=run(f,used);
        assert(x.verified && x.error==rt::Error::Ok && x.attempted);
        assert(x.responseBefore==1 && x.responseAfterClear==0 && x.parameterAfterClear==0);
        assert(x.transcript.responseAfter==1 && x.transcript.versionResult==f.version);
        assert(x.transcript.polls==3 && f.messageWrites==1 && f.writes==3 && used);
        auto second=run(f,used);
        assert(second.error==rt::Error::AlreadyAttempted && f.messageWrites==1);
    }
    {
        Fake f(p);bool used=false;
        auto denied=gate();denied.maySubmit=false;
        auto x=run(f,used,denied);
        assert(x.error==rt::Error::NotAdmitted && !used && f.writes==0);
        x=run(f,used,gate(),false);
        assert(x.error==rt::Error::NotAdmitted && !used && f.writes==0);
    }
    {
        Fake f(p);bool used=false;f.response=0;
        auto x=run(f,used);
        assert(x.error==rt::Error::ResponseChangedBeforeSubmit && f.writes==0 && used);
    }
    {
        Fake f(p);bool used=false;f.ignoreResponseClear=true;
        auto x=run(f,used);
        assert(x.error==rt::Error::ClearNotObserved && f.writes==1 && f.messageWrites==0);
    }
    {
        Fake f(p);bool used=false;f.ignoreParameterClear=true;
        auto x=run(f,used);
        assert(x.error==rt::Error::ParameterClearNotObserved && f.writes==2 && f.messageWrites==0);
    }
    {
        Fake f(p);bool used=false;f.replyAt=smu::kMaxResponsePolls+1;
        auto x=run(f,used);
        assert(x.error==rt::Error::TimedOut && x.transcript.timedOut &&
               x.transcript.polls==smu::kMaxResponsePolls && f.messageWrites==1);
    }
    {
        Fake f(p);bool used=false;f.reply=0xfcu;
        auto x=run(f,used);
        assert(x.error==rt::Error::FirmwareRejected && x.transcript.responseAfter==0xfcu && f.messageWrites==1);
    }
    for (uint32_t invalid : {0u,0xffffffffu}) {
        Fake f(p);bool used=false;f.version=invalid;
        auto x=run(f,used);
        assert(x.error==rt::Error::InvalidVersion && !x.verified && f.messageWrites==1);
    }
}
