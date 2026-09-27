# Cloudflare Pages project + custom domain + DNS for kama-lang.org, as code.
# Secrets come from the root .env (loaded by `./ops`): CLOUDFLARE_API_TOKEN is read by
# the provider; account_id / zone_id are TF_VAR_* so there are no tfvars/secret files.
#
#   ./ops provision      # tofu init + apply
#   ./ops exec tofu plan # dry-run
#
# NOTE: the Cloudflare provider renamed many resources v4->v5; this targets v5. Pin and
# verify attribute names against the provider docs for the version you install.
#
# Repair resources here by editing this file and applying — a change made in the dashboard or through
# the API is invisible until the next plan, where it turns into a collision.

terraform {
  required_providers {
    cloudflare = {
      source  = "cloudflare/cloudflare"
      version = "~> 5"
    }
  }
}

variable "account_id" { type = string }
variable "zone_id" { type = string } # kama-lang.org zone

provider "cloudflare" {} # reads CLOUDFLARE_API_TOKEN from the environment

# The Pages project; `./ops deploy-site` (wrangler direct-upload) pushes builds to it.
resource "cloudflare_pages_project" "kama" {
  account_id        = var.account_id
  name              = "kama"
  production_branch = "main"
}

resource "cloudflare_pages_domain" "apex" {
  account_id   = var.account_id
  project_name = cloudflare_pages_project.kama.name
  name         = "kama-lang.org"
}

resource "cloudflare_pages_domain" "www" {
  account_id   = var.account_id
  project_name = cloudflare_pages_project.kama.name
  name         = "www.kama-lang.org"
}

# Cloudflare flattens the apex CNAME to A records automatically.
#
# ⚠️ Use the project's computed `subdomain`, never "<name>.pages.dev" — pages.dev names are globally
# unique, so this project got `kama-adu.pages.dev`. Hardcoding the name aimed the domain at a stranger's
# project, which Cloudflare serves as `error code: 1014`.
resource "cloudflare_dns_record" "apex" {
  zone_id = var.zone_id
  name    = "kama-lang.org"
  type    = "CNAME"
  content = cloudflare_pages_project.kama.subdomain
  proxied = true
  ttl     = 1
}

resource "cloudflare_dns_record" "www" {
  zone_id = var.zone_id
  name    = "www"
  type    = "CNAME"
  content = cloudflare_pages_project.kama.subdomain
  proxied = true
  ttl     = 1
}

# ---- the package registry: registry.kama-lang.org ----------------------------------------------------
# A second Pages project serving the static tree in the kama-registry repo, which deploys `registry/` to it
# on every push to main. Declared HERE, beside the site, so every record in the kama-lang.org zone lives in
# one state — two states managing one zone is how records get overwritten.
#
# This hostname is the MACHINE endpoint: it is compiled into kama binaries as the default registry, so it
# must never move. `packages.kama-lang.org` is reserved for a human browse/search site and has no record yet.
resource "cloudflare_pages_project" "registry" {
  account_id        = var.account_id
  name              = "kama-registry"
  production_branch = "main"
}

resource "cloudflare_pages_domain" "registry" {
  account_id   = var.account_id
  project_name = cloudflare_pages_project.registry.name
  name         = "registry.kama-lang.org"
}

resource "cloudflare_dns_record" "registry" {
  zone_id = var.zone_id
  name    = "registry"
  type    = "CNAME"
  content = cloudflare_pages_project.registry.subdomain   # computed — the pages.dev name may be suffixed
  proxied = true
  ttl     = 1
}

# ---- the registry's tarballs: dl.kama-lang.org (R2) ---------------------------------------------------
# The registry's INDEX lives in git and deploys to the Pages project above; its TARBALLS live here. A
# tarball is write-once and never deleted, so a repository holding them grows by every byte ever published
# and every clone downloads all of it — and Pages refuses any single file over 25 MiB. R2 egress is free,
# and the lock below makes write-once a property of the storage itself, not only of the checks publish runs.
#
# Clients never see this hostname. The registry's `_redirects` sends `/@kama/<pkg>/<file>` here, after a
# rule that keeps every `index.json` on Pages, so an index entry's `tarball` stays relative to
# registry.kama-lang.org and a lockfile records only that URL: this name can change with one line there.
resource "cloudflare_r2_bucket" "registry_tarballs" {
  account_id = var.account_id
  name       = "kama-registry-tarballs"
}

resource "cloudflare_r2_custom_domain" "registry_tarballs" {
  account_id  = var.account_id
  bucket_name = cloudflare_r2_bucket.registry_tarballs.name
  domain      = "dl.kama-lang.org"
  zone_id     = var.zone_id
  enabled     = true
  min_tls     = "1.2"
}

# Every object, forever: a published tarball can be neither overwritten nor deleted — not by `ops`, not by a
# leaked token, not by hand in the dashboard. ⚠️ So never upload a probe or test object to this bucket: it
# would be permanent too. `kama-registry`'s `ops publish` uploads only after every check has passed.
resource "cloudflare_r2_bucket_lock" "registry_tarballs" {
  account_id  = var.account_id
  bucket_name = cloudflare_r2_bucket.registry_tarballs.name
  rules = [{
    id        = "published-tarballs-are-permanent"
    enabled   = true
    prefix    = ""
    condition = { type = "Indefinite" }
  }]
}
