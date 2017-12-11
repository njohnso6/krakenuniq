/*
 * Copyright 2017, Florian Breitwieser
 *
 * This file is part of the KrakenHLL taxonomic sequence classification system.
 *
 * KrakenHLL is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * KrakenHLL is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Kraken.  If not, see <http://www.gnu.org/licenses/>.
 */
/*
 * hyperloglogplus.h
 *
 * Implementation of HyperLogLog++ algorithm by Flajolet et al., 
 *   with extensions described by Stefan Heule et al. and Otmar Ertl
 *
 *  Created on: Apr 25, 2015
 *      Author: fbreitwieser
 */

#ifndef HYPERLOGLOGPLUS_H_
#define HYPERLOGLOGPLUS_H_

#include<vector>
#include<stdexcept>
#include<iostream>
#include<fstream>
#include<math.h>    //log
#include<algorithm> //vector.count
#include<bitset>
#include<unordered_set>

#include "hyperloglogbias.h"
#include "assert_helpers.h"

using namespace std;

//#define HLL_DEBUG
//#define NDEBUG
//#define HLL_DEBUG2

#ifdef HLL_DEBUG 
#define D(x) x
#else 
#define D(x)
#endif


#define arr_len(a) (a + sizeof a / sizeof a[0])

// experimentally determined threshold values for  p - 4
static const uint32_t threshold[] = {
  10,      // precision 4
  20, 
  40, 
  80, 
  220, 
  400, 
  900, 
  1800, 
  3100,
  6500, 
  11500, 
  20000, 
  50000, 
  120000, 
  350000  // precision 18
};

///////////////////////

//
/**
 * Gives the estimated cardinality for m bins, v of which are non-zero
 * using linear counting of Whang et al., 1990: n_hat = -m ln(v)
 * @param m number of bins in the matrix
 * @param v number of non-zero bins
 * @return
 */
double linearCounting(const uint32_t m, const uint32_t v) {
  if (v > m) {
      throw std::invalid_argument("number of v should not be greater than m");
  }
  return double(m) * log(double(m)/double(v));
}

/**
 * from Numerical Recipes, 3rd Edition, p 352
 * Returns hash of u as a 64-bit integer.
 *
 */
inline uint64_t ranhash (uint64_t u) {
  uint64_t v = u * 3935559000370003845 + 2691343689449507681;
  v ^= v >> 21; v ^= v << 37; v ^= v >>  4;
  v *= 4768777513237032717;
  v ^= v << 20; v ^= v >> 41; v ^= v <<  5;

  return v;
}

/**
 * Avalanche mixer/finalizer from MurMurHash3
 * https://github.com/aappleby/smhasher
 * from https://github.com/aappleby/smhasher/blob/master/src/MurmurHash3.cpp
 */
inline uint64_t murmurhash3_finalizer (uint64_t key)  {
  key += 1; // murmurhash returns a hash value of 0 for the key 0 - avoid that.
  key ^= key >> 33;
  key *= 0xff51afd7ed558ccd;
  key ^= key >> 33;
  key *= 0xc4ceb9fe1a85ec53;
  key ^= key >> 33;
  return key;
}

/*
  64-bit mixer developed by Thomas Wang
  Proposed for HLL by https://github.com/dnbaker/hll
  https://gist.github.com/badboy/6267743
*/
inline uint64_t wang_mixer(uint64_t key) {
  key = (~key) + (key << 21); // key = (key << 21) - key - 1;
  key = key ^ (key >> 24);
  key = (key + (key << 3)) + (key << 8); // key * 265
  key = key ^ (key >> 14);
  key = (key + (key << 2)) + (key << 4); // key * 21
  key = key ^ (key >> 28);
  key = key + (key << 31);
  return key;
}

/**
 * Bias correction factors for specific m's
 * @param m
 * @return
 */
