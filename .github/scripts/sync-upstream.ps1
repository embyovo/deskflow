# SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
# SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#
# Merges the latest upstream commits into the current feature branch.
#
# A merge (not a rebase) is used on purpose: the feature branch is pushed to a
# public fork, and rebasing would rewrite already published commits and require
# a force-push. This script never pushes on its own and never resolves conflicts.

[CmdletBinding()]
param(
  [string]$UpstreamRemote = 'upstream',
  [string]$UpstreamUrl = 'https://github.com/deskflow/deskflow.git',
  [string]$Branch = '',
  [switch]$Rebase
)

$ErrorActionPreference = 'Stop'

function Write-Step([string]$Message) {
  Write-Host "==> $Message" -ForegroundColor Cyan
}

function Write-Note([string]$Message) {
  Write-Host "    $Message" -ForegroundColor DarkGray
}

function Write-Fail([string]$Message) {
  Write-Host "ERROR: $Message" -ForegroundColor Red
}

function Invoke-Git {
  param([Parameter(ValueFromRemainingArguments = $true)][string[]]$Arguments)

  # Native commands write progress to stderr, which PowerShell turns into
  # error records. Temporarily allow those so only the exit code decides.
  $previous = $ErrorActionPreference
  $ErrorActionPreference = 'Continue'
  try {
    $output = & git @Arguments 2>&1
    $exitCode = $LASTEXITCODE
  } finally {
    $ErrorActionPreference = $previous
  }

  if ($exitCode -ne 0) {
    throw "git $($Arguments -join ' ') failed with exit code $exitCode:`n$($output -join "`n")"
  }
  return $output
}

try {
  # --- make sure we are inside a work tree ---------------------------------
  & git rev-parse --is-inside-work-tree *> $null
  if ($LASTEXITCODE -ne 0) {
    Write-Fail 'This script must be run from inside a git work tree.'
    exit 1
  }

  $repoRoot = (& git rev-parse --show-toplevel).Trim()
  Write-Step "Repository: $repoRoot"

  # --- refuse to start with uncommitted work -------------------------------
  $status = & git status --porcelain --untracked-files=no
  if ($status) {
    Write-Fail 'The working tree has uncommitted changes. Commit or stash them first:'
    $status | ForEach-Object { Write-Host "    $_" }
    Write-Note 'Hint: git stash push -u -m "before upstream sync"'
    exit 1
  }

  # --- configure and validate the upstream remote ---------------------------
  $remotes = & git remote
  if ($remotes -notcontains $UpstreamRemote) {
    Write-Step "Adding remote '$UpstreamRemote' -> $UpstreamUrl"
    Invoke-Git remote add $UpstreamRemote $UpstreamUrl | Out-Null
  } else {
    $currentUrl = (& git remote get-url $UpstreamRemote).Trim()
    if ($currentUrl -ne $UpstreamUrl) {
      Write-Fail "Remote '$UpstreamRemote' points to '$currentUrl' instead of '$UpstreamUrl'."
      Write-Note "Fix it with: git remote set-url $UpstreamRemote $UpstreamUrl"
      exit 1
    }
    Write-Step "Remote '$UpstreamRemote' -> $currentUrl"
  }

  # --- fetch ----------------------------------------------------------------
  Write-Step "Fetching '$UpstreamRemote'"
  Invoke-Git fetch --prune $UpstreamRemote | ForEach-Object { Write-Note $_ }

  # --- figure out upstream's default branch ---------------------------------
  & git remote set-head $UpstreamRemote --auto *> $null
  $upstreamHead = (& git symbolic-ref --short "refs/remotes/$UpstreamRemote/HEAD" 2>$null)
  if ($LASTEXITCODE -ne 0 -or -not $upstreamHead) {
    $upstreamHead = "$UpstreamRemote/master"
    Write-Note "Could not detect the upstream default branch, assuming '$upstreamHead'."
  }
  $upstreamBranch = $upstreamHead -replace "^$UpstreamRemote/", ''
  $target = "$UpstreamRemote/$upstreamBranch"

  & git show-ref --verify --quiet "refs/remotes/$target"
  if ($LASTEXITCODE -ne 0) {
    Write-Fail "Branch '$target' does not exist after fetching."
    Write-Note "Known upstream branches:"
    & git branch --remotes --list "$UpstreamRemote/*" | ForEach-Object { Write-Host "    $_" }
    exit 1
  }
  Write-Step "Upstream default branch: $upstreamBranch"

  # --- pick the branch to update -------------------------------------------
  if (-not $Branch) {
    $Branch = (& git rev-parse --abbrev-ref HEAD).Trim()
  }
  & git show-ref --verify --quiet "refs/heads/$Branch"
  if ($LASTEXITCODE -ne 0) {
    Write-Fail "Local branch '$Branch' does not exist. Create it first, e.g.: git switch -c $Branch"
    exit 1
  }
  if ($Branch -eq $upstreamBranch) {
    Write-Fail "'$Branch' is the upstream branch itself, refusing to merge it into itself."
    exit 1
  }

  Write-Step "Switching to '$Branch'"
  Invoke-Git switch $Branch | ForEach-Object { Write-Note $_ }

  # --- already up to date? -------------------------------------------------
  & git merge-base --is-ancestor $target "refs/heads/$Branch"
  if ($LASTEXITCODE -eq 0) {
    Write-Step "Nothing to do, '$Branch' already contains '$target'."
    exit 0
  }

  # --- merge or rebase -----------------------------------------------------
  if ($Rebase) {
    Write-Step "Rebasing '$Branch' onto '$target' (history is rewritten, a force-push will be needed)"
    $gitArgs = @('rebase', $target)
  } else {
    Write-Step "Merging '$target' into '$Branch' (no history rewrite, no force-push needed)"
    $gitArgs = @('merge', '--no-edit', $target)
  }

  & git @gitArgs
  $mergeExitCode = $LASTEXITCODE

  if ($mergeExitCode -ne 0) {
    Write-Host ''
    Write-Fail "The $($gitArgs[0]) stopped. Conflicts have to be resolved by hand."
    Write-Note 'Conflicted files:'
    & git diff --name-only --diff-filter=U | ForEach-Object { Write-Host "    $_" }
    Write-Host ''
    Write-Note 'Resolve the files, then:'
    if ($Rebase) {
      Write-Note '    git add <files> && git rebase --continue'
      Write-Note '    git rebase --abort        # to give up and go back'
    } else {
      Write-Note '    git add <files> && git commit --no-edit'
      Write-Note '    git merge --abort         # to give up and go back'
    }
    Write-Note 'Nothing was pushed. Review the result, then push with:'
    Write-Note "    git push origin $Branch"
    exit 1
  }

  Write-Step "Done. '$Branch' now includes '$target'."
  & git --no-pager log --oneline --max-count 5
  Write-Note 'Nothing was pushed. Review and push with:'
  Write-Note "    git push origin $Branch"
  exit 0
} catch {
  Write-Fail $_.Exception.Message
  if ($repoRoot) {
    $mergeHead = Join-Path $repoRoot '.git\MERGE_HEAD'
    $rebaseDir = Join-Path $repoRoot '.git\rebase-merge'
    if ((Test-Path $mergeHead) -or (Test-Path $rebaseDir)) {
      Write-Note 'The repository is in the middle of a merge/rebase. Use `git status`, then `git merge --abort` to give up.'
    }
  }
  exit 1
}
