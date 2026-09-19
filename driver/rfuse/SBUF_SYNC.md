# sbuf 동기 쓰기의 캐시 공개 순서

변경 이유: 캐시 선반영과 비동기 범위 대기를 제거하고, 서버 응답 전에는
쓰기 대상 캐시 페이지를 읽을 수 없게 한다. 페이지마다 요청을 나누지 않고
기존 `max_write` 단위의 sbuf WRITE를 유지한다.

## 적용 범위

`LDY_NO_PAGE_CACHE=1`의 buffered sbuf WRITE 경로를 동기 전용으로 만든다.
비동기 WRITE 제출 함수/완료 콜백/범위 트리/대기를 제거한다. READ readahead와
별도의 direct I/O, 페이지 writeback의 일반적인 비동기 기반 구조는 이 제거
대상이 아니다. 공유 request의 이전 쓰기 상태 영역은 예약 바이트로 바꾸어
기존 368바이트 ABI와 후속 필드 offset을 유지한다.

## 순서와 불변식

1. 사용자 데이터를 sbuf에 복사한다. 아직 invalidate/page 잠금을 잡지 않으므로
   같은 파일의 mmap 버퍼에서 읽는 사용자 fault를 스스로 차단하지 않는다.
2. `invalidate_lock`을 독점 획득한다. 기존 대상 페이지를 잠그고 원래 uptodate
   상태를 보관한 뒤 non-uptodate로 만든다. 이어 파일 기반 mmap PTE를 제거한다.
   private COW 페이지는 파일 내용과 별개인 개인 복사본이므로 제거하지 않는다.
3. 페이지 잠금을 잠시 풀고 기존 dirty/writeback 데이터를 먼저 배출한다.
   invalidate_lock은 유지한다. non-uptodate이므로 캐시 적중 읽기도 새로 진행할
   수 없고, miss/readpage/fault는 invalidate_lock의 shared 획득에서 대기한다.
   진행 중 page_mkwrite 역시 non-uptodate 페이지에 PTE를 재설치하지 않는다.
4. 페이지들을 다시 잠그고 RFUSE 자체 writeback 완료를 기다린 뒤 WRITE를
   제출한다. 서버 응답을 기다리는 동안 페이지 상태를 공개하지 않는다.
   direct READ 제출도 invalidate_lock shared로 이 구간을 기다린다.
5. 정상/short 응답이면 확인된 앞부분만 sbuf에서 복사한다. 기존에 유효했던
   페이지 또는 이번에 전체를 채운 페이지만 uptodate로 복구한다. 요청 오류나
   응답 크기 초과이면 non-uptodate로 남겨 다음 읽기에서 서버 내용을 재조회한다.
6. 페이지 잠금과 invalidate_lock을 해제한 뒤 request/sbuf를 해제한다.
   미수락 바이트만 iterator에서 되돌린다. 캐시 완료 처리에는 할당이 없다.

제출 전 배출 실패에는 이번 WRITE를 제출하지 않고 원래 캐시 유효 상태를
복구한다. 0바이트 응답은 진행 없는 반복을 방지하도록 상위 루프를 종료한다.
일부 바이트를 쓰고 뒤 chunk가 실패하면 이미 성공한 바이트 수를 반환한다.

읽기 차단은 대상 페이지를 보호하는 구간부터 서버 응답/반영 완료까지다.
그 전에 이미 시작된 읽기를 취소하거나, 여러 chunk 전체를 하나의 원자적
쓰기처럼 보이게 하는 보장은 아니다. 이미 사용자 공간으로 복사되거나 pin된
데이터와 private COW 메모리까지 회수하는 기능도 아니다.

## 확인

- `python3 tests/rfuse_sync_write/run.py`: 실제 sync 함수 소스를 추출하여
  모의 커널 primitive로 실행한다. 응답 전 기존 데이터 유지, 페이지의 읽기
  차단 상태, mmap 해제/dirty 배출 순서, 미정렬/short/error/0 응답, 자원 해제,
  부분 복사와 cache miss를 검사한다. 실제 VM 경합 테스트를 대신하지 않는다.
- 저장소 Linux 5.15.0으로 임시 O= 빌드 디렉터리를 준비하여 드라이버를 컴파일했다.
  modules_prepare만 수행한 디렉터리에는 Module.symvers가 없으므로 modpost의
  외부 심볼 검증과 로드 가능한 모듈 검증은 완료되지 않았다.
- 설치된 Ubuntu 5.15.0-191 헤더로는 기존 코드의 should_remove_suid 및
  iov_iter_fault_in_readable API 차이 때문에 빌드가 중단된다.

실제 마운트 검증에서는 WRITE 응답을 지연하는 daemon을 사용해 (a) 캐시 적중
read/pread, (b) non-uptodate 부분 페이지 읽기, (c) 이미 fault된 MAP_SHARED
읽기, (d) direct READ가 응답 전에 완료되지 않는지 확인해야 한다. 응답 후
새 값/short write의 미수정 후미/오류 후 재조회도 확인하고 lockdep으로 mmap
writeback과 병행 실행한다. 현재 실행 중인 6.6.8 커널에는 모듈을 교체하지 않았다.
