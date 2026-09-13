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
    *)
        printf '%s\n' 'unknown fake agent' >&2
        exit 2
        ;;
esac
