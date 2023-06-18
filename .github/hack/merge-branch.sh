#!/usr/bin/env bash

set -o errexit
set -o nounset
set -o pipefail

# <>/<>
cat ${GITHUB_EVENT_PATH}


declare -r BRANCH="$1"
declare -r PR_NUMBER="$2"
declare -r MERGE_COMMIT_SHA="$3"

GITHUB_ORG="${GITHUB_ORG:-levb}"
git config user.name "${GITHUB_ACTOR}"
git config user.email "${GITHUB_ACTOR}@users.noreply.github.com"

NEW_BRANCH="automerge-to-${BRANCH}-pr${PR_NUMBER}-$(date +%s)"
declare -r NEW_BRANCH
echo "+++ Create local branch ${NEW_BRANCH} for PR #${PR_NUMBER} at ${MERGE_COMMIT_SHA}"
git checkout -b "${NEW_BRANCH}" "${MERGE_COMMIT_SHA}"

echo "+++ Try merging ${MERGE_COMMIT_SHA} onto ${BRANCH}"
git checkout "${BRANCH}"
set +o errexit
git merge --no-ff "${NEW_BRANCH}"
set -o errexit
if [[ -z $(git status --porcelain) ]]; then
  echo "+++ Merged cleanly, push to GitHub."
  git push origin "${BRANCH}"
  exit 0
fi

echo "+++ Merge failed, create a PR"
git merge --abort
git checkout "${NEW_BRANCH}"
git push origin "${NEW_BRANCH}"

# This looks like an unnecessary use of a tmpfile, but it avoids
# https://github.com/github/hub/issues/976 Otherwise stdin is stolen
# when we shove the heredoc at hub directly, tickling the ioctl
# crash.
prtext="$(mktemp -t prtext.XXXX)" 
cat >"${prtext}" <<EOF
Failed to merge #${PR_NUMBER} to ${BRANCH}

Automated merge to ${BRANCH} triggered by #${PR_NUMBER} failed. Please resolve the conflicts and push manually, see [C Release Instructions](https://github.com/nats-io/nats-internal/blob/master/release-instructions/C.md)

EOF

hub pull-request -F "${prtext}" -h "${GITHUB_ORG}:${NEW_BRANCH}" -b "${GITHUB_ORG}:${BRANCH}"
