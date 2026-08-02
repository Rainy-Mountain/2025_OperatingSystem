# 운영체제 과제 — xv6 커널 실습 (2025)

운영체제 수업에서 진행한 다섯 개의 과제입니다. 리눅스 환경에서 `/proc` 기반 시스템 분석으로 시작해, 교육용 운영체제 **xv6**의 커널을 직접 수정하며 시스템 콜, CPU 스케줄링, 메모리 관리, 파일 시스템까지 운영체제의 핵심 요소를 단계적으로 구현했습니다.

- **작성자**: 조빈 (컴퓨터학부 20213146)
- **개발 환경**: Linux (Ubuntu), xv6, QEMU, C, gcc, Make

## 과제 개요

| 과제 | 주제 | 핵심 내용 |
|------|------|-----------|
| HW0 | 리눅스 시스템 분석 | `/proc`, `top`, `free` 등을 이용한 CPU·메모리·프로세스·디스크 관찰 |
| HW1 | xv6 시스템 콜 추가 | `hello_number`, `get_procinfo` 시스템 콜 구현 및 테스트 프로그램 작성 |
| HW2 | Stride 스케줄러 | tickets 비례 배분 방식의 Stride 스케줄링과 `settickets` 시스템 콜 구현 |
| HW3 | 물리 메모리 추적 및 가상 메모리 확장 | 물리 프레임 사용 현황 추적, 소프트웨어 페이지 워커·IPT·SW-TLB·COW 구현 |
| HW4 | 파일 시스템 스냅샷 | 블록 참조 카운트 기반 COW 스냅샷 생성·복구·삭제 기능 구현 |

---

## HW0. 리눅스 시스템 분석

리눅스 명령어와 `/proc` 가상 파일 시스템을 이용해 시스템의 동작을 직접 관찰하고 분석한 과제입니다.

- 리눅스 커널·배포판, Ubuntu LTS/정규 버전의 개념 정리
- `/proc/cpuinfo`, `lscpu`, `free -h`, `/proc/stat`으로 CPU 개수·주파수, 물리 메모리, 부팅 후 fork 횟수, 문맥 교환 횟수 확인
- `top`을 이용한 CPU 소비 프로세스의 PID·CPU/메모리 사용률·상태(R/S/D/Z/T) 분석
- 부모 프로세스 추적(5세대 이상 조상 PID), I/O 리디렉션(`>`)과 파이프(`|`)가 셸에서 구현되는 원리 분석
- 프로세스별 가상 메모리와 실메모리 크기 비교, 프로그램 실행 중 디스크 I/O 관찰

## HW1. xv6 시스템 콜 추가

xv6를 설치·컴파일하고, 커널에 새로운 시스템 콜 두 개를 추가한 뒤 이를 사용하는 응용 프로그램을 작성한 과제입니다.

**추가한 시스템 콜**

- `hello_number(int n)` : 커널 콘솔에 메시지를 출력하고 `n*2`를 반환하는 시스템 콜
- `get_procinfo(int pid, struct procinfo *)` : PID에 해당하는 프로세스의 상태 정보(PID, 부모 PID, 상태, 메모리 크기, 이름)를 구조체에 담아 반환하는 시스템 콜 (`copyout`으로 커널→유저 데이터 복사)

**작성한 응용 프로그램**

- `helloxv6.c` : 양수/음수 인자로 `hello_number`를 호출해 커널 출력과 반환값 검증
- `psinfo.c` : 명령행 인자로 받은 PID(없으면 자기 자신)의 프로세스 정보를 조회·출력

**수정 파일**: `syscall.h`, `syscall.c`, `sysproc.c`, `usys.S`, `user.h`, `proc.h`, `Makefile`(UPROGS 등록)

## HW2. Stride 스케줄러 구현

xv6의 기본 라운드 로빈 스케줄러를 **Stride 스케줄링**으로 교체한 과제입니다. 각 프로세스에 부여된 tickets(가중치)에 비례해 CPU 시간을 배분합니다.

- `stride = STRIDE_MAX / tickets`로 보폭을 계산하고, 실행할 때마다 `pass += stride` 누적
- RUNNABLE 프로세스 중 `(pass, pid)`가 가장 작은 프로세스를 선택해 공정하게 CPU 배분
- pass 값이 커지면 RUNNABLE 최소 pass 기준으로 정규화하는 **rebase**로 오버플로·편차 방지
- `settickets(int tickets, int end_ticks)` 시스템 콜로 사용자 공간에서 가중치와 프로세스 수명(종료 기준 틱) 제어
- 타이머 인터럽트(`trap.c`)에서 프로세스 틱을 관리하고 `end_ticks` 도달 시 종료 처리
- `fork()` 시 스케줄링 필드 상속·초기화, `ptable.lock` 보호 하에 필드 갱신

**수정 파일**: `syscall.h`, `syscall.c`, `sysproc.c`, `usys.S`, `proc.h`, `proc.c`(scheduler 수정), `trap.c`, `Makefile`(단일 CPU 설정, 테스트 프로그램 등록)

## HW3. 물리 메모리 추적 및 가상 메모리 확장

