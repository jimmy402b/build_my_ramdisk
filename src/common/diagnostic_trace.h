#pragma once

// A one-line-at-a-time trace file, for diagnosing code that cannot report for itself.
//
// **Why this exists rather than a logger.**  It was written to explain a worker that blocked
// for its entire command budget: a process stuck inside a driver call cannot answer, cannot
// log, and cannot tell anyone which step it reached.  The only thing that can speak for it is
// a file it appended to *before* it blocked.  So this writes and flushes **per call**, and
// deliberately holds nothing open: a buffered or long-lived handle would show nothing for
// exactly the step that matters.
//
// **It is off unless asked for.**  `MYRD_TRACE_FILE` names the file; with no such variable
// every call is one `GetEnvironmentVariableW` and nothing else, which is why it is safe to
// leave the call sites in production code.
//
// **It never changes behaviour.**  Every failure -- no variable, unopenable path, failed write
// -- is swallowed.  A trace that altered what the traced code does would be describing a
// different run than the one that failed.

namespace myrd {

// Appends one stamped line naming `step` to the file `MYRD_TRACE_FILE` names.
//
// Best-effort and silent, by design: see above.  Call it before the step, so a step that never
// finishes still leaves its name in the file.
void diagnostic_trace(const char* step);

} // namespace myrd
