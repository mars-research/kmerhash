/*
 * Copyright 2017 Georgia Institute of Technology
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * hyperloglog64, suitable for estimating the number of hash entries for a hash table.
 *  based on publications
 *  	https://static.googleusercontent.com/media/research.google.com/en//pubs/archive/40671.pdf,
 *  	http://algo.inria.fr/flajolet/Publications/FlMa85.pdf,
 *
 *  and implementations
 *  	https://github.com/hideo55/cpp-HyperLogLog and
 *  	https://github.com/dialtr/libcount.
 *  	not yet SIMD enabled like https://github.com/mindis/hll
 *
 * Since this is for a hash table, several modifications were made that are appropriate for hash tables only.
 * 	1. incorporates 64 bit hash values, allowing bypass of large cardinality correction factor (based on hyperloglog++)
 * 	2. bypass bias corrected estimation, and just do linear counting as needed, for 2 reasons below.  note that this can be turned on later for accuracy.
 * 		a. for small cardinality, overestimating cardinality is not significantly costly.
 * 		b. the hash table's load factor provides a buffer that allows real cardinality to be measured for the final resizing.
 *
 * this is structured as a class because we want to be able to merge instances
 *
 * this implementation is not yet correct:  accuracy of prediction is not within 2 % yet.  would prefer to have a
 *
 *  Created on: Mar 1, 2017
 *      Author: tpan
 */

#ifndef KMERHASH_HYPERLOGLOG64_HPP_
#define KMERHASH_HYPERLOGLOG64_HPP_

#include <array>
#include <stdint.h>
#include <iostream> // std::cout

// FROM https://github.com/hideo55/cpp-HyperLogLog.
// modified to conform to the leftmost 1 bit convention, and faster implementation. and to make macro safer.
// zero -> return 65.  else return b+1
#if defined(__has_builtin) && (defined(__GNUC__) || defined(__clang__))

inline uint8_t leftmost_set_bit(uint64_t x) {
	return (x == 0) ? 65 : static_cast<uint8_t>(::__builtin_clzl(x) + 1);
}

#else

#if defined (_MSC_VER)
inline uint8_t leftmost_set_bit(uint64_t x) {
	if (x == 0) return 65;
    uint64_t b = 64;
    ::_BitScanReverse64(&b, x);
    return static_cast<uint8_t>(b);
}
#else
// from https://en.wikipedia.org/wiki/Find_first_set, extended to 64 bit
static const uint8_t clz_table_4bit[16] = { 4, 3, 2, 2, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0 };
inline uint8_t leftmost_set_bit(uint64_t x) {
  if (x == 0) return 65;
  uint8_t n;
  if ((x & 0xFFFFFFFF00000000UL) == 0) {n  = 32; x <<= 32;} else {n = 0;}
  if ((x & 0xFFFF000000000000UL) == 0) {n += 16; x <<= 16;}
  if ((x & 0xFF00000000000000UL) == 0) {n +=  8; x <<=  8;}
  if ((x & 0xF000000000000000UL) == 0) {n +=  4; x <<=  4;}
  n += clz_table_4bit[x >> 60];  // 64 - 4
  return n+1;
}

#endif /* defined(_MSC_VER) */

#endif /* defined(__GNUC__) */


template <typename T, typename Hash, uint8_t precision = 4>
class hyperloglog64 {
	  static_assert((precision >= 4) && (precision <= 16),
			  "ERROR: precision for hyperloglog should be in [4, 16].");

protected:
	// don't need because of 64 bit.  0xRRVVVVVV  // high bits: reg.  low: values
	static constexpr uint64_t val_mask = ~(0x0UL) >> precision;  // e.g. 0x00FFFFFF
	static constexpr uint64_t nRegisters = 0x1UL << precision;   // e.g. 0x00000100
	mutable double amm;

	using REG_T = uint8_t;
	std::array<REG_T, nRegisters> registers;  // stores count of leading zeros