double alpha(uint32_t m)  {
  switch (m) {
  case 16: return 0.673;
  case 32: return 0.697;
  case 64: return 0.709;
  }

  // m >= 128
  return 0.7213 / (1 + 1.079/double(m));
}

/**
 * calculate the raw estimate as harmonic mean of the ranks in the register
 */
inline double calculateRawEstimate(const vector<uint8_t>& M) {
  double inverseSum = 0.0;
  for (size_t i = 0; i < M.size(); ++i) {
    inverseSum += 1. / (1ull << M[i]);
  }
  return alpha(M.size()) * double(M.size() * M.size()) * 1. / inverseSum;
}

uint32_t countZeros(vector<uint8_t> s) {
  return (uint32_t)count(s.begin(), s.end(), 0);
}

/**
 * Extract bits (from uint32_t or uint64_t) using LSB 0 numbering from hi to lo, including lo
 */
template<typename T>
T extractBits(T value, uint8_t hi, uint8_t lo, bool shift_left = false) {

    // create a bitmask:
    //            (T(1) << (hi - lo)                 a 1 at the position (hi - lo)
    //           ((T(1) << (hi - lo) - 1)              1's from position 0 to position (hi-lo-1)
    //          (((T(1) << (hi - lo)) - 1) << lo)      1's from position lo to position hi

  // The T(1) is required to not cause overflow on 32bit machines
  // TODO: consider creating a bitmask only once in the beginning
  T bitmask = (((T(1) << (hi - lo)) - 1) << lo);
    T result = value & bitmask;

    if (!shift_left) {
        // shift resulting bits to the right
        result = result >> lo;
    } else {
        // shift resulting bits to the left
        result = result << (sizeof(T)*8 - hi);
    }
    return result;  
}



inline uint64_t extractHighBits(uint64_t bits, uint8_t hi) {
  return bits >> (64u - hi);
}

inline uint32_t extractHighBits(uint32_t bits, uint8_t hi) {
  return bits >> (32u - hi);
}



template<typename SET>
inline 
void insert_hash(SET& uset, const uint32_t val, const uint8_t pPrime) {
  // this implementation currently does not check if there is a value for an index of length pPrime
  uset.insert(val);
}



inline 
void insert_hash(vector<uint32_t>& vec, const uint32_t val, const uint8_t pPrime) {
  //if (sparseList_is_sorted) {
  auto it = std::lower_bound( vec.begin(), vec.end(), val); // find proper position in descending order
  if (it == vec.end()) { // position at the end
    vec.insert( it, val ); // insert before iterator it
  } else if (*it != val) {      // val not in the vector
    if (extractHighBits(val,pPrime) == extractHighBits(*it,pPrime)) { //the values have the same index
	  // it's pretty unlikely to observe different hashes with the same index
	  // pPrime is 25, which means that the chance of another random number
	  // having the same index is 1/2^25 or about 1/33,500,000. Sparse representation
	  // doesn't go that high, though.
	  // Maybe a hash should be added even if it has the same index? 
	  // (TODO: Check how often this occurs and the relative errors of the estimates)
	  
      if ((*it & 1) == (val & 1)) { // if both are in the same category
        if ((val & 1) == 1) { // both have ones as lsb - replace if val is greater
          if (val > *it) *it = val;
        } else {           // both have zeros as lsb - replace if val is smaller
          if (val < *it) *it = val;
        }
      } else if ((val & 1) == 1) { // replace if lsb of val is 1
        *it = val;
      }
    } else {
      vec.insert( it, val ); // insert before iterator it
    }
  }
  //} else {
  //  vec.push_back(val);
  //}
}

/*
inline 
void merge_lists(vector<uint32_t>& vec1, const vector<uint32_t>& vec2) {
  auto it = std::lower_bound( vec.begin(), vec.end(), val); // find proper position in descending order
  if (it == vec.end()) {
    vec.insert( it, val ); // insert before iterator it
  }
}
*/

// functions for counting the number of leading 0-bits (clz)
//           and counting the number of trailing 0-bits (ctz)
//#ifdef __GNUC__

