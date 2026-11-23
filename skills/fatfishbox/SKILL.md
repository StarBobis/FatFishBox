---
name: fatfishbox
description: Coordinate authorized work using one principal, one persistent-goal manager, ordinary developer self-tests and independent review, without proof collection or formal artifact gates.
---

Read team_get_prompt for the appropriate role. The principal saves a draft with team_start_project, explicitly activates the reviewed version with team_activate_project, waits through team_wait_decisions and finally integrates main. The manager dispatches all ready planned tasks, handles FIFO results, arranges independent review/repairs and confirms completion. All roles share one maximum concurrency and load-balanced enabled models; preserve existing sessions and assignments.

There is no formal evidence workflow. Do not collect screenshots, manifests, hashes, proof packages or exhaustive page/configuration/frame matrices merely to satisfy acceptance. Do not expand a local change into a full-product audit. Broad functional checks are work only if the user explicitly requests them. The user's removal of proof-only work overrides old checklists. Stop extra proof-only subprocesses when safe without cancelling development/test sessions. Report actual checks and skipped coverage honestly.

Developers perform relevant self-tests. Reviewers inspect actual changes and regressions. The manager decides acceptance; extra manager tests are optional and targeted. No verificationId, evidence array, acceptanceResults checklist, manifest or capture is required. Old collection tools are no-ops and omitted from the default catalog. Keep ordinary ownership, stable requestId, valid Git references and honest task status. Do not invent results or fake repair jobs to acknowledge obsolete work; use team_retire_work_item.

Read team_get_contract for complete ledger action templates. add_issue uses message/acceptance; accept uses candidateSha/notes. The manager finishes through team_finish_domain with summary/requestId, then completes the actual DSH goal and exits. The principal merges passing changes or provides a clear repair plan.

Wait only through team_wait_actions (manager) or team_wait_decisions (principal), up to 300000 ms per call. Keep waiting on a pending tool handle. On a quiet timeout resume silently with the returned cursor. On a client deadline reuse the last received cursor with a shorter wait. Inspect worker changes only after completion or a specific decision/error requires it. Genuine blockers can be tied to actionId/jobId with team_escalate while independent work continues. Read team_get_queue after reconnecting and follow allowedActions. Reuse the queue after confirmed cancellation and shutdown. Explicit user requests to stop team work or implement personally take precedence.
