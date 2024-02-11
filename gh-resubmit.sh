#!/bin/bash

# Number of times you want to trigger the workflow
times=5

# Current branch
branch=$(git rev-parse --abbrev-ref HEAD)

for i in $(seq 1 $times); do
    # Change the commit date and keep the commit message the same
    GIT_COMMITTER_DATE="$(date)" git commit --amend --no-edit --date "$(date)"

    # Force push the current branch
    git push --force

    # Wait a bit to avoid any potential rate limiting issues (optional)
    sleep 5
done