// TODO: switch between builtin clz and 64_clz based on architecture
//#define clz(x) __builtin_clz(x)
#if 0
static int clz_manual(uint64_t x)
{
  // This uses a binary search (counting down) algorithm from Hacker's Delight.
   uint64_t y;
   int n = 64;
   y = x >>32;  if (y != 0) {n -= 32;  x = y;}
   y = x >>16;  if (y != 0) {n -= 16;  x = y;}
   y = x >> 8;  if (y != 0) {n -=  8;  x = y;}
   y = x >> 4;  if (y != 0) {n -=  4;  x = y;}
   y = x >> 2;  if (y != 0) {n -=  2;  x = y;}
   y = x >> 1;  if (y != 0) return n - 2;
   return n - x;
}
#endif

#ifdef WIN32
#include <intrin.h>
#define __builtin_clz(x) __lzcnt(x)
#define __builtin_clzl(x) __lzcnt64(x)
#endif

inline uint8_t clz(const uint32_t x) {
  if (x == 0) { return 32; }
  return __builtin_clz(x);
}

inline uint8_t clz(const uint64_t x) {
  if (x == 0) { return 64; }
  return __builtin_clzl(x);
}
//#else
//#endif

template<typename T>
struct NoHash {
  size_t operator() (const T &u) const {
    return u;
  }
};


// Heule et al. encode the sparse list with variable length encoding
//   see section 5.3.2. This implementation just uses a sorted vector or unordered_set.
typedef unordered_set<uint32_t> SparseListType;
// Other possible SparseList types:
// // typedef vector<uint32_t> SparseListType;
// The sorted vector implementation is pretty inefficient currently, because the vector
// is always kept sorted. 
// //typedef unordered_set<uint32_t, NoHash<uint32_t> > SparseListType;
// No real performance gain from using the hash value directly - and probably problems
// with the bucket assignment, since unordered_set expects size_t hashes

/**
 * HyperLogLogPlusMinus class
 * typename T corresponds to the hash size - usually either uint32_t or uint64_t (implemented for uint64_t)
 */
typedef uint64_t T_KEY;
template <typename T_KEY>
class HyperLogLogPlusMinus {

private:

  vector<uint8_t> M;    // registers, size m
  uint8_t p;            // precision
  uint32_t m = 1 << p;  // number of registers
  bool sparse;          // sparse representation of the data?
  SparseListType sparseList;
  uint64_t  (*bit_mixer) (uint64_t);

  // sparse versions of p and m
  static const uint8_t  pPrime = 25; // precision when using a sparse representation
                                     // fixed to 25, because 25 + 6 bits for rank + 1 flag bit = 32
  static const uint32_t mPrime = 1 << (pPrime -1); // 2^pPrime


public:

  ~HyperLogLogPlusMinus() {};

  /**
   * Create new HyperLogLogPlusMinus counter
   * @param precision
   * @param sparse
   */
  HyperLogLogPlusMinus(uint8_t precision=12, bool sparse=true, uint64_t  (*bit_mixer) (uint64_t) = murmurhash3_finalizer):
      p(precision), m(1<<precision), sparse(sparse), bit_mixer(bit_mixer) {
    if (precision > 18 || precision < 4) {
          throw std::invalid_argument("precision (number of register = 2^precision) must be between 4 and 18");
    }

    //this->m = 1 << precision;

    if (sparse) {
      this->sparseList = SparseListType(); // TODO: if SparseListType is changed, initialize with appropriate size
      this->sparseList.reserve(m/4);
    } else {
      this->M = vector<uint8_t>(m);
    }
  }

