# Write Async Sbuf Path 처리 순서

현재 브랜치 기준으로 `write(async sbuf path)`의 핵심 경로는
`LDY_NO_PAGE_CACHE`가 켜진 경우에만 활성화된다. 기본값은
`driver/rfuse/rfuse_file.c`에서 `0`이므로, 빌드 옵션으로 켜지지 않으면
기존 page 기반 write path가 컴파일된다.

## 1. VFS write 진입

- `driver/rfuse/file.c`의 `fuse_file_write_iter()`에서 시작한다.
- DAX가 아니고 `FOPEN_DIRECT_IO`가 아니면 `fuse_cache_write_iter()`로 간다.
- writeback cache가 아니면 `writethrough` 경로에서 `fuse_perform_write()`를 호출한다.
- `fuse_perform_write()`는 곧바로 `rfuse_perform_write()`로 위임한다.

## 2. sbuf write entry 선택

- `driver/rfuse/rfuse_file.c`의 `rfuse_perform_write()`가 호출된다.
- `LDY_NO_PAGE_CACHE`가 true이면 `rfuse_perform_write_sbuf()`를 호출한다.
- 파일 크기 확장 가능성이 있으면 `FUSE_I_SIZE_UNSTABLE`을 설정한다.

## 3. async 가능 여부 판단

- `rfuse_async_allowed()`가 async 가능 여부를 판단한다.
- `IOCB_APPEND`, `O_APPEND`, `O_SYNC`, `O_DSYNC`이면 async를 사용하지 않는다.
- async가 가능하면 `rfuse_perform_write_sbuf()`의 async loop로 진입한다.

## 4. async request 확보

- chunk 크기는 `min(iov_iter_count(ii), fc->max_write)`로 정한다.
- `try_rfuse_get_req(fm, true, false, bytes, fi, NULL)`을 호출한다.
- 내부에서 `rfuse_get_iqueue_for_async()`가 sbuf 여유와 background 상태를 보고
  iqueue를 선택한다.
- request에는 `FR_WAITING`, `FR_BACKGROUND`가 설정된다.

## 5. async write context 등록

- `rfuse_send_write_async()`가 호출된다.
- `rfuse_async_wrt_ctx_init()`이 inode와 fuse_file ref를 잡고 write range를 준비한다.
- `fi->async_writectr++`를 수행한다.
- `fi->async_write_ranges` interval tree에 `[pos, pos + count - 1]` range를 등록한다.
- 이후 read, setattr, fsync 등은 이 range와 counter를 보고 필요 시 대기한다.

## 6. FUSE WRITE metadata 설정

- `struct fuse_write_in`에 `fh`, `offset`, `flags`를 설정한다.
- 필요하면 `FUSE_WRITE_KILL_SUIDGID`를 설정한다.
- `r_req->in.opcode = FUSE_WRITE`를 설정한다.
- `r_req->in.nodeid = ff->nodeid`를 설정한다.

## 7. sbuf 예약 및 user buffer 복사

- `rfuse_reserve_sbuf(r_req, count, RFUSE_PAYLOAD_IN, true)`를 호출한다.
- sbuf 부족 시 fallback하지 않고 `riq->sbuf_waitq`에서 대기 후 재시도한다.
- `rfuse_sbuf_copy_from_iter()`가 user `iov_iter` 내용을 kernel-mapped sbuf로 복사한다.
- `r_req->sbuf_len = copied`를 설정한다.
- `fuse_write_in.size = copied`를 설정한다.
- `r_req->in.arglen[0] = copied`를 설정한다.

## 8. background submit

- completion callback으로 `r_req->end = rfuse_sbuf_write_complete_req`를 설정한다.
- `rfuse_simple_background()`를 호출한다.
- 내부적으로 `rfuse_request_queue_background()`가 request를 bg queue에 넣고,
  background slot이 있으면 pending queue로 이동한다.
- `rfuse_queue_request()`가 pending queue tail에 commit하고 idle user thread를 깨운다.
- 이 시점에서 kernel write syscall은 userspace 완료를 기다리지 않고 `copied`를
  반환할 수 있다.

## 9. userspace daemon 처리

- `lib/librfuse/lib/rfuse_lowlevel.c`의 request dispatch에서
  `FUSE_WRITE && rfuse_req_has_input_sbuf()`이면 `rfuse_do_write()`로 바로 간다.
- 이 경우 `write_buf` 경로보다 sbuf input path가 우선한다.
- `rfuse_do_write()`는 `rfuse_req_sbuf_buffer()`로 sbuf 주소와 길이를 얻고,
  별도 `pread()` 없이 다음 callback을 호출한다.

```c
se->op.write(req, nodeid, sbuf.addr, sbuf.len, offset, fi);
```

## 10. userspace reply

- filesystem write handler가 `fuse_reply_write(req, count)`를 호출한다.
- `fuse_reply_write()`가 `struct fuse_write_out.size = count`를 설정한다.
- `rfuse_send_reply_ok()`에서 `rfuse_send_msg()`로 이어진다.
- background request이면 `RFUSE_REPLY_ASYNC` ioctl을 호출한다.

## 11. kernel async completion

- `driver/rfuse/dev.c`의 ioctl handler가 `RFUSE_REPLY_ASYNC`를 받아
  `rfuse_request_end(r_req)`를 호출한다.
- `rfuse_request_end()`는 background accounting을 줄인다.
- sbuf OUT이면 import를 시도하지만, 이 write path는 `RFUSE_PAYLOAD_IN`이라 import
  대상은 아니다.
- `FR_ASYNC` request이면 `r_req->end()`를 호출한다.

## 12. write completion callback

- `rfuse_sbuf_write_complete_req()`가 호출된다.
- `rfuse_write_complete_status()`로 reply count를 검증한다.
- `out.size > ctx->count`이면 `-EIO`로 처리한다.
- short write도 mapping error로 기록한다.
- 성공이면 `rfuse_apply_to_page()`로 sbuf에 있던 write payload를 page cache 쪽에
  반영한다.
- attr을 invalidate한다.
- `rfuse_async_wrt_ctx_reset()`으로 다음 작업을 수행한다.
  - async range 제거
  - `async_writectr--`
  - 필요 시 `FUSE_I_SIZE_UNSTABLE` clear
  - waiters wakeup
  - file/inode refs release
- 마지막으로 `rfuse_put_request()`에서 sbuf reservation과 request buffer가 반환된다.

## 핵심 함수

- `rfuse_perform_write_sbuf()`: async/sync sbuf write orchestration
- `rfuse_send_write_async()`: async sbuf request 생성, payload 복사, background submit
- `rfuse_sbuf_write_complete_req()`: async write 완료 처리
- `rfuse_do_write()`: userspace에서 sbuf input write dispatch
- `fuse_reply_write()`: userspace write reply 생성
