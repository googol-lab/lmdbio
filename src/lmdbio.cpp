/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil ; -*- */
/*
 *  (C) 2017 by Argonne National Laboratory.
 *      See COPYRIGHT in top-level directory.
 */

#include "lmdbio.h"
#include <map>
#include <iostream>
#include <unistd.h>
#include <assert.h>
#include <new>
#include <sys/mman.h>
#include <signal.h>

using std::cout;
using std::endl;
static int tmp = 0;
static uint64_t num_page_faults = 0;
char* lmdb_me_map;
char* lmdb_me_fmap;

#define GET_PAGE(x) ((char *) (((unsigned long long) (x)) & ~(PAGE_SIZE - 1)))
#define META_PAGE_NUM (2)

int sigsegv_handler(int dummy1, siginfo_t *__sig, void *dummy2)
{
    num_page_faults++;
    printf("got a SIGSEGV at %p, num_page_faults %llu\n", __sig->si_addr,
           num_page_faults);

    /* set the page to be accessible again */
    printf("unprotecting page %p, %d bytes\n", GET_PAGE(__sig->si_addr), getpagesize());
    if (GET_PAGE(__sig->si_addr) == __sig->si_addr)
        mprotect(GET_PAGE(__sig->si_addr), getpagesize(), PROT_READ);
    else
        mprotect(GET_PAGE(__sig->si_addr), getpagesize(), PROT_READ | PROT_EXEC);

    return 0;
}

int lmdb_fault_handler(int dummy1, siginfo_t *__sig, void *dummy2)
{
  char* fault_addr = GET_PAGE(__sig->si_addr);
  //printf("lmdbio: FAULT at %p, for 4096 -------\n", fault_addr);
  size_t offset = (size_t) (fault_addr - lmdb_me_map);
  mprotect(fault_addr, getpagesize(), PROT_READ | PROT_WRITE);
  memcpy(fault_addr, lmdb_me_fmap + offset, getpagesize());
  return 0;
}

void lmdbio::db::lmdb_direct_io(int start_pg, int read_pages) {
  MPI_Status status;
  size_t offset = (size_t) start_pg * getpagesize();
  size_t bytes = (size_t) read_pages * getpagesize();
  char* buff = lmdb_buffer + offset;
  int count = 0, rc, len = 0;
  char err[MPI_MAX_ERROR_STRING + 1];

  //printf("lmdbio: DIRECTIO from addr %p for %zd bytes, offset %zd, start pg %d, read pages %d -------\n", buff, bytes, offset, start_pg, read_pages);
  mprotect(buff, bytes, PROT_READ | PROT_WRITE);
  rc = MPI_File_read_at_all(fh, offset, buff, bytes, MPI_BYTE,
      &status);
  bytes_read += bytes;
  if (rc) {
    MPI_Error_string(rc, err, &len);
    printf("lmdbio: MPI file read error %s\n", err);
  }
  assert(rc == 0);
}

void lmdbio::db::init(MPI_Comm parent_comm, const char* fname, int batch_size,
    int reader_size) {
#ifdef BENCHMARK
  double start, end;
  init_time.init_var_time = 0.0;
  init_time.init_db_time = 0.0;
  init_time.open_db_time = 0.0;
  iter_time.mpi_time = 0.0;
  iter_time.set_record_time = 0.0;
  iter_time.remap_time = 0.0;
  iter_time.mkstemp_time = 0.0;
  iter_time.unlink_time = 0.0;
  iter_time.write_time = 0.0;
  iter_time.mmap_time = 0.0;
  iter_time.close_time = 0.0;
  iter_time.unmap_time = 0.0;
  iter_time.load_meta_time = 0.0;
  iter_time.mprotect_time = 0.0;
  iter_time.prefetch_time = 0.0;
  iter_time.seek_time = 0.0;
  iter_time.mdb_seek_time = 0.0;
  iter_time.access_time = 0.0;
  iter_time.compute_offset_time = 0.0;
  iter_time.cursor_get_current_time = 0.0;
  iter_time.cursor_sz_recv_time = 0.0;
  iter_time.cursor_sz_send_time = 0.0;
  iter_time.cursor_recv_time = 0.0;
  iter_time.cursor_send_time = 0.0;
  iter_time.cursor_dsrl_time = 0.0;
  iter_time.cursor_srl_time = 0.0;
  iter_time.cursor_malloc_time = 0.0;
  iter_time.cursor_free_time = 0.0;
  iter_time.cursor_restoring_time = 0.0;
  iter_time.cursor_storing_time = 0.0;
  iter_time.barrier_time = 0.0;
  start = MPI_Wtime();
#endif

  //MPI_Comm_dup(parent_comm, &global_comm);

  /* initialize class attributes */
  global_np = 0; 
  global_rank = 0;

  MPI_Comm_size(MPI_COMM_WORLD, &global_np);
  MPI_Comm_rank(MPI_COMM_WORLD, &global_rank);

  cout << "Global rank " << global_rank << endl;
  cout << "Global np " << global_np << endl;

  this->batch_size = batch_size;
  this->subbatch_size = batch_size / global_np;
  cout << "Subbatch size " << this->subbatch_size << endl;
#ifdef BENCHMARK
  end = MPI_Wtime();
  init_time.init_var_time = get_elapsed_time(start, end);
  start = MPI_Wtime();
#endif

  this->reader_size = reader_size;
  assert(reader_size <= global_np);
  cout << "Reader size is set to " << reader_size << endl;

  this->local_reader_size = 0;

  assign_readers(fname, batch_size);
  bytes_read = 0;

#ifdef BENCHMARK
  end = MPI_Wtime();
  init_time.init_db_time = get_elapsed_time(start, end);
#endif
}

