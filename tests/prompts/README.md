# Prompt and operation-result regressions

Run from the repository root. The test harness loads the **actual** inline translator in `index.html`, language packs and complete production JavaScript into a VM. It does not import the hand-transcribed reproduction package.

```sh
npm ci --prefix tests/prompts
npm test --prefix tests/prompts
npm run test:browser --prefix tests/prompts
node tests/certificate/test_ui.cjs
node tests/runtime/test_ui.cjs
```

The browser suite requires locally installed Chrome (`channel: 'chrome'`). It serves the actual WebUI on an ephemeral localhost port, intercepts all `/api/` calls, replaces WebSocket with a stub and rejects external requests. Only the terminal retry test supplies local fixture responses for CDN URLs. It does not use device addresses, credentials, firmware, or real commands. The xterm fixture verifies resource recovery and production initialization, not xterm rendering itself.

No device validation is performed. Real file storage, browser disk download completion, SSH execution, power state, protection, firmware upgrades and embedded resource packaging require separate authorized acceptance.

`static.test.cjs`: literal/conditional translations, HTML attributes, finite dynamic families and placeholder contracts. `regression.test.cjs`: actual API/UI/terminal functions with controlled network, DOM, time and device-result inputs. `browser.test.cjs`: actual HTML/deferred-script loading, DOM safety, language recovery and terminal CDN retry. Browser screenshots are written under `output/playwright/`.

During this repair, existing local Acorn/Playwright installations were used through `NODE_PATH`; no dependencies or browser binaries were installed. The public submission summary is in `docs/PROMPT_REPAIR_REPORT.md`.

`ownership.browser.test.cjs` covers endpoint contracts, startup/navigation ownership, upload generations, modal ownership, out-of-order outcomes, and independent directory refresh. It is included in `test:browser`.
