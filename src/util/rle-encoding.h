// Copyright 2012 Cloudera Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef IMPALA_RLE_ENCODING_H
#define IMPALA_RLE_ENCODING_H

#include <glog/logging.h>

#include <algorithm>

#include "gutil/port.h"
#include "util/bitmap.h"
#include "util/bit-stream-utils.inline.h"

namespace kudu {

// Utility classes to do run length encoding (RLE) for fixed bit width values.  If runs
// are sufficiently long, RLE is used, otherwise, the values are just bit-packed
// (literal encoding).
// For both types of runes, there is a byte aligned indicator which encodes the length
// of the run and the type of the run.
// This encoding has the benefit that when there aren't any long enough runs, values
// are always decoded at fixed (can be precomputed) bit offsets OR both the value and
// the run length are byte aligned. This allows for very efficient decoding
// implementations.
// The encoding is:
//    encoded-block := run *
//    run := literal-run | repeated-run
//    literal-run := literal-indicator < literal bytes >
//    repeated-run := repeated-indicator < repeated value. padded to byte boundary >
//    literal-indicator := varint_encode( number_of_groups << 1 | 1)
//    repeated-indicator := varint_encode( number_of_repetitions << 1 )
//
// Each run is preceded by a varint. The varint's least significant bit is
// used to indicate whether the run is a literal run or a repeated run. The rest
// of the varint is used to determine the length of the run (eg how many times the
// value repeats).
//
// In the case of literal runs, the run length is always a multiple of 8 (i.e. encode
// in groups of 8), so that no matter the bit-width of the value, the sequence will end
// on a byte boundary without padding.
// Given that we know it is a multiple of 8, we store the number of 8-groups rather than
// the actual number of encoded ints.
// There is a break-even point when it is more storage efficient to do run length
// encoding.  For 1 bit-width values, that point is 8 values.  They require 2 bytes
// for both the repeated encoding or the literal encoding.  This value can always
// be computed based on the bit-width.
// TODO: think about how to use this for strings.  The bit packing isn't quite the same.
//
// Examples with bit-width 1 (eg encoding booleans):
// ----------------------------------------
// 100 1s followed by 100 0s:
// <varint(100 << 1)> <1, padded to 1 byte> <varint(100 << 1)> <0, padded to 1 byte>
//  - (total 4 bytes)
//
// alternating 1s and 0s (200 total):
// 200 ints = 25 groups of 8
// <varint((25 << 1) | 1)> <25 bytes of values, bitpacked>
// (total 26 bytes, 1 byte overhead)
//
// TODO: this implementation is tailored to bit-width 1 and will need more work to
// make it general.

// Decoder class for RLE encoded data.
class RleDecoder {
 public:
  // Create a decoder object. buffer/buffer_len is the encoded data.
  RleDecoder(const uint8_t* buffer, int buffer_len)
    : bit_reader_(buffer, buffer_len),
      current_value_(false),
      repeat_count_(0),
      literal_count_(0) {
  }

  RleDecoder() {}

  // Skip n bits, and returns the number of set bits skipped
  size_t Skip(size_t to_skip);

  // Gets the next value.  Returns false if there are no more.
  bool Get(bool *val);

  // Gets the next range of the same 'val'. Returns false if there are no more
  bool GetNextRun(bool *val, size_t *run_length);

 private:
  bool ReadHeader();

  BitReader bit_reader_;
  bool current_value_;
  uint32_t repeat_count_;
  uint32_t literal_count_;
};

// Class to incrementally build the rle data.
// The encoding has two modes: encoding repeated runs and literal runs.
// If the run is sufficiently short, it is more efficient to encode as a literal run.
// This class does so by buffering 8 values at a time.  If they are not all the same
// they are added to the literal run.  If they are the same, they are added to the
// repeated run.  When we switch modes, the previous run is flushed out.
class RleEncoder {
 public:
  // buffer: buffer to write bits to.
  explicit RleEncoder(faststring *buffer)
    : bit_writer_(buffer) {
    Clear();
  }

  // Encode value.  Returns true if the value fits in buffer, false otherwise.
  void Put(bool value, size_t run_length = 1);

  // Flushes any pending values to the underlying buffer.
  // Returns the total number of bytes written
  int Flush();

  // Resets all the state in the encoder.
  void Clear();

  // Returns pointer to underlying buffer
  faststring *buffer() { return bit_writer_.buffer(); }
  int32_t len() { return bit_writer_.bytes_written(); }

 private:
  // Flushes any buffered values.  If this is part of a repeated run, this is largely
  // a no-op.
  // If it is part of a literal run, this will call FlushLiteralRun, which writes
  // out the buffered literal values.
  // If 'done' is true, the current run would be written even if it would normally
  // have been buffered more.  This should only be called at the end, when the
  // encoder has received all values even if it would normally continue to be
  // buffered.
  void FlushBufferedValues(bool done);

