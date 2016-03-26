/*
 * Copyright (C) 2016 Dato, Inc.
*
* This program is free software: you can redistribute it and/or modify
* it under the terms of the GNU Affero General Public License as
* published by the Free Software Foundation, either version 3 of the
* License, or (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU Affero General Public License for more details.
*
* You should have received a copy of the GNU Affero General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
#ifndef GRAPHLAB_2D_SPARSE_PARALLEL_ARRAY
#define GRAPHLAB_2D_SPARSE_PARALLEL_ARRAY

#include <graphlab/util/bitops.hpp>
#include <util/cityhash_gl.hpp>
#include <parallel/pthread_tools.hpp>
#include <parallel/atomic.hpp>
#include <parallel/lambda_omp.hpp>
#include <sparsehash/dense_hash_set>

namespace graphlab {

/** A sparse array structure
 */
template <typename T>
class sparse_parallel_2d_array {
public:

  typedef T value_type;

  sparse_parallel_2d_array(size_t n_rows, size_t n_cols)
      : kv_temp_container_v(thread::cpu_count())
  {
    resize(n_rows, n_cols);
  }

  size_t rows() const { return n_rows; }
  size_t cols() const { return n_cols; }

  /** Provides concurrant access to a particular element.  Must be
   * through the apply function; it is assumed that all changes to the
   * element are completed when the element
   */
  template <typename ApplyFunction>
  GL_HOT
  void apply(size_t i, size_t j, ApplyFunction&& apply_f) {
    DASSERT_LT(i, rows());
    DASSERT_LT(j, cols());

    size_t thread_idx = thread::thread_id();

    kv_container& kv = kv_temp_container_v[thread_idx];
    kv.set_key(i, j, n_col_bits);

    size_t base_idx = get_first_level_hash(i, j, kv);

    auto& hs = hash_maps[base_idx];

    std::lock_guard<simple_spinlock> lg(hs.access_lock);

    auto ret = hs.hash_map.insert(std::move(kv));

    apply_f(ret.first->value);
  }

  /** Provides non-locking access to a particular element.  Cannot be
   * used in parallel.
   */
  GL_HOT_INLINE_FLATTEN
  T& operator()(size_t i, size_t j) {
    DASSERT_LT(i, rows());
    DASSERT_LT(j, cols());

    kv_container& kv = kv_temp_container_v[0];
    kv.set_key(i, j, n_col_bits);

    size_t base_idx = get_first_level_hash(i, j, kv);

    auto& hs = hash_maps[base_idx];
    auto ret = hs.hash_map.insert(std::move(kv));

    return ret.first->value;
  }

  /** Calls apply_f, in parallel, for every value currently in the
   *  table.  The signature of the apply function is assumed to be
   *  apply_f(size_t i, size_t j, const T& t); Note this is the const
   *  overload.
   *
   *  The storage and scheduling gaurantees that each unique value of
   *  i is called within the same thread. In other words, there are
   *  never two simultaneous calls to apply_f with the same value of
   *  i.
   */
  template <typename ApplyFunction>
  void apply_all(ApplyFunction&& apply_f) const {

    atomic<size_t> current_block_idx = 0;

    in_parallel([&](size_t thread_idx, size_t num_threads)
                GL_GCC_ONLY(GL_HOT_NOINLINE_FLATTEN) {

        while(true) {
          size_t block_idx = (++current_block_idx) - 1;

          if(block_idx >= n_thread_blocks) {
            break;
          }

          size_t start_idx = n_levels_per_block * block_idx;
          size_t end_idx = n_levels_per_block * (block_idx + 1);

          for(size_t i = start_idx; i < end_idx; ++i) {
            const hash_block& hb = hash_maps[i];

            for(auto it = hb.hash_map.begin(); it != hb.hash_map.end(); ++it) {
              const kv_container& kv = *it;
              const auto idx_pair = kv.get_indices(n_col_bits);
              const auto& value = kv.value;
              apply_f(idx_pair.first, idx_pair.second, value);
            }
          }
        }
      });
  }

