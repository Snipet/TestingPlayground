### Projects in git: when both sides changed

**Added**
- **Pull** on a project whose remote and this computer both have new versions merges them when they changed different
  things (different files, or different lines), saving the merge as a new version to push. When both changed the same
  lines, the Git card offers three ways, and nothing is lost by any of them: **Keep mine** (their side saved as a
  version, yours again on top), **Take theirs** (their side the newest version, yours the one before) and
  **Compare...**, which shows yours against theirs in the Compare card before you choose.