/* assign one reader per node */
void lmdbio::db::assign_readers(const char* fname, int batch_size) {
  int size = 0;
  string mmap_env;
  int is_rank_0 = 0;
  int is_reader_ = 0;
  int sublocal_id = 0;
  local_rank = 0;
  local_np = 0;
  reader_id = 0;
  fetch_size = 0;

  /* communicator between processes within a node */
  local_comm = MPI_COMM_NULL;
  MPI_Comm_split_type(MPI_COMM_WORLD, MPI_COMM_TYPE_SHARED, global_rank,
      MPI_INFO_NULL, &local_comm);
  assert(local_comm != MPI_COMM_NULL);
  MPI_Comm_size(local_comm, &local_np);
  MPI_Comm_rank(local_comm, &local_rank);

  /* get number of nodes */
  is_rank_0 = local_rank == 0 ? 1 : 0;
  MPI_Allreduce(&is_rank_0, &node_size, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);

  /* get a number of readers */
  reader_size = reader_size && reader_size >= node_size ? reader_size : node_size;

  /* get a number of readers within a node */
  local_reader_size = reader_size / node_size;
  assert(local_reader_size);

  //cout << "Local reader size is " << local_reader_size << endl;

  is_single_reader_per_node = local_reader_size == 1;

  /* communicator between the reader and processes */
  if (!is_single_reader_per_node) {
    sublocal_id = local_rank / (local_np / local_reader_size);
    sublocal_comm = MPI_COMM_NULL;
    MPI_Comm_split(local_comm, sublocal_id, local_rank, &sublocal_comm);
    assert(sublocal_comm != MPI_COMM_NULL);
    MPI_Comm_size(sublocal_comm, &sublocal_np);
    MPI_Comm_rank(sublocal_comm, &sublocal_rank);
    //cout << "Rank " << global_rank << " sublocal id " << sublocal_id << 
    //  " sublocal rank " << sublocal_rank << endl;
  }

  /* get number of readers */
  is_reader_ = is_reader() ? 1 : MPI_UNDEFINED;

  /* communicator between readers */
  reader_comm = MPI_COMM_NULL;
  MPI_Comm_split(MPI_COMM_WORLD, is_reader_, global_rank, &reader_comm);
  if (is_reader()) {
    assert(reader_comm != MPI_COMM_NULL);
    MPI_Comm_size(reader_comm, &reader_size);
    MPI_Comm_rank(reader_comm, &reader_id);
  }

  /* open database and set fetch size */
  if (is_reader(local_rank)) {
    char hostname[256];
    gethostname(hostname, 255);
    cout << "Number of readers " << reader_size << endl;
    cout << "Rank " << global_rank << " is a reader id " << reader_id << 
      " on host " << hostname << endl;

    fetch_size = batch_size / reader_size;
    assert(fetch_size);
  }

  MPI_Barrier(MPI_COMM_WORLD);

  if (is_reader(local_rank)) {
#ifdef BENCHMARK
    double end_db, start_db;
    start_db = MPI_Wtime();
#endif
    //cout << "OPEN DB" << endl;
    open_db(fname);
    //printf("tmp %d\n", tmp);

#ifdef BENCHMARK
    end_db = MPI_Wtime();
    init_time.open_db_time = get_elapsed_time(start_db, end_db);
#endif
  }

  MPI_Barrier(MPI_COMM_WORLD);

  if (global_rank == 0) {
    /* get size */
    size = lmdb_value_size();
  }

  /* broadcast a size of data to allocate the shared buffer */
  MPI_Bcast(&size, 1, MPI_INT, 0, MPI_COMM_WORLD);
  
  this->win_size = subbatch_size * size * 2;

  /* allocate neccessary buffer */
  //cout << "FETCH SIZE " << fetch_size << endl;
  if (is_reader(local_rank))
    batch_ptrs = new char*[fetch_size];
  if (dist_mode == MODE_SCATTERV) {
    if (is_reader(local_rank)) {
      int io_np = get_io_np();
      send_sizes = new int[fetch_size];
      send_displs = new int[io_np];
      send_counts = new int[io_np];
      batch_bytes = (char*) malloc(fetch_size * size * 2 * sizeof(char));
    }
    sizes = new int[subbatch_size];
    subbatch_bytes = (char*) malloc(win_size);
  }
  else if (dist_mode == MODE_SHMEM) {
    MPI_Comm io_comm = get_io_comm();
    MPI_Win_allocate_shared(subbatch_size * sizeof(int), sizeof(int),
        MPI_INFO_NULL, io_comm, &sizes, &size_win);
    MPI_Win_lock_all(MPI_MODE_NOCHECK, size_win);
    MPI_Win_allocate_shared(win_size, sizeof(char), MPI_INFO_NULL, io_comm,
        &subbatch_bytes, &batch_win);
    MPI_Win_lock_all(MPI_MODE_NOCHECK, batch_win);
  }

  /* allocate record's array */
  this->records = new (std::nothrow) record[subbatch_size];
}

