# Two-loop workflow (0.1.11 / policy v13)

## Principal
Read the request and repository instructions. Write a clear plan limited to the requested behavior, with independent task boundaries, dependencies and proportionate functional checks. Do not add proof obligations, screenshot matrices, manifests or full-product audits. Commit the plan and start one persistent-goal manager through team_start_project. Use team_get_queue to resume an existing project rather than duplicate it.

Wait through team_wait_decisions for up to 300000 ms. React to early completion or decisions immediately; never sleep or fill unused wait time. On manager completion, inspect the changes and merge accepted work into main, recording integration through team_record_merge. If functional work needs repair, provide a complete updated plan. Do not ask workers to collect proof of routine completion.

## Project manager
Read the bootstrap plan, team_get_contract and team_domain_context. Preserve project/domain credentials, request IDs, work item identities and existing sessions across reconnects.

1. Call team_dispatch_ready to queue all ready independent tasks. The service limits concurrency and balances enabled account/model capacity.
2. Handle team_next_action in FIFO order, dispatching ready work again after each result. Developers implement and run relevant self-tests before returning their summary.
3. Dispatch independent review using team_delegate with sourceJobId and the candidate branch/SHA. Review actual behavior and changes, without bulk capture or artifact collection.
4. Record the manager's review conclusion with team_domain_action, operation=cluster_update_work_item, action=record_review. Put the action and details inside arguments. Use contract templates rather than guessing fields.
5. Repair actual defects with a repair worker and independent review. Metadata differences or missing attachments do not require fake repairs or replacement reviews.
6. Judge whether extra targeted manager testing is useful after review. team_verify_candidate is optional; there is no required second build or verificationId. Confirm the work item with action=accept and details:{candidateSha,notes}.
7. Escalate genuine blockers with actionId/jobId and continue other independent work. When nothing is actionable, call team_wait_actions with timeoutMs up to 300000. Completion wakes the call early. Never sleep or recreate the manager goal just to wait.
8. When all current tasks are confirmed and no work remains active, call team_finish_domain with requestId and summary, then complete the actual host goal and exit naturally. No evidence array, acceptanceResults, checkpoint or integrator is required. The principal handles main integration.

## Developers and reviewers
Use the assigned isolated workspace and scope. Developers perform relevant builds/self-tests and report actual outcomes, limitations and candidate SHA. Reviewers independently inspect the changes and reproduce likely defects. Report concise findings; do not generate proof packages, file-hash inventories, screenshot archives or exhaustive page/configuration/frame sweeps.

The optional fatfish-delivery block may include workItemId, baseSha, candidateSha, status, issues, nextAction, worktreeClean and tests. Attachments and manifests are unnecessary. Do not claim tests or coverage that were skipped.

## Existing work and recovery
Policy v13 removes formal proof gates, including proof-only obligations in older plans. Stop further proof-only capture; stop only its subprocess when safe, preserving development/testing and existing DSH sessions. Broad functional audits require an explicit user request and are never a default acceptance ritual. Retired collection calls are harmless no-ops; do not retry them or invent replacement collectors. Historical archives remain readable.

Use team_retire_work_item for obsolete terminal historical work instead of fake repair jobs. Genuine ownership, current revision, actual Git integration, idempotency and active-session lifecycle checks remain for correct bookkeeping. Never fabricate completion or silently discard actual defects.
