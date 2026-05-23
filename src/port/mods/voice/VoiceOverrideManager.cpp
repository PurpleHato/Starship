#include "port/mods/voice/VoiceOverrideManager.h"
#include <unordered_map>
#include <string>
#include <cstring>
#include <spdlog/spdlog.h>
#include "libultraship/src/Context.h"
#include "libultraship/src/resource/ResourceManager.h"
#include "libultraship/src/resource/archive/ArchiveManager.h"
#include "libultraship/src/resource/archive/Archive.h"
#include <dr_wav.h>
#include <dr_mp3.h>
#include <vorbis/vorbisfile.h>

static std::unordered_map<uint32_t, VoiceOverrideData> sVoiceOverrides;

static std::shared_ptr<Ship::File> TryLoadVoiceFile(const std::string& path) {
    auto am = Ship::Context::GetInstance()->GetResourceManager()->GetArchiveManager();
    if (!am->HasFile(path)) {
        return nullptr;
    }
    return am->LoadFile(path);
}

static const uint8_t* FileData(const std::shared_ptr<Ship::File>& file) {
    return reinterpret_cast<const uint8_t*>(file->Buffer->data());
}

static size_t FileSize(const std::shared_ptr<Ship::File>& file) {
    return file->Buffer->size();
}

static bool DecodeWav(std::shared_ptr<Ship::File> file, VoiceOverrideData& out) {
    drwav wav;
    if (!drwav_init_memory(&wav, FileData(file), FileSize(file), nullptr)) {
        return false;
    }
    drwav_uint64 numFrames;
    drwav_get_length_in_pcm_frames(&wav, &numFrames);

    SPDLOG_INFO("[VoiceOverride] WAV header: sampleRate={}, channels={}, bitsPerSample={}, frames={}",
        wav.sampleRate, wav.channels, wav.bitsPerSample, numFrames);
    SPDLOG_INFO("[VoiceOverride] WAV raw: fileSize={} bytes",
        FileSize(file));

    out.sampleRate = (uint32_t)wav.sampleRate;
    out.channels = (uint32_t)wav.channels;
    out.numSamples = (uint32_t)numFrames;
    out.size = (uint32_t)(numFrames * wav.channels * 2);
    out.tuning = 1.0f;
    out.sampleData = new uint8_t[out.size];
    drwav_read_pcm_frames_s16(&wav, numFrames, (int16_t*)out.sampleData);
    drwav_uninit(&wav);

    float duration = (float)out.numSamples / (float)out.sampleRate;
    SPDLOG_INFO("[VoiceOverride] WAV decoded: {} samples, {} bytes PCM, {:.3f}s duration",
        out.numSamples, out.size, duration);
    return true;
}

static bool DecodeMp3(std::shared_ptr<Ship::File> file, VoiceOverrideData& out) {
    drmp3 mp3;
    if (!drmp3_init_memory(&mp3, FileData(file), FileSize(file), nullptr)) {
        return false;
    }
    drwav_uint64 numFrames = drmp3_get_pcm_frame_count(&mp3);

    out.sampleRate = (uint32_t)mp3.sampleRate;
    out.channels = (uint32_t)mp3.channels;
    out.numSamples = (uint32_t)numFrames;
    out.size = (uint32_t)(numFrames * mp3.channels * 2);
    out.tuning = 1.0f;
    out.sampleData = new uint8_t[out.size];
    drmp3_read_pcm_frames_s16(&mp3, (drwav_uint64)out.numSamples, (int16_t*)out.sampleData);
    drmp3_uninit(&mp3);
    return true;
}

struct OggFileData {
    const uint8_t* data;
    size_t pos;
    size_t size;
};

static size_t VorbisRead(void* out, size_t size, size_t elems, void* src) {
    auto* d = static_cast<OggFileData*>(src);
    size_t toRead = size * elems;
    if (toRead > d->size - d->pos) toRead = d->size - d->pos;
    memcpy(out, d->data + d->pos, toRead);
    d->pos += toRead;
    return toRead / size;
}

static int VorbisSeek(void* src, ogg_int64_t pos, int whence) {
    auto* d = static_cast<OggFileData*>(src);
    size_t newPos;
    switch (whence) {
        case SEEK_SET: newPos = (size_t)pos; break;
        case SEEK_CUR: newPos = d->pos + (size_t)pos; break;
        case SEEK_END: newPos = d->size + (size_t)pos; break;
        default: return -1;
    }
    if (newPos > d->size) return -1;
    d->pos = newPos;
    return 0;
}

static int VorbisClose(void*) { return 0; }
static long VorbisTell(void* src) { return (long)static_cast<OggFileData*>(src)->pos; }

static bool DecodeOgg(std::shared_ptr<Ship::File> file, VoiceOverrideData& out) {
    OggFileData fileData = { FileData(file), 0, FileSize(file) };
    ov_callbacks cb = { VorbisRead, VorbisSeek, VorbisClose, VorbisTell };

    OggVorbis_File vf;
    if (ov_open_callbacks(&fileData, &vf, nullptr, 0, cb) != 0) {
        return false;
    }

    vorbis_info* vi = ov_info(&vf, -1);
    ogg_int64_t totalFrames = ov_pcm_total(&vf, -1);

    out.sampleRate = (uint32_t)vi->rate;
    out.channels = (uint32_t)vi->channels;
    out.numSamples = (uint32_t)totalFrames;
    out.size = (uint32_t)(totalFrames * vi->channels * 2);
    out.tuning = 1.0f;
    out.sampleData = new uint8_t[out.size];

    size_t pos = 0;
    char buf[4096];
    int bitStream = 0;
    while (true) {
        long read = ov_read(&vf, buf, sizeof(buf), 0, 2, 1, &bitStream);
        if (read == 0) break;
        if (read < 0) continue;
        memcpy(out.sampleData + pos, buf, read);
        pos += read;
    }
    ov_clear(&vf);
    return true;
}

