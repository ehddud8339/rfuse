# RFUSE iqueue and request structures

이 문서는 `driver/rfuse/rfuse.h`의 `struct rfuse_iqueue`와 `struct rfuse_req`의 현재 목적과 필드 의미를 정리한다.
이후 두 구조체가 수정될 때는 아래 "변경 기록"에 어떤 수정이 왜 들어갔고, 목적이 무엇인지 함께 기록한다.

## 개요

`rfuse_req`는 RFUSE 요청 1개의 메타데이터, 입출력 인자 위치, 상태 플래그, 완료 대기/콜백 정보를 담는 요청 슬롯이다.
커널은 `rfuse_iqueue.kreq[]`에서 요청 슬롯을 할당하고, 요청 인덱스를 pending queue에 넣어 userspace FUSE daemon에 전달한다.

`rfuse_iqueue`는 커널과 userspace FUSE daemon 사이의 공유 메모리 기반 요청 큐 단위이다.
pending, interrupt, forget, complete ring buffer와 request/argument buffer pool을 함께 관리하며, daemon에 mmap으로 노출되는 커널/유저 주소 쌍을 가진다.

요청 흐름의 핵심은 다음과 같다.

1. 커널이 `rfuse_iqueue.kreq[]`에서 `rfuse_req` 슬롯을 할당한다.
2. 요청별 입력/출력 인자는 필요하면 `rfuse_iqueue.karg[]` argument buffer에 배치하고, `rfuse_req.in.arg[]` 또는 `rfuse_req.out.arg`에 인덱스를 저장한다.
3. 커널이 pending queue에 request index를 넣는다.
4. userspace daemon이 mmap된 queue/request/argument 영역을 읽고 요청을 처리한다.
5. daemon이 complete queue에 request index를 넣으면 커널이 해당 `rfuse_req`를 완료 처리한다.

## `struct rfuse_req`

| 필드 | 목적 |
|---|---|
| `in.unique` | 요청 고유 ID. pending queue에 넣을 때 `rfuse_get_unique()`로 할당된다. |
| `in.nodeid` | 대상 FUSE inode/node ID. |
| `in.opcode` | FUSE operation 코드. 예: `FUSE_LOOKUP`, `FUSE_READ`, `FUSE_WRITE`. |
| `in.uid`, `in.gid`, `in.pid` | 요청을 발생시킨 task의 credential/context. |
| `in.arg[2]` | 입력 operation-specific argument가 저장된 `karg/uarg` buffer index. |
| `in.arglen[2]` | 각 입력 argument의 크기. 해제 시 0이 아니면 argument buffer를 반환한다. |
| `out.error` | userspace daemon이 반환한 결과 코드. |
| `out.arg` | 출력 argument가 저장될 argument buffer index. |
| `out.arglen` | 출력 argument 크기. variable output이면 실제 반환 길이로도 사용된다. |
| `out.padding` | 정렬/패딩용 필드. |
| `index` | 이 요청이 `riq->kreq[]/ureq[]` request buffer pool에서 차지하는 index. |
| `riq_id` | 이 요청이 속한 `rfuse_iqueue` ID. 완료/해제 시 해당 queue를 다시 찾는 키이다. |
| `flags` | `FR_PENDING`, `FR_SENT`, `FR_FINISHED`, `FR_BACKGROUND` 등 요청 상태 비트. |
| `fm` | 요청이 속한 `struct fuse_mount`. connection 접근과 buffer 반환에 사용된다. |
| `count` | 요청 refcount. 0이 되면 request/argument buffer를 반환한다. |
| `waitq` | 동기 요청이 완료될 때까지 sleep/wakeup 하는 wait queue. |
| `args.argument_space[112]` | 작은 operation-specific argument를 구조체 내부에 직접 담는 공간. |
| `force` | interrupt 상태여도 강제로 전송해야 하는 요청 성격. |
| `noreply` | reply가 필요 없는 요청 표시. |
| `nocreds` | credential 강제 설정을 생략해야 하는 요청 표시. |
| `in_pages` | 입력 데이터가 page 기반 I/O buffer에 있음을 표시. |
| `out_pages` | 출력 데이터가 page 기반 I/O buffer로 받아져야 함을 표시. |
| `out_argvar` | 출력 argument 길이가 가변임을 표시. 성공 시 `out.arglen`이 반환값이 될 수 있다. |
| `page_zeroing` | 출력 page의 남는 영역을 zeroing해야 함을 표시. |
| `page_replace` | page를 복사 대신 교체하는 방식으로 처리할 수 있음을 표시. |
| `may_block` | I/O 처리 중 blocking 가능성이 있음을 표시. |
| `rp` | page 기반 read/write에 쓰이는 `struct rfuse_pages` 포인터. |
| `end` | 비동기/background 요청 완료 시 호출되는 completion callback. |