void lmdbio::db::lmdb_load_meta() {
  memcpy(lmdb_buffer, meta_buffer, META_PAGE_NUM);
  mdb_set_meta(mdb_env_);
}

void lmdbio::db::lmdb_remap_buff() {
  //printf("lmdbio: remap buff\n");
  //printf("lmdbio: LMDBIO %p\n", lmdb_buffer);
#ifdef BENCHMARK
  double start_remap, end_remap, start, end;
  start_remap = MPI_Wtime();
#endif
  //printf("lmdbio: remap at iter %d, total bytes read %zd\n", iter, bytes_read);
  bytes_read = 0;
  /* time for mdb_unmap_vdb and mdb_create_map_vdb is from LMDB (VDB_time) */
  mdb_unmap_vdb(mdb_env_);
  assert(mdb_create_map_vdb(mdb_env_) == 0);
#ifdef BENCHMARK
  VDB_time vdb_time;
  mdb_vdb_time(mdb_env_, &vdb_time);
  iter_time.mkstemp_time += vdb_time.vdb_mkstemp_time;
  iter_time.unlink_time += vdb_time.vdb_unlink_time;
  iter_time.seek_time += vdb_time.vdb_seek_time;
  iter_time.write_time += vdb_time.vdb_write_time;
  iter_time.mmap_time += vdb_time.vdb_mmap_time;
  iter_time.close_time += vdb_time.vdb_close_time;
  iter_time.unmap_time += vdb_time.vdb_unmap_time;
  start = MPI_Wtime();
#endif
  mprotect(lmdb_buffer, (size_t) mdb_get_mapsize(mdb_env_), PROT_NONE);
#ifdef BENCHMARK
  end = MPI_Wtime();
  iter_time.mprotect_time += get_elapsed_time(start, end);
  start = MPI_Wtime();
#endif
  lmdb_load_meta();
#ifdef BENCHMARK
  end = MPI_Wtime();
  iter_time.load_meta_time += get_elapsed_time(start, end);
  end_remap = MPI_Wtime();
  iter_time.remap_time += get_elapsed_time(start_remap, end_remap);
#endif
}