	Hash h;

public:
	hyperloglog64() {
		registers.fill(static_cast<REG_T>(0));

        switch (precision) {
            case 4:
                amm = 0.673;
                break;
            case 5:
                amm = 0.697;
                break;
            case 6:
                amm = 0.709;
                break;
            default:
                amm = 0.7213 / (1.0 + 1.079 / nRegisters);
                break;
        }
        amm *= static_cast<double>(0x1ULL << (precision + precision));
	}

	hyperloglog64(hyperloglog64 const & other) :
		amm(other.amm), registers(other.registers) {
	}

	hyperloglog64(hyperloglog64 && other) :
		amm(other.amm), registers(std::move(other.registers)) {
	}

	hyperloglog64& operator=(hyperloglog64 const & other) {
		amm = other.amm;
		registers = other.registers;
	}

	hyperloglog64& operator=(hyperloglog64 && other) {
		amm = other.amm;
		registers.swap(std::move(other.registers));
	}

	void swap(hyperloglog64 & other) {
		std::swap(amm, other.amm);
		std::swap(registers, other.registers);

	}

	inline void update(T const & val) {
        update_via_hashval(h(val));
	}
	inline void update_via_hashval(uint64_t const & hash) {
        uint64_t i = hash >> (64 - precision);   // first precision bits are for register id
        REG_T rank = leftmost_set_bit(hash & val_mask) - precision;  // then find leading 1 in remaining
        if (rank > registers[i]) {
            registers[i] = rank;
        }
	}

	double estimate() const {
        double estimate = 0.0;
        double sum = 0.0;

        // compute the denominator of the harmonic mean
        for (size_t i = 0; i < nRegisters; i++) {
            sum += 1.0 / static_cast<double>(1ULL << registers[i]);
        }
        estimate = amm / sum; // E in the original paper

        if (estimate <= (5 * (nRegisters >> 1))) {
        	size_t zeros = count_zeros();
        	if (zeros > 0) {
//        		std::cout << "linear_count: zero: " << zeros << " estimate " << estimate << std::endl;
				return linear_count(zeros);
        	} else
        		return estimate;
//        } else if (estimate > (1.0 / 30.0) * pow_2_32) {
        	// don't need this because of 64bit.
//            estimate = neg_pow_2_32 * log(1.0 - (estimate / pow_2_32));
        } else
        	return estimate;
	}



	void merge(hyperloglog64 const & other) {
		// procisions identical, so don't need to check number of registers either.

		// iterate over both, merge, and update the zero count.
		for (size_t i = 0; i < nRegisters; ++i) {
			registers[i] = std::max(registers[i], other.registers[i]);
		}
	}

	inline uint64_t count_zeros() const {
		uint64_t zeros = 0;
		for (size_t i = 0; i < nRegisters; i++) {
			if (registers[i] == 0) ++zeros;
		}
		return zeros;
	}

	inline double linear_count(uint64_t const & zeros) const {
		return static_cast<double>(nRegisters) *
				std::log(static_cast<double>(nRegisters)/ static_cast<double>(zeros));  //natural log
	}

	void clear() {
		registers.fill(static_cast<REG_T>(0));
	}

};

//========= specializations for std::hash. std::hash passes through primitive types, e.g. uint8_t.
//   this creates problems in that the high bits are all 0.  so for std::hash,
//   we use the lower bits for register and upper bits for the bit counts.
//   also, for small datatypes, the number of leading zeros may be artificially large.
//      we can fix this by tracking the minimum leading zero as well.

template <typename T, uint8_t precision>
class hyperloglog64<T, std::hash<T>, precision> {

	  static_assert((precision < (sizeof(T) * 8)) && (precision >= 4) && (precision <= 16),
			  "precision must be set to lower than sizeof(T) * 8 and in range [4, 16]");

protected:
	  static constexpr uint8_t data_bits = ((sizeof(T) * 8) > 64) ? 64 : (sizeof(T) * 8);
	  static constexpr uint8_t value_bits = data_bits - precision;
	  static constexpr uint8_t lead_zero_bits = 64 - value_bits;  // in case sizeof(T) < 8.  exclude reg bits.

