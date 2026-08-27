# Release Process

This guide explains how `.github/workflows/setup-release.yml` builds and
packages the Spotify3DS application and desktop setup app. It covers temporary
test builds and unpublished draft releases. The install links and QR code in
`README.md` depend on the latest release containing a file named exactly
`Spotify3DS.cia`, which this workflow builds automatically.

## GitHub Actions Concepts

| Term | Meaning in this repository |
| --- | --- |
| Workflow | The automation recipe in `.github/workflows/setup-release.yml`. |
| Workflow run | One execution of that recipe. Each run has an ID and a web page. |
| Input | A value supplied when manually starting a run, such as `destination`. |
| Job | A group of steps performed on one fresh virtual computer. |
| Runner | The temporary Linux, Windows, or macOS computer that executes a job. |
| Matrix | One job definition expanded into multiple platform variants. |
| Step | One command or reusable GitHub Action inside a job. |
| Artifact | A temporary file stored with a workflow run. It is not a GitHub Release. |
| Tag | A permanent name, such as `v1.0.0`, pointing to one Git commit. |
| Draft release | A GitHub Release visible to repository collaborators but not yet public. |
| `GITHUB_TOKEN` | A short-lived credential GitHub creates for the run. No personal token is stored in the repository. |

The workflow is manual because it uses `workflow_dispatch`. Pushing a commit or
tag does not start this release workflow by itself.

## Outputs

A successful run assembles these files:

| File | Contents |
| --- | --- |
| `Spotify3DS-Setup-windows-x64.zip` | Unsigned Windows x64 executable. |
| `Spotify3DS-Setup-windows-arm64.zip` | Unsigned native Windows ARM64 executable. |
| `Spotify3DS-Setup-linux-x64.tar.gz` | Linux x64 executable. |
| `Spotify3DS-Setup-linux-arm64.tar.gz` | Linux ARM64 executable. |
| `Spotify3DS-Setup-macos-universal.zip` | Universal x64/ARM64 macOS app, ad-hoc signed. |
| `Spotify3DS.cia` | Installable Nintendo 3DS application. |
| `Spotify3DS.3dsx` | Nintendo 3DS homebrew application. |

Windows packages are not Authenticode-signed. The macOS app has a free ad-hoc
signature for bundle integrity, but it is not Developer ID signed or notarized.
Users can therefore see operating-system security warnings.

## Workflow Stages

The jobs form this dependency chain:

```text
validate
  |-- linux x64
  |-- linux ARM64
  |-- windows x64
  |-- windows ARM64
  |-- macOS x64 + ARM64 slices -> universal macOS app
  `-- console -> CIA + 3DSX
                         |
                         `-> publish
```

1. `validate` checks out the requested code, validates the inputs, installs Go
   and Linux GUI dependencies, and runs `go test ./...`.
2. `linux` expands into x64 and ARM64 jobs. Each builds a native executable,
   inspects its shared-library dependencies, and creates a `.tar.gz` archive.
3. `windows` expands into x64 and ARM64 jobs. Each uses Fyne to embed the app
   icon and metadata, inspects the executable, and creates a ZIP archive.
4. Windows ARM64 downloads a pinned llvm-mingw compiler and verifies its SHA-256
   hash before using it. Fyne requires cgo, and the runner's default compiler
   cannot assemble native ARM64 code.
5. `macos-slices` builds separate x64 and ARM64 executables. `macos` combines
   them with `lipo`, creates the `.app`, ad-hoc signs it, verifies both
   architectures, and creates a ZIP with `ditto`.
6. `console` uses a digest-pinned devkitARM container, verifies pinned `makerom`
   and `bannertool` downloads, runs the host tests, and builds the CIA and 3DSX.
7. `publish` starts only after every required package job succeeds. It downloads
   all seven packages and sends them to the selected destination.

The strategy uses `fail-fast: false`, so one platform failure does not
immediately cancel the other platform builds. The final `publish` job does not
run unless every required platform succeeds.

## Prerequisites

Run release commands from the repository root. Install the GitHub CLI (`gh`),
authenticate it, and make sure the commit to release is on GitHub:

```sh
gh --version
gh auth status
git status --short
git branch --show-current
git push origin main
```

| Command | Purpose |
| --- | --- |
| `gh --version` | Confirms the GitHub CLI is installed. |
| `gh auth status` | Shows which GitHub account is authenticated and whether it has repository access. |
| `git status --short` | Shows local changes. Uncommitted changes cannot be built by GitHub. |
| `git branch --show-current` | Confirms which local branch is checked out. |
| `git push origin main` | Sends local `main` commits to the GitHub repository named `origin`. |