/* open the database and initialize a position of a cursor */
void lmdbio::db::open_db(const char* fname) {
  int flags = MDB_RDONLY | MDB_NOTLS | MDB_NOLOCK;
  size_t addr;
  char addr_str[100]; // enough to hold all numbers up to 64-bits
  int max_try = 200;
  int rc = 0;
  int success = 0;
  struct sigaction __sig;
  MDB_envinfo stat;

  srand(time(NULL));
  check_lmdb(mdb_env_create(&mdb_env_), "Created environment", false);
  check_lmdb(mdb_env_set_maxreaders(mdb_env_, reader_size), "Set maxreaders",
      false);

  char *e = getenv("ENABLE_MPROTECT");
  if (e && !strcmp(e, "1")) {
      __sig.sa_sigaction = (void (*) (int, siginfo_t *, void *))
          sigsegv_handler;
      __sig.sa_flags = SA_SIGINFO;
      sigaction(SIGSEGV, &__sig, 0);
  }

#if defined(ICPADS) || defined(DIRECTIO)
  /* random an address and fix mmap's address */
  for (int i = 0; i < max_try; i++) {
    if (global_rank == 0) {
      addr = (size_t) getpagesize() * rand();
      snprintf(addr_str, 100, "MMAP_ADDRESS=%llu", addr);
      cout << addr_str << endl;
      putenv(addr_str);
      //cout << "mapping file " << fname << endl;
      rc = mdb_env_open(mdb_env_, fname, flags, 0664);
      //cout << "reader " << reader_id << " error code " << rc << endl;
      if (rc != 0) {
        cout << "mmap failed; trying again\n";
        continue;
      }
    }
    MPI_Bcast(addr_str, 100, MPI_CHAR, 0, reader_comm);
    if (global_rank != 0) {
      putenv(addr_str);
      cout << "trying to mmap with address " << addr_str << endl;
      rc = mdb_env_open(mdb_env_, fname, flags, 0664);
      //cout << "reader " << reader_id << " error code " << rc << endl;
    }
    MPI_Allreduce(&rc, &success, 1, MPI_INT, MPI_MAX, reader_comm);
    if (success == 0) {
      printf("all processes got the same address %llu\n", addr);
      break;
    }
    if (rc == 0)
      mdb_env_close(mdb_env_);
    rc = success;
  }
  if (rc != 0) {
    cout << "Cannot open db" << endl;
    exit(1);
  }
  /* set lmdb buffer */
  strtok(addr_str, "=");
  addr = atoll(strtok(NULL, "="));
  lmdb_buffer = (char*) addr;
  lmdb_me_map = lmdb_buffer;
#ifdef DIRECTIO
  cout << "lmdbio: DIRECT IO mode" << endl;

  /* set sigaction handler */
  __sig.sa_sigaction = (void (*) (int, siginfo_t *, void *))
    lmdb_fault_handler;
  __sig.sa_flags = SA_SIGINFO;
  sigaction(SIGSEGV, &__sig, 0);

  /* protect the main buffer */
  mprotect(lmdb_buffer, (size_t) mdb_get_mapsize(mdb_env_), PROT_NONE);
  lmdb_me_fmap = mdb_get_fmap(mdb_env_);

  char filename[1000];
  snprintf(filename, 1000, "%s/data.mdb", fname);

  /* set ROMIO hints */
  MPI_Info info;
  MPI_Info_create(&info);
  MPI_Info_set(info, "romio_cb_write", "disable");
  MPI_Info_set(info, "romio_cb_read", "enable");

  /* open a file to perform direct I/O */
  MPI_File_open(reader_comm, filename, MPI_MODE_RDONLY, info, &fh);
  MPI_Info_free(&info);

  /* load meta pages (first 2 pages) */
  meta_buffer = (char*) malloc(META_PAGE_NUM * sizeof(char));
  lmdb_direct_io(0, META_PAGE_NUM);
  memcpy(meta_buffer, lmdb_buffer, META_PAGE_NUM);
  mdb_set_meta(mdb_env_);
#endif
#else
  rc = mdb_env_open(mdb_env_, fname, flags, 0664);
  //cout << "reader " << reader_id << " error code " << rc << endl;
#endif

  check_lmdb(mdb_txn_begin(mdb_env_, NULL, MDB_RDONLY, &mdb_txn),
      "Begun transaction", false);
  check_lmdb(mdb_dbi_open(mdb_txn, NULL, 0, &mdb_dbi_),
      "Opened database", false);
  check_lmdb(mdb_cursor_open(mdb_txn, mdb_dbi_, &mdb_cursor),
      "Opened cursor", false);
  lmdb_init_cursor();
  sample_size = lmdb_value_size();
  /* round sample size to a page unit */
  sample_size = (sample_size & (~(PAGE_SIZE - 1))) 
    + (PAGE_SIZE * !!(sample_size % PAGE_SIZE));
  num_read_pages = (sample_size * fetch_size) / PAGE_SIZE;

  max_read_pages = min_read_pages = read_pages = num_read_pages;
  printf("LMDB buffer address %p\n", lmdb_buffer);

  /* skip the first few pages as they are the meta pages */
  start_pg = (reader_id * num_read_pages);
  if (reader_id == 0)
      start_pg += 3;
  printf("setting start page to %d\n", start_pg);
  fflush(stdout);

  if (e && !strcmp(e, "1")) {
    /* protect the buffer against read accesses */
    mdb_env_info(mdb_env_, &stat);

    printf("protecting buffer %p\n", lmdb_buffer);
    mprotect(lmdb_buffer, (size_t) stat.me_mapsize, PROT_NONE);
  }
}

