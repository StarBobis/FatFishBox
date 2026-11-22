---
name: fatfishbox
description: Coordinate authorized FatFishBox tasks through one principal and one persistent-goal project manager using MCP, independent reviews and the durable task ledger.
---

Use this skill for an existing FatFishBox connection or an explicit user request. If the user provides connection instructions without a task, acknowledge briefly and wait for the requirement. Do not invent work or ask the user to schedule agents.

The principal reads `fatfish://workflow` once and then `team_get_prompt(role=principal)`. It commits a complete plan, decomposes independent tasks within totalConcurrency minus one manager slot, starts or resumes one manager, and waits through `team_wait_decisions` for up to 300000 ms per call. It does not consume the development queue or perform daily scheduling. After status=completed it independently accepts each candidate, actually merges main, and calls team_record_merge. Failed acceptance requires a complete replacement charter/tasks plan and another manager round.

The manager reads `team_get_contract`, `team-tools.json` and `team_domain_context`. All tasks require name/steps/acceptance/files/dependsOn/startCondition/tests. steps is one string. Dispatch every ready item with team_dispatch_ready; repeat after each handled FIFO result. Dependencies unlock only on accepted/integrated ledger state. The manager does not re-split the principal's work or serialize independent files because of array order. Missing structured plans require a complete principal update.

All roles share one saved maximum concurrency (default 20). Enabled account/model pools are selected automatically by global account load; preserve user settings and existing session bindings. The service creates an isolated worktree for every team worker. Legacy cluster_start_session callers must prepare isolated workspaces themselves. Never allow concurrent writers or builds to share main.

Developers and repair workers must first run their own builds and tests and return successful command/exitCode/logPath evidence for their exact clean candidate. The manager rejects untested candidates and dispatches repair rather than doing their pre-review work. Each valid candidate gets its own independent reviewer immediately, without waiting for a full batch. Failed review requires repair and a fresh review. Only after review passes does the manager use team_verify_candidate to execute final tests; acceptance requires its passing verificationId. No integrator or preliminary merge is required.

When no actions remain, the manager waits through team_wait_actions for up to 300000 ms. Neither principal nor manager may sleep, use shell timers or poll scripts instead. Handle early completion/action/decision feedback immediately without filling the remaining time. Timeout is not completion or failure and does not justify ending the turn. Stay in the same goal session through waiting and compaction. Finish only after all current planned work is accepted, then team_finish_domain and the actual DSH goal complete.

Use stable requestId and original arguments across uncertain dispatch, repair, message and ledger updates. Read expectedRevision from current state. Message accepted means queued, not executed. Keep queueId, projectId, workItemId, candidate SHA and resultVersion bindings exact. Read compact summaries first and page detailed logs/evidence only when needed. domainId is one manager's compatibility identity; legacy priorityResults does not imply preemption. Keep protocol fields out of user-facing steps.

Retire obsolete terminal historical work with team_retire_work_item; never dispatch fake read-only repairs to link handoffs. Retain audit records and do not call cancellation acceptance. Current-round retirement needs explicit principal authorization.

Evidence failures return review_evidence_pending and a decision bound to the affected job. They leave the actionable FIFO so MCP waiting does not spin. Managers can recapture in-worktree evidence through team_domain_action(operation=cluster_capture_evidence). The principal explicitly selects exact external paths with team_capture_evidence. Files must exist and be successfully captured; boolean flags cannot bypass collection. A reported test, a capture-time HEAD and a file hash prove different facts. Historical receipts never prove a merge to main.

Read prompts/principal.md, prompts/lead.md and prompts/worker.md for the full role instructions. Obey explicit user requests to stop team work or implement personally.
