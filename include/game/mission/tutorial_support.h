#pragma once
//
// Tutorial support: capability registry (§5.7), atomic progress store (§6.7),
// and the rich-text token renderer (§1.2/§3.5) from TUTORIAL_MODE_DESIGN.md.
//
#include <string>
#include <vector>

namespace Mission { struct Mission; }

namespace Mission::Tutorial {

// ---- capabilities (§5.7) ----
// A lesson's `requires` array is exhaustive; anything not in the supported set
// makes the row UNAVAILABLE (visible with a reason in development builds),
// never silently approximated.
bool IsCapabilitySupported(const std::string& cap);
std::vector<std::string> MissingCapabilities(const std::vector<std::string>& requires);
// Mission-aware preflight: catches content that parses but still has no honest
// runtime contract (placeholder move 0, unimplemented checkpoints/scripts,
// undeclared sequence/input dependencies, missing exact baseline, ...).
std::vector<std::string> RuntimeSupportIssues(const ::Mission::Mission& lesson);

// ---- progress store (§6.7) ----
struct LessonProgress {
    bool cleared = false;
    int  attempts = 0;
    long long firstClearAt = 0;
    long long lastClearAt = 0;
    int  revisionCleared = 0;
};

// Lazy-loads on first use. Returns false + warning when the store had to be
// recovered (corrupt file preserved as .bad, clean start).
bool ProgressLoad(std::string& warning);
LessonProgress ProgressGet(const std::string& packId, const std::string& lessonId);
// Records a clear exactly once per entry into Complete. Never revokes.
bool ProgressRecordClear(const std::string& packId, const std::string& lessonId,
                         int revision, int attemptsThisRun, std::string& warning);
void ProgressNoteAttempt(const std::string& packId, const std::string& lessonId);
std::string ProgressLastLesson(const std::string& packId);
void ProgressSetLastLesson(const std::string& packId, const std::string& lessonId);

// ---- rich text ----
// Converts {dir:4}/{btn:A}/{input:236A}/{term:plain|jargon}/{ui:confirm}
// tokens to display text. Unknown tokens render their raw inner value;
// escaped braces (\{ \}) render literally.
std::string RenderRichText(const std::string& text);

} // namespace Mission::Tutorial