/* a function for readers */
void lmdbio::db::read_batch() {
  int *read_sizes;
  int size = 0;
  int id = 0;
  int count = 0;
  int cursor_buffer_size = 0;
  void* start_cursor_buffer;
  void* end_cursor_buffer;
#ifdef BENCHMARK
  struct rusage rstart, rend;
  double ttime, utime, stime, sltime, start, end, start_;

  start = MPI_Wtime();
  getrusage(RUSAGE_SELF, &rstart);
  start_ = MPI_Wtime();
#endif
#ifdef ICPADS
  lmdb_touch_pages();
#elif DIRECTIO
  lmdb_direct_io(start_pg, read_pages);
#endif
#ifdef BENCHMARK
  iter_time.prefetch_time += get_elapsed_time(start_, MPI_Wtime());
#endif

#if defined(ICPADS) || defined(DIRECTIO)
  if (reader_id != 0) {
#ifdef BENCHMARK
    start_ = MPI_Wtime();
#endif
    MPI_Recv(&cursor_buffer_size, 1, MPI_INT, reader_id - 1, 0, reader_comm, 
        MPI_STATUS_IGNORE);
#ifdef BENCHMARK
    iter_time.cursor_sz_recv_time += get_elapsed_time(start_, MPI_Wtime());
    start_ = MPI_Wtime();
#endif
    start_cursor_buffer = malloc(cursor_buffer_size);
#ifdef BENCHMARK
    iter_time.cursor_malloc_time += get_elapsed_time(start_, MPI_Wtime());
    start_ = MPI_Wtime();
#endif
    MPI_Recv(start_cursor_buffer, cursor_buffer_size, MPI_BYTE, reader_id - 1, 0, 
        reader_comm, MPI_STATUS_IGNORE);
#ifdef BENCHMARK
    iter_time.cursor_recv_time += get_elapsed_time(start_, MPI_Wtime());
    start_ = MPI_Wtime();
#endif
    mdb_deserialize_cursor(start_cursor_buffer, cursor_buffer_size, mdb_cursor);
#ifdef BENCHMARK
    iter_time.cursor_dsrl_time += get_elapsed_time(start_, MPI_Wtime());
#endif
  }
  else {
#ifdef BENCHMARK
    start_ = MPI_Wtime();
#endif
    mdb_serialize_cursor(mdb_cursor, &start_cursor_buffer, &cursor_buffer_size);
#ifdef BENCHMARK
    iter_time.cursor_storing_time += get_elapsed_time(start_, MPI_Wtime());
#endif
  }

  if (reader_id != reader_size - 1) {
    /* shift the cursor */
#ifdef BENCHMARK
    start_ = MPI_Wtime();
#endif
    lmdb_seek_multiple(fetch_size);
#ifdef BENCHMARK
    iter_time.mdb_seek_time += get_elapsed_time(start_, MPI_Wtime());
    start_ = MPI_Wtime();
#endif
    mdb_serialize_cursor(mdb_cursor, &end_cursor_buffer, &cursor_buffer_size);
#ifdef BENCHMARK
    iter_time.cursor_srl_time += get_elapsed_time(start_, MPI_Wtime());
    start_ = MPI_Wtime();
#endif
    MPI_Send(&cursor_buffer_size, 1, MPI_INT, reader_id + 1, 0, reader_comm);
#ifdef BENCHMARK
    iter_time.cursor_sz_send_time += get_elapsed_time(start_, MPI_Wtime());
    start_ = MPI_Wtime();
#endif
    MPI_Send(end_cursor_buffer, cursor_buffer_size, MPI_BYTE, reader_id + 1, 0, 
        reader_comm);
#ifdef BENCHMARK
    iter_time.cursor_send_time += get_elapsed_time(start_, MPI_Wtime());
    start_ = MPI_Wtime();
#endif
    /* restore the cursor */
    mdb_deserialize_cursor(start_cursor_buffer, cursor_buffer_size, mdb_cursor);
#ifdef BENCHMARK
    iter_time.cursor_restoring_time += get_elapsed_time(start_, MPI_Wtime());
    start_ = MPI_Wtime();
#endif
    free(end_cursor_buffer);
    free(start_cursor_buffer);
#ifdef BENCHMARK
    iter_time.cursor_free_time += get_elapsed_time(start_, MPI_Wtime());
#endif
  }
#endif
  /* compute size of send buffer and get pointers */
  count = 0;
  total_byte_size = 0;
  if (dist_mode == MODE_SCATTERV) {
    read_sizes = this->send_sizes;
    for (int i = 0; i < fetch_size; i++) {
      if (i % subbatch_size == 0) {
        send_displs[id] = total_byte_size;
        //cout << "send displs " << id << " " << total_byte_size;
        total_byte_size += count;
        if (i != 0)
          send_counts[id - 1] = count;
        id++;
        count = 0;
      }
      this->batch_ptrs[i] = (char*) lmdb_value_data();
      size = lmdb_value_size();
      count += size;
      read_sizes[i] = size;
      lmdb_next();
    }
    total_byte_size += count;
    send_counts[id - 1] = count;
  }
  else if (dist_mode == MODE_SHMEM) {
      size_t start, end;
      size_t fetch_start, fetch_end;
    read_sizes = this->sizes;
#ifdef BENCHMARK
    start_ = MPI_Wtime();
#endif
    lmdb_get_current();
#ifdef BENCHMARK
    iter_time.cursor_get_current_time += get_elapsed_time(start_, MPI_Wtime());
    start_ = MPI_Wtime();
#endif
    for (int i = 0; i < fetch_size; i++) {
      //cout << "Reader " << reader_id << " reads item " << i << " key " << key() << endl;
      size = lmdb_value_size();
      batch_ptrs[i] = (char*) lmdb_value_data();
      read_sizes[i] = size;
      total_byte_size += size;
      //cout << "reader " << reader_id << ": " << key() << endl;
      lmdb_next();
    }
#ifdef BENCHMARK
    iter_time.access_time += get_elapsed_time(start_, MPI_Wtime());
    start_ = MPI_Wtime();
#endif
    fetch_start = (batch_ptrs[0] - lmdb_buffer) / getpagesize();
    fetch_end = (batch_ptrs[fetch_size - 1] - lmdb_buffer) / getpagesize();
    
    /*printf("fetched data from page %lld to %lld\n",
           fetch_start, fetch_end);*/
    
    num_missed_pages += 
        (fetch_start < start_pg ? start_pg - fetch_start : 0) +
        (fetch_end > start_pg + read_pages ? fetch_end - start_pg - read_pages : 0);
    //printf("total num missed pages so far: %d\n", num_missed_pages);

    num_extra_pages +=
        (fetch_start > start_pg ? fetch_start - start_pg : 0) +
        (fetch_end < start_pg + read_pages ? start_pg + read_pages - fetch_end : 0);
    //printf("total num extra pages so far: %d\n", num_extra_pages);

    read_pages = (batch_ptrs[fetch_size - 1] - batch_ptrs[0]) * (fetch_size + 1) /
        (fetch_size * getpagesize());
    if (read_pages < min_read_pages)
        min_read_pages = read_pages;
    if (read_pages > max_read_pages)
        max_read_pages = read_pages;

    //printf("min read pages %d max read pages %d\n", min_read_pages, max_read_pages);

    start = (size_t) (batch_ptrs[0] - lmdb_buffer) / getpagesize();
    start_pg = start + (min_read_pages * reader_size);
    read_pages = max_read_pages + (max_read_pages - min_read_pages) * reader_size;
#ifdef BENCHMARK
    iter_time.compute_offset_time += get_elapsed_time(start_, MPI_Wtime());
#endif
  }

  /* determine if the data is larger than a buffer */
  assert(total_byte_size <= win_size * get_io_np());

#if defined(ICPADS) || defined(DIRECTIO)
  /* move a cursor to the next location */
  if (read_mode == MODE_STRIDE) {
    if (reader_id == reader_size - 1) {
#ifdef BENCHMARK
      start_ = MPI_Wtime();
#endif
      mdb_serialize_cursor(mdb_cursor, &end_cursor_buffer, &cursor_buffer_size);
#ifdef BENCHMARK
      iter_time.cursor_srl_time += get_elapsed_time(start_, MPI_Wtime());
      start_ = MPI_Wtime();
#endif
      MPI_Send(&cursor_buffer_size, 1, MPI_INT, 0, 0, reader_comm);
#ifdef BENCHMARK
      iter_time.cursor_sz_send_time += get_elapsed_time(start_, MPI_Wtime());
      start_ = MPI_Wtime();
#endif
      MPI_Send(end_cursor_buffer, cursor_buffer_size, MPI_BYTE, 0, 0, reader_comm);
#ifdef BENCHMARK
      iter_time.cursor_send_time += get_elapsed_time(start_, MPI_Wtime());
      start_ = MPI_Wtime();
#endif
      free(end_cursor_buffer);
#ifdef BENCHMARK
      iter_time.cursor_free_time += get_elapsed_time(start_, MPI_Wtime());
      start_ = MPI_Wtime();
#endif
    }
    else if (reader_id == 0) {
#ifdef BENCHMARK
      start_ = MPI_Wtime();
#endif
      MPI_Recv(&cursor_buffer_size, 1, MPI_INT, reader_size - 1, 0, reader_comm, 
          MPI_STATUS_IGNORE);
#ifdef BENCHMARK
      iter_time.cursor_sz_recv_time += get_elapsed_time(start_, MPI_Wtime());
      start_ = MPI_Wtime();
#endif
      start_cursor_buffer = malloc(cursor_buffer_size);
#ifdef BENCHMARK
      iter_time.cursor_malloc_time += get_elapsed_time(start_, MPI_Wtime());
      start_ = MPI_Wtime();
#endif
      MPI_Recv(start_cursor_buffer, cursor_buffer_size, MPI_BYTE, reader_size - 1, 0, 
          reader_comm, MPI_STATUS_IGNORE);
#ifdef BENCHMARK
      iter_time.cursor_recv_time += get_elapsed_time(start_, MPI_Wtime());
      start_ = MPI_Wtime();
#endif
      mdb_deserialize_cursor(start_cursor_buffer, cursor_buffer_size, mdb_cursor);
#ifdef BENCHMARK
      iter_time.cursor_dsrl_time += get_elapsed_time(start_, MPI_Wtime());
      start_ = MPI_Wtime();
#endif
      free(start_cursor_buffer);
#ifdef BENCHMARK
      iter_time.cursor_free_time += get_elapsed_time(start_, MPI_Wtime());
      start_ = MPI_Wtime();
#endif
    }
  }
#else
  /* shift the cursor */
  lmdb_seek_multiple(fetch_size * (reader_size - 1));
#endif

#ifdef BENCHMARK
  getrusage(RUSAGE_SELF, &rend);
  end = MPI_Wtime();
  ttime = get_elapsed_time(start, end);
  utime = get_utime(rstart, rend);
  stime = get_stime(rstart, rend);
  sltime = get_sltime(ttime, utime, stime);
  read_stat.add_stat(get_ctx_switches(rstart, rend), 
      get_inv_ctx_switches(rstart, rend),
      ttime, utime, stime, sltime);

  start = MPI_Wtime();
  getrusage(RUSAGE_SELF, &rstart);
#endif

  count = 0;
  if (dist_mode == MODE_SCATTERV) {
    for (int i = 0; i < fetch_size; i++) {
      size = read_sizes[i];
      memcpy(batch_bytes + count, batch_ptrs[i], size);
      count += size;
    }
  }
  else if (dist_mode == MODE_SHMEM) {
    for (int i = 0; i < fetch_size; i++) {
      size = read_sizes[i];
      if (i % subbatch_size == 0)
        count = (i / subbatch_size) * win_size;
      memcpy(subbatch_bytes + count, batch_ptrs[i], size);
      count += size;
    }
  }

#ifdef BENCHMARK
  getrusage(RUSAGE_SELF, &rend);
  end = MPI_Wtime();
  ttime = get_elapsed_time(start, end);
  utime = get_utime(rstart, rend);
  stime = get_stime(rstart, rend);
  sltime = get_sltime(ttime, utime, stime);
  parse_stat.add_stat(get_ctx_switches(rstart, rend), 
      get_inv_ctx_switches(rstart, rend),
      ttime, utime, stime, sltime);
#endif
}

