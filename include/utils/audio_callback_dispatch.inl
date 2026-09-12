// The shipping callback scope. Native exceptions must release the recursive
// boundary; a C++ lock_guard alone is insufficient under the product's /EHsc.
#define EFZ_AUDIO_CALLBACK(call) \
    EnterAudioBoundary(); \
    __try { return call; } \
    __finally { LeaveAudioBoundary(); }
