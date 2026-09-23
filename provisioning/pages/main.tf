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
