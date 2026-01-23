# FUSE 연구 프로젝트 목적 정리

---

## 1. 프로젝트의 최상위 목적 (What)

**RFUSE에서 16KB 이하 small I/O의 지배적 오버헤드(`/dev/fuse` read/write 진입 + user↔kernel 전환 + daemon wakeup/스케줄링)를 낮추기 위해**, per-CPU ring channel에 **고정 크기(16KB) payload slot pool(고정 32개)**을 추가하고, **데이터 복사(copy)를 daemon이 아니라 kernel thread/kworker가 수행하도록 데이터 경로를 재설계**하는 것이 프로젝트의 핵심 목적이다.

---

## 2. 목적이 필요한 이유 (Why: 문제 정의)

1. RFUSE는 **제어 경로(control path)**에서 shared memory ring channel로 이미 상당한 개선(복사/컨텍스트 스위치 감소)을 달성했다.
2. 그러나 **데이터 경로(data path)**(read/write payload)는 여전히 `/dev/fuse` 기반 read()/write()를 통해 daemon이 커널 모드로 들어가 복사를 수행한다.
3. 특히 **small I/O(≤16KB)**에서는 memcpy 자체보다 **syscall 진입 및 user↔kernel 전환**이 더 지배적이므로, 데이터 복사 “자체”를 없애기보다 **복사를 수행하는 주체/문맥을 바꿔 오버헤드를 줄이는 것**이 목표가 된다.

---

## 3. 구체 목표 (Goals: 무엇을 달성해야 ‘성공’인가)

### 3.1 기능 목표

1. **≤16KB read/write**에서 `/dev/fuse` 기반 데이터 복사 경로를 **우회 또는 최소화**한다.
2. per-CPU ring channel에 **16KB×32 payload slot pool**을 추가하고, 요청 단위로 slot을 **할당/반납**한다.
3. **copy 작업을 daemon이 아니라 kernel thread 또는 kworker가 수행**하도록 책임을 이동한다(daemon은 IO 수행/응답 생성에 집중).

### 3.2 성능 목표(측정 가능한 형태)

동일 워크로드(예: fio 4K~16K random read/write 등)에서 다음을 개선한다.

- **latency(p50/p99) 감소**
- **throughput(IOPS/BW) 증가**
- **daemon CPU usage 감소(특히 kernel time)**
- **컨텍스트 스위치 및 스케줄링 오버헤드 감소(정량화)**

---

## 4. 핵심 설계 의도 (How: 설계 철학)

이 프로젝트는 “zero-copy를 당장 달성”하는 것이 아니라, **small I/O에서 비싼 경로를 치우는 구조적 최적화**에 초점이 있다.

### 4.1 READ 경로의 의도

- daemon이 backing FS I/O를 수행하고 **slot에 데이터를 채운 뒤**, 커널에 “READY + len + slot_idx”를 통지
- 커널이 **slot → page(또는 target buffer)**로 복사한 뒤 완료 처리
- 목적: daemon이 `/dev/fuse`로 커널 진입하며 복사하는 비용을 줄이고, daemon이 다음 요청 처리를 더 빨리 진행하게 함

### 4.2 WRITE 경로의 의도

- 커널이 **page → slot**로 먼저 복사해두고 요청을 발행
- daemon은 slot에서 읽어 backing FS에 write 수행
- 목적: daemon이 `/dev/fuse` 경로로 커널 전환하며 복사하는 비용을 제거

### 4.3 Async/Background 고려

- 요청 문맥에서 즉시 copy가 불가능/부적절한 경우(이미 user mode로 복귀 등)에는 **kworker(workqueue)로 위임**하는 방향이 포함된다.

---

## 5. 범위의 경계 (Non-goals: 의도적으로 하지 않는 것)

- **16KB 초과 I/O에 대한 대규모 zero-copy 리워크**는 범위 밖
- payload slot 기반으로 **완전한 zero-copy를 즉시 달성**하는 것이 1차 목표는 아님(우선은 “daemon 복사 제거 + 커널 측 복사”가 목표)
- ring full/slot exhaustion 같은 극단 상황의 “완벽한” 무손실 처리 정책은 추후 결정(현재는 정책 후보만 존재)

---

## 6. 한 문장 결론

이 프로젝트의 목적은 **RFUSE의 data path에서 small I/O(≤16KB)의 성능을 갉아먹는 `/dev/fuse` 기반 복사·전환 오버헤드를 제거하기 위해, per-CPU shared-memory payload slot(16KB×32)을 도입하고 copy 책임을 커널로 이동시키는 것**이다.

# RFUSE/LibRFUSE 로컬 FS 동작 파일 정리 (DAX, virtio-fs 제외)

요청 조건:
- DAX 관련 파일 제외
- virtio-fs 관련 파일 제외
- `driver/rfuse/`, `lib/librfuse/` 하위 파일의 목적을 list-up

