# Development provenance

The user requested Claude Code development and explicitly authorized sending the design, brief and copied protocol sources to the existing DeepSeek provider configured in Claude Code.

- Claude Code 2.1.278 produced the React components, protocol catalog/encoder, simulation engine, stylesheet, README and protocol tests.
- A second Claude Code pass corrected issues found by coordinator review and browser testing.
- Codex coordinated setup, conducted Playwright and image comparison QA, refined final layout/capability copy, and fixed the copy-frame import and deferred-sync state update discovered in final interaction checks.
- Design source: `references/design.png`.
- Protocol sources: copied reference files only; no motor firmware files were modified.
- The demo never connects to real hardware; all traffic is local simulation.

Read `design-qa.md` for validation and known boundaries.
