// Production lane transform; provenance comes only from the acknowledged tuple.
inline int ApplyAudioLaneGain(const AudioSettingsView& view, bool bgm, int raw) noexcept {
    const int percent = bgm ? view.bgmPercent : view.sePercent;
    const bool owns = bgm ? view.trainingOwnsBgmGain : view.trainingOwnsSeGain;
    if (!owns) return raw;
    return EfzAudioApplyGain(raw, EfzAudioGainForPercent(percent));
}
