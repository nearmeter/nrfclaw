# GitHub Release and Maintenance

Canonical organization: https://github.com/nearmeter

## First publication

Create the repository `nearmeter/nrfclaw`, preferably empty (no auto-generated README/license because this tree already contains them), then:

```bash
git init
git branch -M main
git add .
git commit -m "Initial public nRFClaw release"
git remote add origin git@github.com:nearmeter/nrfclaw.git
git push -u origin main
```

## Branch policy
Keep `main` releasable. Use `feature/*`, `fix/*`, and `docs/*` branches. Merge through pull requests after firmware/bootloader build and CLI semantic validation.

## Release checklist
1. Confirm Studio/private files are absent.
2. Confirm no API keys, `.env`, credentials, build outputs, caches or test artifacts are tracked.
3. Populate only the required Nordic SDK subset in `vendor/nrf5sdk` and preserve original notices/licenses.
4. Build firmware from a clean checkout.
5. Build bootloader from a clean checkout.
6. Capture exact `arm-none-eabi-size` Flash/RAM results and update README Resource Usage.
7. Validate every executable natural-language prompt shown in README.
8. Validate CLI `--help`, agent configuration syntax and device commands.
9. Check `LICENSE` and `THIRD_PARTY_NOTICES.md`.
10. Tag only after the release commit is on `main`.

## Tagging
```bash
git checkout main
git pull --ff-only
git tag -a v1.0.0 -m "nRFClaw v1.0.0"
git push origin v1.0.0
```

## Ongoing maintenance
Never rewrite published release tags. Use a new patch/minor version for fixes. Keep semantic compiler changes reviewable and accompany new language families with positive and negative validation cases before release. Public examples are part of the compatibility surface: a README prompt that stops compiling is a release regression.