	  // register mask is the upper most precision bits.
  static constexpr uint64_t nRegisters = 0x1UL << precision;
  static constexpr uint64_t reg_mask = nRegisters - 1;   // need to shift right value_bits first.
  static constexpr uint64_t val_mask = ~(0x0UL) >> lead_zero_bits;
  double amm;

	using REG_T = uint8_t;
	std::array<REG_T, nRegisters> registers;  // stores count of leading zeros

  std::hash<T> h;

public:
  hyperloglog64() {
    registers.fill(static_cast<REG_T>(0));

        switch (precision) {
            case 4:
                amm = 0.673;
                break;
            case 5:
                amm = 0.697;
                break;
            case 6:
                amm = 0.709;
                break;
            default:
                amm = 0.7213 / (1.0 + 1.079 / nRegisters);
                break;
        }
        amm *= static_cast<double>(0x1UL << (precision + precision));
  }

  hyperloglog64(hyperloglog64 const & other) : amm(other.amm), registers(other.registers) {}

  hyperloglog64(hyperloglog64 && other) : amm(other.amm),
      registers(std::move(other.registers)) {}


  hyperloglog64& operator=(hyperloglog64 const & other) {
    amm = other.amm;
    registers = other.registers;
  }

  hyperloglog64& operator=(hyperloglog64 && other) {
    amm = other.amm;
    registers.swap(std::move(other.registers));
  }

  void swap(hyperloglog64 & other) {
    std::swap(amm, other.amm);
    std::swap(registers, other.registers);
  }

  inline void update(T const & val) {
        update_via_hashval(h(val));
  }
  inline void update_via_hashval(uint64_t const & hash) {
        uint64_t i = (hash >> value_bits) & reg_mask;   // first precision bits are for register id
        uint8_t rank = leftmost_set_bit(hash & val_mask) - lead_zero_bits;  // then find leading 1 in remaining
        if (rank > registers[i]) {
            registers[i] = rank;
        }
  }

  double estimate() const {
        double estimate = 0.0;
        double sum = 0.0;

        // compute the denominator of the harmonic mean
        for (size_t i = 0; i < nRegisters; i++) {
            sum += 1.0 / static_cast<double>(1ULL << registers[i] );
        }
        estimate = amm / sum; // E in the original paper

        if (estimate <= (5 * (nRegisters >> 1))) {
        	size_t zeros = count_zeros();
        	if (zeros > 0) {
//        		std::cout << "linear_count: zero: " << zeros << " estimate " << estimate << std::endl;
				return linear_count(zeros);
        	} else
        		return estimate;
//        } else if (estimate > (1.0 / 30.0) * pow_2_32) {
            // don't need below because of 64bit.
//            estimate = neg_pow_2_32 * log(1.0 - (estimate / pow_2_32));
        } else
          return estimate;
  }



  void merge(hyperloglog64 const & other) {
    // precisions identical, so don't need to check number of registers either.

    // iterate over both, merge, and update the zero count.
    for (size_t i = 0; i < nRegisters; ++i) {
      registers[i] = std::max(registers[i], other.registers[i]);
    }
  }

	inline uint64_t count_zeros() const {
		uint64_t zeros = 0;
		for (size_t i = 0; i < nRegisters; i++) {
			if (registers[i] == 0) ++zeros;
		}
		return zeros;
	}

	inline double linear_count(uint64_t const & zeros) const {
		return static_cast<double>(nRegisters) *
				std::log(static_cast<double>(nRegisters)/ static_cast<double>(zeros));  //natural log
	}

	void clear() {
		registers.fill(static_cast<REG_T>(0));
	}

};




#endif // KMERHASH_HYPERLOGLOG64_HPP_