void lmdbio::db::send_batch() {
  int count = 0;
  int size = 0;
#ifdef BENCHMARK
  double start;
  start = MPI_Wtime();
#endif
  MPI_Comm comm = get_io_comm();
  //cout << "send batch " << send_sizes[0] << endl;
  /* send data size */
  MPI_Scatter(send_sizes, subbatch_size, MPI_INT, sizes, subbatch_size, MPI_INT,
      0, comm);

  //cout << "scatter size completed" << endl; 
  /* get total size */
  for (int i = 0; i < subbatch_size; i++) {
    count += sizes[i];
  }

  //cout << "count size completed" << endl;
  /* send sub-batch */
  MPI_Scatterv(batch_bytes, send_counts, send_displs, MPI_BYTE,
      subbatch_bytes, count, MPI_BYTE, 0, comm);

  //cout << "scatterv batch completed" << endl;
#ifdef BENCHMARK
  iter_time.mpi_time += get_elapsed_time(start, MPI_Wtime());
#endif
}

/* set records */
void lmdbio::db::set_records() {
  int count = 0;
  int size = 0;
#ifdef BENCHMARK
  double start;
#endif
  if (dist_mode == MODE_SHMEM) {
    MPI_Win_sync(batch_win);
    MPI_Win_sync(size_win);
    MPI_Barrier(get_io_comm());
    MPI_Win_sync(batch_win);
    MPI_Win_sync(size_win);
  }
#ifdef BENCHMARK
  start = MPI_Wtime();
#endif
  for (int i = 0; i < subbatch_size; i++) {
    size = sizes[i];
    records[i].set_record(subbatch_bytes + count, size);
    count += size;
  }
  int fsize = records[0].get_record_size();
#ifdef BENCHMARK
 iter_time.set_record_time += get_elapsed_time(start, MPI_Wtime());
#endif
}

