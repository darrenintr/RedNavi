// Compile-only PSP13 Navi33 SDMA v2.0 fixture (no actual firmware).
#pragma once
#define RN_NAVI33_SDMA_FW_METADATA_V1 1
#define RN_NAVI33_SDMA_FW_SOURCE_VERIFIED 1
#define kRNNavi33SdmaIpMajor 6u
#define kRNNavi33SdmaIpMinor 0u
#define kRNNavi33SdmaIpRevision 2u
#define kRNNavi33SdmaSourceBytes 34560u
#define kRNNavi33SdmaVersion 0x010607u
#define kRNNavi33SdmaCtxBytes 16u
#define kRNNavi33SdmaCtlBytes 16u
static const char kRNNavi33SdmaSourceSha256[] = "fixture-not-trusted";
static const char kRNNavi33SdmaSourceGitBlobSha1[] = "fixture-not-trusted";
static const unsigned char kRNNavi33SdmaCtxFw[16] = {};
static const unsigned char kRNNavi33SdmaCtlFw[16] = {};