## `struct rfuse_iqueue`

| 필드 | 목적 |
|---|---|
| `riq_id` | queue 식별자. 현재 `RFUSE_NUM_IQUEUE`만큼 생성된다. |
| `pending` | 커널이 daemon에 보낼 일반 요청 index를 담는 ring buffer. |
| `interrupts` | 이미 전송된 요청을 interrupt하기 위한 interrupt entry ring buffer. |
| `forgets` | FUSE forget 요청을 전달하는 ring buffer. |
| `completes` | daemon이 완료한 요청 index를 커널에 돌려주는 ring buffer. |
| `uarg` | userspace에 mmap된 argument buffer 주소. |
| `karg` | 커널에서 접근하는 argument buffer 주소. |
| `ureq` | userspace에 mmap된 request buffer 주소. |
| `kreq` | 커널에서 접근하는 request buffer pool. `rfuse_req` 배열이다. |
| `connected` | 이 queue/connection이 사용 가능한 상태인지 표시. |
| `waitq` | request/argument buffer pool이 full일 때 대기하는 wait queue. |
| `lock` | ring buffer, bitmap 등 queue 내부 공유 상태 보호용 spinlock. |
| `reqctr` | 다음 요청 unique ID 생성을 위한 counter. |
| `priv` | device-specific private state. 초기화 시 전달된 `priv`를 저장한다. |
| `argbm.bitmap_size` | argument buffer pool 크기. |
| `argbm.full` | argument buffer pool이 full인지 표시. |
| `argbm.bitmap` | 사용 중인 argument buffer index를 추적하는 bitmap. |
| `reqbm.bitmap_size` | request buffer pool 크기. |
| `reqbm.full` | request buffer pool이 full인지 표시. |
| `reqbm.bitmap` | 사용 중인 `kreq[]/ureq[]` request index를 추적하는 bitmap. |
| `idle_user_waitq` | pending 요청이 들어왔을 때 idle daemon/user thread를 깨우는 wait queue. |
| `num_sync_sleeping` | 동기 요청 congestion/queue 선택 회피에 쓰이는 sleeping count. |
| `bg_queue` | background 요청을 pending queue로 보내기 전 임시 보관하는 kernel list. |
| `bg_lock` | background queue와 background accounting 보호용 spinlock. |
| `max_background` | 동시에 활성화할 수 있는 background 요청 상한. |
| `congestion_threshold` | background 요청이 이 값에 도달하면 backing device congestion 표시. |
| `num_background` | 현재 queue에 계상된 전체 background 요청 수. |
| `active_background` | pending queue로 실제 제출되어 처리 중인 background 요청 수. |
| `blocked` | background congestion으로 새 요청이 막힌 상태 표시. |
| `blocked_waitq` | background congestion 해소를 기다리는 요청들이 대기하는 wait queue. |

## 변경 기록

이 섹션에는 `rfuse_iqueue` 또는 `rfuse_req`의 구조, 필드 의미, lifecycle, queueing/completion 흐름이 바뀔 때마다 기록한다.

기록 형식:

| 날짜 | 변경 대상 | 변경 내용 | 목적/이유 |
|---|---|---|---|
| 2026-05-29 | `rfuse_iqueue`, `rfuse_req` | 현재 코드 기준 구조체 목적과 필드 의미를 최초 문서화. | 이후 구조체 변경의 의도와 배경을 추적하기 위한 기준 문서 마련. |
