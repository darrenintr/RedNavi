#pragma once
#define RN_PSP_FW_METADATA_V1 1
#define RN_PSP_FW_DIGEST_TRUSTED 1
#define kRNPspTargetMajor 13u
#define kRNPspTargetMinor 0u
#define kRNPspTargetRevision 7u
#define kRNPspExactTargetName 1u
#define kRNPspSourceBytes 248496u
#define kRNPspComponentMask 0xFu
#define kRNPspMaxComponentBytes 4096u
static const char kRNPspSourceSha256[] =
    "94db505fa6482f258c33a0a8d412050f6d843ab4ada368252e988f82f8a26fa8";
static const unsigned char kRnPspFixtureData[16] = {};
static const struct {
    const char *name;
    unsigned int cmd;
    unsigned int version;
    const unsigned char *data;
    unsigned int size;
} kRNPspChain[] = {
    {"SYS_DRV", 0x10000u, 1u, kRnPspFixtureData, sizeof(kRnPspFixtureData)},
    {"SOS", 0x20000u, 1u, kRnPspFixtureData, sizeof(kRnPspFixtureData)},
};
static const unsigned char kRNPspToc[16] = {};
