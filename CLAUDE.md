# Unreal Championship 2 recompilation

Open work is in GitHub Issues: start with the pinned Roadmap issue (`gh issue list`, `gh issue view <n>`) for order and
evidence. `README.md` covers setup and status, `docs/DEVELOPING.md` the debugging tools, and `docs/HISTORY.md` records
past bugs and their fixes.

Issues are public: put symptoms, addresses and measurements in them, never disassembly or game-derived dumps. When work
fixes an issue, say `Fixes #<n>` in the commit, and add new findings to the issue rather than to a file.

## Commands
- Build: `tools/build.cmd` (log in `build/build.log`, ends with `BUILD_EXIT=`)
- Play: `powershell -File tools/play.ps1`
- Scripted match: `powershell -File tools/run_match.ps1 -Secs 300 -Log run.txt`
- Profile a live run: `python -I tools/prof.py --secs 20`
- Disassemble: `python -I tools/dis.py <hex va> <hex len>`, or `sections`
- Regenerate the toolkit patch: `powershell -File tools/update_patches.ps1` (`-Check` only compares)

## Rules
- Never commit game data or anything derived from it: `assets/`, `src/recomp/gen/`, `build/`, screenshots, disassembly.
- `assets/default_analysis.json` holds keys from the XBE certificate. Keep it in `assets/` and do not quote them.
- Commit locally without asking, on `main`. Ask before pushing.
- Don't build or run tests while the owner is playing: they lock the exe and open a window.
- Adding or removing a hook, wrapper or hand-written function in `src/recomp_manual.c` means rerunning `tools.recomp` with
  the README's flags, then a rebuild. Allow about 10 minutes.
- `third_party/xboxrecomp` is pinned at f0f8a46 and shows as modified because `patches/` are applied in its working tree.
  After editing it, regenerate the patch.

## Before concluding
- Check a claim against evidence: consecutive frames for rendering, a profile for speed. Scripted runs can stall in a
  menu, so look at the frames.
- Before naming a culprit from an address, check its section with `tools/dis.py sections`.
- Run the game with a timeout. It can spin forever.

## Machine quirks
- New shells need a PATH refresh before Python, CMake and Ninja are found:
  `$env:Path=[Environment]::GetEnvironmentVariable('Path','Machine')+';'+[Environment]::GetEnvironmentVariable('Path','User')`
- Plain `git push` has no credentials. Use:
  `git -c credential.helper= -c 'credential.helper=!gh auth git-credential' push origin main`
- Pass `</dev/null` to long commands in Bash. A bare `cat >` with no input hangs until stopped.
