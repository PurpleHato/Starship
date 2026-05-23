#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct VoiceOverrideData {
    uint8_t* sampleData;  // Raw S16 PCM, interleaved if stereo
    uint32_t numSamples;  // Total sample frames (per channel)
    uint32_t sampleRate;  // Original sample rate of the source file
    uint32_t channels;    // 1 or 2
    float tuning;         // (sampleRate * channels) / 32000.0f
    uint32_t size;        // numSamples * channels * 2 (bytes)
} VoiceOverrideData;

bool VoiceOverride_HasOverride(uint32_t msgId);
VoiceOverrideData* VoiceOverride_GetData(uint32_t msgId);
void VoiceOverride_Init(void);

#ifdef __cplusplus
}
#endif
