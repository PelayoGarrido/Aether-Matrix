# Security policy

v0.1 is experimental software loaded inside a DAW process. Use trusted builds and trusted, self-contained ONNX models only. Model execution is not sandboxed: a malicious graph can consume resources or exploit a native runtime vulnerability despite size and shape checks. There is no network or model download path in the plugin.

Report vulnerabilities privately through GitHub's **Security → Report a vulnerability** when enabled. If unavailable, ask the maintainer for a private reporting channel without posting exploit details in a public issue. Do not attach private music projects or personal data.

Only the current main branch is maintained. Dependencies are reviewed before releases; automated checks cannot detect unknown vulnerabilities. See `docs/DEPENDENCIES.md` for the last recorded review and pinning decisions.
