#pragma once

#include <stddef.h>
#include <stdint.h>

// Pure CSV line-accumulation + decimation, extracted out of
// SDLogger::readGraphSeries() (#43) so it has no Arduino/FreeRTOS
// dependency and test/test_csvdecimation can include and run *this* code
// natively (`pio test -e native`), same pattern as Glideslope.h/
// CellSmoother.h/StatusFrame.h. Doesn't know about TelemetrySchema or
// files: the adapter (SDLogger.cpp) resolves the field indices it wants
// via TelemetrySchema::index() and passes them in as bare integers; this
// class only ever extracts "the idx'th comma-separated field" from raw
// line bytes.
//
// Usage: construct once with the resolved field indices and the skip
// interval, then feed() every chunk read from the source file, in order
// (a line may span more than one feed() call - the accumulator buffers a
// line internally across calls, exactly like the pre-refactor
// SDLogger.cpp's lineBuf did inline). Call finish() once after the last
// feed() to flush a final line that had no trailing newline. Each kept
// line is written as "field,field,...,field\n" (in the given field order)
// into the caller-supplied output buffer, bounds-checked - never a String,
// per #43 (this header must compile under `platform = native`, which has
// no Arduino core).
namespace CsvDecimation
{
    // Matches the pre-refactor SDLogger.cpp's `char lineBuf[320]` exactly -
    // a full telemetry row is ~230 bytes today, generous margin.
    constexpr size_t kLineBufSize = 320;

    // Generous upper bound on how many fields a caller can request per
    // decimated line - readGraphSeries() resolves 7 today
    // (Timestamp..ReqI).
    constexpr size_t kMaxFields = 16;

    // Returns the idx'th comma-separated field of a (buf, len) line span
    // (0-based) by appending its bytes to out+*outLen, bounds-checked
    // against outCap; appends nothing if idx is past the line's last
    // comma-separated field (matches the pre-refactor csvFieldFromBuf()
    // returning "" in that case).
    inline void appendField(const char *lineBuf, size_t lineLen, size_t idx,
                             char *out, size_t outCap, size_t *outLen)
    {
        size_t start = 0;
        for (size_t i = 0; i < idx; i++)
        {
            size_t j = start;
            while (j < lineLen && lineBuf[j] != ',')
                j++;
            if (j >= lineLen)
                return; // field idx doesn't exist in this line -> empty, append nothing
            start = j + 1;
        }
        size_t end = start;
        while (end < lineLen && lineBuf[end] != ',')
            end++;
        for (size_t k = start; k < end && *outLen < outCap; k++)
            out[(*outLen)++] = lineBuf[k];
    }

    class Accumulator
    {
    public:
        // fieldIndices/fieldCount: the 0-based column indices, in the
        // order they should be comma-joined into each emitted output
        // line - resolved by the caller (e.g. via
        // TelemetrySchema::index()); this class never interprets them,
        // just extracts "field idx" from each kept line's raw bytes.
        // fieldCount is clamped to kMaxFields.
        // skip: every skip'th line (0-based line counter, first line fed
        // in is line 0 and is always kept) is decimated into the output.
        // skip == 0 is treated as 1 (keep every line), matching the
        // pre-refactor `if (skip == 0) skip = 1`.
        Accumulator(const size_t *fieldIndices, size_t fieldCount, size_t skip)
            : fieldCount_(fieldCount > kMaxFields ? kMaxFields : fieldCount),
              skip_(skip == 0 ? 1 : skip),
              lineLen_(0),
              lineIdx_(0),
              keepLine_(true) // lineIdx_ starts at 0, and 0 % skip_ == 0 always
        {
            for (size_t i = 0; i < fieldCount_; i++)
                fieldIndices_[i] = fieldIndices[i];
        }

        // Feed one chunk of raw file bytes (not necessarily line-aligned).
        // For each complete ('\n'-terminated) line found, if it's a kept
        // line per the skip interval, appends its decimated form to
        // out+*outLen (bounds-checked against outCap; *outLen is updated
        // to reflect what was written). A line longer than kLineBufSize is
        // safely truncated: only the first kLineBufSize-1 bytes are kept
        // for field extraction (matching the pre-refactor lineBuf's
        // `lineLen < sizeof(lineBuf) - 1` bounds check) - the rest of an
        // oversized line's bytes are still scanned (so the '\n' search
        // stays correct) but not stored or used for extraction.
        void feed(const uint8_t *data, size_t len, char *out, size_t outCap, size_t *outLen)
        {
            for (size_t i = 0; i < len; i++)
            {
                uint8_t c = data[i];
                if (c == '\n')
                {
                    flushCurrentLine(out, outCap, outLen);
                    lineLen_ = 0;
                    lineIdx_++;
                    keepLine_ = (lineIdx_ % skip_ == 0);
                }
                else if (lineLen_ < sizeof(lineBuf_) - 1)
                {
                    lineBuf_[lineLen_++] = (char)c;
                }
            }
        }

        // Call once after the last feed(), to flush a final line that had
        // no trailing newline (e.g. the writer task's last flush hadn't
        // landed yet on disk) - matches the pre-refactor code's
        // flushKeptLine() call after its read loop.
        void finish(char *out, size_t outCap, size_t *outLen)
        {
            flushCurrentLine(out, outCap, outLen);
        }

    private:
        void flushCurrentLine(char *out, size_t outCap, size_t *outLen)
        {
            if (!keepLine_ || lineLen_ == 0)
                return;
            for (size_t f = 0; f < fieldCount_; f++)
            {
                if (f > 0 && *outLen < outCap)
                    out[(*outLen)++] = ',';
                appendField(lineBuf_, lineLen_, fieldIndices_[f], out, outCap, outLen);
            }
            if (*outLen < outCap)
                out[(*outLen)++] = '\n';
        }

        size_t fieldIndices_[kMaxFields];
        size_t fieldCount_;
        size_t skip_;

        char lineBuf_[kLineBufSize];
        size_t lineLen_;
        size_t lineIdx_;
        bool keepLine_;
    };
}