GitHub runners can only see commits and tags that have been pushed. They cannot
read uncommitted files or commits that exist only on this computer.

## Temporary Artifact Test

Use artifact mode before creating a release tag:

```sh
gh workflow run setup-release.yml \
  --ref main \
  -f ref=main \
  -f destination=artifact
```

The arguments have separate jobs:

| Argument | Meaning |
| --- | --- |
| `workflow run setup-release.yml` | Manually starts the workflow with this filename. |
| `--ref main` | Loads the workflow YAML itself from the `main` branch. |
| `-f ref=main` | Tells artifact mode which branch, tag, or commit to build. |
| `-f destination=artifact` | Stores the final files as a temporary Actions artifact instead of creating a release. |

There are two `ref` values because GitHub first needs to know which version of
the workflow definition to run, and the workflow then needs to know which
version of the application source to build. They are normally both `main` for a
dry run.

The command prints the new run's URL. The number at the end is the run ID. These
commands find, follow, and inspect a run:

```sh
gh run list --workflow setup-release.yml --limit 5
gh run watch RUN_ID --exit-status
gh run view RUN_ID
gh run view RUN_ID --log-failed
```

| Command | Purpose |
| --- | --- |
| `gh run list` | Lists recent runs and their IDs. |
| `gh run watch` | Refreshes until the run ends. `--exit-status` makes the command fail if the workflow fails. |
| `gh run view` | Shows the jobs and steps in one run. |
| `gh run view --log-failed` | Prints logs from failed steps for diagnosis. |

After a successful artifact run, download and inspect its final bundle:

```sh
gh run download RUN_ID \
  --name Spotify3DS-release-test \
  --dir /tmp/spotify3ds-release-test
```

`gh run download` downloads the named artifact from that run. Confirm that it
contains all seven packages listed above.

Artifact mode creates no tag and no GitHub Release. The final artifact and the
per-platform package artifacts are retained for seven days. The temporary
macOS slice artifacts are retained for one day.

## Draft Release

### 1. Choose and create a version tag

Replace every `v1.0.0` below with the intended version. Use a semantic version
of the form `vMAJOR.MINOR.PATCH` so the packaged app receives the expected
version number.

```sh
git status --short
git log -1 --oneline
git push origin main
git tag -a v1.0.0 -m "Spotify3DS v1.0.0"
git push origin v1.0.0
```

`git tag -a` creates an annotated local tag pointing to the current commit.
`git push origin v1.0.0` sends that tag to GitHub. The workflow verifies that
the remote tag exists and builds the exact commit it identifies, making the
release reproducible even after `main` changes.

Do not move or reuse a published version tag. If published code needs a fix,
create a new patch version such as `v1.0.1`.

### 2. Build the tag into a draft

```sh
gh workflow run setup-release.yml \
  --ref main \
  -f ref=v1.0.0 \
  -f destination=draft \
  -f release_tag=v1.0.0
```

In draft mode, `release_tag` is required and is the code that actually gets
built. The `ref` input remains required by the workflow form but is not used to
select code in this mode. Setting both to the tag makes the command's intent
clear.

The platform build is identical to artifact mode. The difference is the final
step: `softprops/action-gh-release` uses the run's temporary `GITHUB_TOKEN` to
create or update an unpublished release for the tag, generate release notes,
and upload all seven packages.

Follow the run and inspect the resulting draft:

```sh
gh run list --workflow setup-release.yml --limit 5
gh run watch RUN_ID --exit-status
gh release view v1.0.0 \
  --json tagName,name,isDraft,isPrerelease,url,assets
```

Confirm that `isDraft` is `true` and that all seven expected packages are listed.
You can also open the repository's **Releases** page; collaborators can see the
draft, but ordinary visitors cannot.

### 3. Review the packages and release

Download the draft assets for testing:

```sh
gh release download v1.0.0 --dir /tmp/spotify3ds-v1.0.0
```

Review the generated release title and notes in GitHub. Test packages on real
systems when practical. GitHub records a SHA-256 digest for each uploaded asset;
you can inspect those values with:

```sh
gh release view v1.0.0 --json assets \
  --jq '.assets[] | {name, size, digest}'
```

Test the downloaded console packages in an emulator and on real hardware when
practical. You can also reproduce the console checks locally:

```sh
./tests/run_host_tests.sh
go test ./...
make -j8
```

Confirm that the draft contains both `Spotify3DS.cia` and `Spotify3DS.3dsx`
before making it the latest release.

### 4. Publish only after review

Publishing is intentionally manual. The safest option for a first release is
to open the draft in GitHub, review everything, and select **Publish release**.
The command-line equivalent is:

```sh
gh release edit v1.0.0 --draft=false --latest
```

`--draft=false` makes the release public immediately. `--latest` makes GitHub's
`/releases/latest` URL and latest-download links point to this release.

After publishing, check both the release and the CIA download link:

```sh
gh release view v1.0.0 --web
curl -I -L \
  https://github.com/avncharlie/spotify3ds/releases/latest/download/Spotify3DS.cia
```

Do not publish merely to test the workflow. Artifact mode and unpublished draft
mode provide the complete test paths without notifying users or changing the
latest release.

## Rerunning a Draft

If a package fails, fix the problem on the tagged commit before publication.
For an abandoned draft, it is usually clearer to delete the draft and tag,
commit the fix, and create the tag again. Never rewrite a tag after its release
has been published.

If the tag still points to the desired commit, rerunning draft mode creates or
updates the same draft release and uploads the newly built package files.

## Abandoning an Unpublished Draft

Use these commands only for a draft or disposable test version that has never
been published:

```sh
gh release delete v1.0.0 --yes
git push origin --delete v1.0.0
git tag -d v1.0.0
```

| Command | Purpose |
| --- | --- |
| `gh release delete` | Deletes the unpublished GitHub Release. |
| `git push origin --delete` | Deletes the tag from GitHub. |
| `git tag -d` | Deletes the tag from this computer. |

Deleting a public release does not undo downloads or notifications. Published
versions should normally remain available and be superseded by a newer version.

## GitHub Website Equivalent

The CLI is a convenient way to run and inspect the automation, but it is not
required to start a workflow:

1. Open the repository on GitHub.
2. Select **Actions**.
3. Select **Spotify3DS release** in the workflow list.
4. Select **Run workflow**.
5. Keep **Use workflow from** set to `main`.
6. Enter the source ref, choose `artifact` or `draft`, and enter an existing tag
   when using draft mode.
7. Select the green **Run workflow** button.
8. Open the new run to see jobs, steps, logs, timing, and downloadable artifacts.

The website fields correspond directly to `--ref` and the `-f` values in the
CLI commands.

## Common Failures

| Failure location | Likely meaning |
| --- | --- |
| `Validate release inputs` | The draft tag is missing, does not exist, or cannot be checked out. |
| `Test` | `go test ./...` failed; no packages or release should be produced. |
| Linux `Build` | Go, cgo, OpenGL, or X11 compilation failed on that architecture. |
| Windows ARM64 toolchain | The compiler download, checksum, extraction, or expected layout failed. |
| Windows `Build executable with icon` | Fyne packaging or native cgo compilation failed. |
| macOS `Create universal executable` | One architecture slice is missing or invalid. |
| macOS `Ad-hoc sign and verify` | Bundle creation, signing, plist validation, or architecture verification failed. |
| Console `Install packaging tools` | A pinned tool download, checksum, or archive layout is invalid. |
| Console `Run host tests` or `Build console packages` | A host test failed or the 3DS source did not compile and package. |
| `publish` | A package is missing, artifact upload failed, or GitHub rejected a draft-release operation. |

Open the failed job, expand the red step, and read from the first concrete error
rather than the final `Process completed with exit code 1` message. Use
`gh run view RUN_ID --log-failed` to retrieve the same information in a terminal.

## Command Summary

```sh
# Authenticate and check the repository.
gh auth status
git status --short

# Run a temporary seven-day artifact build.
gh workflow run setup-release.yml --ref main \
  -f ref=main -f destination=artifact

# Find and follow the run.
gh run list --workflow setup-release.yml --limit 5
gh run watch RUN_ID --exit-status
gh run view RUN_ID --log-failed

# Create and push a release tag.
git tag -a v1.0.0 -m "Spotify3DS v1.0.0"
git push origin v1.0.0

# Build that tag and create an unpublished draft release.
gh workflow run setup-release.yml --ref main \
  -f ref=v1.0.0 -f destination=draft -f release_tag=v1.0.0

# Inspect the draft.
gh release view v1.0.0 --json isDraft,url,assets

# Publish only after review.
gh release edit v1.0.0 --draft=false --latest
```
