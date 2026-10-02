#!/bin/zsh
# 逐页对照全部页面稿（组 A–D），每页一行摘要；详情在 output/macos27-r2/compare/<tag>/。用法：zsh tests/macos27-r2/compare-all.sh [标签前缀，默认 all]
cd "$(dirname "$0")/../.." || exit 1
P=${1:-all}
PROF=tests/macos27-r2/profile-board.json
run() { # tag board route [extra args...]
  local tag="$P-$1" board="$2" route="$3"; shift 3
  node tests/macos27-r2/compare.cjs --board "$board" --route "$route" --tag "$tag" "$@" 2>&1 | sed -n '3,6p' | sed "s/^/[$tag] /"
}
run sys-main   Main /          --profile $PROF
run net        Net  /network
run files      Files /files    --pre "document.querySelectorAll('.file-checkbox')[1].click()"
run term       Term /terminal
run auto       Auto /automation
run cmds       Cmds /commands  --profile $PROF --pre "selectHost('fixture-host');document.getElementById('exec-result-section').style.display='';document.getElementById('nohup-actions').style.display='flex';document.getElementById('cancel-exec-btn').style.display='';document.getElementById('nohup-stop-tail').style.display='';document.getElementById('exec-result').textContent='\$ sudo systemctl restart nginx\n ';"
run ota        Ota  /ota       --pre "clearInterval(refreshInterval);document.getElementById('ota-manual').setAttribute('open','');document.getElementById('ota-progress-section').style.display='block';document.getElementById('ota-progress-bar').style.width='35%';document.getElementById('ota-progress-percent').textContent='35%'"
run sec        Sec  /security
