#pragma once

#include <string>

namespace ExtendedConfigBridge {

struct Status {
    bool modLoaded = false;
    bool sharedConfigFound = false;
    bool audioShared = false;
    bool controlsShared = false;
    bool bgmVolumeAvailable = false;
    bool seVolumeAvailable = false;
    int bgmVolumePercent = 100;
    int seVolumePercent = 100;
    int version = 0;
    int audioRevision = 0;
    int controlsRevision = 0;
    std::string moduleName;
    std::string modulePath;
    std::string sharedConfigPath;
};

// Refreshes module/file discovery and reads the shared status if available.
bool Refresh(bool force = false);

// Imports shared audio into the training-mode runtime config when the extension
// exposes audio as shared. This does not force-save efz_training_config.ini.
bool ImportAudioSettingsIfAvailable(bool force = false);

// Publishes training-mode audio slider changes into the extension-owned shared
// config file. This never touches KEY.ini.
bool PublishAudioSettings(int bgmPercent, int sePercent);

const Status& GetStatus();
bool IsSharedAudioActive();

} // namespace ExtendedConfigBridge
