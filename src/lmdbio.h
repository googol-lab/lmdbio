/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil ; -*- */
/*
 *  (C) 2017 by Argonne National Laboratory.
 *      See COPYRIGHT in top-level directory.
 */

#ifndef LMDBIO_H_INCLUDED
#define LMDBIO_H_INCLUDED

#include <iostream>
#include <string.h>
#include <stdlib.h>
#include "mpi.h"
#include "lmdb.h"
#include <sys/time.h>
#include <sys/resource.h>
#include <vector>
#include <string>

using std::vector;
using std::string;

#define TEST_PHASE (0)
#define TRAIN_PHASE (1)
#define MODE_SCATTERV (0)
#define MODE_SHMEM (1)
#define MODE_STRIDE (0)
#define MODE_CONT (1)
#define PAGE_SIZE (4096)

namespace lmdbio {
class record {
public:
  record() {
    data = NULL;
    record_size = 0;
  }

  ~record() {}

  int get_record_size() { return record_size; }
  void *get_record() { return data; }
  void set_record(void* data, int record_size) {
    string str = string(static_cast<const char*>(data),
        record_size);
    this->data = data;
    this->record_size = record_size;
  }

public:
  void *data;
  int record_size;
};

#ifdef BENCHMARK
class io_stat {
public:
  io_stat() {
    ctx_switches = 0.0;
    inv_ctx_switches = 0.0;
    ttime = 0.0;
    utime = 0.0;
    stime = 0.0;
    sltime = 0.0;
  }

  void add_stat(double ctx_switches, double inv_ctx_switches,
      double ttime, double utime, double stime, double sltime) {
    this->ctx_switches += ctx_switches;
    this->inv_ctx_switches += inv_ctx_switches;
    this->ttime += ttime;
    this->utime += utime;
    this->stime += stime;
    this->sltime += sltime;
  }

  void set_stat(double ctx_switches, double inv_ctx_switches,
      double ttime, double utime, double stime, double sltime) {
    this->ctx_switches = ctx_switches;
    this->inv_ctx_switches = inv_ctx_switches;
    this->ttime = ttime;
    this->utime = utime;
    this->stime = stime;
    this->sltime = sltime;
  }

  double get_ctx_switches() { return ctx_switches; }
  double get_inv_ctx_switches() { return inv_ctx_switches; }
  double get_ttime() { return ttime; }
  double get_utime() { return utime; }
  double get_stime() { return stime; }
  double get_sltime() { return sltime; }

private:
  double ctx_switches;
  double inv_ctx_switches;
  double ttime;
  double utime;
  double stime;
  double sltime;
};

struct init_time_t {
  double init_var_time;
  double init_db_time;
  double open_db_time;
};

struct iter_time_t {
  double mpi_time;
  double prefetch_time;
  double mdb_seek_time;
  double access_time;
  double compute_offset_time;
  double cursor_get_current_time;
  double cursor_sz_recv_time;
  double cursor_sz_send_time;
  double cursor_recv_time;
  double cursor_send_time;
  double cursor_dsrl_time;
  double cursor_srl_time;
  double cursor_malloc_time;
  double cursor_free_time;
  double cursor_restoring_time;
  double cursor_storing_time;
  double set_record_time;
  double remap_time;
  double mkstemp_time;
  double unlink_time;
  double seek_time;
  double write_time;
  double mmap_time;
  double close_time;
  double unmap_time;
  double load_meta_time;
  double mprotect_time;
  double barrier_time;
};

#endif

class db
{
public:
  db() {
    dist_mode = MODE_SHMEM;
    read_mode = MODE_STRIDE;
    num_missed_pages = 0;
    num_extra_pages = 0;
  }

  void init(MPI_Comm parent_comm, const char *fname, int batch_size,
      int reader_size = 0, int prefetch = 1);
  void set_mode(int dist_mode, int read_mode);

  ~db() {
    if (is_reader()) {
      mdb_cursor_close(mdb_cursor);
      mdb_dbi_close(mdb_env_, mdb_dbi_);
      mdb_env_close(mdb_env_);
    }
   
    if (dist_mode == MODE_SHMEM) {
      MPI_Win_unlock_all(batch_win);
      MPI_Win_unlock_all(size_win);
      MPI_Win_free(&batch_win);
      MPI_Win_free(&size_win);
    }
    delete[] records;
    //MPI_Comm_free(&global_comm);
  }

  int read_record_batch(void ***records, int *num_records, int **record_sizes);
  int get_batch_size(void);
  int read_record_batch(void);
  int get_num_records(void);
  record* get_record(int i);

#ifdef BENCHMARK
  int remap_iter;
  size_t bytes_read;

