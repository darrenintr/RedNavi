// Host-only Navi33 RSMU index/data admission contract. No MMIO.
#include "navi33_mp1_fwflags_smn_probe.h"
#include <cassert>
using namespace rn_navi33::mp1_fwflags_smn_probe;
static Input ready() {
    Input i{};
    i.requested = i.explicitOptIn = i.pf0ExplicitOptIn =
        i.exactNavi33 = true;
    i.step = 27;
    i.mp1Revision = 7;
    // Real RX7600 live IP Discovery result (0.0.42).
    i.nbioMajor = 4;
    i.nbioMinor = 3;
    i.nbioRevision = 1;
    i.pmfwAccepted = true;
    i.priorFenceExpected = i.priorFenceObserved = 23;
    i.transportHealthy = true;
    i.nbifBaseFound = true;
    i.nbifBaseDword = 0x5000;
    i.bar5Bytes = 1024u * 1024u;
    return i;
}
static void rejects(Input i, Error error) {
    const Report r = inspect(i);
    assert(r.error == error && !r.mayProbe);
}
int main() {
    static_assert(kSmnMp1FirmwareFlags == 0x03b10028u, "Navi33 MP1 SMN");
    static_assert(kInterruptsEnabledMask == 1u, "Linux MP1 bit0");
    // Linux nbio_v4_3_funcs is selected for NBIO 4.3.0 and 4.3.1.
    // Both use BIF_BX_PF0_RSMU_INDEX/DATA in NBIF segment 1.
    assert(supportedPf0RsmuVersion(4, 3, 0));
    assert(supportedPf0RsmuVersion(4, 3, 1));
    assert(!supportedPf0RsmuVersion(4, 3, 2));
    assert(!supportedPf0RsmuVersion(4, 0, 0));
    assert(!supportedPf0RsmuVersion(7, 11, 0));
    assert(!supportedPf0RsmuVersion(7, 11, 3));
    assert(!supportedPf0RsmuVersion(0, 0, 0));
    assert(kNbifIndexHiDword == 2u);
    // Status and restore must both be observed; not just posted writes.
    Transcript good{};
    good.originalIndex = 0x11223344u;
    good.selectedReadback = kSmnMp1FirmwareFlags;
    good.firmwareFlags = 1u;
    good.restoredIndex = good.originalIndex;
    good.indexWritten = good.dataRead = good.restoreWritten = true;
    assert(validTranscript(good));
    assert(interruptsEnabled(good.firmwareFlags, validTranscript(good)));
    { auto t=good; t.originalIndex=kInvalid; assert(!validTranscript(t)); }
    { auto t=good; t.indexWritten=false; assert(!validTranscript(t)); }
    { auto t=good; t.selectedReadback=0u; assert(!validTranscript(t)); }
    { auto t=good; t.selectedReadback=kInvalid; assert(!validTranscript(t)); }
    { auto t=good; t.dataRead=false; assert(!validTranscript(t)); }
    { auto t=good; t.firmwareFlags=kInvalid; assert(!validTranscript(t)); }
    { auto t=good; t.restoreWritten=false; assert(!validTranscript(t)); }
    { auto t=good; t.restoredIndex=0u; assert(!validTranscript(t)); }
    auto r = inspect(ready());
    assert(r.mayProbe && r.error == Error::Ok);
    assert(r.indexByteOffset == 0x5000u*4u);
    assert(r.dataByteOffset == (0x5000u+1u)*4u);
    assert(!interruptsEnabled(0u, true));
    assert(interruptsEnabled(1u, true));
    assert(!interruptsEnabled(kInvalid, false));
    assert(!interruptsEnabled(1u, false));

    { auto i=ready(); i.requested=false; rejects(i, Error::NotRequested); }
    { auto i=ready(); i.explicitOptIn=false; rejects(i, Error::MissingOptIn); }
    { auto i=ready(); i.step=26; rejects(i, Error::WrongStep); }
    { auto i=ready(); i.exactNavi33=false; rejects(i, Error::WrongAsic); }
    { auto i=ready(); i.mp1Revision=10; rejects(i, Error::WrongMp1Revision); }
    { auto i=ready(); i.nbioMinor=0; i.nbioRevision=0;
      rejects(i, Error::UnsupportedNbioVersion); }
    { auto i=ready(); i.nbioMinor=7;
      rejects(i, Error::UnsupportedNbioVersion); }
    { auto i=ready(); i.nbioRevision=2;
      rejects(i, Error::UnsupportedNbioVersion); }
    { auto i=ready(); i.nbioMajor=7; i.nbioMinor=11; i.nbioRevision=2;
      rejects(i, Error::UnsupportedNbioVersion); }
    { auto i=ready(); i.nbioMajor=0;
      rejects(i, Error::UnsupportedNbioVersion); }
    { auto i=ready(); i.pmfwAccepted=false; rejects(i, Error::PmfwNotAccepted); }
    { auto i=ready(); i.priorFenceObserved=22; rejects(i, Error::PriorFenceMismatch); }
    { auto i=ready(); i.transportHealthy=false; rejects(i, Error::TransportUnhealthy); }
    { auto i=ready(); i.pf0ExplicitOptIn=false; rejects(i, Error::MissingPf0OptIn); }
    { auto i=ready(); i.nbifBaseFound=false; rejects(i, Error::MissingNbif); }
    { auto i=ready(); i.nbifBaseDword=0; rejects(i, Error::InvalidNbifWindow); }
    { auto i=ready(); i.nbifBaseDword=UINT32_MAX; rejects(i, Error::InvalidNbifWindow); }
    { auto i=ready(); i.bar5Bytes=1024; rejects(i, Error::InvalidNbifWindow); }
    {
       uint32_t index=0, data=0;
       assert(checkedNbifOffsets(0x5000u, 1024u*1024u, &index, &data));
       assert(!checkedNbifOffsets(0x5000u, 1024u, &index, &data));
       assert(!checkedNbifOffsets(0x5000u, 1024u*1024u, nullptr, &data));
    }
    return 0;
}
