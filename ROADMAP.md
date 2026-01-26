# RFUSE Small I/O Payload Slot Roadmap

이 문서는 `Context.md`의 설계 의도를 기반으로, **우선순위에 따른 구체적인 수정 로드맵**을 정리한다.

## 목표
- small I/O(≤16KB)에서 `/dev/fuse` 기반 복사·전환 비용을 줄인다.
- per-CPU ring channel에 **16KB × 32 payload slot**을 도입한다.
- copy 책임을 daemon → kernel thread/kworker로 이동한다.

---

## 1) WRITE 경로 (최우선)

### 1-1. 커널: page → slot 복사 경로 추가
- **파일**: `driver/rfuse/rfuse_file.c`
- **작업**:
  - ≤16KB write 요청에 대해 slot 할당 및 page→slot copy 수행
  - 요청 메타데이터에 slot index/len 기록

### 1-2. 유저스페이스: slot 기반 write 처리
- **파일**: `lib/librfuse/lib/rfuse_lowlevel.c`
- **작업**:
  - write 요청 수신 시 slot 메타데이터 확인
  - slot → backing FS write 수행
  - 완료 응답 시 slot 반납 신호 전송

### 1-3. 프로토콜 정의
- **파일**: `driver/rfuse/rfuse_dev.c`, `lib/librfuse/lib/rfuse_lowlevel.c`
- **작업**:
  - slot index/len 전달 형식 및 요청/응답 플래그 정의

---

## 2) READ 경로

### 2-0. 설계 가이드라인 (강화)
- **목표**: daemon이 `/dev/fuse` read/write로 데이터를 복사하지 않고, slot에 데이터를 채운 뒤 완료 통지만 수행하게 한다.
- **핵심 원칙**:
  - **slot 할당은 커널이 요청 생성 시점에 먼저 수행**하고, slot 정보(slot_idx/len)를 요청에 포함한다.
  - **daemon은 slot을 데이터 버퍼로 사용**해 backing FS read를 수행하고, 커널에 **READY + len + slot_idx**만 통지한다.
  - **커널은 응답 수신 후 slot → page(target buffer) 복사**를 수행하고 slot을 반납한다.
  - **/dev/fuse read/write 시스템 호출 경로는 small I/O(≤16KB)에서 사용하지 않는다.**
- **가드 조건**:
  - slot 할당 실패 시 즉시 fallback 경로(iovec 기반)로 전환한다.
  - READ 완료 전에 slot이 반납되지 않도록 **요청 종료 지점에서만 반납**한다.
  - slot 길이는 **최대 16KB**로 제한하며, 초과 요청은 기존 경로 유지.

### 2-1. 유저스페이스: slot READY 통지
- **파일**: `lib/librfuse/lib/rfuse_lowlevel.c`
- **작업**:
  - backing FS read 완료 후 slot 채움
  - READY + len + slot_idx를 커널에 통지

### 2-2. 커널: slot → page 복사
- **파일**: `driver/rfuse/rfuse_file.c`
- **작업**:
  - READ 완료 처리 시 slot→page copy
  - copy 완료 후 slot 반납

### 2-3. 기존 경로 우회 조건
- **파일**: `driver/rfuse/rfuse_dev.c` 또는 `driver/rfuse/dev.c`
- **작업**:
  - ≤16KB에서 slot 경로 사용, 그 외 기존 `/dev/fuse` 경로 유지

---

## 3) Slot exhaustion / ring full 대처

### 3-0. 설계 가이드라인 (fallback 정책)
- **상황 정의**: slot 할당 실패 시 즉시 대응이 필요하며, small I/O 경로의 syscall 회피 목표와 충돌하지 않도록 정책을 구분한다.
- **대안 A: waitq로 제출 thread를 block**
  - **장점**: slot 경로를 유지하여 `/dev/fuse` read/write syscall 회피 목표와 일관됨.
  - **단점**: 제출 thread 지연으로 p99 latency 상승 가능, 교착/우선순위 역전 위험 관리 필요.
  - **적합 조건**: latency 허용 폭이 있고, slot 회수 속도가 빠른 워크로드.
- **대안 B: 기존 경로(iovec/pread/pwrite)로 즉시 fallback**
  - **장점**: 요청 지연 없이 진행 가능, 서비스 레이턴시 안정화에 유리.
  - **단점**: small I/O에서 syscall/복사 오버헤드가 다시 발생.
  - **적합 조건**: tail latency를 최우선으로 보장해야 하는 워크로드.
- **권장 결론**:
  - **기본값은 B(즉시 fallback)** 으로 설정하여 안정적 지연시간을 확보하고,
  - 옵션 또는 튜너블로 **A(blocking) 정책을 선택** 가능하게 제공하는 것이 현실적이다.
  - 측정 지표(p99 latency, IOPS, daemon CPU)를 기준으로 정책을 선택한다.

### 3-1. slot 부족 시 fallback
- **파일**: `driver/rfuse/rfuse_dev.c`
- **작업**:
  - slot 할당 실패 시 기존 경로로 fallback
  - slot 사용률/실패 카운터 기록

### 3-2. 유저스페이스 대응
- **파일**: `lib/librfuse/lib/rfuse_lowlevel.c`
- **작업**:
  - slot unavailable 신호 수신 시 iovec 기반 경로로 재시도

---

## 4) Async/Background 처리

### 4-1. workqueue 기반 copy 위임
- **파일**: `driver/rfuse/rfuse_comp.c`, `driver/rfuse/rfuse_dev.c`
- **작업**:
  - 요청 문맥에서 즉시 copy 불가 시 work item으로 위임

### 4-2. completion 연계
- **파일**: `driver/rfuse/rfuse_comp.c`
- **작업**:
  - workqueue 완료 후 기존 completion 경로와 연결

---

## 단계별 산출물 권장 순서
1. WRITE 경로 구현 PR
2. READ 경로 구현 PR
3. slot exhaustion 대응 PR
4. async/background 대응 PR