  /**
   * Add a new item to the counter.
   * @param item
   */
  void add(T_KEY item) {
    // compute hash for item
    uint64_t hash_value = bit_mixer(item);

#ifdef HLL_DEBUG2
    cerr << "Value: " << item << "; hash(value): " << hash_value << endl;
    cerr << bitset<64>(hash_value) << endl;
#endif

    if (sparse) {
      // sparse mode: put the encoded hash into sparse list
      uint32_t encoded_hash_value = encodeHashIn32Bit(hash_value);
      insert_hash(sparseList, encoded_hash_value, pPrime);

#ifdef HLL_DEBUG2
      cerr << "encoded hash:   " << bitset<32>(encoded_hash_value) << endl;
      idx_n_rank ir = getIndexAndRankFromEncodedHash(encoded_hash_value);
      assert_eq(ir.idx,get_index(hash_value, p));
      assert_eq(ir.rank, get_rank(hash_value, p));
#endif

      // if the sparseList is too large, switch to normal (register) representation
      if (this->sparseList.size() > this->m/4) {
        switchToNormalRepresentation();
      }
    } else {
      // normal mode
      // take first p bits as index  {x63,...,x64-p}
      uint32_t idx = get_index(hash_value, p);
      // shift those p values off, and count leading zeros of the remaining string {x63-p,...,x0}
      uint8_t rank = get_rank(hash_value, p);

      // update the register if current rank is bigger
      if (rank > this->M[idx]) {
        this->M[idx] = rank;
      }
    }
  }

  void add(vector<T_KEY> words) {
    for(size_t i = 0; i < words.size(); ++i) {
      this->add(words[i]);
    }
  }

  /**
   * Reset to its initial state.
   */
  void reset() {
    this->sparse = true;
    this->sparseList.clear();  // 
    this->M.clear();
  }

  /**
   * Convert from sparse representation (using tmpSet and sparseList) to normal (using register)
   */
  void switchToNormalRepresentation() {
#ifdef HLL_DEBUG
    cerr << "switching to normal representation" << endl;
    cerr << " est before: " << cardinality() << endl;
#endif
    this->sparse = false;
    this->M = vector<uint8_t>(this->m);
    if (sparseList.size() > 0) {
      addToRegisters(this->sparseList);
      this->sparseList.clear();
    }
#ifdef HLL_DEBUG
    cerr << " est after: " << cardinality() << endl;
#endif
  }

  /**
   * add sparseList to the registers of M
   */
  void addToRegisters(const SparseListType &sparseList) {
    if (sparseList.size() == 0) {
      return;
    }
    for (SparseListType::const_iterator encoded_hash_value_ptr = sparseList.begin(); encoded_hash_value_ptr != sparseList.end(); ++encoded_hash_value_ptr) {

      idx_n_rank ir = getIndexAndRankFromEncodedHash(*encoded_hash_value_ptr);

      assert_lt(ir.idx,M.size());
      if (ir.rank > this->M[ir.idx]) {
        this->M[ir.idx] = ir.rank;
      }
    }
  }

  /**
   * Merge another HyperLogLogPlusMinus into this. Converts to normal representation
   * @param other
   */
  void merge(const HyperLogLogPlusMinus* other) {
    if (this->p != other->p) {
      throw std::invalid_argument("precisions must be equal");
    }

    if (this->sparse && other->sparse) {
      if (this->sparseList.size()+other->sparseList.size() > this->m) {
        // TODO: this switches to normal representation too soon if there is duplication
        switchToNormalRepresentation();
        addToRegisters(other->sparseList);
      } else {
        
        for (const auto val : other->sparseList) {
          insert_hash(this->sparseList, val, pPrime);
        }
      }
    } else if (other->sparse) {
      // other is sparse, but this is not
      addToRegisters(other->sparseList);
    } else {
      if (this->sparse) {
        switchToNormalRepresentation();
      }
      // merge registers
      for (size_t i = 0; i < other->M.size(); ++i) {
        if (other->M[i] > this->M[i]) {
          this->M[i] = other->M[i];
        }
      }
    }
  }

  HyperLogLogPlusMinus & operator+=(const HyperLogLogPlusMinus* other) {
    merge(other);
    return *this;
  }

