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
    "d6c5a9fa211985ab00438955931adff5eaf456794d38b198ce92ff39d283dc26";
static const unsigned char kRNSmuFw[kRNSmuFwSize] = {};
#endif
