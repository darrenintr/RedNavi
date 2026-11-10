#ifndef REDNAVI_TEST_SMU_FW_H
#define REDNAVI_TEST_SMU_FW_H
#define RN_SMU_FW_METADATA_V1 1
#define RN_SMU_FW_DIGEST_TRUSTED 1
#define kRNSmuFwSourceBytes 320u
#define kRNSmuFwHeaderSize 32u
#define kRNSmuFwUcodeOffset 64u
#define kRNSmuFwVersion 0x00505600u
#define kRNSmuFwSize 256u
static const char kRNSmuFwSourceSha256[] =
    "9376ae64149e6b0b684898ffbc12c2230f8c50a2e9447dc7dafc95c0c16b5227";
static const unsigned char kRNSmuFw[kRNSmuFwSize] = {};
#endif
