# Simplified team implementation and maintenance plan

The current operating procedure is [two-loop-workflow.md](two-loop-workflow.md), policy v13. Earlier implementation-era proof checklists no longer apply.

## Behavior
The principal plans requested work and waits through MCP. One persistent-goal manager dispatches ready tasks, handles result FIFO, arranges independent review and repairs actual defects. Developers run relevant self-tests. The manager confirms completion using judgement; extra targeted tests are optional. Main integration remains with the principal.

No automatic evidence collection, capture-on-read, artifact hash inventory, mandatory verificationId, attachment prerequisite, proof list or exhaustive screenshot matrix may be introduced. Stale collector calls return collectionDisabled without scanning files. Missing report attachments or metadata differences do not automatically reject work or invalidate acceptance. Do not convert local fixes into full-product audits.

## Implementation boundaries
- Keep native scheduling and global account/model load balancing with a single concurrency setting.
- Keep isolated task branches, stable work item IDs, request replay protection, revisions and queue ownership.
- Keep existing DSH session IDs, model/account bindings and completed work during client updates.
- Keep role prompts and agent-facing instructions in English.
- Use MCP waits up to 300000 ms with early feedback; no agent sleep loops.
- Preserve old archives without creating new proof manifests or collecting blobs.

## Validation
Use isolated fixture data and mock DSH endpoints for scheduler/protocol regressions. Run real fixture commands where command execution behavior is under test. Cover normal development/review/repair/confirmation, optional manager tests, completion without proof arrays, result reads without collection, stale collector no-ops, queue ownership, replay safety, load balancing and early MCP wake-up. Do not use live user tasks as test fixtures.

Follow repository AGENTS.md for the patch version, Release build, exact-path client replacement/restart and fresh Desktop ZIP packaging. Do not cancel or reassign DSH work to deliver a client update.
