# Publishing kama

The runbook for the one-time account setup and the recurring release/publish flow. Local
maintainer commands run through `./ops` (which loads secrets from the gitignored `.env` —
seed it with `cp .env.example .env`). CI reads the **same** names as GitHub Actions repo
secrets.

## 0. Secrets — where each one goes

| Secret | Used by | Where to put it |
|---|---|---|
| `VSCE_PAT` | `./ops ext-publish`, release CI | `.env` **and** GitHub Actions secret |
| `OVSX_PAT` | `./ops ext-publish`, release CI | `.env` **and** GitHub Actions secret |
| `CLOUDFLARE_API_TOKEN` | `./ops provision` / `deploy-site`, site CI | `.env` **and** GitHub Actions secret |
| `CLOUDFLARE_ACCOUNT_ID` | `./ops deploy-site`, site CI | `.env` **and** GitHub Actions secret |
| `TF_VAR_account_id` / `TF_VAR_zone_id` | `./ops provision` | `.env` only (OpenTofu vars) |

Add the GitHub secrets under **repo → Settings → Secrets and variables → Actions**.

## 1. VS Code Marketplace (`VSCE_PAT`)

1. Create/sign in to an **Azure DevOps** organization: <https://dev.azure.com>.
2. Create the Marketplace **publisher** `cosmic-canopy`: <https://marketplace.visualstudio.com/manage>
   (must match `"publisher"` in `editor/vscode/package.json`).
3. Create a **Personal Access Token**: Azure DevOps → User settings → Personal access tokens →
   **New Token**, Organization = *All accessible*, Scope = **Marketplace → Manage**.
4. Save it as `VSCE_PAT` (in `.env` and as a GitHub secret).

## 2. Open VSX (`OVSX_PAT`) — for Cursor / VSCodium / Windsurf

1. Sign in at <https://open-vsx.org> with a GitHub/Eclipse account.
2. Sign the **Eclipse publisher agreement** (one-time).
3. Create the namespace **`cosmic-canopy`**, then generate an **access token**.
4. Save it as `OVSX_PAT`.

## 3. Cloudflare (`CLOUDFLARE_*`, `TF_VAR_*`)

1. **Account ID:** Cloudflare dashboard → any domain → right sidebar → *Account ID*
   → `CLOUDFLARE_ACCOUNT_ID` and `TF_VAR_account_id`.
2. **Zone ID:** the `kama-lang.org` domain → *Zone ID* → `TF_VAR_zone_id`.
3. **API token:** My Profile → API Tokens → *Create Token* (custom) with permissions
   **Account → Cloudflare Pages → Edit** and **Zone → DNS → Edit** (scoped to `kama-lang.org`)
   → `CLOUDFLARE_API_TOKEN`.

## 4. Stand up the site (once)

```sh
./ops provision       # tofu: create the Pages project + kama-lang.org domain + DNS
./ops deploy-site      # build _site/ and push it to Cloudflare Pages (kama.pages.dev)
```

DNS is on Cloudflare, so the cert + domain bind automatically. Until `kama-lang.org`
resolves, the site is live at `https://kama.pages.dev`. After this, every push to `main`
that touches `site/**` / installers auto-deploys via `.github/workflows/deploy-site.yml`.

## 5. Cut a release

```sh
# dry-run the extension publish first (optional)
cd editor/vscode && npx @vscode/vsce publish --dry-run && npx ovsx publish --dry-run; cd -

git tag vX.Y.Z && git push --tags     # or: ./ops release  (prints this)
```

The tag triggers `.github/workflows/release.yml`, which:
- builds per-OS binaries and packages **slim** + **`-bundled`** (zig cc) tarballs, each with
  a `.sha256`;
- packages the `.vsix`;
- **publishes the extension** to Marketplace + Open VSX (skips cleanly until the PATs exist);
- creates the GitHub Release with all assets.

## 6. Verify

- `https://kama-lang.org` loads; `curl -fsSL https://kama-lang.org/install.sh | sh` installs.
- Extension listings live: `code --install-extension cosmic-canopy.kama`.
- `kama --version` on a fresh machine; `kama update` bumps to the latest tag.

## Later: GitHub language recognition

When `.kama` clears Linguist's usage bar (~200 repos / ~2000 files), open a
[github-linguist](https://github.com/github-linguist/linguist) PR using
`provisioning/linguist/languages.yml.snippet` + the `kama.tmLanguage.json` grammar +
`samples/Kama/` programs. See that file for the checklist.