static std::string GetArchiveName(const std::string& filePath) {
    auto am = Ship::Context::GetInstance()->GetResourceManager()->GetArchiveManager();
    auto archive = am->GetArchiveFromFile(filePath);
    if (!archive) return "(unknown)";
    const std::string& archivePath = archive->GetPath();
    auto lastSep = archivePath.find_last_of("/\\");
    return (lastSep != std::string::npos) ? archivePath.substr(lastSep + 1) : archivePath;
}

static bool LoadVoiceOverride(uint32_t msgId) {
    VoiceOverrideData data;
    memset(&data, 0, sizeof(data));

    const char* extensions[] = { "wav", "ogg", "mp3" };
    for (auto ext : extensions) {
        std::string path = "ast_radio/MsgID_" + std::to_string(msgId) + "." + ext;
        SPDLOG_INFO("[VoiceOverride] Attempting to load msgId={}, trying path='{}'", msgId, path);
        auto file = TryLoadVoiceFile(path);
        if (!file) continue;

        SPDLOG_INFO("[VoiceOverride] Loaded file for msgId={}, {} bytes from archive '{}', decoding as {}",
            msgId, FileSize(file), GetArchiveName(path), ext);

        bool ok = false;
        if (strcmp(ext, "wav") == 0) ok = DecodeWav(file, data);
        else if (strcmp(ext, "mp3") == 0) ok = DecodeMp3(file, data);
        else if (strcmp(ext, "ogg") == 0) ok = DecodeOgg(file, data);

        if (ok && data.sampleData != nullptr && data.numSamples > 0) {
            float freqMod = (float)(data.sampleRate * data.channels) / 32000.0f;
            SPDLOG_INFO("[VoiceOverride] Decode {} for msgId={} OK: {}Hz, {}ch, {} samples, {} bytes PCM, freqMod={:.4f}",
                ext, msgId, data.sampleRate, data.channels, data.numSamples, data.size, freqMod);
            sVoiceOverrides[msgId] = data;
            return true;
        }

        SPDLOG_INFO("[VoiceOverride] Decode {} for msgId={} FAILED", ext, msgId);
    }
    return false;
}

void VoiceOverride_Init(void) {
    auto am = Ship::Context::GetInstance()->GetResourceManager()->GetArchiveManager();
    auto files = am->ListFiles("ast_radio/*");
    if (!files) {
        SPDLOG_INFO("[VoiceOverride] ListFiles returned null");
        return;
    }

    SPDLOG_INFO("[VoiceOverride] ListFiles found {} entries", files->size());
    if (files->empty()) return;

    for (auto& path : *files) {
        SPDLOG_INFO("[VoiceOverride] Found file: '{}'", path);
        auto lastSlash = path.rfind('/');
        if (lastSlash == std::string::npos) lastSlash = path.rfind('\\');
        std::string filename = (lastSlash != std::string::npos) ? path.substr(lastSlash + 1) : path;

        auto dot = filename.rfind('.');
        if (dot == std::string::npos) continue;

        std::string ext = filename.substr(dot + 1);
        if (ext != "wav" && ext != "mp3" && ext != "ogg") continue;

        // Expect "MsgID_XXXX" prefix
        const std::string prefix = "MsgID_";
        std::string baseName = filename.substr(0, dot);
        if (baseName.compare(0, prefix.size(), prefix) != 0) continue;
        std::string numStr = baseName.substr(prefix.size());

        uint32_t msgId = 0;
        try {
            msgId = (uint32_t)std::stoul(numStr);
        } catch (...) {
            continue;
        }

        sVoiceOverrides[msgId] = {};
        SPDLOG_INFO("[VoiceOverride] Registered msgId={} from archive='{}' path='{}'", msgId, GetArchiveName(path), path);
    }
}

bool VoiceOverride_HasOverride(uint32_t msgId) {
    return sVoiceOverrides.find(msgId) != sVoiceOverrides.end();
}

VoiceOverrideData* VoiceOverride_GetData(uint32_t msgId) {
    auto it = sVoiceOverrides.find(msgId);
    if (it == sVoiceOverrides.end()) return nullptr;

    if (it->second.sampleData == nullptr) {
        SPDLOG_INFO("[VoiceOverride] GetData msgId={} — lazy loading (sampleData was null)", msgId);
        if (!LoadVoiceOverride(msgId)) {
            SPDLOG_INFO("[VoiceOverride] GetData msgId={} — load failed, removing from registry", msgId);
            sVoiceOverrides.erase(it);
            return nullptr;
        }
        it = sVoiceOverrides.find(msgId);
        if (it == sVoiceOverrides.end()) return nullptr;
    }

    return &it->second;
}