  HyperLogLogPlusMinus & operator+=(const HyperLogLogPlusMinus& other) {
    merge(&other);
    return *this;
  }

  /**
   *
   * @return cardinality estimate
   */
  uint64_t cardinality() const {
    if (sparse) {
      // if we are 'sparse', then use linear counting with increased precision pPrime
      D(cerr << "sparse representation - return linear counting estimate" << endl; )
      return uint64_t(linearCounting(mPrime, mPrime-uint32_t(sparseList.size())));
    }

    // use linear counting (lc) estimate if there are zeros in the matrix
    //  AND the lc estimate is smaller than an empirically defined threshold
    uint32_t v = countZeros(M);
    if (v != 0) {
      uint64_t lc_estimate = linearCounting(m, v);
      D(cerr << "linear counting estimate: " << lc_estimate << endl;)
      // check if the lc estimate is below the threshold
      assert(lc_estimate >= 0);
      if (lc_estimate <= double(threshold[p-4])) {
        D(cerr << "below threshold - return it " << endl;)
        return lc_estimate;
      }
      D(cerr << "above threshold of " << threshold[p-4] << " - calculate raw estimate " << endl;)
    }

    // calculate raw estimate on registers
    //double est = alpha(m) * harmonicMean(M, m);
    double est = calculateRawEstimate(M);
    D(cerr << "raw estimate: " << est << endl;)
    // correct for biases if estimate is smaller than 5m
    if (est <= double(m)*5.0) {
      D(cerr << "correct bias; subtract " << getEstimateBias(est) << endl;)
      assert(est > getEstimateBias(est));
      est -= getEstimateBias(est);
    }

    return std::round(est);
  }

  // HyperLogLog++ cardinality estimator of Heule et al., 2015
  uint64_t heuleCardinality() {
    return cardinality();
  }

  // returns a 'register histogram' vector C,
  //  where C[i] is the number of elements in M with value i
  //  it's size is q+1 = 64-p+1
  // used in Ertl's improved estimator
  vector<int> registerHistogram(const vector<uint8_t>& M, size_t q) {
    vector<int> C(q+2, 0);
    for (size_t i = 0; i < m; ++i) {
      if (M[i] >= q+1) {
        cerr << "M["<<i<<"] == " << M[i] << "! larger than " << (q+1) << endl;
      }
      ++C[M[i]]; 
    }
    #ifdef HLL_DEBUG
    cerr << "C = {";
    for (size_t i = 0; i < C.size(); ++i) {
      cerr <<C[i] << ';';
    }
    cerr << "}" << endl;
    #endif
    assert(std::accumulate(C.begin(), C.end(), 0) == m);
    return C;
  }

  vector<int> sparseRegisterHistogram(const SparseListType& sparseList, size_t m, size_t q) {
    vector<int> C(q+2, 0);
    C[0] = m;
    for (const auto& enc_hash : sparseList) {
      idx_n_rank ir  = getIndexAndRankFromEncodedHash(enc_hash);
      ++C[ir.rank]; 
      --C[0];
    }
    return C;
  }

  // Ertl - calculation of sigma correction for 0-registers in M
  //  x is the proportion of 0-registers, thus x \in [0, 1]
  //  sigma := x + sum[from k=1 to Inf] ( x^(2^k) * 2^(k-1) )
  double sigma(double x) {
    assert(x >= 0.0 && x <= 1.0);
    if (x == 1.0) { return std::numeric_limits<double>::infinity(); }
    
    double prev_sigma_x;
    double sigma_x = x;
    double y  = 1.0;
    do { // loop until sigma_x does not change anymore
      prev_sigma_x = sigma_x;
      x *= x; // gives x^(2^k)
      sigma_x += x * y;
      y += y; // gives 2^(k-1)
    } while (sigma_x != prev_sigma_x);
    return sigma_x;
  }

