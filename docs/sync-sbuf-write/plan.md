# Async sbuf write 제거 계획

## 목표와 범위

`driver/rfuse`의 `rfuse_perform_write_sbuf()`에서 async sbuf write 경로를
제거하고 기존 sync sbuf write 경로만 남긴다. 작업 시작 시 `use_async = false`로
고정되어 있으므로 기존 sync 동작을 보존하는 정리 작업이다.

`rfuse.h`는 변경하지 않는다. `rfuse_async_write_range`, `rfuse_async_wrt_ctx`,
`rfuse_req.wrt_ctx`, `rfuse_req.has_wrt_ctx`는 후속 패치에서 제거한다.
Direct I/O의 별도 async 경로, read, writeback, 공통 background 요청 처리는 유지한다.

## 1. Sync 전용 진입점

- `rfuse_perform_write_sbuf()`의 `use_async`, `async_used`, async 분기를 제거한다.
- 기존 sync 분기의 `fc->max_write` 단위 반복문과 오류/부분 성공 반환을 유지한다.
- 크기 갱신 후 `FUSE_I_SIZE_UNSTABLE`을 해제하고 속성을 무효화한다.
- 주석을 sync 제출 동작에 맞춘다.

```text
rfuse_perform_write()
└─ rfuse_perform_write_sbuf()
   ├─ rfuse_get_req(..., for_background=false, ...)
   └─ rfuse_send_write_sync()
      ├─ rfuse_reserve_sbuf()
      ├─ rfuse_sbuf_copy_from_iter()
      ├─ rfuse_apply_to_page()
      ├─ rfuse_simple_request()  [응답 대기]
      ├─ 응답 크기 검증 및 실패/short write 범위 invalidate
      └─ rfuse_put_request()
```

## 2. Async 전용 제출/완료 코드 제거

`rfuse_file.c`에서 다음 함수를 제거한다.

- `rfuse_async_allowed()`
- `rfuse_send_write_async()`
- `rfuse_sbuf_write_complete_req()`
- `rfuse_write_complete_status()`
- `rfuse_async_wrt_ctx_init()`
- `rfuse_async_wrt_ctx_reset()`
- `rfuse_async_wrt_unregister()`

Sync 경로의 `rfuse_write_flags()`, `rfuse_apply_to_page()`,
`rfuse_invalidate_preapplied_cache()`, sbuf 할당/복사/반환, iterator 복원과
오류 및 short write 처리 의미는 보존한다.

## 3. 추적과 대기 코드 제거

- `rfuse_async_writes_inflight()`, `rfuse_wait_async_writes()`를 제거한다.
- `rfuse_wait_async_write()`와 범위 overlap/대기 helper를 제거한다.
- 전용 interval tree 생성 코드, 매크로, include를 제거한다.
- `file.c`, `rfuse_file.c`, `rfuse_dir.c`, `ioctl.c`, `xattr.c`에서 해당 대기 호출을
  제거한다. 단독 호출을 감싸는 조건문도 함께 제거한다.
- `fuse_i.h`의 대기 함수 선언과 전용 inode 필드 `async_writectr`,
  `async_range_wait_count`, `async_write_ranges`, `file.c`의 초기화를 제거한다.
- Sync cache invalidate에서 사용하는 `rfuse_async_range_end()`는
  `rfuse_write_range_end()`로 이름을 바꾸고 overflow 방어를 보존한다.

## 4. 공유 기능 및 구조체 경계 보존

- `rfuse.h`의 필드/구조체 정의 및 레이아웃을 유지한다.
- Read/writeback이 사용하는 `try_rfuse_get_req()` 및 async 큐 선택을 유지한다.
- Background queue, `FR_ASYNC`, `RFUSE_REPLY_ASYNC`, 공통 완료 처리를 유지한다.
- Truncate 등에서 `fuse_set_nowrite()`와 짝을 이루는
  `rfuse_release_nowrite_async()`와 `__rfuse_release_nowrite_async()`는
  `rfuse_release_nowrite()`와 `__rfuse_release_nowrite()`로 이름을 바꾼다.
  선언, 호출부, 주석을 수정하고 lock 및 writectr 처리 의미를 보존한다.

## 5. 검증

1. 제거 대상 참조가 남지 않았는지 확인한다. `rfuse.h`가 변경되지 않았는지,
   sync 본체와 공통 요청 처리가 보존되었는지 diff를 검토한다.
2. `cd driver/rfuse && make`로 실행 중인 커널에 대한 모듈을 빌드한다.
3. RFUSE 마운트가 가능한 테스트 환경에서는 작은 write, max_write를 넘는 write,
   overwrite, 파일 확장 후 데이터를 확인한다.
4. 같은 환경에서 read/mmap cache 일관성, truncate, O_APPEND, O_SYNC/O_DSYNC,
   fsync를 확인한다.
5. 응답 제어가 가능한 환경에서는 오류/short write 응답의 반환값, iterator 복원,
   cache invalidate를 확인한다.

런타임 검증 환경이 없으면 실행한 검증과 미실행 항목 및 이유를 결과 문서에
명시한다. 모듈 빌드 결과를 런타임 검증 성공으로 간주하지 않는다.

## 적용 순서

1. Sync 전용화 및 async callback 제거.
2. 추적/대기 코드와 명칭 정리.
3. 빌드 및 diff 검증 후 같은 디렉터리에 결과 기록.