void lmdbio::db::set_mode(int dist_mode, int read_mode) {
  if (dist_mode == MODE_SCATTERV)
    cout << "Set dist mode to SCATTERV" << endl;
  else if (dist_mode == MODE_SHMEM)
    cout << "Set dist mode to SHMEM" << endl;
  if (read_mode == MODE_STRIDE)
    cout << "Set read mode to STRIDE" << endl;
  else if (read_mode == MODE_CONT)
    cout << "Set read mode to CONT" << endl;

  this->dist_mode = dist_mode;
  this->read_mode = read_mode;
}

void lmdbio::db::lmdb_touch_pages() {
  /*printf("touching data from page %d to %d\n",
      start_pg, start_pg + read_pages);*/
  for (size_t i = start_pg; i < start_pg + read_pages; i++) {
      //printf("touching page %d\n", i);
      //for (size_t j = 0; j < PAGE_SIZE; j++)
      tmp += lmdb_buffer[PAGE_SIZE * i];
  }
  //printf("done touching pages\n");
}

/* a process with local rank = 0 is a reader  */
bool lmdbio::db::is_reader(int local_rank) {
  if (is_single_reader_per_node)
    return local_rank == 0;
  else
    return sublocal_rank == 0;
}

bool lmdbio::db::is_reader() {
  return is_reader(local_rank);
}

