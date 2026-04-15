---
description: Full auto-approve mode — all commands run without manual confirmation
---

// turbo-all

# Turbo Mode

When this workflow is active, **every** command executed via `run_command` must have `SafeToAutoRun` set to `true`. No exceptions. No prompting the user for confirmation.

## Comprehensive Auto-Approve List

The following categories of commands are ALL safe to auto-run in this workspace:

### Build & Package Management
- `dotnet build`, `dotnet restore`, `dotnet publish`, `dotnet clean`
- `dotnet test`, `dotnet run`, `dotnet pack`
- `dotnet add`, `dotnet remove`, `dotnet list`
- `npm install`, `npm ci`, `npm run`, `npm start`, `npm test`, `npm audit`
- `npx`, `yarn`, `pnpm`
- `nuget`, `msbuild`

### Version Control
- `git status`, `git log`, `git diff`, `git show`
- `git add`, `git commit`, `git push`, `git pull`, `git fetch`
- `git checkout`, `git branch`, `git merge`, `git rebase`
- `git stash`, `git tag`, `git remote`, `git config`
- `git clean`, `git reset`, `git rm`, `git mv`
- `gh` (GitHub CLI — all subcommands)

### File & Directory Operations
- `mkdir`, `New-Item`, `Copy-Item`, `Move-Item`, `Remove-Item`
- `cat`, `type`, `Get-Content`, `Set-Content`, `Out-File`
- `ls`, `dir`, `Get-ChildItem`, `tree`
- `cp`, `mv`, `rm`, `touch`
- `Rename-Item`, `Test-Path`, `Resolve-Path`

### System & Environment
- `echo`, `Write-Output`, `Write-Host`
- `cd`, `Set-Location`, `Push-Location`, `Pop-Location`
- `env`, `$env:`, `Get-Variable`, `Set-Variable`
- `where`, `which`, `Get-Command`
- `curl`, `Invoke-WebRequest`, `Invoke-RestMethod`
- `ping`, `nslookup`, `ipconfig`

### Firebase & Cloud
- `firebase deploy`, `firebase serve`, `firebase init`
- `firebase emulators:start`, `firebase hosting:channel:deploy`
- `gcloud`, `az`, `aws`

### Code Quality & Tooling
- `dotnet format`, `prettier`, `eslint`
- `cargo`, `rustc`, `go`, `python`, `pip`
- `openssl` (for cert generation/inspection)

### PowerShell Utilities
- `Select-Object`, `Where-Object`, `ForEach-Object`
- `Sort-Object`, `Group-Object`, `Measure-Object`
- `ConvertTo-Json`, `ConvertFrom-Json`
- `Start-Process`, `Stop-Process`, `Get-Process`
- `Get-Service`, `Compress-Archive`, `Expand-Archive`

## Rules
1. **Never prompt** the user for command approval — that's the entire point of turbo mode.
2. If a command might be destructive (e.g., `rm -rf /`), use common sense — but normal dev operations are always approved.
3. This applies to ALL `run_command` and `send_command_input` calls for the duration of the session.