  init_time_t get_init_time();
  iter_time_t get_iter_time();
  io_stat get_read_stat();
  io_stat get_parse_stat();
#endif

private:
  MPI_Comm global_comm;
  MPI_Comm local_comm;
  MPI_Comm sublocal_comm;
  MPI_Comm reader_comm;
  MPI_Win batch_win;
  MPI_Win size_win;
  MPI_File fh;
  record *records;
  int global_rank;
  int global_np;
  int local_rank;
  int local_np;
  int reader_id;
  int sublocal_np;
  int sublocal_rank;
  char** batch_ptrs;
  int* send_sizes;
  int* sizes;
  int total_byte_size;
  int batch_size;
  int subbatch_size;
  int reader_size;
  int local_reader_size;
  int node_size;
  int fetch_size;
  int* send_displs;
  int* send_counts;
  char* batch_bytes; 
  char* subbatch_bytes;
  int win_size;
  int win_displ;
  int sample_size;
  int num_read_pages;
  bool is_large_dataset;
  bool is_single_reader_per_node;
  MDB_cursor* cursor;
  MDB_env* mdb_env_;
  MDB_txn* mdb_txn;
  MDB_cursor* mdb_cursor;
  MDB_dbi mdb_dbi_;
  MDB_val mdb_key_, mdb_value_;
  int valid_;
  int dist_mode;
  int read_mode;
  char* lmdb_buffer;
  char* meta_buffer;
  int read_pages;
  int min_read_pages, max_read_pages;
  int start_pg;
  int num_missed_pages;
  int num_extra_pages;
  int iter;
  int prefetch;
  int prefetch_count;

  void assign_readers(const char* fname, int batch_size);
  void open_db(const char* fname);
  void send_batch();
  void read_batch();
  void check_diff_batch();
  bool is_reader(int local_rank);
  bool is_reader();
  void set_records();
  void lmdb_touch_pages();
  void lmdb_direct_io(int start_pg, int read_pages);
  void lmdb_load_meta();
  void lmdb_remap_buff();
  MPI_Comm get_io_comm();
  int get_io_np();

#ifdef BENCHMARK
  init_time_t init_time;
  iter_time_t iter_time;
  io_stat read_stat;
  io_stat parse_stat;

  double get_utime(rusage rstart, rusage rend);
  double get_stime(rusage rstart, rusage rend);
  double get_sltime(double ttime, double utime, double stime);
  double get_ctx_switches(rusage rstart, rusage rend);
  double get_inv_ctx_switches(rusage rstart, rusage rend);
  double get_elapsed_time(double start, double end);
#endif
  
  void check_lmdb(int success, const char* msg, bool verbose = true) {
    if (success == 0) {
      if (verbose)
        std::cout << "Successfully " << msg << std::endl;
    }
    else {
      std::cout << "Not successfully " << msg << std::endl;
    }
  }

  void printl(const char* msg) {
    std::cout << msg << std::endl;
  }

  void lmdb_seek(MDB_cursor_op op) {
    int mdb_status = mdb_cursor_get(mdb_cursor, &mdb_key_, &mdb_value_, op);
    if (mdb_status == MDB_NOTFOUND) {
      valid_ = false;
    } else {
      check_lmdb(mdb_status, "Seek", false);
      valid_ = true;
    }
  }

  void lmdb_get_current() {
    mdb_cursor_get(mdb_cursor, &mdb_key_, &mdb_value_,
        MDB_GET_CURRENT);
  }

  void lmdb_seek_to_first() {
      lmdb_seek(MDB_FIRST);
  }

  void lmdb_next() {
      lmdb_seek(MDB_NEXT);
  }

  void lmdb_seek_multiple(int skip_size) {
    int mdb_status = 0;
    for (int i = 0; i < skip_size; i++) {
      mdb_status = mdb_cursor_get(mdb_cursor, NULL, NULL, MDB_NEXT);
      if (mdb_status == MDB_NOTFOUND) {
        lmdb_seek_to_first();
      } else {
        check_lmdb(mdb_status, "Seek multiple", false);
      }
    }
  }

  void lmdb_next_fetch() {
    lmdb_seek_multiple((reader_size - 1) * fetch_size);
  }

  size_t lmdb_value_size() {
    return mdb_value_.mv_size;
  }

  void* lmdb_value_data() {
    return mdb_value_.mv_data;
  }

  void lmdb_init_cursor() {
    int offset = 0;
    lmdb_seek_to_first();
#if !defined(ICPADS) || !defined(DIRECTIO)
    /* shift the cursor */
    if (reader_id != 0)
      lmdb_seek_multiple(reader_id * fetch_size);
#endif
   }

  string key() {
    return string(static_cast<const char*>(mdb_key_.mv_data), mdb_key_.mv_size);
  }
};

};

#endif  /* LMDBIO_H_INCLUDED */