  double sigma_mod(double x) {
    assert(x >= 0.0 && x <= 1.0);
    if (x == 1.0) { return std::numeric_limits<double>::infinity(); }
    
    double sigma_x = x;
    for (double x_sq = x*x, two_exp = 1.0;
         x_sq > std::numeric_limits<double>::epsilon();
         x_sq *= x_sq, two_exp += two_exp) {
      sigma_x += x_sq * two_exp;
    } 
    return sigma_x;
  }

  // Ertl - calculation of tau correction for values higher then q in M
  //  x is the proportion of registers with a value below q in M, thus x \in [0,1]
  //  tau := 1/3 (1 - x - sum[from k=1 to Inf](1-x^(2^(-k))^2 * 2^(-k) )) 
  double tau(double x) {
    assert(x >= 0.0 && x <= 1.0);
    if (x == 0.0 || x == 1.0) { return 0.0; }
    
    double prev_tau_x;
    double y = 1.0;
    double tau_x = 1 - x; 
    do { // loop until tau_x does not change anymore
      prev_tau_x = tau_x;
      x = std::sqrt(x); // gives x^(2^-k)
      y /= 2.0;         // gives 2^(-k)
      tau_x -= std::pow(1-x, 2) * y;
    } while (tau_x != prev_tau_x);
    return tau_x / 3.0;
  }

  // Improved cardinality estimator of Ertl, 2017 (arXiv, section 4)
  //  Based on the underlying distribution, the estimator employs correction 
  //  factors for zero and 'over-subscribed' registers. It does not depend on 
  //  emprically defined bias correction values or a switch between linear 
  //  counting and loglog estimation

  // Formula:
  //                                 alpha_inf * m^2 
  // --------------------------------------------------------------------------------------
  // ( m * sigma(C_0/m) + sum[from k=1 to q] C_k * 2^(-k) + m * tau(1-C_(q+1)/m) * 2^(-q)
  uint64_t ertlCardinality() {
    size_t q, m;
    vector<int> C;
    if (sparse) {
      q = 64  - pPrime;
      m = mPrime;
      C = sparseRegisterHistogram(sparseList, m, q);
    } else {
      q = 64 - p;
      m = this->m;
      C = registerHistogram(M, q);
    }
  
    D(cerr << "\n1. hist. q=" << q << "; m=" << m << endl;)
    D(cerr << "2. m * tau(m*(1.0 - "<<C[q+1]<<"/m) = "; )
    double est_denominator = m * tau(1.0-double(C[q+1])/double(m));
    D(cerr << est_denominator << endl;)
    D(cerr << "3. loop! " ;)
    for (int k = q; k >= 1; --k) {
      est_denominator += C[k];
      est_denominator *= 0.5;
    }
    D(cerr << est_denominator << endl; )
    D(cerr << "4. sigma(" << C[0] << "/m) = " << sigma(double(C[0])/double(m)); )
    est_denominator += m * sigma(double(C[0])/double(m));
    D(cerr << endl;)
    double m_sq_alpha_inf = (m / (2.0*std::log(2))) * m;
    return std::round(m_sq_alpha_inf / est_denominator);
    
  }


private:

    inline uint32_t get_index(const uint64_t hash_value, const uint8_t p) const {
      // take first p bits as index  {x63,...,x64-p}
      return hash_value >> (64 - p);
    }

    inline uint32_t get_index(const uint32_t hash_value, const uint8_t p) const {
      // take first p bits as index  {x31,...,x32-p}
      return hash_value >> (32 - p);
    }

    template<typename T> inline
    T get_trailing_ones(const uint8_t p) const {
      return (T(1) << p ) - 1;
    }

    uint8_t get_rank(const uint32_t hash_value, const uint8_t p) const {
      // shift p values off, and count leading zeros of the remaining string {x63-p,...,x0}
      uint32_t rank_bits (hash_value << p | get_trailing_ones<uint32_t>(p));

      // __builtin_clz is undefined when val is zero, but that will not happen here
      uint8_t rank_val = clz(rank_bits) + 1;
      assert_leq(rank_val,32-p+1);
      return rank_val;
    }


