# RFUSE read/write data plane

이 문서는 `driver/rfuse/rfuse_dev.c`와 `lib/librfuse/lib/rfuse_lowlevel.c` 기준으로 RFUSE read/write data plane 흐름을 정리한다.

RFUSE의 read/write 경로는 metadata와 실제 file data를 분리한다.
요청 metadata는 shared `rfuse_req`와 ring buffer를 통해 전달하고, 실제 file data는 `r_req->rp`에 연결된 커널 page를 `/dev/fuse` read/write/splice 경로로 복사하거나 전달한다.

## 공통 요청 흐름

1. 커널은 `rfuse_req`를 만들고 `pending` ring buffer에 request index를 넣는다.
2. librfuse의 `rfuse_read_queue()`가 pending head를 읽어 `u_req->index`를 얻는다.
3. librfuse는 `riq->ureq[index]`에서 opcode와 인자를 읽는다.
4. `FUSE_READ`는 `rfuse_do_read()`, `FUSE_WRITE`는 `rfuse_do_write()` 또는 `rfuse_do_write_buf()`로 분기한다.
5. 처리 완료 후 librfuse는 `FR_FINISHED`를 세우고 ioctl 또는 async reply로 커널을 깨운다.

## WRITE data flow

WRITE는 application이 쓴 데이터를 커널 page에서 librfuse/filesystem callback으로 넘기는 경로이다.

1. 커널 쪽 write 요청은 `rfuse_req.args`에 `struct fuse_write_in`을 담는다.
2. 실제 write data는 `r_req->rp`의 page 배열에 연결된다.
3. daemon이 pending queue에서 `FUSE_WRITE`를 읽는다.
4. `write_buf` callback이 있으면 `rfuse_prep_write_buf()`가 `splice(fd, off_in, pipe, ...)`로 커널 device에서 pipe로 데이터를 가져온다.
5. `write_buf` callback이 없으면 `rfuse_do_write()`가 `pread(fd, buffer, ..., encoded_offset)`로 커널 device에서 userspace buffer로 데이터를 가져온다.
6. 커널의 `rfuse_dev_do_read()`는 encoded offset에서 `riq_id`와 `req_index`를 복원하고, `r_req->rp->pages[]`의 데이터를 userspace buffer 또는 pipe로 복사한다.
7. librfuse는 받은 데이터를 filesystem의 `op.write()` 또는 `op.write_buf()`에 넘긴다.
8. filesystem이 `fuse_reply_write()`를 호출하면 `struct fuse_write_out`의 `size`를 `r_req->args`에 기록하고 완료 reply를 보낸다.

WRITE의 data 방향은 다음과 같다.

```text
application write data
 -> kernel rfuse_pages(r_req->rp)
 -> /dev/fuse pread/splice
 -> librfuse buffer/pipe
 -> filesystem write callback
```

## READ data flow

READ는 filesystem이 만든 read 결과를 librfuse에서 커널 page로 돌려주는 경로이다.

1. 커널 쪽 read 요청은 `rfuse_req.args`에 `struct fuse_read_in`을 담는다.
2. read 결과를 받을 output page들은 `r_req->rp`에 연결된다.
3. daemon이 pending queue에서 `FUSE_READ`를 읽는다.
4. `rfuse_do_read()`가 filesystem의 `op.read(u_req, nodeid, size, offset, fi)`를 호출한다.
5. filesystem은 read 결과를 `fuse_reply_buf()` 또는 `fuse_reply_data()`로 반환한다.
6. `fuse_reply_buf()`는 `pwrite(fd, buf, size, encoded_offset)`로 데이터를 커널 device에 쓴다.
7. `fuse_reply_data()`는 가능하면 pipe와 `splice()`로 데이터를 커널 device에 보낸다.
8. 커널의 `rfuse_dev_do_write()` 또는 `rfuse_dev_splice_write()`가 encoded offset에서 `riq_id`와 `req_index`를 복원하고, userspace가 보낸 데이터를 `r_req->rp->pages[]`에 복사한다.
9. 커널은 요청을 완료 처리하고 application read가 해당 page 데이터를 받는다.

READ의 data 방향은 다음과 같다.

```text
filesystem read result
 -> librfuse buffer/pipe
 -> /dev/fuse pwrite/splice
 -> kernel rfuse_pages(r_req->rp)
 -> application read buffer/page cache
```

## 주의할 점

커널 함수 이름은 `/dev/fuse` file operation 관점이다.
그래서 이름이 data plane 의미와 반대로 보일 수 있다.

| 커널 함수 | `/dev/fuse` 관점 | RFUSE data plane 의미 |
|---|---|---|
| `rfuse_dev_do_read()` | daemon이 device에서 읽음 | WRITE payload를 커널 page에서 daemon으로 내보냄 |
| `rfuse_dev_do_write()` | daemon이 device에 씀 | READ reply payload를 daemon에서 커널 page로 채움 |

`pread()`, `pwrite()`, `splice()`에서 쓰는 offset에는 `riq_id`와 `req_index`가 인코딩된다.
커널은 이 값을 사용해 대상 `rfuse_iqueue`와 `rfuse_req`를 다시 찾고, 해당 요청의 `r_req->rp` page 배열에 접근한다.
