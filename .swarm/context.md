# Pending QA Gate Selection

User selected: **Full quality suite**

- critic_pre_plan: enabled (critic reviews the plan before implementation begins)
- drift_check: enabled (phase-complete blocked until drift-verifier reports APPROVED)
- reviewer: enabled (reviewer gate passes on each phase)
- test_engineer: enabled (test-engineer gate passes on each phase)
- hallucination_guard: enabled (API/signature/doc claims verified against real artifacts)
- sast_enabled: enabled (SAST scan as a hard security gate; medium+ severity blocks)
- council_mode: disabled
- sme_enabled: disabled
- mutation_test: disabled

Profile hash to be locked by the framework after first critic-approved plan snapshot.

## Agent Activity

| Tool | Calls | Success | Failed | Avg Duration |
|------|-------|---------|--------|--------------|
| read | 298 | 298 | 0 | 1179ms |
| test_runner | 63 | 63 | 0 | 10857ms |
| glob | 60 | 60 | 0 | 4245ms |
| apply_patch | 56 | 56 | 0 | 118ms |
| bash | 47 | 47 | 0 | 13721ms |
| task | 41 | 41 | 0 | 235711ms |
| write | 39 | 39 | 0 | 112ms |
| update_task_status | 36 | 36 | 0 | 180ms |
| invalid | 32 | 32 | 0 | 80ms |
| edit | 26 | 26 | 0 | 85ms |
| search | 16 | 16 | 0 | 59603ms |
| save_plan | 6 | 6 | 0 | 156ms |
| phase_complete | 4 | 4 | 0 | 4776ms |
| get_approved_plan | 3 | 3 | 0 | 28ms |
| syntax_check | 3 | 3 | 0 | 37953ms |
| todowrite | 3 | 3 | 0 | 14ms |
| knowledge_query | 2 | 2 | 0 | 14ms |
| set_qa_gates | 2 | 2 | 0 | 61ms |
| build_check | 2 | 2 | 0 | 172061ms |
| get_qa_gate_profile | 1 | 1 | 0 | 25ms |
| checkpoint | 1 | 1 | 0 | 258ms |
| skill | 1 | 1 | 0 | 308ms |
| question | 1 | 1 | 0 | 14780ms |
| write_retro | 1 | 1 | 0 | 133ms |
| lint | 1 | 1 | 0 | 731ms |
| secretscan | 1 | 1 | 0 | 3844ms |
| placeholder_scan | 1 | 1 | 0 | 5367ms |
| write_drift_evidence | 1 | 1 | 0 | 73ms |
| write_hallucination_evidence | 1 | 1 | 0 | 61ms |
