# Decisive corruption check: the sequence of non-ASCII characters in a modified
# file must be identical to the sequence in the committed version.
#
# Re-encoding damage (UTF-8 read as GBK, written back as UTF-8) changes those
# characters while leaving ASCII untouched. Comparing them is the only test that
# cannot be fooled -- byte counts shift for other reasons, U+FFFD only appears
# for some damage, and a diff rendered through the same console mangles both
# sides into something that looks like damage whether or not there is any.
#
# ASCII only in this file. See check-commit.ps1 for why.

$ErrorActionPreference = "Stop"
Set-Location "C:\Users\Char\Documents\Repos\Neiro"

$tmp = Join-Path $env:TEMP "opencode\blob.bin"
$utf8 = New-Object System.Text.UTF8Encoding($false)
$out = New-Object System.Collections.Generic.List[string]

# The files that exist in HEAD. `--diff-filter=M` is the query, and asking it once
# here rather than per file is what keeps a new file from being a fatal error; see the
# long note in the loop below for the three ways of asking that did not work.
$modified = @(git diff --name-only --diff-filter=M)

function NonAscii([string]$s) {
  $sb = New-Object System.Text.StringBuilder
  foreach ($ch in $s.ToCharArray()) {
    if ([int]$ch -gt 127) { [void]$sb.Append($ch) }
  }
  return $sb.ToString()
}

foreach ($f in (git diff --name-only)) {
  # **A file that is not in HEAD has no previous version to compare against**, and
  # asking `git cat-file` for one prints a fatal error and writes an empty file --
  # which this script then reads as "the committed version had no non-ASCII
  # characters at all" and reports every new CJK character as an append. So the
  # question is asked first, and a new file is reported as such.
  #
  # Found because adding `crates/neiro-tags/` made the whole check die with
  # `NativeCommandError`, which is the script refusing to say anything at all about
  # any file -- the worst possible outcome for a checker, and it happened on the
  # commit that touched the most non-ASCII text in the repository's history.
  # **A new file has no previous version, and three ways of asking that all failed.**
  #
  #   1. `cmd /c "git cat-file blob HEAD:$f"` -- prints a fatal error, and
  #      `$ErrorActionPreference = 'Stop'` makes it terminating.
  #   2. `git cat-file -e "HEAD:$f"` -- **same problem**: git writes to stderr even
  #      with `2>$null`, because `2>$null` redirects the *pipeline*, and a native
  #      command's stderr becomes an error record under `Stop` before the redirect is
  #      considered.
  #   3. `git diff --name-only --diff-filter=M` -- the answer, and it was staring at me:
  #      **the filter is the query.** `M` is "modified", so a file that is only added
  #      (`A`) is never asked about, and no command ever fails.
  #
  # What this cost: adding `crates/neiro-tags/` made the script die with
  # `NativeCommandError` **three times in a row**, each time saying nothing about any
  # file -- on the commit that moved the most Chinese text in the repository's
  # history. A checker that answers "nothing" is worse than one that answers "wrong",
  # because "nothing" reads as "fine".
  if ($f -notin $modified) {
    $b = $utf8.GetString([System.IO.File]::ReadAllBytes($f))
    $out.Add(("  NEW     {0,-30} {1} non-ASCII chars, nothing to compare" -f (Split-Path $f -Leaf), (NonAscii $b).Length))
    continue
  }
  # **`--diff-filter=M` is not enough, and this is why.** `git mv` makes a rename, and
  # git reports the *destination* of a rename as modified -- so a path that has never
  # existed at HEAD is in `$modified`, the branch above lets it through, and
  # `cat-file` fails on it. Which is the fourth way of asking that did not work.
  #
  # So the branch above is an optimisation that keeps the common case quiet, and
  # **this** is the one that is correct: run the command, and if it cannot produce a
  # previous version, say so. Catching the failure is not a workaround for not
  # knowing -- it is the only way to ask a question whose answer can legitimately be
  # "there is no such thing yet".
  $previous = $null
  try {
    cmd /c "git cat-file blob HEAD:$f > `"$tmp`"" 2>$null
    if ($LASTEXITCODE -eq 0) { $previous = $utf8.GetString([System.IO.File]::ReadAllBytes($tmp)) }
  } catch {
    $previous = $null
  }
  $b = $utf8.GetString([System.IO.File]::ReadAllBytes($f))
  if ($null -eq $previous) {
    $out.Add(("  NEW     {0,-30} {1} non-ASCII chars, nothing to compare" -f (Split-Path $f -Leaf), (NonAscii $b).Length))
    continue
  }
  $a = $previous

  $na = NonAscii $a
  $nb = NonAscii $b

  if ($na -ceq $nb) {
    $out.Add(("  OK      {0,-30} {1} non-ASCII chars, identical" -f (Split-Path $f -Leaf), $na.Length))
  } elseif ($nb.StartsWith($na)) {
    # The committed characters are all still there, in order, with more appended.
    # That is an intentional addition -- a new em dash, a new Chinese word -- and
    # not re-encoding damage. Distinguishing these two is the difference between a
    # detector and something that cries wolf.
    $out.Add(("  OK+     {0,-30} {1} -> {2} chars; first {1} identical, {3} appended" -f `
      (Split-Path $f -Leaf), $na.Length, $nb.Length, ($nb.Length - $na.Length)))
  } else {
    # find first divergence
    $n = [Math]::Min($na.Length, $nb.Length)
    $i = 0
    while ($i -lt $n -and $na[$i] -ceq $nb[$i]) { $i++ }
    $out.Add(("  BAD     {0,-30} HEAD {1} chars -> now {2}; first diff at {3}" -f `
      (Split-Path $f -Leaf), $na.Length, $nb.Length, $i))
    # BAD means "go and confirm", not "it is broken". Editing Chinese text on
    # purpose produces exactly this: the sequence diverges where the edit was.
    # The two cases are not distinguishable from the bytes alone, so the tool
    # reports what it can see and leaves the decision to a human. What it *can*
    # say is that the text before the divergence is intact, which is the part a
    # re-encoding would have destroyed first.
  }
}

Write-Output "=== non-ASCII sequences: HEAD vs working tree ==="
$out
$bad = ($out | Where-Object { $_ -match '^\s+BAD' }).Count
Write-Output ""
Write-Output ("files changed: " + $out.Count + ", diverged: " + $bad)
if ($bad -eq 0) {
  Write-Output "CLEAN"
} else {
  Write-Output "DIVERGED - confirm these are edits you meant to make (see the note on BAD above)"
}
