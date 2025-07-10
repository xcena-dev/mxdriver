#!/bin/bash

SYNC_DIR="."

IGNORE_FILE="$SYNC_DIR/.ignore_syn_push"

if [ -f "$IGNORE_FILE" ]; then
  echo ".ignore_syn_push 파일을 찾았습니다. 삭제할 파일/폴더 목록을 처리합니다."
  while IFS= read -r line || [ -n "$line" ]; do
    target="$SYNC_DIR/$line"
    if [ -e "$target" ]; then
      echo "삭제 중: $target"
      rm -rf "$target"
    else
      echo "존재하지 않음: $target"
    fi
  done <"$IGNORE_FILE"
  echo ".ignore_syn_push 파일 삭제"
  rm -f "$IGNORE_FILE"
else
  echo ".ignore_syn_push 파일이 없습니다."
fi
