// Shared production body; dependencies are supplied by the including translation unit.
int PercentToDirectSoundVolume(int percent, int baseDirectSoundVolume) {
    if (percent <= 0) {
        return kMinDirectSoundVolume;
    }

    if (percent <= 100 && baseDirectSoundVolume >= -10000 && baseDirectSoundVolume <= 0) {
        return EfzAudioApplyGain(baseDirectSoundVolume, EfzAudioGainForPercent(percent));
    }

    // Preserve the general helper's legacy out-of-domain conversion.
    const double baseAmplitude = std::pow(10.0, static_cast<double>(baseDirectSoundVolume) / 2000.0);
    const double scaledAmplitude = baseAmplitude * (static_cast<double>(percent) / 100.0);
    if (scaledAmplitude <= 0.0) {
        return kMinDirectSoundVolume;
    }

    const double directSoundVolume = 2000.0 * std::log10(scaledAmplitude);
    return std::clamp(static_cast<int>(std::lround(directSoundVolume)),
                      kMinDirectSoundVolume,
                      kMaxDirectSoundVolume);
}