int lmdbio::db::read_record_batch(void) 
{
  if (is_reader()) {
    read_batch();
    lmdb_remap_buff();
  }
  if (dist_mode == MODE_SCATTERV)
    send_batch();
  set_records();

#ifdef BENCHMARK
  double start;
  start = MPI_Wtime();
#endif
  MPI_Barrier(MPI_COMM_WORLD);
#ifdef BENCHMARK
  iter_time.barrier_time += get_elapsed_time(start, MPI_Wtime());
#endif
  return 0;
}

int lmdbio::db::get_batch_size() {
  return batch_size;
}

int lmdbio::db::get_num_records() {
  return subbatch_size;
}

lmdbio::record* lmdbio::db::get_record(int i) {
  return &records[i];
}


#ifdef BENCHMARK
/* calculate users time */
double lmdbio::db::get_utime(rusage rstart, rusage rend) {
  return 1e6 * (rend.ru_utime.tv_sec - rstart.ru_utime.tv_sec) +
    rend.ru_utime.tv_usec - rstart.ru_utime.tv_usec;
}

/* calculate kernel time */
double lmdbio::db::get_stime(rusage rstart, rusage rend) {
  return 1e6 * (rend.ru_stime.tv_sec - rstart.ru_stime.tv_sec) +
    rend.ru_stime.tv_usec - rstart.ru_stime.tv_usec;
}

/* calculate sleep time */
double lmdbio::db::get_sltime(double ttime, double utime, double stime) {
  return ttime - utime - stime;
}

double lmdbio::db::get_ctx_switches(rusage rstart, rusage rend) {
  return rend.ru_nvcsw - rstart.ru_nvcsw;
}

double lmdbio::db::get_inv_ctx_switches(rusage rstart, rusage rend) {
  return rend.ru_nivcsw - rstart.ru_nivcsw;
}

double lmdbio::db::get_elapsed_time(double start, double end) {
  return 1e6 * (end - start);
}

lmdbio::init_time_t lmdbio::db::get_init_time() {
  return init_time;
}

lmdbio::iter_time_t lmdbio::db::get_iter_time() {
  return iter_time;
}

lmdbio::io_stat lmdbio::db::get_read_stat() {
  return read_stat;
}

lmdbio::io_stat lmdbio::db::get_parse_stat() {
  return parse_stat;
}

MPI_Comm lmdbio::db::get_io_comm() {
  return is_single_reader_per_node ? local_comm : sublocal_comm;
}

int lmdbio::db::get_io_np() {
  return is_single_reader_per_node ? local_np : sublocal_np;
}
#endif
