# Shared console steps for tools/switch_cycle.sh and tools/switch_bench.sh (docs/switch.md "Remote testing").
# Source it from the repo root. Transfer: WLAN FTP (tools/switch_ftp.py, sys-ftpd) when the console answers on it,
# else USB MTP (tools/switch_mtp.py, DBI's MTP responder); TRANSFER=ftp|mtp forces one.
#   B <switch_bot.py args>     bot command with 3 tries
#   F <ls|get|put|rm> ...      file command over the chosen transfer
#   sw_transfer                sets SW_T=ftp|mtp (call once)
#   sw_ready                   FTP: close whatever runs, home menu.  MTP: check we are in hbmenu/DBI (MTP screen)
#   sw_push_update <dir>       put <dir>/atmosphere/contents/0100D0D0600A0000/exefs/{main,main.npdm}
#   sw_launch <shots dir>      start the game from the state sw_ready left (user "claude"), check its title
#   sw_close <shots dir>       game -> home menu, close it (FTP) / back to DBI's MTP screen (MTP)
#   sw_pull <dest dir>         log.txt + pcs_*.txt
GAME_TID=0100D0D0600A0000
HB_TID=0197BB45716E0000
B() { for k in 1 2 3; do timeout 60 python -I tools/switch_bot.py "$@" 2>/dev/null && return 0; sleep 3; done; return 1; }
F() {
  local tool=tools/switch_mtp.py to=300 wait=10
  [ "$SW_T" = ftp ] && { tool=tools/switch_ftp.py; to=600; wait=3; }
  for k in 1 2 3; do timeout $to python -I $tool "$@" && return 0; [ "$1" = rm ] && return 1; sleep $wait; done; return 1
}
sw_transfer() {
  SW_T=${TRANSFER:-}
  if [ -z "$SW_T" ]; then
    if timeout 30 python -I tools/switch_ftp.py check >/dev/null 2>&1; then SW_T=ftp; else SW_T=mtp; fi
  fi
  echo "transfer: $SW_T"
}
# HOME until the home menu shows (a missed press leaves the game's darker, smaller JPEG), then close the app
sw_home_close() {
  local S=$1
  for k in 1 2 3 4; do B click HOME; sleep 5; B shot "$S/h_$k.jpg" >/dev/null; [ $(stat -c %s "$S/h_$k.jpg") -gt 150000 ] && break; done
  B click X; sleep 2; B click A; sleep 3
}
sw_ready() {
  local S=$1 t
  t=$(B title)
  if [ "$SW_T" = mtp ]; then
    [ "$t" = "$HB_TID" ] || { echo "not in hbmenu/DBI (title $t)"; return 1; }
    return 0
  fi
  # FTP: nothing may run but the home menu; the closed app's tile becomes the first one, the cursor on it
  SW_CLOSED=none
  if [ -n "$t" ] && [ "$t" != 0 ] && [ "$t" != 0000000000000000 ]; then
    sw_home_close "$S"; SW_CLOSED=$t
  fi
}
sw_push_update() {
  local E=$1/atmosphere/contents/$GAME_TID/exefs
  F put $E/main atmosphere/contents/$GAME_TID/exefs || return 1
  F put $E/main.npdm atmosphere/contents/$GAME_TID/exefs || return 1
}
# "Who is using the software?" (the NACP asks for a user at every start): test runs use the local test user "claude"
# (no Nintendo Account, created 2026-10-25), the last of the five users, so the user's own profiles and saves stay
# untouched. The cursor starts on the first user; claude is the 5th (DRIGHT x4), a 6th press would reach the "+" (add user) tile. SW_USER_RIGHTS overrides the count.
sw_pick_user() {
  local S=$1 k
  for k in $(seq ${SW_USER_RIGHTS:-4}); do B click DRIGHT; done
  sleep 1; B shot "$S/0_user.jpg" >/dev/null
  B click A; sleep 6
}
sw_launch() {
  local S=$1 t
  if [ "$SW_T" = mtp ]; then
    B click HOME; sleep 2; B click X; sleep 2; B click A; sleep 3   # close DBI; game = second tile
    B click DRIGHT; sleep 1
  else
    case "$SW_CLOSED" in
      none|$GAME_TID) ;;                 # the game is the first tile (last app run), the cursor on it
      *) B click DRIGHT; sleep 1 ;;      # another app was just closed: it is first, the game second
    esac
  fi
  B shot "$S/0_tile.jpg" >/dev/null
  B click A; sleep 6
  t=$(B title); [ "$t" = "$GAME_TID" ] || { sw_pick_user "$S"; t=$(B title); }   # user selection, if asked
  if [ "$SW_T" = ftp ] && [ "$t" = "$HB_TID" ]; then
    # nothing ran and hbmenu was the first tile (e.g. after a reboot from Quick Reboot): close it, game is second
    echo "hbmenu started instead of the game: closing it, taking the second tile"
    sw_home_close "$S"; B click DRIGHT; sleep 1; B shot "$S/0_tile2.jpg" >/dev/null
    B click A; sleep 6
    t=$(B title); [ "$t" = "$GAME_TID" ] || { sw_pick_user "$S"; t=$(B title); }
  fi
  [ "$t" = "$GAME_TID" ] || { echo "game did not start (title $t)"; B shot "$S/fail.jpg" >/dev/null; return 1; }
}
sw_close() {
  local S=$1
  sw_home_close "$S"
  [ "$SW_T" = ftp ] && { SW_CLOSED=$GAME_TID; return 0; }
  # MTP: hbmenu (second tile), DBI, "Run MTP responder"
  B click DRIGHT; sleep 1; B click A; sleep 6; B click A; sleep 6
  B shot "$S/1_hb.jpg" >/dev/null
  B click DDOWN DRIGHT DRIGHT DRIGHT DRIGHT DRIGHT; B click A; sleep 5; B click A; sleep 1
  B click DDOWN DDOWN DDOWN DDOWN DDOWN DDOWN DDOWN DDOWN DDOWN; B shot "$S/2_dbi.jpg" >/dev/null; B click A; sleep 5
}
sw_pull() {
  local D=$1
  mkdir -p "$D"
  F get switch/gow/log.txt "$D/log.txt" || { echo "$SW_T: log.txt not pulled"; return 1; }
  for t in GSWorker GameThread GSStage GSThread; do F get switch/gow/pcs_$t.txt "$D/pcs_$t.txt" >/dev/null 2>&1; done
  return 0
}
