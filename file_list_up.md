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