## 제외 대상
- `driver/rfuse/dax.c` — DAX 관련 구현.
- `driver/rfuse/virtio_fs.c` — virtio-fs 구현.

## driver/rfuse/ (커널 드라이버)

| 파일 | 목적 |
| --- | --- |
| `driver/rfuse/acl.c` | POSIX ACL 조회/설정 처리. |
| `driver/rfuse/control.c` | FUSE control filesystem(ctl fs) 및 연결 관리. |
| `driver/rfuse/cuse.c` | CUSE(캐릭터 디바이스) 사용자 공간 구현 지원. |
| `driver/rfuse/dev.c` | `/dev/fuse` 요청/응답 큐 및 요청 구조체 관리. |
| `driver/rfuse/dir.c` | dentry 캐시/lookup/entry timeout 등 디렉터리 처리. |
| `driver/rfuse/file.c` | 파일 핸들 구조 생성, open/close 관련 처리. |
| `driver/rfuse/inode.c` | inode 및 슈퍼블록 메타데이터 처리. |
| `driver/rfuse/ioctl.c` | ioctl iovec 검증/복사 및 ABI 호환 처리. |
| `driver/rfuse/readdir.c` | readdir 캐시 적재/검증 등 디렉터리 읽기 처리. |
| `driver/rfuse/xattr.c` | xattr set/get/list/remove 처리(일부 함수는 stub 형태). |
| `driver/rfuse/rfuse_comp.c` | rfuse 완료(completion) 대기/폴링 및 sleep 처리. |
| `driver/rfuse/rfuse_dev.c` | rfuse 전용 요청 큐/타임스탬프/스케줄링 기반 device 처리. |
| `driver/rfuse/rfuse_dir.c` | rfuse 디렉터리 처리 확장(기존 FUSE 기능 복제/확장). |
| `driver/rfuse/rfuse_file.c` | rfuse 파일 I/O 및 동기화 처리 확장. |
| `driver/rfuse/rfuse_inode.c` | rfuse inode/초기화 관련 처리 확장. |

## lib/librfuse/lib/ (userspace 라이브러리)

| 파일 | 목적 |
| --- | --- |
| `lib/librfuse/lib/buffer.c` | `fuse_buf`/`fuse_bufvec` 버퍼 유틸리티. |
| `lib/librfuse/lib/cuse_lowlevel.c` | CUSE low-level API 처리. |
| `lib/librfuse/lib/fuse.c` | high-level FUSE API 구현(저수준 API 위). |
| `lib/librfuse/lib/fuse_i.h` | 내부 공통 구조체/요청 정의 헤더. |
| `lib/librfuse/lib/fuse_log.c` | 로깅 API 제공 및 기본 로거 구현. |
| `lib/librfuse/lib/fuse_loop.c` | 단일 스레드 세션 루프 구현. |
| `lib/librfuse/lib/fuse_loop_mt.c` | 멀티 스레드 세션 루프 구현. |
| `lib/librfuse/lib/fuse_lowlevel.c` | low-level FUSE API 구현(커널 프로토콜 처리). |
| `lib/librfuse/lib/fuse_misc.h` | 플랫폼별 매크로/심볼 버전 유틸 헤더. |
| `lib/librfuse/lib/fuse_opt.c` | 옵션 파싱(`struct fuse_args`) 구현. |
| `lib/librfuse/lib/fuse_signals.c` | 시그널 핸들링 유틸리티 제공. |
| `lib/librfuse/lib/fuse_versionscript` | 공개 심볼 버전 스크립트 정의. |
| `lib/librfuse/lib/helper.c` | FUSE 프로그램 작성 보조 유틸리티. |
| `lib/librfuse/lib/mount.c` | Linux용 FUSE 마운트 로직/옵션 파싱. |
| `lib/librfuse/lib/mount_bsd.c` | FreeBSD용 FUSE 마운트 로직/옵션 파싱. |
| `lib/librfuse/lib/mount_util.c` | 아키텍처 독립 마운트 유틸리티. |
| `lib/librfuse/lib/mount_util.h` | 마운트 유틸 함수 선언 헤더. |
| `lib/librfuse/lib/rfuse_loop.c` | rfuse 전용 유저스페이스 데몬 루프. |
| `lib/librfuse/lib/rfuse_loop_mt.c` | rfuse 전용 멀티 스레드 루프. |
| `lib/librfuse/lib/rfuse_lowlevel.c` | rfuse 전용 low-level 처리(링버퍼/큐 처리). |
| `lib/librfuse/lib/modules/iconv.c` | 파일명 문자셋 변환 모듈(iconv). |
| `lib/librfuse/lib/modules/subdir.c` | 경로를 특정 서브디렉토리 기준으로 재매핑 모듈. |
| `lib/librfuse/lib/meson.build` | libfuse 빌드 대상 소스/링크 설정. |


