#!/bin/sh
set -eu

prompt=
for value in "$@"; do
    prompt=$value
done

case ${MULTICA_CORE_AGENT_ID:-} in
    leader)
        case $prompt in
            *"worker completed with verification"*)
                printf '%s\n' 'The delegated work is complete and verified.'
                ;;
            *)
                printf '%s\n' '[@Worker](mention://agent/worker) handle the implementation and report verification.'
                ;;
        esac
        ;;
    worker)
        printf '%s\n' 'worker completed with verification'
        ;;
    sleeper)
        sleep 30
        printf '%s\n' 'sleeper completed'
        ;;
    flaky)
        state=${MULTICA_CORE_HOME:?}/.${MULTICA_CORE_ISSUE_ID:?}.flaky
        if [ ! -f "$state" ]; then
            : > "$state"
            printf '%s\n' 'transient failure' >&2
            exit 3
        fi
        printf '%s\n' 'flaky agent recovered on retry'
        ;;
    *)
        printf '%s\n' 'unknown fake agent' >&2
        exit 2
        ;;
esac
