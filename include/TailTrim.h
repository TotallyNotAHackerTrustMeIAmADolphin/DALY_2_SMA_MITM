#pragma once

#include <stddef.h>
#include <stdint.h>
#include <vector>

// Pure rolling tail-trim, extracted out of SDLogger::readTail() (#43) so it
// has no Arduino/FreeRTOS dependency and test/test_tailtrim can include and
// run *this* code natively (`pio test -e native`), same pattern as
// Glideslope.h/CellSmoother.h/StatusFrame.h/CsvDecimation.h.
//
// Usage: construct once with maxBytes and the caller's own read-chunk size,
// feed() every chunk read from the source file in order, then call finish()
// once after the last feed(). data()/length() then give the retained tail.
namespace TailTrim
{
    class Trimmer
    {
    public:
        // Default chunkBytes when the caller doesn't pass its own - matches
        // SDLogger.cpp's actual read-buffer size (SdTuning::kSdReadChunkBytes)
        // at the time this was written. A caller with a different read
        // buffer size should pass it explicitly rather than rely on this.
        static constexpr size_t kDefaultChunkBytes = 512;

        // maxBytes: how many trailing bytes of the fed stream to retain.
        // chunkBytes: the size of each feed() call the caller will make
        // (its source read buffer) - reserve() below adds one chunk's
        // worth of slack on top of maxBytes so appending never reallocates
        // mid-scan: repeated grow/shrink across a file scanned in hundreds
        // of small chunks was observed live to fragment the heap badly
        // enough to make a later, unrelated allocation (the caller's own
        // copy of the finished tail into an HTTP response) fail silently,
        // even though this function's own output was correct - see
        // readTail()'s comment in SDLogger.cpp before this extraction.
        // Passing a chunkBytes bigger than what feed() is actually called
        // with just reserves extra headroom (harmless); a smaller one
        // reintroduces the reallocation this constructor exists to avoid,
        // so the caller must pass its real read-buffer size, not a guess.
        explicit Trimmer(size_t maxBytes, size_t chunkBytes = kDefaultChunkBytes)
            : maxBytes_(maxBytes), truncated_(false)
        {
            buf_.reserve(maxBytes + chunkBytes);
        }

        // Feed one chunk of raw file bytes, in the order read from the
        // source. Appends it, then - if the buffered content now exceeds
        // maxBytes - drops bytes off the front (keeping only the last
        // maxBytes) and marks truncated().
        void feed(const uint8_t *data, size_t len)
        {
            buf_.insert(buf_.end(), data, data + len);
            if (buf_.size() > maxBytes_)
            {
                buf_.erase(buf_.begin(), buf_.begin() + (buf_.size() - maxBytes_));
                truncated_ = true;
            }
        }

        // Call once after the last feed(): if truncation ever occurred,
        // drops the leading partial line (up to and including its first
        // '\n') so the retained tail doesn't start mid-line. Only fires
        // when truncation actually happened - a file that never exceeded
        // maxBytes is left completely untouched, since its first line is
        // real content from byte 0, not a truncation artifact. Same guard
        // as the pre-refactor code: doesn't drop the newline if it's the
        // very last byte (so a tail that happens to end right at a
        // newline isn't wiped out entirely).
        void finish()
        {
            if (!truncated_ || buf_.empty())
                return;
            for (size_t i = 0; i < buf_.size(); i++)
            {
                if (buf_[i] == '\n')
                {
                    if (i < buf_.size() - 1)
                        buf_.erase(buf_.begin(), buf_.begin() + i + 1);
                    return;
                }
            }
        }

        const char *data() const { return buf_.data(); }
        size_t length() const { return buf_.size(); }
        bool truncated() const { return truncated_; }

    private:
        std::vector<char> buf_;
        size_t maxBytes_;
        bool truncated_;
    };
}
