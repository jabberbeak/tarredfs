# Bash auto completion for beak

_beakusermounts()
{
    local cur=${COMP_WORDS[COMP_CWORD]}
    local uid=$(id -u)
    local mounts=$(cat /proc/mounts | grep fuse.beak | grep user_id=${uid} | cut -f 2 -d ' ' | sort)
    if [ "$mounts" != "" ]; then
        COMPREPLY=($(compgen -W "$mounts" -- $cur))
    fi
    return 0
}

_beaksources()
{
  local cur=${COMP_WORDS[COMP_CWORD]}
  local sources=$(grep -o \\[.*\\] ~/.beak.conf | tr -d '[' | tr ']' ':' | sort)
  if [ "$sources" != "" ]; then 
      COMPREPLY=($(compgen -W "$sources" -- $cur))
      if [ -z "$COMPREPLY" ]; then
          # No match for configured source trees. Try directories instead.
          _filedir -d
      fi
  fi
  return 0
}

_beakremotes()
{
  local cur=${COMP_WORDS[COMP_CWORD]}
  local prev=${COMP_WORDS[COMP_CWORD-1]}
  local remotes=$(sed -n '/^\[work\]/,/^\[/p' ~/.beak.conf | grep remote | sed 's/remote.*= \?//g' | sort)
  if [ "$remotes" != "" ]; then
      COMPREPLY=($(compgen -W "$remotes" -- $cur))
      if [ -z "$COMPREPLY" ]; then
          # No match for configured remotes trees. Try directories instead.
          _filedir -d
      fi
  fi
  return 0
}

_beak()
{
    local cur prev prevprev prevprevprev
    cur=${COMP_WORDS[COMP_CWORD]}
    prev=${COMP_WORDS[COMP_CWORD-1]}
    prevprev=${COMP_WORDS[COMP_CWORD-2]}
    prevprevprev=${COMP_WORDS[COMP_CWORD-3]}

    # The colon in configured sources gets its own CWORD....
    if [ "$prev" = ":" ]; then prev="$prevprev" ; prevprev="$prevprevprev"; fi
    
    case "$prev" in
        push) _beaksources ;;
        pull) _beakremotes ;;        
        mount) _beaksources ;;
        umount) _beakusermounts ;;
    esac

    case "$prevprev" in
        push) _beakremotes ;;
        pull) _beaksources ;;
        mount) _filedir -d ;;
    esac

    if [ -z "$COMPREPLY" ]; then
        COMPREPLY=($(compgen -W "push pull mount umount" -- $cur))
    fi
}

complete -F _beak beak
