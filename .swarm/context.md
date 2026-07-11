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
| read | 322 | 322 | 0 | 1100ms |
| bash | 86 | 86 | 0 | 13552ms |
| test_runner | 66 | 66 | 0 | 10520ms |
| glob | 63 | 63 | 0 | 4059ms |
| apply_patch | 56 | 56 | 0 | 118ms |
| write | 45 | 45 | 0 | 125ms |
| edit | 45 | 45 | 0 | 86ms |
| task | 41 | 41 | 0 | 235711ms |
| update_task_status | 36 | 36 | 0 | 180ms |
| invalid | 32 | 32 | 0 | 80ms |
| search | 16 | 16 | 0 | 59603ms |
| todowrite | 7 | 7 | 0 | 20ms |
| save_plan | 6 | 6 | 0 | 156ms |
| phase_complete | 4 | 4 | 0 | 4776ms |
| get_approved_plan | 3 | 3 | 0 | 28ms |
| syntax_check | 3 | 3 | 0 | 37953ms |
| knowledge_query | 2 | 2 | 0 | 14ms |
| set_qa_gates | 2 | 2 | 0 | 61ms |
| build_check | 2 | 2 | 0 | 172061ms |
| batch_symbols | 2 | 2 | 0 | 118ms |
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