`kalloc`/`kfree`를 확장하여 모든 물리 페이지 프레임의 사용 현황을 커널에서 추적하고, 가상 메모리 관리 구조를 소프트웨어 수준에서 구현·시각화한 과제입니다.

**A. 물리 프레임 추적**

- 프레임별 할당 여부, 소유 PID, 사용 시작 틱을 기록하는 `physframe_info` 테이블 구현
- `kalloc()`/`kfree()`에서 할당·해제 시점에 테이블 갱신
- `dump_physmem_info` 시스템 콜로 유저 공간에서 프레임 테이블 조회

**B. 테스트 프로그램**

- `memdump.c` : 물리 프레임 테이블을 출력 (`-a` 할당된 프레임만, `-p PID` 특정 PID 필터링)
- `memstress.c` : `sbrk`로 메모리를 할당하고 옵션(`-n` 페이지 수, `-t` 유지 틱, `-w` 쓰기 여부)에 따라 부하 생성
- `memtest.c` : 여러 프로세스를 fork해 메모리 압박 시나리오를 만들고 memdump로 상태 관찰

**C. 가상 메모리 확장**

- 소프트웨어 페이지 워커 `sw_vtop()` : 하드웨어 변환 없이 pgdir/PTE를 직접 파싱해 VA→PA 변환 (`vtop` 시스템 콜)
- **IPT(역페이지 테이블)** : `(pid, va)→pfn` 매핑을 등록해 특정 물리 페이지를 참조 중인 모든 프로세스를 역조회 (`phys2virt` 시스템 콜)
- **SW-TLB** : 고정 크기 소프트웨어 주소 변환 캐시와 히트/미스 카운터, 무효화 로직 구현
- **Copy-On-Write fork** : `copyuvm()`에서 `PTE_W` 제거 + `PTE_COW` 설정, 페이지 폴트(`T_PGFLT`) 시 COW 폴트를 감지해 새 페이지 복사, 참조 카운트로 프레임 공유 관리

**수정 파일**: `kalloc.c`, `vm.c`, `proc.c`, `trap.c`, `sysproc.c`, `syscall.h/c`, `usys.S`, `user.h`, `defs.h`, `mmu.h`, `types.h`, `param.h`, `Makefile`

## HW4. 파일 시스템 COW 스냅샷

xv6 파일 시스템에 **COW 기반 스냅샷** 기능을 추가한 과제입니다. 데이터 블록을 실제로 복사하지 않고 공유하다가, 수정이 발생할 때만 새 블록을 할당합니다.

**A. 블록 참조 카운트**

- 디스크 블록별로 몇 개의 inode(파일/스냅샷)가 공유 중인지 추적하는 메타데이터 배열 구현
- `incref_block` / `decref_block` / `getref_block` / `free_block_if_last` : 참조 카운트가 0이 될 때만 실제로 `bfree` 하는 COW 정책 구현

**B. 스냅샷 시스템 콜**

- `snapshot_create()` : 현재 루트(/) 상태를 `/snapshot/[ID]` 디렉토리로 캡처. 디렉토리 구조만 새로 만들고 파일 데이터 블록은 참조 카운트만 올려 공유
- `snapshot_rollback(id)` : 현재 루트 내용을 정리한 뒤 스냅샷 내용을 루트로 복원
- `snapshot_delete(id)` : 스냅샷 트리를 재귀 삭제하고 공유 블록의 참조 카운트를 조정, 마지막 참조인 블록만 해제 (inode 락 순서를 고려해 데드락 방지)

**C. 테스트 프로그램**

- `mk_test_file.c`(여러 블록에 걸친 테스트 파일 생성), `append.c`(COW 유발용 파일 추가 쓰기), `print_addr.c`(inode의 direct/indirect 블록 번호 출력), `snap_create.c` / `snap_rollback.c` / `snap_delete.c`(시스템 콜 래퍼)
- 스냅샷 생성 후 블록 공유 확인 → append 후 COW로 인한 블록 주소 변화 관찰 → 롤백·삭제 검증 시나리오 수행

**수정 파일**: `fs.c`, `sysfile.c`, `defs.h`, `syscall.h/c`, `usys.S`, `user.h`, `Makefile`

---

## 빌드 및 실행 (xv6 공통)

```bash
make          # xv6 커널 및 유저 프로그램 컴파일
make qemu-nox # QEMU에서 xv6 부팅 (그래픽 없이 터미널 모드)
```

## 배운 점

- 시스템 콜이 유저 공간에서 커널까지 연결되는 전체 경로(`user.h` → `usys.S` → `syscall.c` → `sysproc.c`)와 `argint`/`copyout`을 통한 커널-유저 데이터 교환
- 스케줄러 내부 구조와 비례 배분(Stride) 스케줄링, 타이머 인터럽트·spinlock을 이용한 커널 동기화
- 물리/가상 메모리 관리 구조 — 페이지 테이블 워킹, 역페이지 테이블, TLB 캐싱, Copy-On-Write의 원리와 구현
- 파일 시스템의 inode·블록 구조와 참조 카운트 기반 COW 스냅샷, 트랜잭션(로그)·락 순서를 고려한 커널 자료구조 설계