  /** Calls apply_f, in parallel, for every value currently in the
   *  table.  The signature of the apply function is assumed to be
   *  apply_f(size_t i, size_t j, T& t);
   *
   *  The storage and scheduling gaurantees that each unique value of
   *  i is called within the same thread. In other words, there are
   *  never two simultaneous calls to apply_f with the same value of
   *  i.
   *
   *  mutable overload.
   */
  template <typename ApplyFunction>
  void apply_all(ApplyFunction&& apply_f) {

    atomic<size_t> current_block_idx = 0;

    in_parallel([&](size_t thread_idx, size_t num_threads)
                GL_GCC_ONLY(GL_HOT_NOINLINE_FLATTEN) {
        while(true) {
          size_t block_idx = (++current_block_idx) - 1;

          if(block_idx >= n_thread_blocks) {
            break;
          }

          size_t start_idx = n_levels_per_block * block_idx;
          size_t end_idx = n_levels_per_block * (block_idx + 1);

          for(size_t i = start_idx; i < end_idx; ++i) {
            const hash_block& hb = hash_maps[i];

            for(auto it = hb.hash_map.begin(); it != hb.hash_map.end(); ++it) {
              const kv_container& kv = *it;
              const auto idx_pair = kv.get_indices(n_col_bits);
              apply_f(idx_pair.first, idx_pair.second, kv.value);
            }
          }
        }
      });
  }


  void clear() {
    parallel_for(size_t(0), hash_maps.size(), [&](size_t i) {
        hash_maps[i].hash_map.clear();
      });
  }

  void resize(size_t _n_rows, size_t _n_cols) {
    n_cols = _n_cols;
    n_rows = _n_rows;
    n_col_bits = bitwise_log2_ceil(n_cols + 1);
  }

////////////////////////////////////////////////////////////////////////////////

 private:

  ////////////////////////////////////////////////////////////////////////////////
  // The internal data structures to make this efficient.

  struct kv_container {
    size_t key = 0;
    mutable T value;

    // For the empty key, use the hash of
    static kv_container as_empty() {
      kv_container kv;
      kv.key = index_hash(0); // Will never occur in practice, as we
                              // add 1 to the key.
      kv.value = T();
      return kv;
    }

    ////////////////////////////////////////

    inline bool operator==(const kv_container& kv) const {
      return key == kv.key;
    }

    /** Sets the key.
     */
    void set_key(size_t i, size_t j, size_t n_col_bits) GL_HOT_INLINE_FLATTEN {
      key = index_hash((i << n_col_bits) + j + 1);

#ifndef NDEBUG
      auto p = get_indices(n_col_bits);
      DASSERT_EQ(p.first, i);
      DASSERT_EQ(p.second, j);
#endif
    }

    // Get the indices
    inline std::pair<size_t, size_t> get_indices(size_t n_col_bits) const GL_HOT_INLINE_FLATTEN {
      size_t idx = reverse_index_hash(key) - 1;
      return std::pair<size_t, size_t>{(idx >> n_col_bits), idx & bit_mask<size_t>(n_col_bits)};
    }
  };

  kv_container empty_container;

  // The goal of this two-level system is to both allow us to have
  // each row index be always called within the same thread, and to
  // minimize collisions among writing threads.
  static constexpr size_t n_thread_block_bits = 6;
  static constexpr size_t n_levels_per_block_bits = 5;
  static constexpr size_t n_thread_blocks = (size_t(1) << n_thread_block_bits);
  static constexpr size_t n_levels_per_block = (size_t(1) << n_levels_per_block_bits);
  static constexpr size_t n_level_bits = n_thread_block_bits + n_levels_per_block_bits;
  static constexpr size_t n_levels = (size_t(1) << n_level_bits);

  GL_HOT_INLINE_FLATTEN
  inline size_t get_first_level_hash(size_t i, size_t j, const kv_container& kv) const {

    // The first index points to the thread block we end up using.
    // All values within each block will be accessed by the same
    // thread in the apply_all call.  After that, it's randomized to
    // reduce thread contention.
    size_t first_idx = i & bit_mask<size_t>(n_thread_block_bits);
    DASSERT_LT(first_idx, n_thread_blocks);

    size_t second_idx = kv.key >> (bitsizeof(size_t) - n_levels_per_block_bits);
    DASSERT_LT(second_idx, n_levels_per_block);

    size_t base_idx = first_idx * n_levels_per_block + second_idx;
    DASSERT_LT(base_idx, n_levels);
    return base_idx;
  }

  size_t n_rows = 0, n_cols = 0;
  size_t n_col_bits = 0;

  /** Sets the key.
   */
  struct hash_block {
    hash_block() {
      hash_map.set_empty_key(kv_container::as_empty());
    }

    simple_spinlock access_lock;

    struct trivial_kv_container_hash {
      GL_HOT_INLINE_FLATTEN size_t operator()(const kv_container& k) const {
        return k.key;
      }
    };

    google::dense_hash_set<kv_container, trivial_kv_container_hash> hash_map;
  };

  // The first level table for this.
  std::array<hash_block, n_levels> hash_maps;

  // Temporary things to avoid potential reallocations and stuff.
  std::vector<kv_container> kv_temp_container_v;


};

}
#endif /* GRAPHLAB_2D_SPARSE_PARALLEL_ARRAY
 */
