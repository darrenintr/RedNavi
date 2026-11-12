// Compile-only fake MES payloads; NEVER DEPLOY ON GPU.
#pragma once
#define RN_NAVI33_MES_FW_METADATA_V1 1
#define RN_NAVI33_MES_FW_SOURCE_VERIFIED 1
#define kRNNavi33MesIpMajor 11u
#define kRNNavi33MesIpMinor 0u
#define kRNNavi33MesIpRevision 2u
#define kRNNavi33MesSchedSourceBytes 256288u
#define kRNNavi33MesSchedVersion 0u
#define kRNNavi33MesSchedCodeBytes 16u
#define kRNNavi33MesSchedDataBytes 16u
#define kRNNavi33MesSchedSourceBlob "fixture-only"
#define kRNNavi33MesSchedSourceSha256 "fixture-only"
#define kRNNavi33MesKiqSourceBytes 218400u
#define kRNNavi33MesKiqVersion 0u
#define kRNNavi33MesKiqCodeBytes 16u
#define kRNNavi33MesKiqDataBytes 16u
#define kRNNavi33MesKiqSourceBlob "fixture-only"
#define kRNNavi33MesKiqSourceSha256 "fixture-only"
static const unsigned char kRNNavi33MesSchedCodeFw[16] = {};
static const unsigned char kRNNavi33MesSchedDataFw[16] = {};
static const unsigned char kRNNavi33MesKiqCodeFw[16] = {};
static const unsigned char kRNNavi33MesKiqDataFw[16] = {};
