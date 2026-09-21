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
auto-deploys via `.github/workflows/deploy-site.yml` when it touches any of its trigger paths:
`site/**`, `tools/site/**`, `tools/build-site`, `assets/**`, `docs/**`, `VERSION`, `llms.txt`,
`install.sh`, `install.ps1`, `tests/site_sample.kama`, `package.json`, `package-lock.json`, or the
workflow itself. The site is built *from* the docs, so in practice nearly every push to `main` deploys.

## 5. Go live (once)

Work happens on `dev`; `main` is the default branch and the one the site deploys from. Until the first
launch, `origin/main` is a stale ancestor of `dev` (check: `git merge-base --is-ancestor origin/main dev
&& echo ancestor`), and the tags are stuck far behind `VERSION` (check: `git tag | sort -V | tail -3`).
Do these in order:

1. **Make the repo public** (repo → Settings → General → *Danger Zone* → Change visibility). Nothing
   else works for an outside user until then: `install.sh` / `install.ps1` resolve the version from
   `api.github.com/repos/cosmic-canopy/kama/releases/latest` **unauthenticated**, `kama update` runs
   that same installer, and the Helix and Zed grammar pins (`editor/helix/languages.toml`,
   `editor/zed/extension.toml`) name commits that GitHub must serve to anyone.
   Check: `curl -s -o /dev/null -w '%{http_code}\n' https://api.github.com/repos/cosmic-canopy/kama`
   prints `200`, not `404`.
2. **Cut the release for the current `VERSION`** (section 6) and wait for `release.yml` to go green.
   Do this *before* the site goes out: the site's installer serves whatever `releases/latest` is, and
   with the tags stuck behind, that is an old binary.
3. **Push `dev` to `main`:** `git push origin dev:main`. It is a fast-forward (main is an ancestor), so no
   merge commit and nothing lost. ⚠️ **This deploys the site** — it touches `docs/**` and `VERSION`, so
   `deploy-site.yml` fires. From then on, pushing `main` is how the site ships.
4. Verify (section 7), *then* announce.

## 6. Cut a release

```sh
# dry-run the extension publish first (optional)
cd editor/vscode && npx @vscode/vsce publish --dry-run && npx ovsx publish --dry-run; cd -

git tag "v$(cat VERSION)" && git push origin "v$(cat VERSION)"   # or: ./ops release (prints the recipe)
```

**The tag must be `v` + the contents of `VERSION`.** CI does not read `VERSION`: `release.yml` derives the
version from the tag name (`VERSION=${GITHUB_REF_NAME#v}`) and builds with `make VERSION=…`, which stamps
exactly that into `kama --version` (no `+g<sha>` suffix). A tag that disagrees with the file ships a binary
whose number names no commit's `VERSION`. Tag, and let the release finish, **before** announcing anything
that points people at the installer or `kama update`.

The tag triggers `.github/workflows/release.yml`, which:
- builds per-OS binaries and packages **slim** + **`-bundled`** (zig cc) tarballs, each with
  a `.sha256`;
- packages the `.vsix`;
- **publishes the extension** to Marketplace + Open VSX (skips cleanly until the PATs exist);
- creates the GitHub Release with all assets.

**The VS Code extension has its own version**, `"version"` in `editor/vscode/package.json`, independent of
`VERSION` — bumping it on every kama patch would push an auto-update to every user for changes that never
touched the editor. The rule is *bump it when you change the extension* (`tools/check-ext-version.sh`
holds that down; the Marketplace requires a strict `major.minor.patch`). The `.vsix` takes its version from
`package.json`, not from the tag, so a release whose extension is unchanged reports "already published"
and skips with a warning — that is the normal case. To ship an extension change: bump `package.json`,
then either tag a kama release (CI publishes it) or publish it on its own with `./ops ext-publish`
(uses `VSCE_PAT` / `OVSX_PAT` from `.env`). `./dev ext-package` builds the `.vsix` locally without
publishing, to inspect it first.

## 7. Verify

- `https://kama-lang.org` loads; `curl -fsSL https://kama-lang.org/install.sh | sh` installs.
- Extension listings live: `code --install-extension cosmic-canopy.kama`.
- `kama --version` on a fresh machine prints the `VERSION` you tagged; `kama update` bumps to the latest tag.

## Later: GitHub language recognition

When `.kama` clears Linguist's usage bar (~200 repos / ~2000 files), open a
[github-linguist](https://github.com/github-linguist/linguist) PR using
`provisioning/linguist/languages.yml.snippet` + the `kama.tmLanguage.json` grammar +
`samples/Kama/` programs. See that file for the checklist.