    uint8_t get_rank(const uint64_t hash_value, const uint8_t p) const {
      // shift p values off, and count leading zeros of the remaining string {x63-p,...,x0}
      T_KEY rank_bits (hash_value << p | get_trailing_ones<uint64_t>(p));

      uint8_t rank_val = (clz(rank_bits)) + 1;
      assert_leq(rank_val,64-p+1);
      return rank_val;
    }

  vector<double> rawEstimateData(size_t p) const {
    switch (p) {
      case  4: return vector<double>(rawEstimateData_precision4,arr_len(rawEstimateData_precision4));
      case  5: return vector<double>(rawEstimateData_precision5,arr_len(rawEstimateData_precision5));
      case  6: return vector<double>(rawEstimateData_precision6,arr_len(rawEstimateData_precision6));
      case  7: return vector<double>(rawEstimateData_precision7,arr_len(rawEstimateData_precision7));
      case  8: return vector<double>(rawEstimateData_precision8,arr_len(rawEstimateData_precision8));
      case  9: return vector<double>(rawEstimateData_precision9,arr_len(rawEstimateData_precision9));
      case 10: return vector<double>(rawEstimateData_precision10,arr_len(rawEstimateData_precision10));
      case 11: return vector<double>(rawEstimateData_precision11,arr_len(rawEstimateData_precision11));
      case 12: return vector<double>(rawEstimateData_precision12,arr_len(rawEstimateData_precision12));
      case 13: return vector<double>(rawEstimateData_precision13,arr_len(rawEstimateData_precision13));
      case 14: return vector<double>(rawEstimateData_precision14,arr_len(rawEstimateData_precision14));
      case 15: return vector<double>(rawEstimateData_precision15,arr_len(rawEstimateData_precision15));
      case 16: return vector<double>(rawEstimateData_precision16,arr_len(rawEstimateData_precision16));
      case 17: return vector<double>(rawEstimateData_precision17,arr_len(rawEstimateData_precision17));
      case 18: return vector<double>(rawEstimateData_precision18,arr_len(rawEstimateData_precision18));
    }
    return vector<double>();
  }

  vector<double> biasData(size_t p) const {
    switch(p) {
      case  4: return vector<double>(biasData_precision4,arr_len(biasData_precision4));
      case  5: return vector<double>(biasData_precision5,arr_len(biasData_precision5));
      case  6: return vector<double>(biasData_precision6,arr_len(biasData_precision6));
      case  7: return vector<double>(biasData_precision7,arr_len(biasData_precision7));
      case  8: return vector<double>(biasData_precision8,arr_len(biasData_precision8));
      case  9: return vector<double>(biasData_precision9,arr_len(biasData_precision9));
      case 10: return vector<double>(biasData_precision10,arr_len(biasData_precision10));
      case 11: return vector<double>(biasData_precision11,arr_len(biasData_precision11));
      case 12: return vector<double>(biasData_precision12,arr_len(biasData_precision12));
      case 13: return vector<double>(biasData_precision13,arr_len(biasData_precision13));
      case 14: return vector<double>(biasData_precision14,arr_len(biasData_precision14));
      case 15: return vector<double>(biasData_precision15,arr_len(biasData_precision15));
      case 16: return vector<double>(biasData_precision16,arr_len(biasData_precision16));
      case 17: return vector<double>(biasData_precision17,arr_len(biasData_precision17));
      case 18: return vector<double>(biasData_precision18,arr_len(biasData_precision18));
    }
    return vector<double>();
  }

