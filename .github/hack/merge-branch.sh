#!/usr/bin/env bash

# Copyright 2015 The Kubernetes Authors.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# Usage Instructions: https://git.k8s.io/community/contributors/devel/sig-release/cherry-picks.md

# Checkout a PR from GitHub. (Yes, this is sitting in a Git tree. How
# meta.) Assumes you care about pulls from remote "upstream" and
# checks them out to a branch named:
#  automated-cherry-pick-of-<pr>-<target branch>-<timestamp>

# Modified by Mattermost to do a real cherry-pick instead of applying patches
# via git am
#
# Modified by Synadia.

set -o errexit
set -o nounset
set -o pipefail
set -x

GITHUB_ORG="${GITHUB_ORG:-nats-io}"

# <>/<>
# pwd 
# export
# echo '<>/<>--------------------'
# cat ${GITHUB_EVENT_PATH}
# echo '<>/<>--------------------'

declare -r BRANCH="$1"
declare -r PR_NUMBER="$2"
declare -r COMMIT_SHA="$3"

git fetch origin

NEW_BRANCH="failed-automerge-to-${BRANCH}-pr${PR_NUMBER}-$(date +%s)"
declare -r NEW_BRANCH
echo "+++ Create local branch ${NEW_BRANCH} for PR #${PR_NUMBER} at ${COMMIT_SHA}"
# git log | grep 'f37bc4b'
git log --max-count=50
git checkout "${COMMIT_SHA}"
git checkout -b "${NEW_BRANCH}"
# git checkout -b "${NEW_BRANCH}" "${COMMIT_SHA}"

echo "+++ Try merging ${COMMIT_SHA} onto ${BRANCH}"
git checkout "${BRANCH}"
git merge --no-ff "${NEW_BRANCH}"
if [[ -z $(git status --porcelain) ]]; then
  echo "+++ Merged cleanly, push to GitHub."
  git push origin "${BRANCH}"
  exit 0
fi

git merge --abort
git checkout "${NEW_BRANCH}"
echo "+++ Merge failed, create a PR"

# This looks like an unnecessary use of a tmpfile, but it avoids
# https://github.com/github/hub/issues/976 Otherwise stdin is stolen
# when we shove the heredoc at hub directly, tickling the ioctl
# crash.
prtext="$(mktemp -t prtext.XXXX)" 
cat >"${prtext}" <<EOF
Failed merge: #${PR_NUMBER}

Automated merge to `${BRANCH}` triggered by #${PR_NUMBER} failed. Please resolve the conflicts and push manually, see [C Release Instructions](https://github.com/nats-io/nats-internal/blob/master/release-instructions/C.md)

EOF

exit 0