  // Flushes literal values to the underlying buffer.  If update_indicator_byte,
  // then the current literal run is complete and the indicator byte is updated.
  void FlushLiteralRun(bool update_indicator_byte);

  // Flushes a repeated run to the underlying buffer.
  void FlushRepeatedRun();

  // Underlying buffer.
  BitWriter bit_writer_;

  // We need to buffer at most 8 values for literals.  This happens when the
  // bit_width is 1 (so 8 values fit in one byte).
  int64_t buffered_values_[8];

  // Number of values in buffered_values_
  int num_buffered_values_;

  // The current (also last) value that was written and the count of how
  // many times in a row that value has been seen.  This is maintained even
  // if we are in a literal run.  If the repeat_count_ get high enough, we switch
  // to encoding repeated runs.
  bool current_value_;
  int repeat_count_;

  // Number of literals in the current run.  This does not include the literals
  // that might be in buffered_values_.  Only after we've got a group big enough
  // can we decide if they should part of the literal_count_ or repeat_count_
  int literal_count_;

  // Index of a byte in the underlying buffer that stores the indicator byte.
  // This is reserved as soon as we need a literal run but the value is written
  // when the literal run is complete.
  int literal_indicator_byte_;
};

inline bool RleDecoder::ReadHeader() {
  if (PREDICT_FALSE(literal_count_ == 0 && repeat_count_ == 0)) {
    // Read the next run's indicator int, it could be a literal or repeated run
    // The int is encoded as a vlq-encoded value.
    int32_t indicator_value = 0;
    bool result = bit_reader_.GetVlqInt(&indicator_value);
    if (PREDICT_FALSE(!result)) {
      return(false);
    }

    // lsb indicates if it is a literal run or repeated run
    bool is_literal = indicator_value & 1;
    if (is_literal) {
      literal_count_ = (indicator_value >> 1) * 8;
      DCHECK_GT(literal_count_, 0);
    } else {
      repeat_count_ = indicator_value >> 1;
      DCHECK_GT(repeat_count_, 0);
      result = bit_reader_.GetBool(&current_value_);
      DCHECK(result);
    }
  }
  return true;
}

inline bool RleDecoder::Get(bool *val) {
  if (PREDICT_FALSE(!ReadHeader())) {
    return false;
  }

  if (PREDICT_TRUE(repeat_count_ > 0)) {
    *val = current_value_;
    --repeat_count_;
  } else {
    DCHECK(literal_count_ > 0);
    bool result = bit_reader_.GetBool(val);
    DCHECK(result);
    --literal_count_;
  }

  return true;
}

inline bool RleDecoder::GetNextRun(bool *val, size_t *run_length) {
  *run_length = 0;
  while (ReadHeader()) {
    if (PREDICT_TRUE(repeat_count_ > 0)) {
      if (PREDICT_FALSE(*run_length > 0 && *val != current_value_)) {
        return true;
      }
      *val = current_value_;
      *run_length += repeat_count_;
      repeat_count_ = 0;
    } else {
      DCHECK(literal_count_ > 0);
      if (*run_length == 0) {
        bool result = bit_reader_.GetBool(val);
        DCHECK(result);
        literal_count_--;
        (*run_length)++;
      }

      while (literal_count_ > 0) {
        bool result = bit_reader_.GetBool(&current_value_);
        DCHECK(result);
        if (current_value_ != *val) {
          bit_reader_.RewindBool();
          return true;
        }
        (*run_length)++;
        literal_count_--;
      }
    }
  };
  return (*run_length) > 0;
}

inline size_t RleDecoder::Skip(size_t to_skip) {
  size_t set_count = 0;
  while (to_skip > 0) {
    bool result = ReadHeader();
    DCHECK(result);

    if (PREDICT_TRUE(repeat_count_ > 0)) {
      size_t nskip = (repeat_count_ < to_skip) ? repeat_count_ : to_skip;
      repeat_count_ -= nskip;
      to_skip -= nskip;
      set_count = current_value_ * nskip;
    } else {
      DCHECK(literal_count_ > 0);
      size_t nskip = (literal_count_ < to_skip) ? literal_count_ : to_skip;
      literal_count_ -= nskip;
      to_skip -= nskip;
      while (nskip--) {
        bool value = false;
        bool result = bit_reader_.GetBool(&value);
        DCHECK(result);
        set_count += value;
      }
    }
  }
  return set_count;
}

// This function buffers input values 8 at a time.  After seeing all 8 values,
// it decides whether they should be encoded as a literal or repeated run.
inline void RleEncoder::Put(bool value, size_t run_length) {
  DCHECK_LT(value, 1 << 1);

  // TODO(perf): remove the loop and use the repeat_count_
  while (run_length--) {
    if (PREDICT_TRUE(current_value_ == value)) {
      ++repeat_count_;
      if (repeat_count_ > 8) {
        // This is just a continuation of the current run, no need to buffer the
        // values.
        // Note that this is the fast path for long repeated runs.
        continue;
      }
    } else {
      if (repeat_count_ >= 8) {
        // We had a run that was long enough but it has ended.  Flush the
        // current repeated run.
        DCHECK_EQ(literal_count_, 0);
        FlushRepeatedRun();
      }
      repeat_count_ = 1;
      current_value_ = value;
    }

    buffered_values_[num_buffered_values_] = value;
    if (++num_buffered_values_ == 8) {
      DCHECK_EQ(literal_count_ % 8, 0);
      FlushBufferedValues(false);
    }
  }
}


inline void RleEncoder::FlushLiteralRun(bool update_indicator_byte) {
  if (literal_indicator_byte_ < 0) {
    // The literal indicator byte has not been reserved yet, get one now.
    literal_indicator_byte_ = bit_writer_.GetByteIndexAndAdvance();
    DCHECK(literal_indicator_byte_ >= 0);
  }

  // Write all the buffered values as bit packed literals
  for (int i = 0; i < num_buffered_values_; ++i) {
    bit_writer_.PutBool(buffered_values_[i]);
  }

  num_buffered_values_ = 0;
  if (update_indicator_byte) {
    // At this point we need to write the indicator byte for the literal run.
    // We only reserve one byte, to allow for streaming writes of literal values.
    // The logic makes sure we flush literal runs often enough to not overrun
    // the 1 byte.
    int num_groups = BitmapSize(literal_count_);
    DCHECK_LT(num_groups, 128);
    uint32_t indicator_value = (num_groups << 1) | 1;
    DCHECK_EQ(indicator_value & 0xFFFFFF80, 0);
    buffer()->data()[literal_indicator_byte_] = indicator_value;
    literal_indicator_byte_ = -1;
    literal_count_ = 0;
  }
}

inline void RleEncoder::FlushRepeatedRun() {
  DCHECK_GT(repeat_count_, 0);
  // The lsb of 0 indicates this is a repeated run
  int32_t indicator_value = repeat_count_ << 1 | 0;
  bit_writer_.PutVlqInt(indicator_value);
  bit_writer_.PutAligned<uint8_t>(current_value_);
  num_buffered_values_ = 0;
  repeat_count_ = 0;
}

// Flush the values that have been buffered.  At this point we decide whether
// we need to switch between the run types or continue the current one.
inline void RleEncoder::FlushBufferedValues(bool done) {
  if (repeat_count_ >= 8) {
    // Clear the buffered values.  They are part of the repeated run now and we
    // don't want to flush them out as literals.
    num_buffered_values_ = 0;
    if (literal_count_ != 0) {
      // There was a current literal run.  All the values in it have been flushed
      // but we still need to update the indicator byte.
      DCHECK_EQ(literal_count_ % 8, 0);
      DCHECK_EQ(repeat_count_, 8);
      FlushLiteralRun(true);
    }
    DCHECK_EQ(literal_count_, 0);
    return;
  }

  literal_count_ += num_buffered_values_;
  int num_groups = BitmapSize(literal_count_);
  if (num_groups + 1 >= (1 << 6)) {
    // We need to start a new literal run because the indicator byte we've reserved
    // cannot store more values.
    DCHECK(literal_indicator_byte_ >= 0);
    FlushLiteralRun(true);
  } else {
    FlushLiteralRun(done);
  }
  repeat_count_ = 0;
}

inline int RleEncoder::Flush() {
  if (literal_count_ > 0 || repeat_count_ > 0 || num_buffered_values_ > 0) {
    bool all_repeat = literal_count_ == 0 &&
        (repeat_count_ == num_buffered_values_ || num_buffered_values_ == 0);
    // There is something pending, figure out if it's a repeated or literal run
    if (repeat_count_ > 0 && all_repeat) {
      FlushRepeatedRun();
    } else  {
      literal_count_ += num_buffered_values_;
      FlushLiteralRun(true);
      repeat_count_ = 0;
    }
  }
  DCHECK_EQ(num_buffered_values_, 0);
  DCHECK_EQ(literal_count_, 0);
  DCHECK_EQ(repeat_count_, 0);
  return bit_writer_.Finish();
}

inline void RleEncoder::Clear() {
  current_value_ = false;
  repeat_count_ = 0;
  num_buffered_values_ = 0;
  literal_count_ = 0;
  literal_indicator_byte_ = -1;
  bit_writer_.Clear();
}

}
#endif