  /**
   * Estimate the bias of raw estimate using empirically determined values.
   * Uses weighted average of the two cells between which the estimate falls.
   * TODO: Check if nearest neighbor average gives better values, as proposed in the paper
   * @param est
   * @return correction value for
   */
  double getEstimateBias(double estimate) const {
    vector<double> rawEstimateTable = rawEstimateData(p);
    vector<double> biasTable = biasData(p);
  
    // check if estimate is lower than first entry, or larger than last
    if (rawEstimateTable.front() >= estimate) { return biasTable.front(); }
    if (rawEstimateTable.back()  <= estimate) { return biasTable.back(); }
  
    // get iterator to first element that is not smaller than estimate
    vector<double>::const_iterator it = lower_bound(rawEstimateTable.begin(),rawEstimateTable.end(),estimate);
    size_t pos = it - rawEstimateTable.begin();

    double e1 = rawEstimateTable[pos-1];
    double e2 = rawEstimateTable[pos];
  
    double c = (estimate - e1) / (e2 - e1);
    D(cerr << "bias correction factor c: (estimate - e1)/ (e2 - e1) = (" << estimate << " - " << e1 << ")/(" << e2 << " - " << e1 << ")" << endl;) 
    D(cerr << "biasTable[" << pos-1 << "]*(1-c) + biasTable[" << pos << "]*c" << endl; )
    return biasTable[pos-1]*(1-c) + biasTable[pos]*c;
  }
  

  /**
   * Encode the 64-bit hash code x as an 32-bit integer, to be used in the sparse representation.
   *
   * Difference from the algorithm described in the paper:
   * The index always is in the p most significant bits
   *
   * see section 5.3 in Heule et al.
   * @param x the hash bits
   * @return encoded hash value
   */
  inline
  uint32_t encodeHashIn32Bit(uint64_t hash_value) {
    // extract first pPrime bits as index
    uint32_t idx = (uint32_t)(extractHighBits(hash_value,pPrime) << (32-pPrime));

#ifdef HLL_DEBUG2
    cerr << "encoding hash:  " << bitset<64>(hash_value) << endl;
    cerr << "index':         " << bitset<32>(idx) << " ( bits from 64 to " << 64-pPrime << "; " << idx << ")" << endl;
#endif

    // are the bits after bit p in index' all 0?
    if (idx << p == 0) {
      // compute the additional rank (minimum rank is already p'-p)
      // the maximal size will be below 2^6=64. We thus combine the 25 bits of the index with 6 bits for the rank, and one bit as flag
      uint8_t additional_rank = get_rank(hash_value, pPrime); // this is rank - (p'-p), as we know that positions p'...p are 0
#ifdef HLL_DEBUG2
      cerr << "All zero " << endl;
#endif
      return idx | uint32_t(additional_rank<<1) | 1;
    } else {
      // else, return the idx, only - it has enough length to calculate the rank (left-shifted, last bit = 0)
      assert_eq((idx & 1),0);
      return idx;
    }
  }


  /**
   * struct holding the index and rank/rho of an entry
   */
  struct idx_n_rank {
    uint32_t idx;
    uint8_t rank;
    idx_n_rank(uint32_t _idx, uint8_t _rank) : idx(_idx), rank(_rank) {}
  };

  //
  //
  /**
   * Decode hash from sparse representation.
   * Returns the index and number of leading zeros with precision p stored in k
   * @return index and rank in non-sparse format
   */
  idx_n_rank getIndexAndRankFromEncodedHash(const uint32_t encoded_hash_value) const  {

    // difference to paper: Index can be recovered in the same way for pPrime and normally encoded hashes
    uint32_t idx = get_index(encoded_hash_value, p);
    uint8_t rank_val;

    // check if the least significant bit is 1
    if ( (encoded_hash_value & 1) == 1) {
      // if yes: the hash was stored with higher precision, bits p to pPrime were 0
      uint8_t additional_rank = pPrime - p;
      rank_val = additional_rank + extractBits(encoded_hash_value, 7, 1);
    } else {
      rank_val = get_rank(encoded_hash_value,p);

      // clz counts 64 bit only, it seems
      //assert_leq(rank_val,32);
    }

    return(idx_n_rank(idx,rank_val));
  }

};




#endif /* HYPERLOGLOGPLUS_H_ */
