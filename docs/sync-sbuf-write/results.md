# 적용 및 검증 결과

## 후속 작업: async sbuf 필드 제거

사용자의 후속 요청으로 아래 최초 패치에서 보존했던 `rfuse.h` 필드를 제거했다.

- `driver/rfuse/rfuse.h`의 `wrt_ctx`, `has_wrt_ctx` 및 전용 구조체
  `rfuse_async_write_range`, `rfuse_async_wrt_ctx`를 제거했다.
- 공유 요청 배열의 stride를 일치시키기 위해 `lib/librfuse/include/rfuse.h`에도
  같은 변경을 적용했다. `rfuse_req`는 368바이트에서 280바이트로 줄었고,
  `end`의 offset은 360에서 272로 바뀌었다.
- 양쪽 헤더의 크기/offset static assertion으로 레이아웃을 검증했다.
- 커널 모듈 빌드와 librfuse 빌드가 통과했다. 기존 라이브러리 build 디렉터리는
  다른 소유자의 파일로 쓰기 불가능하여 `/tmp/rfuse-sbuf-fields-build`에서
  Meson/Ninja로 새로 빌드했다. C++11 헤더 컴파일도 통과했다.
- 빌드 로그: `/tmp/rfuse-sbuf-fields-kernel-build.log`,
  `/tmp/rfuse-sbuf-fields-library-build.log`.
- 제거한 필드/구조체의 C/H 소스 참조가 없고 `git diff --check`도 통과했다.

공유 ABI가 변경되므로 적용 시 커널 모듈과 librfuse를 함께 교체하고 daemon을
재시작해야 한다. 이 작업에서는 시스템 설치/모듈 교체 및 런타임 I/O 검증은
수행하지 않았다.

## 적용 결과

- 계획: [plan.md](plan.md)
- `rfuse_perform_write_sbuf()`의 async 분기와 상태 변수를 제거하고 기존 sync
  chunk 반복문을 유지했다. 크기 갱신 후 `FUSE_I_SIZE_UNSTABLE`을 해제한다.
- Async sbuf 전용 제출/완료/context 함수 및 interval tree 추적과 대기 코드를
  제거했다. 대기만 감싸던 조건문도 함께 제거했다.
- `fuse_i.h`의 async sbuf 전용 inode 필드 세 개와 초기화를 제거했다.
- `rfuse_write_range_end()`로 이름을 변경하고 sbuf 조건부 컴파일 영역으로
  옮겼다. Overflow 방어는 그대로 유지했다.
- Nowrite release 함수에서 `_async` 접미사를 제거하고 선언/호출/주석을 수정했다.
- `rfuse.h`는 변경하지 않았다. 후속 제거 대상 필드와 관련 구조체 정의는 유지한다.
- 공통 background 제출/완료, read/writeback의 요청 할당, 별도 async direct I/O를
  유지했다.

## 실행한 검증

| 항목 | 결과 |
| --- | --- |
| 시작 시 working tree 확인 | 기존 변경 없음 |
| `git diff --check` | 통과 |
| 제거 대상 실행 심볼 검색 | `rfuse.h`의 유지 대상 정의 외 참조 없음 |
| `rfuse.h` 및 `rfuse_dev.c`, `dev.c`, `rfuse_comp.c`와 HEAD 비교 | 모두 동일 |
| 기존 sync chunk loop와 비교 | 들여쓰기 외 동일 |
| `rfuse_send_write_sync()`와 HEAD 비교 | 동일 |
| Write flags, cache 선반영 및 미수락 범위 invalidate helper와 HEAD 비교 | 동일; range helper 이름만 변경 |
| Range-end overflow 방어와 HEAD 비교 | 이름 외 동일 |
| Nowrite release 함수 본체와 HEAD 비교 | 이름 외 동일 |
| Direct I/O `rfuse_send_write()`, `rfuse_async_req_send()`와 HEAD 비교 | 동일 |
| `cd driver/rfuse && make` | 종료 코드 0, `fuse.ko` 생성 |
| 마지막 소스 정리 후 `make -C driver/rfuse` | 종료 코드 0 |

커널은 `5.15.0`, 빌드 트리는 `/lib/modules/5.15.0/build`가 가리키는
`/usr/src/linux-5.15.0`이다. 마지막 빌드 로그는
`/tmp/rfuse-sync-sbuf-build.log`에 저장했다.

첫 빌드에서 수정하지 않은 `rfuse_dev.c`의 기존 코드에 대해 세 경고가 나왔다.

- `select_numa_aware_v2()`: 선언과 실행문 혼용, 사용하지 않는 변수 `i`.
- `select_numa_aware()`: 사용하지 않는 함수.

수정한 소스에서는 빌드 경고가 나오지 않았다. 빌드 산출물은 로컬에 유지하며
소스 패치 또는 커밋 대상으로 포함하지 않는다.

## 실행하지 못한 런타임 검증

현재 환경에는 `/dev/fuse`가 없고 RFUSE 테스트 마운트도 없다. 모듈을 설치하거나
로드하지 않았으며 다음 항목은 실제 RFUSE I/O로 검증하지 못했다.

- 작은 write, max_write를 넘는 write, overwrite 및 파일 확장 후 데이터 확인.
- Read/mmap cache 일관성, truncate, O_APPEND, O_SYNC/O_DSYNC 및 fsync.
- 응답 오류/short write 주입 시 반환값, iterator 복원 및 cache invalidate.

위 항목은 RFUSE를 로드하고 마운트할 수 있는 호스트에서 수행해야 한다.
오류/short write는 응답을 제어할 수 있는 daemon이 추가로 필요하다.
빌드 및 소스 비교 결과는 이 런타임 검증을 대신하지 않는다.
