// GC11/SMU pre-autoload policy: hardware-free host tests.
#include "navi33_rlc_readonly_preflight.h"
#include <cassert>

using namespace rn_navi33::rlc_readonly_preflight;

static Input good() {
    Input i{};
    i.requested = i.basesDiscovered = true;
    i.gc0Base = 0x100u;
    i.gc1Base = 0x200u;
    i.mp1Base = 0x300u;
    i.bar5Bytes = 512u * 1024u;
    i.gcSamplesRead = true;
    i.cpStat = 0;
    i.rlcBootloadStatus = 0; // Valid pre-start read, NOT boot complete.
    i.imuScratch0 = 0;
    i.pmfwAccepted = true;
    i.smuMailboxRead = true;
    i.smuResponse = 1; // Prior PPSMC_OK alone does NOT prove readiness.
    i.smuHandshakeVerified = true;
    i.mesSchedCodeFence = 17;
    i.mesSchedDataFence = 18;
    i.mesKiqCodeFence = 19;
    i.mesKiqDataFence = 20;
    i.imuIFence = 21;
    i.imuDFence = 22;
    i.rlcGFence = 23;
    return i;
}

static void rejects(Input i, Error err) {
    const auto report = inspect(i);
    assert(report.error == err && !report.mayAutoload);
}

int main() {
    uint32_t off = 0;
    assert(checkedRegister(0x100u, kCpStatDword, 512u * 1024u, &off));
    assert(off == (0x100u + kCpStatDword) * 4u);
    assert(!checkedRegister(0u, kCpStatDword, 512u * 1024u, &off));
    assert(!checkedRegister(UINT32_MAX, kCpStatDword, UINT64_MAX, &off));
    assert(!checkedRegister(0x100u, kRlcBootloadDword, 4096u, &off));
    assert(!checkedRegister(0x100u, kCpStatDword, 512u * 1024u, nullptr));

    const auto ok = inspect(good());
    assert(ok.mayAutoload && ok.error == Error::Ok);
    assert(ok.gcAddressesValid && ok.gcSnapshotReadable);
    assert(ok.smuMailboxReadable && ok.smuHandshakeVerified);
    assert(ok.linuxFirmwareOrder);
    assert(ok.cpStatByteOffset == (0x100u + kCpStatDword) * 4u);
    assert(ok.rlcBootloadByteOffset == (0x200u + kRlcBootloadDword) * 4u);

    { auto i = good(); i.requested = false; rejects(i, Error::NotRequested); }
    { auto i = good(); i.gc1Base = 0; rejects(i, Error::BadRegisterAddress); }
    { auto i = good(); i.bar5Bytes = 4096; rejects(i, Error::BadRegisterAddress); }
    { auto i = good(); i.gcSamplesRead = false; rejects(i, Error::UnreadableGcSnapshot); }
    { auto i = good(); i.cpStat = kInvalid; rejects(i, Error::UnreadableGcSnapshot); }
    { auto i = good(); i.imuScratch0 = kInvalid; rejects(i, Error::UnreadableGcSnapshot); }
    { auto i = good(); i.pmfwAccepted = false; rejects(i, Error::PmfwNotAccepted); }
    { auto i = good(); i.smuMailboxRead = false; rejects(i, Error::SmuMailboxUnreadable); }
    { auto i = good(); i.smuResponse = kInvalid; rejects(i, Error::SmuMailboxUnreadable); }
    // A readable, previously successful mailbox response is not a handshake.
    { auto i = good(); i.smuHandshakeVerified = false; rejects(i, Error::SmuHandshakeUnverified); }
    { auto i = good(); i.mesSchedCodeFence = 0; rejects(i, Error::FirmwareLoadOrderMismatch); }
    { auto i = good(); i.imuDFence = i.rlcGFence; rejects(i, Error::FirmwareLoadOrderMismatch); }

    // Current RedNavi 0.0.39: PSP accepted everything, but RLC_G was loaded
    // at fence 17 *before* IMU (18-19) and MES (20-23), unlike Linux.
    {
        auto i = good();
        i.rlcGFence = 17;
        i.imuIFence = 18;
        i.imuDFence = 19;
        i.mesSchedCodeFence = 20;
        i.mesSchedDataFence = 21;
        i.mesKiqCodeFence = 22;
        i.mesKiqDataFence = 23;
        i.smuHandshakeVerified = true;
        rejects(i, Error::FirmwareLoadOrderMismatch);
        i.smuHandshakeVerified = false;
        rejects(i, Error::SmuHandshakeUnverified);
    }
    return 0;
}
