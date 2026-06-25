# Releasing `brotli_splice`

Releases are automated through [RubyGems Trusted Publishing](https://guides.rubygems.org/trusted-publishing/).
Publishing a GitHub Release runs [`.github/workflows/release.yml`](.github/workflows/release.yml),
which builds the gem and pushes it to [RubyGems.org](https://rubygems.org/gems/brotli_splice)
using a short-lived OIDC token. There are **no API keys or secrets** to manage.

## One-time setup (already done — for reference)

These only need to happen once for the gem and are recorded here so they can be
re-created if needed:

1. **Pending trusted publisher** registered on RubyGems.org (RubyGems → your
   avatar → *Trusted Publishers* → *Pending* → *Create*) with:
   - Gem name: `brotli_splice`
   - Repository owner: `Shopify`
   - Repository name: `brotli_splice`
   - Workflow filename: `release.yml`
   - Environment: `release`

   After the first successful publish this auto-converts to a normal trusted
   publisher under the gem's settings.
2. **GitHub Actions environment** named `release` exists (repo *Settings →
   Environments*). The release job references it.

## Releasing a new version

1. **Bump the version.** Edit [`lib/brotli_splice/version.rb`](lib/brotli_splice/version.rb)
   following [SemVer](https://semver.org/):
   - patch (`0.1.1` → `0.1.2`): backwards-compatible bug fixes
   - minor (`0.1.1` → `0.2.0`): backwards-compatible features
   - major (`0.1.1` → `1.0.0`): breaking changes

   ```ruby
   module BrotliSplice
     VERSION = "X.Y.Z"
   end
   ```

2. **Open a PR with the bump and merge it to `main`.** The version must be on
   `main` *before* you tag, because the release workflow builds the gem from the
   commit the tag points at. Let CI ([`.github/workflows/ci.yml`](.github/workflows/ci.yml))
   pass.

3. **Create and publish a GitHub Release.** Tag the merged commit `vX.Y.Z` (the
   `v` prefix is the convention; the tag must match `version.rb`). Use the
   release notes as the changelog — the gemspec's `changelog_uri` points at the
   Releases page.

   Via the UI: *Releases → Draft a new release → Choose a tag → `vX.Y.Z` →
   Publish release*.

   Or via the CLI:
   ```
   gh release create vX.Y.Z --repo Shopify/brotli_splice --title "vX.Y.Z" --generate-notes
   ```

4. **Watch the release run.** Publishing the release triggers
   `.github/workflows/release.yml`, which sets up Ruby, builds the gem, and
   pushes it via trusted publishing. Confirm the run is green:
   ```
   gh run list --repo Shopify/brotli_splice --workflow release.yml
   ```

5. **Verify the gem is live** (usually within a minute):
   ```
   gem list -r brotli_splice
   ```
   or open <https://rubygems.org/gems/brotli_splice>.

## Notes

- The tag/release **must** match the version in `version.rb`; a mismatch ships a
  gem version that doesn't line up with the tag.
- No `gem signin` / `gem push` from a laptop is needed. If you ever publish
  manually as a fallback, note the gemspec sets `rubygems_mfa_required = true`,
  so an OTP is required.
- `brotli_splice` is a C extension; the published gem ships source and compiles
  on `gem install`, so the release workflow only needs to package it, not
  cross-compile.
